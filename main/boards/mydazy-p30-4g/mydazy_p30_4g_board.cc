#include "dual_network_board.h"
#include "ml307_board.h"
#include "wifi_board.h"
#include "assets/lang_config.h"
#include "codecs/box_audio_codec.h"
#include "display/display.h"
#include "display/emote_display.h"
#include "display/lcd_display.h"
#include "display/ui_display.h"
#include "esp_lcd_jd9853.h"
#include "axs5106l_touch.h"
#include "sc7a20h.h"
#include "i2c_bus_worker.h"
#include "application.h"
#include "audio/music_player.h"
#include "button.h"
#include "config.h"
#include "settings.h"
#include "system_info.h"
#include "system_reset.h"
#include "network_interface.h"
#include "http.h"
#include "power_save_timer.h"
#include "power_manager.h"
#include "mcp_server.h"
#include "ota.h"
#include "education_mcp_tools.h"
#include "assets.h"
#include <font_awesome.h>
#include <sys/stat.h>
#include <sys/unistd.h>
#include <dirent.h>
#include "mbedtls/md5.h"
#include "esp_partition.h"

// ESP-IDF 头文件
#include <esp_log.h>
#include <esp_err.h>
#include <esp_lvgl_port.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_spiffs.h>
#include <esp_sleep.h>
#include <esp_wifi.h>
#include <nvs_flash.h>
#include <esp_vfs.h>
#include "wifi_station.h"
#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <driver/rtc_io.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>

#include <atomic>
#include <math.h>

#define TAG "MyDazyP30_4GBoard"

namespace {
// 小工具：处于 Speaking 时先 Abort，避免状态冲突
inline void AbortIfSpeaking() {
    auto& app = Application::GetInstance();
    if (app.GetDeviceState() == kDeviceStateSpeaking) {
        app.AbortSpeaking(kAbortReasonNone);
    }
}

// 同时打断 Speaking 与 Listening（双击退出/出厂确认/9 连击等场景）
// 与 AbortIfSpeaking 区别：StopListening(false) 不送 stop 信号，让服务端立即释放，不留 Listening 等 TTS
inline void AbortAnyConversation() {
    auto& app = Application::GetInstance();
    auto state = app.GetDeviceState();
    if (state == kDeviceStateSpeaking) {
        app.AbortSpeaking(kAbortReasonNone);
    } else if (state == kDeviceStateListening) {
        app.StopListening(false);
    }
}

// 切换网络前统一暂停：① 停 MP3 播放（含退 Player UI）② 打断对话（Speaking/Listening）
// 避免重启或进配网时音频任务/协议通道未释放导致的资源残留与异响
inline void PauseAudioAndChatBeforeSwitch() {
    auto& app = Application::GetInstance();

    if (MusicPlayer::GetInstance().IsPlaying()) {
        ESP_LOGI(TAG, "切网前停止 MP3 播放");
        MusicPlayer::GetInstance().Stop();
        if (auto* ui = dynamic_cast<UiDisplay*>(Board::GetInstance().GetDisplay())) {
            ui->SwitchOutPlayerMode();
        }
    }

    auto state = app.GetDeviceState();
    if (state == kDeviceStateSpeaking || state == kDeviceStateListening) {
        ESP_LOGI(TAG, "切网前打断对话 (state=%d)", (int)state);
        app.AbortSpeaking(kAbortReasonNone);
    }
}
}  // namespace

class MyDazyP30_4GBoard : public DualNetworkBoard {
private:
    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    i2c_worker_handle_t     i2c_worker_ = nullptr;   /* v3.0+ 共享总线串行化 worker */
    Button boot_button_;
    Button volume_up_button_;
    Button volume_down_button_;

    // SC7A20H 三轴加速度传感器（mydazy/esp_sc7a20h component）
    sc7a20h_handle_t sc7a20h_sensor_ = nullptr;
    bool sc7a20h_initialized_ = false;

    // 显示
    Display* display_ = nullptr;
    PowerManager* power_manager_ = nullptr;
    PowerSaveTimer* power_save_timer_ = nullptr;

    // 触摸屏（mydazy/esp_lcd_touch_axs5106l component）
    axs5106l_touch_handle_t touch_driver_ = nullptr;

    // 音频编解码器
    BoxAudioCodec* audio_codec_ = nullptr;

    bool first_boot_ = false;
    bool is_alarm_clock_ = false;

    // 音量调节任务（多核共享，用 atomic）
    TaskHandle_t vol_up_task_ = nullptr;
    TaskHandle_t vol_down_task_ = nullptr;
    std::atomic<bool> vol_up_running_{false};
    std::atomic<bool> vol_down_running_{false};

    // 长按录音状态跟踪（按键线程 vs 主循环）
    std::atomic<bool> is_recording_for_test_{false};
    std::atomic<bool> is_recording_for_send_{false};

    // 关机三段长按状态（3秒提醒、5秒执行、PressUp 取消）
    std::atomic<bool> shutdown_armed_{false};

    // 欢迎音异步任务句柄
    std::atomic<TaskHandle_t> welcome_task_handle_{nullptr};

    // 恢复出厂设置确认状态
    std::atomic<bool> waiting_factory_reset_confirm_{false};
    uint64_t factory_reset_request_time_ = 0;

    // 触摸 RF 风暴节流：1.2s 内 ≥3 个 gesture 视为干扰，静默 3s
    int64_t last_gesture_us_ = 0;
    int64_t storm_mute_until_us_ = 0;
    uint8_t gesture_burst_ = 0;
    // PTT 配对保护：仅在 LongPress 真正启动了录音时，LongPressRelease 才生效
    std::atomic<bool> ptt_active_{false};
    // PTT 超时兜底：60s 内若未收到 release（RF 干扰或硬件离手丢事件），自动 stop
    esp_timer_handle_t ptt_timeout_timer_ = nullptr;

    // 状态定时上报
    esp_timer_handle_t status_timer_ = nullptr;

    // 摇一摇识别任务句柄（仅识别 + 日志，业务接入留待后续）
    TaskHandle_t shake_detect_task_ = nullptr;

    // ========================================================
    // 硬件初始化
    // ========================================================

    void InitializeI2c() {
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = I2C_NUM_1,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            // 4G RF 干扰场景下提高毛刺过滤阈值（~187ns，原值 7≈87ns）
            .glitch_ignore_cnt = 15,
            .intr_priority = 3,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 0,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));

        /* v3.0+: 共享总线串行化 worker —— 5 driver（codec×2 / 触摸 / sensor / NFC）
           的 I2C 访问全部汇入此 worker 单线程顺序执行，杜绝跨 transaction 污染。
           Core 0 P10 与 audio_output 同位，高于 LVGL P5 / 网络 P5。 */
        i2c_worker_config_t wcfg = I2C_WORKER_DEFAULT_CONFIG(i2c_bus_);
        ESP_ERROR_CHECK(i2c_worker_create(&wcfg, &i2c_worker_));
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {
            .mosi_io_num = DISPLAY_SPI_MOSI,
            .miso_io_num = DISPLAY_SPI_MISO,
            .sclk_io_num = DISPLAY_SPI_SCLK,
            .quadwp_io_num = GPIO_NUM_NC,
            .quadhd_io_num = GPIO_NUM_NC,
            .max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t),
            .flags = SPICOMMON_BUSFLAG_MASTER,
        };
        ESP_ERROR_CHECK(spi_bus_initialize(DISPLAY_SPI_HOST, &buscfg, SPI_DMA_DISABLED));
    }

    void HandleWakeupCause() {
        esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
        esp_reset_reason_t reset_reason = esp_reset_reason();
        // 诊断日志：同时打印 wakeup_cause + reset_reason，可区分
        ESP_LOGI(TAG, "Boot diag: wakeup_cause=%d, reset_reason=%d", (int)wakeup_reason, (int)reset_reason);

        switch (wakeup_reason) {
            case ESP_SLEEP_WAKEUP_EXT0:
                ESP_LOGI(TAG, "从开机键唤醒");
                first_boot_ = true;
                // 按 BOOT 键唤醒时，要求持续按住 2 秒才真正开机（防止口袋误触）
                if (!CheckBootHoldOnWakeup()) {
                    ESP_LOGI(TAG, "开机长按未达 2 秒，立即回深睡");
                    EnterDeepSleep(true);  // 不会返回
                }
                break;
            case ESP_SLEEP_WAKEUP_EXT1:
                ESP_LOGI(TAG, "从陀螺仪唤醒 (ext1_status=0x%llx)", esp_sleep_get_ext1_wakeup_status());
                first_boot_ = true;
                break;
            case ESP_SLEEP_WAKEUP_TIMER:
                ESP_LOGI(TAG, "从定时器唤醒");
                is_alarm_clock_ = true;
                break;
            default:
                ESP_LOGI(TAG, "首次启动或复位 (wakeup=%d, reset=%d)", (int)wakeup_reason, (int)reset_reason);
                first_boot_ = true;
                break;
        }
    }

    // 开机长按 2 秒检测：返回 true=按够 2 秒可开机，false=未到 2 秒应回深睡
    bool CheckBootHoldOnWakeup() {
        gpio_config_t io_conf = {
            .pin_bit_mask = (1ULL << BOOT_BUTTON_GPIO),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&io_conf);

        if (gpio_get_level(BOOT_BUTTON_GPIO) != 0) {
            // 唤醒瞬间已经松开（按一下就放）→ 不开机
            return false;
        }

        const int kHoldRequiredMs = 2000;
        const int kStepMs = 50;
        int elapsed = 0;
        while (elapsed < kHoldRequiredMs) {
            vTaskDelay(pdMS_TO_TICKS(kStepMs));
            if (gpio_get_level(BOOT_BUTTON_GPIO) != 0) {
                return false;  // 中途松开 → 不开机
            }
            elapsed += kStepMs;
        }
        ESP_LOGI(TAG, "开机长按 2 秒确认，准备启动");
        return true;
    }

    void InitializeGpio() {
        // 先关闭背光，防止重启后显示随机GRAM数据
        gpio_reset_pin(DISPLAY_BACKLIGHT);
        gpio_set_direction(DISPLAY_BACKLIGHT, GPIO_MODE_OUTPUT);
        gpio_set_level(DISPLAY_BACKLIGHT, 0);

        // PA_EN 置高
        gpio_reset_pin(AUDIO_CODEC_PA_PIN);
        gpio_set_direction(AUDIO_CODEC_PA_PIN, GPIO_MODE_OUTPUT);
        gpio_set_level(AUDIO_CODEC_PA_PIN, 1);

        // 避免 hold_dis 释放后到 gpio_set_level 之间 GPIO 浮动 → LDO 接通毛刺 → LCD GRAM 闪烁
        gpio_config_t output_conf = {
            .pin_bit_mask = (1ULL << AUDIO_PWR_EN_GPIO),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&output_conf);
        gpio_set_level(AUDIO_PWR_EN_GPIO, 0);    // 先锁低
        rtc_gpio_hold_dis(AUDIO_PWR_EN_GPIO);    // 再释放 RTC hold（无浮动窗口）

        // 软启动音频电源
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(AUDIO_PWR_EN_GPIO, 1);
        ESP_LOGI(TAG, "音频电源已启用 (GPIO%d)", AUDIO_PWR_EN_GPIO);

        // 音频芯片上电稳定时间 200ms（ES8311/ES7210 datasheet 建议）
        vTaskDelay(pdMS_TO_TICKS(200));

        // 配置TE输入GPIO（当前未启用 VSYNC，硬件预留）
        gpio_config_t input_conf = {
            .pin_bit_mask = (1ULL << DISPLAY_LCD_TE),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&input_conf);

        HandleWakeupCause();
    }

    void InitializeSc7a20h() {
        sc7a20h_sensor_ = sc7a20h_init(i2c_worker_, 384 /*mg*/, 160 /*ms*/);
        sc7a20h_initialized_ = (sc7a20h_sensor_ != nullptr);
        if (!sc7a20h_initialized_) {
            ESP_LOGE(TAG, "SC7A20H 初始化失败");
        }
    }

    // 摇一摇识别（孩子拿起来甩 → 切话题 / 换 emoji / 抽奖等）
    //       触发后冷却 1.5s 防连发。
    static void ShakeDetectTaskEntry(void* arg) {
        auto* board = static_cast<MyDazyP30_4GBoard*>(arg);
        constexpr TickType_t kPeriodTicks   = pdMS_TO_TICKS(100);
        constexpr int32_t    kStrongMgSq    = 1500 * 1500;   // 偏离 1g 阈值的平方
        constexpr int32_t    kGravitySq     = 1000 * 1000;   // (1g)²
        constexpr int        kWindowSize    = 6;             // 600ms 窗口
        constexpr int        kStrongTarget  = 3;             // 窗口内 ≥3 帧 = 摇
        constexpr int64_t    kCooldownUs    = 1500 * 1000LL; // 触发后 1.5s 冷却

        int32_t window_dev_sq[kWindowSize] = {0};   // 每帧 (|a|² - 1g²) 绝对值
        int     wi = 0;
        int64_t last_shake_us = 0;

        TickType_t last_wake = xTaskGetTickCount();
        while (true) {
            vTaskDelayUntil(&last_wake, kPeriodTicks);
            if (!board->sc7a20h_initialized_ || !board->sc7a20h_sensor_) continue;

            int16_t x, y, z;
            if (sc7a20h_read_mg(board->sc7a20h_sensor_, &x, &y, &z) != ESP_OK) continue;

            // 模长平方 - 重力平方（无 sqrt）
            int32_t mag_sq = (int32_t)x * x + (int32_t)y * y + (int32_t)z * z;
            int32_t dev    = mag_sq - kGravitySq;
            if (dev < 0) dev = -dev;
            window_dev_sq[wi] = dev;
            wi = (wi + 1) % kWindowSize;

            int     strong = 0;
            int32_t peak   = 0;
            for (int i = 0; i < kWindowSize; ++i) {
                if (window_dev_sq[i] > kStrongMgSq) strong++;
                if (window_dev_sq[i] > peak) peak = window_dev_sq[i];
            }

            int64_t now_us = esp_timer_get_time();
            if (strong >= kStrongTarget && (now_us - last_shake_us) > kCooldownUs) {
                ESP_LOGI(TAG, "Shake detected! peak_dev_sq=%ld, strong=%d/%d",
                         (long)peak, strong, kWindowSize);
                last_shake_us = now_us;
                // TODO: 后续在此挂业务——切话题 / 换 emoji / 抽奖
            }
        }
    }

    void StartShakeDetect() {
        if (!sc7a20h_initialized_) return;
        // Core 1（PSRAM 栈红线 + Core 0 红线均不沾）+ P1 后台 + 2.5KB 内部 RAM 栈
        // 100ms 周期，I2C 读 6B + 一次 sqrtf；CPU < 1%
        xTaskCreatePinnedToCore(&MyDazyP30_4GBoard::ShakeDetectTaskEntry,
                                "accel_shake", 2560, this, 1, &shake_detect_task_, 1);
    }

    void PrepareTouchHardware() {
        axs5106l_touch_config_t cfg = {
            .worker     = i2c_worker_,
            .rst_gpio   = TOUCH_RST_NUM,
            .int_gpio   = TOUCH_INT_NUM,
            .width      = DISPLAY_WIDTH,
            .height     = DISPLAY_HEIGHT,
            .wake_cb    = &MyDazyP30_4GBoard::OnTouchWake,
            .gesture_cb = &MyDazyP30_4GBoard::OnTouchGesture,
            .cb_ctx     = this,
        };
        if (axs5106l_touch_new(&cfg, &touch_driver_) != ESP_OK) {
            ESP_LOGE(TAG, "触摸屏硬件初始化失败");
            touch_driver_ = nullptr;
        }
    }

    // C 回调蹦床：把 void* user_ctx 还原为 board 实例
    static void OnTouchWake(void *ctx) {
        static_cast<MyDazyP30_4GBoard*>(ctx)->WakeUp();
    }

    static void OnTouchGesture(axs5106l_gesture_t g, int16_t x, int16_t y, void *ctx) {
        auto* self = static_cast<MyDazyP30_4GBoard*>(ctx);
        self->WakeUp();

        // 触摸交互矩阵（量产稳定向）：
        //   SINGLE_CLICK         → Idle 唤醒 / Speaking 打断（与 BOOT 单击同义）
        //   DOUBLE_CLICK         → 退出对话回时钟主屏（孩子最熟悉的"退出"心智）
        //   LONG_PRESS / RELEASE → 微信 PTT，按住说话松开发送（500ms 门槛，驱动内）
        //   SWIPE_DOWN/UP/LEFT/RIGHT → 全部丢弃（ControlCenter 量产期已 stub，无入口）
        bool is_click        = (g == AXS5106L_GESTURE_SINGLE_CLICK);
        bool is_double_click = (g == AXS5106L_GESTURE_DOUBLE_CLICK);
        bool is_long_press   = (g == AXS5106L_GESTURE_LONG_PRESS);
        bool is_long_release = (g == AXS5106L_GESTURE_LONG_PRESS_RELEASE);
        if (!is_click && !is_double_click && !is_long_press && !is_long_release) return;

        // 状态栏区域（顶部 HEADER_HEIGHT=36 px）单击/双击由 LVGL CLICKED 独占处理
        // 这里跳过避免双路径同时唤醒/打断
        if ((is_click || is_double_click) && y < 36) {
            ESP_LOGD(TAG, "状态栏点击交由 LVGL 处理，driver 路径忽略");
            return;
        }
        (void)x;

        // RF 风暴节流：仅对 SINGLE_CLICK / DOUBLE_CLICK 计 burst。
        // LONG_PRESS / RELEASE 是低频长事件（按住-松开成对），不计入风暴指标。
        if (is_click || is_double_click) {
            int64_t now = esp_timer_get_time();
            if (now < self->storm_mute_until_us_) {
                ESP_LOGD(TAG, "RF 风暴静默期，丢弃 gesture=%d", (int)g);
                return;
            }
            if (now - self->last_gesture_us_ < 1200000) {
                if (++self->gesture_burst_ >= 3) {
                    ESP_LOGW(TAG, "检测到 RF 触摸风暴，静默 3s");
                    self->storm_mute_until_us_ = now + 3000000;
                    self->gesture_burst_ = 0;
                    return;
                }
            } else {
                self->gesture_burst_ = 1;
            }
            self->last_gesture_us_ = now;
        }

        if (is_click) {
            self->HandleTouchSingleClick();
        } else if (is_double_click) {
            self->HandleTouchDoubleClick();
        } else if (is_long_press) {
            self->HandleTouchLongPress();
        } else if (is_long_release) {
            self->HandleTouchLongPressRelease();
        }
    }

    void InitializeTouch() {
        if (touch_driver_ == nullptr) return;

        // v4.0 极简：回调已通过 cfg 注入，attach_lvgl 失败仅置空 handle 不再 del。
        if (axs5106l_touch_attach_lvgl(touch_driver_) != ESP_OK) {
            ESP_LOGE(TAG, "触摸屏 LVGL attach 失败");
            touch_driver_ = nullptr;
            return;
        }
        ESP_LOGI(TAG, "触摸屏初始化完成（v4.0 worker 路径）");
    }

    void HandleTouchSingleClick() {
        Settings settings("status", false);
        int touch_interrupt = settings.GetInt("touchInterrupt", 1);
        if (!touch_interrupt) return;

        auto& app = Application::GetInstance();
        auto state = app.GetDeviceState();

        // Player 模式下：屏幕中央 LVGL 按钮独占处理 Pause/Resume，
        // gesture SingleClick 不再 toggle，避免双触发互相抵消（"按了没反应"）。
        if (MusicPlayer::GetInstance().IsPlaying()) {
            ESP_LOGD(TAG, "Player 模式 SingleClick 由 LVGL 按钮处理，gesture 忽略");
            return;
        }

        if (state == kDeviceStateIdle) {
            ESP_LOGI(TAG, "单击唤醒对话");
            app.PlaySound(Lang::Sounds::OGG_WAKEUP);
            vTaskDelay(pdMS_TO_TICKS(800));
            app.ToggleChatState();
        } else if (state == kDeviceStateSpeaking) {
            ESP_LOGI(TAG, "单击打断TTS");
            app.AbortSpeaking(kAbortReasonNone);
        }
    }

    // 触摸长按 PTT：微信对讲式"按住说话，松开发送"（500ms 门槛由驱动 LONG_PRESS_TIME_US 决定）
    // 行为：Idle/Speaking → AbortSpeaking + StartListening(ManualStop)
    // ManualStop 模式下服务端不会因 VAD 自动停录，等 release 才送 stop 信号
    // MP3 播放中让位给屏幕暂停键
    void HandleTouchLongPress() {
        auto& app = Application::GetInstance();

        if (MusicPlayer::GetInstance().IsPlaying()) {
            ESP_LOGD(TAG, "Player 模式忽略长按 PTT");
            return;
        }

        auto state = app.GetDeviceState();
        if (state == kDeviceStateIdle || state == kDeviceStateSpeaking) {
            ESP_LOGI(TAG, "长按PTT：开始录音(ManualStop), state=%u", state);
            app.StartListening();
            ptt_active_ = true;
            ArmPttTimeout();   // 60s 兜底防 release 丢失
        } else {
            ESP_LOGD(TAG, "长按PTT忽略：state=%u 不在 Idle/Speaking", state);
        }
    }

    // 松开 PTT：停止录音 + 发送提示音
    // 仅在 LongPress 真正启动了录音时才执行，防止 RF 风暴假触的孤立 release 误停 Listening
    void HandleTouchLongPressRelease() {
        DisarmPttTimeout();
        if (!ptt_active_.exchange(false)) {
            ESP_LOGD(TAG, "长按 release 忽略：PTT 未启动（可能是 RF 干扰假触）");
            return;
        }
        auto& app = Application::GetInstance();
        if (app.GetDeviceState() == kDeviceStateListening) {
            ESP_LOGI(TAG, "PTT松开：停止录音 + 发送提示音");
            // play_sound=true：脱离 ManualStop、保持 Listening 等服务端 TTS（不切 Idle 不闪主屏）
            app.StopListening(true);
            app.PlaySound(Lang::Sounds::OGG_POPUP);
        }
    }

    // 触摸双击：退出对话回 Idle（孩子最熟悉的"退出"心智）
    // Listening / Speaking → AbortAnyConversation + 切 Idle + 提示音
    // Idle / MP3 / 其他 → 忽略（避免误触干扰）
    void HandleTouchDoubleClick() {
        auto& app = Application::GetInstance();

        if (MusicPlayer::GetInstance().IsPlaying()) {
            ESP_LOGD(TAG, "Player 模式忽略双击退出（暂停键独占）");
            return;
        }

        auto state = app.GetDeviceState();
        if (state == kDeviceStateListening || state == kDeviceStateSpeaking) {
            ESP_LOGI(TAG, "双击退出对话：state=%u → Idle", (int)state);
            // 同步清理 PTT 残留状态，避免下一次 release 卡死
            DisarmPttTimeout();
            ptt_active_.store(false);
            AbortAnyConversation();
            app.SetDeviceState(kDeviceStateIdle);
            app.PlaySound(Lang::Sounds::OGG_EXITCHAT);
        } else {
            ESP_LOGD(TAG, "双击忽略：state=%u 不在 Listening/Speaking", (int)state);
        }
    }

    // PTT 60s 超时兜底：release 事件因 RF/接触不良丢失时自动停录，防止 ptt_active_ 残留卡死
    void ArmPttTimeout() {
        if (ptt_timeout_timer_ == nullptr) {
            esp_timer_create_args_t args = {
                .callback = [](void* arg) {
                    auto* self = static_cast<MyDazyP30_4GBoard*>(arg);
                    if (!self->ptt_active_.exchange(false)) return;
                    ESP_LOGW(TAG, "PTT 60s 超时未收到 release，强制 stop（RF 干扰或离手丢事件？）");
                    auto& app = Application::GetInstance();
                    if (app.GetDeviceState() == kDeviceStateListening) {
                        app.StopListening(false);
                    }
                },
                .arg = this,
                .dispatch_method = ESP_TIMER_TASK,
                .name = "ptt_timeout",
            };
            esp_timer_create(&args, &ptt_timeout_timer_);
        }
        esp_timer_stop(ptt_timeout_timer_);
        esp_timer_start_once(ptt_timeout_timer_, 60ULL * 1000ULL * 1000ULL);
    }

    void DisarmPttTimeout() {
        if (ptt_timeout_timer_) esp_timer_stop(ptt_timeout_timer_);
    }

    void InitializePowerManager() {
        power_manager_ = new PowerManager(POWER_MANAGER_GPIO);

        power_manager_->OnLowBatteryStatusChanged([this](bool is_low_battery) {
            if (is_low_battery) {
                Application::GetInstance().Alert("电量不足", "请充电", "", Lang::Sounds::OGG_CHARGE);
            }
        });
        vTaskDelay(pdMS_TO_TICKS(200));

        int battery_level = power_manager_->GetBatteryLevel();
        bool is_charging = power_manager_->IsCharging();
        ESP_LOGI(TAG, "电池监控器初始化完成，当前电量: %d%%, 充电状态: %s",
                 battery_level, is_charging ? "充电中" : "未充电");

        if (power_manager_->IsOffBatteryLevel() && battery_level > 0 && !is_charging) {
            ESP_LOGE(TAG, "电量过低，强制关机");
            ShutdownOrSleep("电量过低", "强制关机", Lang::Sounds::OGG_LOW_BATTERY, 3000, false);
        }
    }

    void InitializePowerSaveTimer() {
        Settings settings("status", false);
        int deep_sleep_enabled = settings.GetInt("deepSleep", 1);

        power_save_timer_ = new PowerSaveTimer(120, 60, 300);
        power_save_timer_->OnEnterSleepMode([this]() {
            ESP_LOGI(TAG, "进入省电模式");
            GetBacklight()->SetBrightness(15);
            // 状态上报触发点：进入省电前打一次（替代周期轮询）
            ReportStatus();
        });
        power_save_timer_->OnExitSleepMode([this]() {
            ESP_LOGI(TAG, "退出省电模式");
            GetBacklight()->RestoreBrightness();
        });
        power_save_timer_->OnShutdownRequest([this, deep_sleep_enabled]() {
            if (deep_sleep_enabled) {
                ESP_LOGI(TAG, "5分钟无操作，进入深度睡眠");
                // 自动休眠：仅屏幕提示 + 陀螺仪可唤醒（拍拍即醒）
                // 不播提示音 —— 用户没主动操作，夜间/会议中突然响会打扰
                ShutdownOrSleep("休眠中", "拍拍唤醒", "", 1500, true);
            }
        });

        power_save_timer_->SetEnabled(deep_sleep_enabled != 0);
    }

    // ========================================================
    // 深度睡眠（拆分子步骤，保证每个函数 ≤ 50 行）
    // ========================================================

    void ShutdownTouchAndAudioForSleep() {
        if (touch_driver_) {
            // v4.0 极简：del 已删除，深睡前用 sleep + 关 INT ISR 即可。
            // 整机即将进 deep_sleep，I2C device handle 由 worker_destroy 统一回收。
            axs5106l_touch_sleep(touch_driver_);
            touch_driver_ = nullptr;
            vTaskDelay(pdMS_TO_TICKS(100));
        }

        ESP_LOGI(TAG, "关闭音频电源");
        gpio_set_level(AUDIO_PWR_EN_GPIO, 0);
        rtc_gpio_hold_en(AUDIO_PWR_EN_GPIO);
    }

    void ConfigureDeepSleepWakeupSources(bool enable_gyro_wakeup) {
        // 等用户松开 BOOT 键（带 5 秒兜底，避免硬件按死时永远卡住）。
        // CheckBootHoldOnWakeup 看到仍按住 → 误判为新一次开机长按 → 关机失败。
        const int kReleaseWaitMaxMs = 5000;
        const int kStepMs = 50;
        int waited_ms = 0;
        while (gpio_get_level(BOOT_BUTTON_GPIO) == 0 && waited_ms < kReleaseWaitMaxMs) {
            vTaskDelay(pdMS_TO_TICKS(kStepMs));
            waited_ms += kStepMs;
        }
        if (waited_ms > 0) {
            ESP_LOGI(TAG, "等待 BOOT 键释放 %dms（关机前必须松开，避免立即唤醒）", waited_ms);
        }

        // 按键唤醒（低电平 = 按下）
        ESP_ERROR_CHECK(esp_sleep_enable_ext0_wakeup(BOOT_BUTTON_GPIO, 0));
        ESP_ERROR_CHECK(rtc_gpio_pullup_en(BOOT_BUTTON_GPIO));
        ESP_ERROR_CHECK(rtc_gpio_pulldown_dis(BOOT_BUTTON_GPIO));

        vTaskDelay(pdMS_TO_TICKS(200));

        if (!enable_gyro_wakeup) return;
        Settings settings("status", false);
        int32_t pickup_wake = settings.GetInt("pickupWake", 1);
        if (pickup_wake && sc7a20h_initialized_) {
            ESP_ERROR_CHECK(esp_sleep_enable_ext1_wakeup_io(
                (1ULL << SC7A20H_GPIO_INT1), ESP_EXT1_WAKEUP_ANY_LOW));
            ESP_ERROR_CHECK(rtc_gpio_pullup_en(SC7A20H_GPIO_INT1));
            ESP_ERROR_CHECK(rtc_gpio_pulldown_dis(SC7A20H_GPIO_INT1));
        }
    }

    void ResetAllGpiosForSleep() {
        constexpr gpio_num_t kGpiosToReset[] = {
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DIN,  AUDIO_I2S_GPIO_DOUT, AUDIO_CODEC_I2C_SDA_PIN,
            AUDIO_CODEC_I2C_SCL_PIN, AUDIO_CODEC_PA_PIN,
            DISPLAY_SPI_MOSI, DISPLAY_SPI_SCLK, DISPLAY_LCD_DC, DISPLAY_LCD_CS,
            DISPLAY_BACKLIGHT, TOUCH_RST_NUM, TOUCH_INT_NUM,
        };
        uint64_t pin_mask = 0;
        for (gpio_num_t pin : kGpiosToReset) {
            gpio_reset_pin(pin);
            pin_mask |= (1ULL << pin);
        }

        // 配置GPIO为输入模式降低功耗
        gpio_config_t input_conf = {
            .pin_bit_mask = pin_mask,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE
        };
        gpio_config(&input_conf);
    }

    // 4G modem 优雅释放：进飞行模式让基站清理 PPP/RRC 会话。
    // driver SetFlightMode 内部 = AT+CFUN=4 + DTR 高（如已启用 GPIO6 DTR）。
    // 失败不阻塞断电流程（modem 异常时仍走电源级硬复位）。
    void GracefulShutdownModem() {
        if (GetNetworkType() != NetworkType::ML307) return;
        auto& ml307 = static_cast<Ml307Board&>(GetCurrentBoard());
        auto* modem = ml307.GetModem();
        if (!modem) return;
        ESP_LOGI(TAG, "AT+CFUN=4 优雅释放 4G PPP 会话");
        modem->SetFlightMode(true);
        vTaskDelay(pdMS_TO_TICKS(800));   // 给 modem 时间释放 PPP/RRC（实测 200-500ms）
    }

    void EnterDeepSleep(bool enable_gyro_wakeup = true) {
        ESP_LOGI(TAG, "====== 开始进入深度睡眠流程 ======");

        // ⚠️ 必须最先停 AudioService：让 audio_input/AFE/encode/output 等 FreeRTOS 任务退出。
        // 否则后面切 AUDIO_PWR_EN 关电源时任务仍在 I2S 读写已掉电的 codec → I2S timeout/panic
        // → CPU reset（表现为"deep sleep 立即重启"，烧录日志的根因）。
        ESP_LOGI(TAG, "停止 AudioService（释放 codec / 退出 audio_* 任务）");
        Application::GetInstance().GetAudioService().Stop();
        vTaskDelay(pdMS_TO_TICKS(100));   // 等任务循环检测 service_stopped_ 后退出

        if (power_save_timer_) {
            power_save_timer_->SetEnabled(false);
        }
        gpio_set_level(DISPLAY_BACKLIGHT, 0);

        ESP_LOGI(TAG, "主动断开 MQTT/WS 长连接（优雅 close）");
        Application::GetInstance().ResetProtocol();
        vTaskDelay(pdMS_TO_TICKS(500));  // 等异步 Schedule lambda 完成 close + 析构

        // 优雅释放 4G PPP（必须在 ResetProtocol 之后、断电前 — 此时 4G 仍可用）
        GracefulShutdownModem();

        if (enable_gyro_wakeup && sc7a20h_initialized_ && sc7a20h_sensor_) {
            Settings settings("status", false);
            int32_t pickup_wake = settings.GetInt("pickupWake", 1);
            if (pickup_wake) {
                esp_err_t r = sc7a20h_arm_wakeup(sc7a20h_sensor_, SC7A20H_GPIO_INT1);
                if (r != ESP_OK) {
                    ESP_LOGW(TAG, "sc7a20h_arm_wakeup failed: %s", esp_err_to_name(r));
                }
            }
        }

        ShutdownTouchAndAudioForSleep();
        ConfigureDeepSleepWakeupSources(enable_gyro_wakeup);

        if (GetNetworkType() == NetworkType::WIFI) {
            WifiStation::GetInstance().Stop();
        }

        ResetAllGpiosForSleep();

        ESP_LOGI(TAG, "准备进入深度睡眠");
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_deep_sleep_start();
    }

    // 统一关机/休眠入口：先显示 Alert + 播提示音，等指定时长后进 deep sleep
    //   title/msg/sound 任一可空跳过；delay_ms 覆盖提示音 + 视觉感知（建议 ≥ 2000ms）
    //   enable_gyro_wakeup=true：陀螺仪可唤醒（自动休眠场景）/ false：仅按键唤醒（按键关机场景）
    void ShutdownOrSleep(const char* title, const char* msg, const std::string_view& sound,
                         int delay_ms, bool enable_gyro_wakeup) {
        auto& app = Application::GetInstance();
        AbortIfSpeaking();
        if (title) app.Alert(title, msg ? msg : "", "", sound);
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
        EnterDeepSleep(enable_gyro_wakeup);
    }

    // ========================================================
    // 显示
    // ========================================================

    void InitializeDisplay() {
        esp_lcd_panel_io_handle_t panel_io_ = nullptr;
        esp_lcd_panel_handle_t panel_ = nullptr;

        ESP_ERROR_CHECK(esp_lcd_jd9853_create_panel(
            DISPLAY_SPI_HOST, DISPLAY_LCD_CS, DISPLAY_LCD_DC, DISPLAY_LCD_RESET,
            DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY, DISPLAY_INVERT_COLOR,
            &panel_io_, &panel_));

#if CONFIG_USE_EMOTE_MESSAGE_STYLE
        display_ = new emote::EmoteDisplay(panel_, panel_io_, DISPLAY_WIDTH, DISPLAY_HEIGHT);
        ESP_LOGI(TAG, "表情包显示模式已启用");
#else
        display_ = new UiDisplay(panel_io_, panel_,
                                 DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
                                 DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
        ESP_LOGI(TAG, "UiDisplay 已启用 (时钟 + 配网 + 激活 + 控制中心)");
#endif

        SystemInfo::PrintHeapStats();
    }

    // ========================================================
    // 按键处理（单独方法，InitializeButtons 仅做注册）
    // ========================================================

    void HandleBootClick() {
        auto& app = Application::GetInstance();
        auto status = app.GetDeviceState();
        ESP_LOGI(TAG, "单击 button 状态: %u", status);
        waiting_factory_reset_confirm_.store(false);

        // MP3 播放期间：按键 = 先打断 MP3 + 退 Player UI，再走唤醒/对话流程
        if (MusicPlayer::GetInstance().IsPlaying()) {
            ESP_LOGI(TAG, "按键打断 MP3 → 唤醒对话");
            MusicPlayer::GetInstance().Stop();
            if (auto* ui = dynamic_cast<UiDisplay*>(Board::GetInstance().GetDisplay())) {
                ui->SwitchOutPlayerMode();
            }
        }

        if (status != kDeviceStateIdle && status != kDeviceStateListening && status != kDeviceStateSpeaking) {
            return;
        }
        if (status == kDeviceStateIdle) {
            app.PlaySound(Lang::Sounds::OGG_WAKEUP);
            vTaskDelay(pdMS_TO_TICKS(1500));
        } else if (status == kDeviceStateListening) {
            app.PlaySound(Lang::Sounds::OGG_EXITCHAT);
        }
        app.ToggleChatState();
    }

    void HandleBootDoubleClick() {
        auto& app = Application::GetInstance();
        auto status = app.GetDeviceState();

        if (waiting_factory_reset_confirm_.load()) {
            uint64_t now = esp_timer_get_time();
            // 10 秒确认窗口（与 9 连击 Alert 文案"10秒内双击确认"对齐）
            if (now - factory_reset_request_time_ > 10000000) {
                waiting_factory_reset_confirm_.store(false);
                return;
            }
            waiting_factory_reset_confirm_.store(false);
            AbortIfSpeaking();
            app.Alert("确认恢复", "开始执行", "logo", Lang::Sounds::OGG_START_RESET);
            // mydazy 业务：先通知服务器解绑（失败不阻塞本地擦除）
            RequestServerUnbind();
            // 9 连击+双击确认触发恢复出厂：NVS 全擦 + 3 秒倒计时 esp_restart
            SystemReset::CheckButtons(true);
            return;
        }

        // 配网态双击：BLUFI ↔ AP 切换（提示音由 SwitchConfigMode 内部 PlaySound）
        if (status == kDeviceStateWifiConfiguring) {
            static std::atomic_flag switching = ATOMIC_FLAG_INIT;
            if (!switching.test_and_set()) {
                // P1 修：Pin Core 1（与 wifi_ap / blufi_wifi / config_done 同核 · 配网切换任务）
                xTaskCreatePinnedToCore([](void* arg) {
                    auto* self = static_cast<MyDazyP30_4GBoard*>(arg);
                    auto* wifi = dynamic_cast<WifiBoard*>(&self->GetCurrentBoard());
                    if (wifi != nullptr) {
                        wifi->SwitchConfigMode();
                    } else {
                        ESP_LOGW(TAG, "SwitchConfigMode skip: current board is not WifiBoard");
                    }
                    switching.clear();
                    vTaskDelete(nullptr);
                }, "config_switch", 4096, this, 3, nullptr, 1);
            }
            return;
        }

#if CONFIG_USE_DEVICE_AEC
        if (status == kDeviceStateIdle || status == kDeviceStateListening || status == kDeviceStateSpeaking) {
            AbortIfSpeaking();
            app.SetDeviceState(kDeviceStateIdle);
            app.SetAecMode(app.GetAecMode() == kAecOff ? kAecOnDeviceSide : kAecOff);
            WakeUp();
        }
#endif
    }

    void HandleBootMultiClick3_SwitchNetwork() {
        auto& app = Application::GetInstance();

        // 任何分支切换前都先暂停音频和对话（覆盖 MP3 播放 + Speaking/Listening 对话）
        PauseAudioAndChatBeforeSwitch();

        if (app.GetDeviceState() == kDeviceStateWifiConfiguring) {
            app.Alert(Lang::Strings::WIFI_CONFIG_MODE, "切换到4G", "logo", Lang::Sounds::OGG_NETWORK_4G);
            vTaskDelay(pdMS_TO_TICKS(1500));
            SwitchNetworkType();
            return;
        }
        if (GetNetworkType() == NetworkType::ML307) {
            app.Alert(Lang::Strings::WIFI_CONFIG_MODE, "切换到WiFi", "logo", Lang::Sounds::OGG_NETWORK_WIFI);
            vTaskDelay(pdMS_TO_TICKS(1500));
            SwitchNetworkType();
        } else {
            app.Alert(Lang::Strings::WIFI_CONFIG_MODE, "切换到配网", "logo", Lang::Sounds::OGG_WIFI_CONFIG);
            vTaskDelay(pdMS_TO_TICKS(1500));
            app.Schedule([this]() {
                auto& wifi_board = static_cast<WifiBoard&>(GetCurrentBoard());
                wifi_board.ResetWifiConfiguration();
            });
        }
    }

    void HandleBootMultiClick4_PowerOff() {
        // 用户主动关机：从用户视角说"再见"，仅按键唤醒防陀螺仪误开机
        ShutdownOrSleep("再见", "", Lang::Sounds::OGG_SHUTDOWN, 2500, false);
    }

    void HandleBootMultiClick6_AudioTest() {
        auto& app = Application::GetInstance();
        AbortIfSpeaking();
        app.SetDeviceState(kDeviceStateWifiConfiguring);
        app.StartListening();
        app.Alert("音频测试", "", "", Lang::Sounds::OGG_AUDIO_TEST);
        vTaskDelay(pdMS_TO_TICKS(3000));
        app.SetDeviceState(kDeviceStateAudioTesting);
        app.StopListening();
    }

    void HandleBootMultiClick9_FactoryReset() {
        auto& app = Application::GetInstance();
        AbortIfSpeaking();
        waiting_factory_reset_confirm_.store(true);
        factory_reset_request_time_ = esp_timer_get_time();
        app.Alert("恢复出厂设置", "10秒内双击确认", "logo", Lang::Sounds::OGG_FACTORY_RESET);
    }

    void HandleBootLongPress() {
        auto& app = Application::GetInstance();
        auto status = app.GetDeviceState();
        if (status == kDeviceStateIdle || status == kDeviceStateSpeaking || status == kDeviceStateListening) {
            if (status != kDeviceStateListening) {
                app.StartListening();
            }
            is_recording_for_send_.store(true);
        } else if (status == kDeviceStateStarting || status == kDeviceStateActivating || status == kDeviceStateWifiConfiguring) {
            is_recording_for_test_.store(true);
            app.SetDeviceState(kDeviceStateWifiConfiguring);
            vTaskDelay(pdMS_TO_TICKS(100));
            app.StartListening();
        }
    }

    void HandleBootPressUp() {
        auto& app = Application::GetInstance();
        // 关机倒计时未到 5 秒就松开 → 取消
        if (shutdown_armed_.exchange(false)) {
            ESP_LOGI(TAG, "关机倒计时取消");
            app.PlaySound(Lang::Sounds::OGG_POPUP);
            return;
        }
        if (is_recording_for_test_.exchange(false)) {
            app.SetDeviceState(kDeviceStateAudioTesting);
            app.StopListening();
        } else if (is_recording_for_send_.exchange(false)) {
            app.StopListening();
            vTaskDelay(pdMS_TO_TICKS(100));
            app.PlaySound(Lang::Sounds::OGG_POPUP);
        }
    }

    // 长按 3 秒：提醒"再按 2 秒关机"，同时停止录音避免冲突
    void HandleBootLongPress3s_ShutdownWarn() {
        auto& app = Application::GetInstance();

        // 停掉可能的录音（不发送）
        if (is_recording_for_send_.exchange(false) || is_recording_for_test_.exchange(false)) {
            app.StopListening();
        }

        shutdown_armed_.store(true);
        ESP_LOGI(TAG, "长按 3 秒：关机倒计时开始");
        app.Alert("长按 5 秒关机", "继续按住...", "logo", Lang::Sounds::OGG_REBOOT);
    }

    // 长按 5 秒：真正关机
    void HandleBootLongPress5s_ShutdownConfirm() {
        if (!shutdown_armed_.load()) return;
        shutdown_armed_.store(false);
        ESP_LOGI(TAG, "长按 5 秒：执行关机");
        // 不再播提示音 —— 3s 警告已播 OGG_REBOOT，间隔 2s 短于音频时长，
        // 这里再播会重叠成"两遍"。让 3s 的 OGG_REBOOT 自然延续到关机即可。
        // 屏幕"关机中"文字 + 2s 后熄屏，视听连贯。
        ShutdownOrSleep("关机中", "", "", 2000, false);
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() { HandleBootClick(); });
        boot_button_.OnDoubleClick([this]() { HandleBootDoubleClick(); });
        boot_button_.OnMultipleClick([this]() { HandleBootMultiClick3_SwitchNetwork(); }, 3);
        boot_button_.OnMultipleClick([this]() { HandleBootMultiClick4_PowerOff(); }, 4);
        boot_button_.OnMultipleClick([this]() { HandleBootMultiClick6_AudioTest(); }, 6);
        boot_button_.OnMultipleClick([this]() { HandleBootMultiClick9_FactoryReset(); }, 9);
        // 长按多段：0.7s 录音、3s 关机提醒、5s 真正关机
        boot_button_.OnLongPress([this]() { HandleBootLongPress(); }, 700);
        boot_button_.OnLongPress([this]() { HandleBootLongPress3s_ShutdownWarn(); }, 3000);
        boot_button_.OnLongPress([this]() { HandleBootLongPress5s_ShutdownConfirm(); }, 5000);
        boot_button_.OnPressUp([this]() { HandleBootPressUp(); });

        volume_up_button_.OnClick([this]() { AdjustVolume(+10); });
        volume_down_button_.OnClick([this]() { AdjustVolume(-10); });

        volume_up_button_.OnLongPress([this]() {
            StartVolumeTask(+5, &vol_up_task_, &vol_up_running_);
        });
        volume_up_button_.OnPressUp([this]() { vol_up_running_.store(false); });

        volume_down_button_.OnLongPress([this]() {
            StartVolumeTask(-5, &vol_down_task_, &vol_down_running_);
        });
        volume_down_button_.OnPressUp([this]() { vol_down_running_.store(false); });
    }

    // ========================================================
    // 状态上报
    // ========================================================

    // 周期上报开关：默认关闭。唤醒事件触发的一次性上报不受此开关影响。
    // 改 true 重编即可恢复 90s 周期；后续可改成 NVS 设置项。
    static constexpr bool kEnablePeriodicStatusReport = false;

    void ReportStatus() {
        // 仅在 idle 上报；对话期让位给音频上传和 TTS，避免 HTTPS/TLS 抢资源。
        auto state = Application::GetInstance().GetDeviceState();
        if (state != kDeviceStateIdle) {
            ESP_LOGD(TAG, "skip status report, state=%d (仅 idle 上报)", (int)state);
            return;
        }
        // 防御性：MP3 流式播放期间禁止上报（4G PPP 链路抢带宽尤为致命，
        // 实测 OSS Range 续传与 /status POST 同时进行会导致 SSL -76 retry 用尽）
        if (MusicPlayer::GetInstance().IsPlaying()) {
            ESP_LOGD(TAG, "skip status report, music playing");
            return;
        }

        // 精简后上报：电量 + 音量 + 亮度 + theme + 网络信号（P30-4G 无定位）
        int battery = power_manager_ ? power_manager_->GetBatteryLevel() : -1;
        bool charging = power_manager_ ? power_manager_->IsCharging() : false;
        auto* codec = GetAudioCodec();
        int volume = codec ? codec->output_volume() : -1;
        auto* bl = GetBacklight();
        int brightness = bl ? bl->brightness() : -1;
        std::string theme;
        auto* disp = GetDisplay();
        if (disp && disp->GetTheme()) {
            theme = disp->GetTheme()->name();
        }

        int csq = -1;
        std::string carrier;
        if (GetNetworkType() == NetworkType::ML307) {
            auto& ml307 = dynamic_cast<Ml307Board&>(GetCurrentBoard());
            auto* modem = ml307.GetModem();
            if (modem) {
                csq = modem->GetCsq();
                carrier = modem->GetCarrierName();
            }
        }

        Application::GetInstance().Schedule(
            [battery, charging, volume, brightness, theme, csq, carrier]() {
            cJSON* p = cJSON_CreateObject();
            cJSON_AddNumberToObject(p, "battery", battery);
            cJSON_AddBoolToObject(p, "charging", charging);
            if (volume >= 0)     cJSON_AddNumberToObject(p, "volume", volume);
            if (brightness >= 0) cJSON_AddNumberToObject(p, "brightness", brightness);
            if (!theme.empty())  cJSON_AddStringToObject(p, "theme", theme.c_str());

            cJSON* net = cJSON_CreateObject();
            cJSON_AddStringToObject(net, "type", "cellular");
            if (!carrier.empty()) cJSON_AddStringToObject(net, "carrier", carrier.c_str());
            if (csq >= 0) cJSON_AddNumberToObject(net, "csq", csq);
            cJSON_AddItemToObject(p, "network", net);

            Ota::ReportStatus(p);
        });
    }

    void StartStatusTimer() {
        if (!kEnablePeriodicStatusReport) {
            ESP_LOGI(TAG, "Periodic status report disabled (wake-only mode)");
            return;
        }
        esp_timer_create_args_t args = {
            .callback = [](void* arg) {
                static_cast<MyDazyP30_4GBoard*>(arg)->ReportStatus();
            },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "status_report",
        };
        esp_timer_create(&args, &status_timer_);
        esp_timer_start_periodic(status_timer_, 90 * 1000000ULL);
    }

    // ========================================================
    // 音量
    // ========================================================

    // 应用一次音量增量；clamp 到 [0,100] 并刷新状态栏 + 唤醒省电定时器
    void ApplyVolumeDelta(int delta) {
        auto* codec = GetAudioCodec();
        if (!codec) return;
        int v = codec->output_volume() + delta;
        if (v > 100) v = 100;
        if (v < 0) v = 0;
        codec->SetOutputVolume(v);
        char buf[32];
        snprintf(buf, sizeof(buf), "%s %d", Lang::Strings::VOLUME, v);
        // 改用 ShowNotification（1.5s 自动消失） · clock/player 模式下也能可见
        // 见 LvglDisplay::ShowNotification: 临时浮起 status_bar_ + timer 还原原状态
        GetDisplay()->ShowNotification(buf, 1500);
        WakeUp();
    }

    void AdjustVolume(int delta) { ApplyVolumeDelta(delta); }

    struct VolumeTaskCtx {
        int delta;
        std::atomic<bool>* running;
        TaskHandle_t* task_handle;
    };

    void StartVolumeTask(int delta, TaskHandle_t* task_handle, std::atomic<bool>* running) {
        if (*task_handle != nullptr) return;
        running->store(true);

        auto* ctx = new VolumeTaskCtx{delta, running, task_handle};
        xTaskCreatePinnedToCore([](void* arg) {
            auto* ctx = static_cast<VolumeTaskCtx*>(arg);
            auto& board = static_cast<MyDazyP30_4GBoard&>(Board::GetInstance());
            while (ctx->running->load()) {
                board.ApplyVolumeDelta(ctx->delta);
                vTaskDelay(pdMS_TO_TICKS(200));
            }
            *(ctx->task_handle) = nullptr;
            delete ctx;
            vTaskDelete(NULL);
        }, "vol_adjust", 2048, ctx, 5, task_handle, 1);
    }

    // ========================================================
    // 初始化业务逻辑
    // ========================================================

    void ApplyDefaultSettings() {
        // 音量范围修正（50-100）
        Settings audio_settings("audio", true);
        int DEFAULT_VOLUME = 80;
        int original_volume = audio_settings.GetInt("output_volume", DEFAULT_VOLUME);
        if (original_volume < 50) {
            audio_settings.SetInt("output_volume", DEFAULT_VOLUME);
            ESP_LOGI(TAG, "检测到音量%d小于50，自动调整为%d", original_volume, DEFAULT_VOLUME);
        }
        // 默认使用蓝牙配网
        Settings wifi_settings("wifi", true);
        wifi_settings.SetInt("blufi", 1);
    }

    static void WelcomeTaskEntry(void* arg) {
        auto* self = static_cast<MyDazyP30_4GBoard*>(arg);
        auto& app = Application::GetInstance();
        vTaskDelay(pdMS_TO_TICKS(1500));

        if (app.GetDeviceState() != kDeviceStateWifiConfiguring) {
            Settings audio_settings("audio", false);
            if (self->first_boot_ && audio_settings.GetInt("playWelcome", 1)) {
                app.PlaySound(Lang::Sounds::OGG_WELCOME);
                vTaskDelay(pdMS_TO_TICKS(3000));
                if (app.GetDeviceState() == kDeviceStateIdle) {
                    app.ToggleChatState();
                }
            }
        }
        vTaskDelete(NULL);
    }

    void StartWelcomeTask() {
        if (!first_boot_) return;
        xTaskCreatePinnedToCore(&MyDazyP30_4GBoard::WelcomeTaskEntry,
                                "welcome_init", 3072, this, 3, nullptr, 1);
    }

    // ========================================================
    // MCP 工具注册（板专属能力）— 通用工具由 McpServer::AddCommonTools 自动注册
    // 详见 docs/mcp-usage.md / docs/custom-board_zh.md
    // ========================================================
    void InitializeTools() {
        auto& mcp = McpServer::GetInstance();

        // AEC 开关（公开）：动态开/关 device-side AEC（声学回声消除）。
        mcp.AddTool("self.audio.set_aec",
            "Enable or disable on-device AEC (Acoustic Echo Cancellation). "
            "When enabled, AEC removes speaker echo from the microphone input. "
            "Disable it to capture raw microphone audio (e.g. for recording).",
            PropertyList({
                Property("enable", kPropertyTypeBoolean)
            }),
            [](const PropertyList& props) -> ReturnValue {
                bool enable = props["enable"].value<bool>();
                Application::GetInstance().GetAudioService().EnableDeviceAec(enable);
                return true;
            });

        // 识字笔画动画（512KB 上限直接下载，含 GIF 头尾校验）
        RegisterEducationMcpTools(mcp, dynamic_cast<UiDisplay*>(GetDisplay()));
    }

public:
    MyDazyP30_4GBoard() :
        // 当前量产线路板未扩展 GPIO6 DTR；下版硬件改板后改用 MODEM_DTR_GPIO 即可。
        // GracefulShutdownModem 内部用 AT+CFUN=4 优雅释放（不依赖 DTR 硬件连接）。
        DualNetworkBoard(ML307_TX_PIN, ML307_RX_PIN, MODEM_DTR_GPIO, 1),
        boot_button_(BOOT_BUTTON_GPIO, false, 800, 400),
        volume_up_button_(VOLUME_UP_BUTTON_GPIO),
        volume_down_button_(VOLUME_DOWN_BUTTON_GPIO) {

        // 注册重启钩子：任何 esp_restart() 调用前自动断 LDO 复位 LCD（OTA 升级 / 出厂复位 / 网络切换 / 双击恢复出厂等所有路径）
        esp_register_shutdown_handler(ShutdownHandler);

        InitializeGpio();
        InitializeI2c();
        PrepareTouchHardware();
        InitializeSpi();
        InitializeDisplay();
        InitializeTouch();
        InitializeSc7a20h();
        StartShakeDetect();
        InitializePowerManager();
        InitializePowerSaveTimer();
        InitializeButtons();
        StartStatusTimer();

        GetAudioCodec();

        ApplyDefaultSettings();

        // 注册板专属 MCP 工具（含教育卡 self.education.show_stroke）— 必须在 Display 初始化之后
        InitializeTools();

        ESP_LOGI(TAG, "MyDazy P30 4G 初始化完成 (ES8311+ES7210, 支持4G、电源管理、触摸屏)");

        StartWelcomeTask();
    }


    virtual AudioCodec* GetAudioCodec() override {
        if (audio_codec_ == nullptr) {
            audio_codec_ = new BoxAudioCodec(
                i2c_worker_,                       /* v3.0+ 通过 worker 串行化 codec I2C */
                AUDIO_INPUT_SAMPLE_RATE,
                AUDIO_OUTPUT_SAMPLE_RATE,
                AUDIO_I2S_GPIO_MCLK,
                AUDIO_I2S_GPIO_BCLK,
                AUDIO_I2S_GPIO_WS,
                AUDIO_I2S_GPIO_DOUT,
                AUDIO_I2S_GPIO_DIN,
                AUDIO_CODEC_PA_PIN,
                AUDIO_CODEC_ES8311_ADDR,
                AUDIO_CODEC_ES7210_ADDR,
                AUDIO_INPUT_REFERENCE);
        }
        return audio_codec_;
    }

    virtual void SetPowerSaveLevel(PowerSaveLevel level) override {
        if (level == PowerSaveLevel::PERFORMANCE) {
            if (power_save_timer_) {
                power_save_timer_->WakeUp();
            }
        }
        DualNetworkBoard::SetPowerSaveLevel(level);
    }

    virtual bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        if (!power_manager_) {
            level = 0;
            charging = false;
            discharging = false;
            return false;
        }
        charging = power_manager_->IsCharging();
        discharging = power_manager_->IsDischarging();
        level = power_manager_->GetBatteryLevel();
        return true;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }

    // Control Center → 自动休眠开关。持久化到 NVS "deepSleep"，下次启动也生效。
    virtual void EnableAutoSleep(bool enable) override {
        Settings settings("status", true);
        settings.SetInt("deepSleep", enable ? 1 : 0);
        if (power_save_timer_) {
            power_save_timer_->SetEnabled(enable);
        }
        ESP_LOGI(TAG, "EnableAutoSleep: %s", enable ? "ON" : "OFF");
    }

    virtual bool IsAutoSleepEnabled() const override {
        Settings settings("status", false);
        return settings.GetInt("deepSleep", 1) != 0;
    }

    void WakeUp() {
        if (power_save_timer_) {
            power_save_timer_->WakeUp();
        }
    }

    static void ShutdownHandler() {
        // 先静音功放：避免直接断 LDO 导致音圈因瞬态电压释放产生 POP
        gpio_set_level(AUDIO_CODEC_PA_PIN, 0);
        esp_rom_delay_us(20 * 1000);            // 20ms 让功放进入静音
        gpio_set_level(AUDIO_PWR_EN_GPIO, 0);   // 断 LDO 总开关（LCD + audio + ext）
        rtc_gpio_hold_en(AUDIO_PWR_EN_GPIO);    // 穿越 esp_restart() 保持 LOW
        esp_rom_delay_us(500 * 1000);           // 等 22uF 电容放电（shutdown 上下文用 ROM delay 更稳妥）
    }

    // 通知服务器解绑设备：mydazy 业务，仅在 9 连击双击确认时使用。失败不阻塞本地擦除。
    static bool RequestServerUnbind() {
        std::string url = CONFIG_OTA_URL;
        if (url.find("mydazy") == std::string::npos) {
            ESP_LOGI("P30_4G", "Not mydazy server, skip unbind request");
            return false;
        }
        url += "/reset";

        auto network = Board::GetInstance().GetNetwork();
        if (!network) {
            ESP_LOGW("P30_4G", "Network not available, skip server unbind");
            return false;
        }
        auto http = network->CreateHttp(0);
        if (!http) {
            ESP_LOGW("P30_4G", "Failed to create HTTP client");
            return false;
        }
        http->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
        http->SetHeader("Client-Id", Board::GetInstance().GetUuid().c_str());
        http->SetHeader("User-Agent", SystemInfo::GetUserAgent().c_str());
        http->SetTimeout(5000);

        if (!http->Open("GET", url)) {
            ESP_LOGW("P30_4G", "Failed to connect to server for unbind");
            return false;
        }
        int status_code = http->GetStatusCode();
        std::string response = http->ReadAll();
        http->Close();
        ESP_LOGI("P30_4G", "Server unbind response: code=%d, body=%s", status_code, response.c_str());
        return status_code == 200 || status_code == 204;
    }

    // 切换网络：写 NVS + esp_restart()，电源复位由 ShutdownHandler 自动接管
    void SwitchNetworkType() {
        auto display = GetDisplay();
        Settings net_settings("network", true);  // 复刻基类 SaveNetworkTypeToSettings 逻辑（基类已设 private）
        if (GetNetworkType() == NetworkType::WIFI) {
            net_settings.SetInt("type", 1);  // 1 = ML307
            display->ShowNotification(Lang::Strings::SWITCH_TO_4G_NETWORK);
        } else {
            net_settings.SetInt("type", 0);  // 0 = WIFI
            display->ShowNotification(Lang::Strings::SWITCH_TO_WIFI_NETWORK);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
        Application::GetInstance().GetAudioService().Stop();
        esp_restart();   // ShutdownHandler 自动断 LDO 复位 LCD
    }
};

DECLARE_BOARD(MyDazyP30_4GBoard);
