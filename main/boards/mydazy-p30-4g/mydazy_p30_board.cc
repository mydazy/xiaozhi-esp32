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
#include "alarm_manager.h"
#include "audio/alarm_ringer.h"
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

// EXT0 唤醒时若距上次 sleep < 500ms = 死按延续（硬件极速被触发）· 直接再 sleep
// 真心开机必然 = 用户松手再按 · 间隔 ≥ 200ms · 实测远超 500ms
// 用 gettimeofday（POSIX 标准 · ESP-IDF 内部基于 RTC · 跨 deep sleep 持续 · 无组件依赖）
#include <sys/time.h>
RTC_DATA_ATTR static uint64_t s_last_sleep_us = 0;
static constexpr uint64_t kDeadHoldWindowUs = 500 * 1000;  // 500ms
static inline uint64_t NowRtcUs() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return (uint64_t)tv.tv_sec * 1000000ULL + tv.tv_usec;
}

namespace {
// 闹钟响铃中：任何用户输入（按键/触摸/摇晃）都优先关停闹钟，不进对话流程
inline bool TryStopAlarmRinger(const char* reason) {
    if (AlarmRinger::GetInstance().IsRinging()) {
        AlarmRinger::GetInstance().Stop(reason);
        return true;
    }
    return false;
}

// 小工具：处于 Speaking 时先 Abort，避免状态冲突
inline void AbortIfSpeaking() {
    auto& app = Application::GetInstance();
    if (app.GetDeviceState() == kDeviceStateSpeaking) {
        app.AbortSpeaking(kAbortReasonNone);
    }
}

// 同时打断 Speaking 与 Listening（双击退出/出厂确认/9 连击等场景）
// 与 AbortIfSpeaking 区别：StopListening 不送 stop 信号，让服务端立即释放，不留 Listening 等 TTS
inline void AbortAnyConversation() {
    auto& app = Application::GetInstance();
    auto state = app.GetDeviceState();
    if (state == kDeviceStateSpeaking) {
        app.AbortSpeaking(kAbortReasonNone);
    } else if (state == kDeviceStateListening) {
        app.StopListening();
    }
}

// MP3 播放期间：停播 + 退 Player UI（按键打断 / 切网前都需要）
inline void StopMp3AndExitPlayerUi() {
    if (!MusicPlayer::GetInstance().IsPlaying()) return;
    MusicPlayer::GetInstance().Stop();
    if (auto* ui = dynamic_cast<UiDisplay*>(Board::GetInstance().GetDisplay())) {
        ui->SwitchOutPlayerMode();
    }
}

// 切换网络前统一暂停：① 停 MP3 播放（含退 Player UI）② 打断对话（Speaking/Listening）
// 避免重启或进配网时音频任务/协议通道未释放导致的资源残留与异响
inline void PauseAudioAndChatBeforeSwitch() {
    auto& app = Application::GetInstance();

    if (MusicPlayer::GetInstance().IsPlaying()) {
        ESP_LOGI(TAG, "切网前停止 MP3 播放");
        StopMp3AndExitPlayerUi();
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

    std::atomic<bool> shutdown_armed_{false};
    esp_timer_handle_t shutdown_delay_timer_ = nullptr;

    // 开机长按 grace 期：注册按键时若 GPIO 仍按下，置 true；首次 PressUp 自动清。
    std::atomic<bool> boot_hold_grace_active_{false};

    // 恢复出厂设置确认状态
    std::atomic<bool> waiting_factory_reset_confirm_{false};
    uint64_t factory_reset_request_time_ = 0;


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
        ESP_LOGI(TAG, "Boot diag: wakeup_cause=%d, reset_reason=%d", (int)wakeup_reason, (int)reset_reason);

        switch (wakeup_reason) {
            case ESP_SLEEP_WAKEUP_EXT0:
                ESP_LOGI(TAG, "从开机键唤醒");
                // 距上次 sleep < 500ms + GPIO 低 = 死按延续（硬件极速被 EXT0 触发）
                // 直接重 arm + sleep · 永不"误判开机播欢迎音"· 用户死按多久都安全
                if (s_last_sleep_us > 0) {
                    uint64_t since_us = NowRtcUs() - s_last_sleep_us;
                    if (since_us < kDeadHoldWindowUs && gpio_get_level(BOOT_BUTTON_GPIO) == 0) {
                        ESP_LOGW(TAG, "距 sleep %llu ms · 用户未松手 · 短路 sleep", since_us / 1000);
                        // 防御性断 LDO + hold（B1 修复）· 与 EnterDeepSleep 路径对称
                        // 硬件 R47=100K 已下拉 GPIO9 到 GND，sleep 失驱时自动断电；本两行作 belt-and-suspenders
                        // 防 R47 虚焊 / 缺件，并让 sleep 期 GPIO9 立即 LOW 不依赖 RC 放电
                        gpio_set_level(AUDIO_PWR_EN_GPIO, 0);
                        rtc_gpio_hold_en(AUDIO_PWR_EN_GPIO);
                        s_last_sleep_us = NowRtcUs();
                        esp_sleep_enable_ext0_wakeup(BOOT_BUTTON_GPIO, 0);
                        rtc_gpio_pullup_en(BOOT_BUTTON_GPIO);
                        esp_deep_sleep_start();
                    }
                }
                first_boot_ = true;
                if (!CheckBootHoldOnWakeup()) {
                    ESP_LOGI(TAG, "开机长按未达 1.5 秒，立即回深睡");
                    EnterDeepSleep(true);  // 不会返回
                }
                break;
            case ESP_SLEEP_WAKEUP_EXT1:
                ESP_LOGI(TAG, "从陀螺仪唤醒 (ext1_status=0x%llx)", esp_sleep_get_ext1_wakeup_status());
                first_boot_ = true;
                break;
            case ESP_SLEEP_WAKEUP_TIMER:
                ESP_LOGI(TAG, "从定时器唤醒 · 闹钟模式");
                AlarmManager::MarkTimerWakeup();  // 允许未校时时用 RTC fallback 触发闹钟
                first_boot_ = true;
                break;
            default:
                ESP_LOGI(TAG, "首次启动或复位 (wakeup=%d, reset=%d)", (int)wakeup_reason, (int)reset_reason);
                // 仅正常启动放欢迎音：上电 / USB 接入 / 主动 esp_restart（OTA/出厂复位/切网）
                // panic / wdt / brownout 等异常重启不放，避免"崩溃后假装一切正常"误导用户
                first_boot_ = (reset_reason == ESP_RST_POWERON ||
                               reset_reason == ESP_RST_USB ||
                               reset_reason == ESP_RST_SW);
                break;
        }
    }

    // 开机长按检测：返回 true=按够阈值可开机，false=未到阈值应回深睡
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

        const int kHoldRequiredMs = 1500;
        const int kStepMs = 50;
        int elapsed = 0;
        while (elapsed < kHoldRequiredMs) {
            vTaskDelay(pdMS_TO_TICKS(kStepMs));
            if (gpio_get_level(BOOT_BUTTON_GPIO) != 0) {
                return false;  // 中途松开 → 不开机
            }
            elapsed += kStepMs;
        }
        ESP_LOGI(TAG, "开机长按 %d ms 确认，准备启动", kHoldRequiredMs);
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
    }

    void InitializeSc7a20h() {
        sc7a20h_sensor_ = sc7a20h_init(i2c_worker_, 320 /*mg*/, 100 /*ms*/);
        sc7a20h_initialized_ = (sc7a20h_sensor_ != nullptr);
        if (!sc7a20h_initialized_) {
            ESP_LOGE(TAG, "SC7A20H 初始化失败");
        }
    }

    // 摇一摇识别（孩子拿起来甩 → 云端决定返回什么）
    // 算法在驱动 sc7a20h_start_shake_detect() 内，回调出事件 → Schedule 主线程。
    // Listening 中不调 StopListening 避免 UI 抖回时钟，直接 SendTextToAI 平滑过渡。
    static void OnShake(void* /*ctx*/) {
        Application::GetInstance().Schedule([]() {
            if (TryStopAlarmRinger("shake")) return;
            auto& app = Application::GetInstance();
            auto state = app.GetDeviceState();
            if (state != kDeviceStateIdle && state != kDeviceStateListening) {
                ESP_LOGD(TAG, "Shake ignored: state=%d", (int)state);
                return;
            }
            app.PlaySound(Lang::Sounds::OGG_POPUP);
            ESP_LOGI(TAG, "Shake → 摇一摇 (state=%d)", (int)state);
            app.SendTextToAI("摇一摇随机互动");
        });
    }

    void StartShakeDetect() {
        if (!sc7a20h_initialized_) return;
        sc7a20h_start_shake_detect(sc7a20h_sensor_, &MyDazyP30_4GBoard::OnShake, this);
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
        bool is_click        = (g == AXS5106L_GESTURE_SINGLE_CLICK);
        bool is_double_click = (g == AXS5106L_GESTURE_DOUBLE_CLICK);
        if (!is_click && !is_double_click) return;

        if (y < 36) {
            ESP_LOGD(TAG, "状态栏点击交由 LVGL 处理，driver 路径忽略");
            return;
        }
        (void)x;

        if (is_double_click) {
            self->HandleTouchDoubleClick();
        } else {
            self->HandleTouchSingleClick();
        }
    }

    void InitializeTouch() {
        if (touch_driver_ == nullptr) return;
        if (axs5106l_touch_attach_lvgl(touch_driver_) != ESP_OK) {
            ESP_LOGE(TAG, "触摸屏 LVGL attach 失败");
            touch_driver_ = nullptr;
            return;
        }
        ESP_LOGI(TAG, "触摸屏初始化完成（v4.0 worker 路径）");
    }

    void HandleTouchSingleClick() {
        if (TryStopAlarmRinger("touch")) return;
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

    // 触摸双击：退出对话回 Idle（孩子最熟悉的"退出"心智）
    // Listening / Speaking → AbortAnyConversation + 切 Idle + 提示音
    // Idle / MP3 / 其他 → 忽略（避免误触干扰）
    void HandleTouchDoubleClick() {
        if (TryStopAlarmRinger("touch")) return;
        auto& app = Application::GetInstance();

        if (MusicPlayer::GetInstance().IsPlaying()) {
            ESP_LOGD(TAG, "Player 模式忽略双击退出（暂停键独占）");
            return;
        }

        auto state = app.GetDeviceState();
        if (state == kDeviceStateListening || state == kDeviceStateSpeaking) {
            ESP_LOGI(TAG, "双击退出对话：state=%u → Idle", (int)state);
            AbortAnyConversation();
            app.SetDeviceState(kDeviceStateIdle);
            app.PlaySound(Lang::Sounds::OGG_EXITCHAT);
        } else {
            ESP_LOGD(TAG, "双击忽略：state=%u 不在 Listening/Speaking", (int)state);
        }
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
                ShutdownOrSleep("休眠中", "拿起唤醒", "", 1500, true);
            }
        });

        power_save_timer_->SetEnabled(deep_sleep_enabled != 0);
    }

    // ========================================================
    // 深度睡眠（拆分子步骤，保证每个函数 ≤ 50 行）
    // ========================================================

    void ShutdownTouchAndAudioForSleep() {
        if (touch_driver_) {
            axs5106l_touch_sleep(touch_driver_);
            touch_driver_ = nullptr;
            vTaskDelay(pdMS_TO_TICKS(100));
        }

        ESP_LOGI(TAG, "关闭音频电源");
        gpio_set_level(AUDIO_PWR_EN_GPIO, 0);
        rtc_gpio_hold_en(AUDIO_PWR_EN_GPIO);
    }

    void ConfigureDeepSleepWakeupSources(bool /*enable_gyro_wakeup*/) {
        // 此函数只负责 EXT0（BOOT 键）：EXT1（SC7A20H INT1）由 sc7a20h_arm_wakeup 负责，
        // 必须紧贴 esp_deep_sleep_start 之前调用，否则 ResetAllGpiosForSleep 会先 reset
        // 共用的 I2C 引脚（GPIO11/12），让 INT1_SRC clear 静默失败 → 睡后立即唤醒。
        ESP_ERROR_CHECK(esp_sleep_enable_ext0_wakeup(BOOT_BUTTON_GPIO, 0));
        ESP_ERROR_CHECK(rtc_gpio_pullup_en(BOOT_BUTTON_GPIO));
        ESP_ERROR_CHECK(rtc_gpio_pulldown_dis(BOOT_BUTTON_GPIO));

        vTaskDelay(pdMS_TO_TICKS(200));
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

        // ⚠ arm_wakeup 必须在 ShutdownTouchAndAudioForSleep（AUDIO_PWR_EN=0）之前 ·
        // 否则失电的 ES8311/ES7210 通过 ESD 二极管把 SDA/SCL 钉死 → i2c_master_transmit_receive
        // 返回 INVALID_STATE → INT1_SRC 清 latch 失败（2026-05-12 量产二阶根因实测复现）。
        // 配合驱动 v4.0.1 LIR_INT1=0 改动，本调用现在也兼具 defense-in-depth。
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

        // 闹钟：arm RTC 定时唤醒（与 EXT0/EXT1 三源并存 · 含分段睡眠策略）
        AlarmManager::GetInstance().FlushNvs();
        AlarmManager::GetInstance().ConfigureTimerWakeup();

        ESP_LOGI(TAG, "准备进入深度睡眠");
        // 让 AUDIO_PWR_EN=0 引发的瞬态机械振动衰减（>100ms 实测充裕）·
        // LIR_INT1=0 后即使瞬态振动也不会留 latch，只要静下来 INT1 自动回 HIGH。
        vTaskDelay(pdMS_TO_TICKS(200));

        ResetAllGpiosForSleep();

        // 每次进 sleep 都记 RTC 时戳 · 唤醒后比较 < 500ms 即死按延续 · 直接短路 sleep
        s_last_sleep_us = NowRtcUs();
        esp_deep_sleep_start();
    }

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

    void InitializeButtons() {
        // 开机长按 grace 入口
        if (first_boot_ && gpio_get_level(BOOT_BUTTON_GPIO) == 0) {
            boot_hold_grace_active_.store(true);
            const int kReleaseWaitMaxMs = 5000;
            const int kStepMs = 50;
            int waited = 0;
            while (gpio_get_level(BOOT_BUTTON_GPIO) == 0 && waited < kReleaseWaitMaxMs) {
                vTaskDelay(pdMS_TO_TICKS(kStepMs));
                waited += kStepMs;
            }
            if (gpio_get_level(BOOT_BUTTON_GPIO) != 0) {
                boot_hold_grace_active_.store(false);
            }
            ESP_LOGI(TAG, "InitializeButtons: 开机长按等松手 %d ms (grace=%d)",
                     waited, boot_hold_grace_active_.load());
        }

        boot_button_.OnClick([this]() {
            if (TryStopAlarmRinger("button")) return;
            auto& app = Application::GetInstance();
            auto status = app.GetDeviceState();
            ESP_LOGI(TAG, "单击 button 状态: %u", status);
            waiting_factory_reset_confirm_.store(false);
            if (MusicPlayer::GetInstance().IsPlaying()) {
                ESP_LOGI(TAG, "按键打断 MP3 → 唤醒对话");
                StopMp3AndExitPlayerUi();
            }
            if (status != kDeviceStateIdle && status != kDeviceStateListening && status != kDeviceStateSpeaking) return;
            if (status == kDeviceStateIdle) {
                app.PlaySound(Lang::Sounds::OGG_WAKEUP);
                vTaskDelay(pdMS_TO_TICKS(1500));
            } else if (status == kDeviceStateListening) {
                app.PlaySound(Lang::Sounds::OGG_EXITCHAT);
            }
            app.ToggleChatState();
        });

        // 双击：① 出厂确认（10s 窗口）② 配网态 BLUFI↔AP 切换 ③ AEC 模式切换
        boot_button_.OnDoubleClick([this]() {
            if (TryStopAlarmRinger("button")) return;
            auto& app = Application::GetInstance();
            auto status = app.GetDeviceState();
            // ① 9 连击后 10s 内双击 = 确认出厂
            if (waiting_factory_reset_confirm_.load()) {
                if (esp_timer_get_time() - factory_reset_request_time_ > 10000000) {
                    waiting_factory_reset_confirm_.store(false);
                    return;
                }
                waiting_factory_reset_confirm_.store(false);
                AbortIfSpeaking();
                app.Alert("确认恢复", "开始执行", "logo", Lang::Sounds::OGG_START_RESET);
                xTaskCreatePinnedToCore(&MyDazyP30_4GBoard::FactoryResetTaskEntry,
                                        "factory_reset", 6144, nullptr, 3, nullptr, 0);
                return;
            }
            // ② 配网态：BLUFI ↔ AP 切换（提示音由 SwitchConfigMode 内部 PlaySound）
            if (status == kDeviceStateWifiConfiguring) {
                static std::atomic_flag switching = ATOMIC_FLAG_INIT;
                if (!switching.test_and_set()) {
                    xTaskCreatePinnedToCore([](void* arg) {
                        auto* self = static_cast<MyDazyP30_4GBoard*>(arg);
                        auto* wifi = dynamic_cast<WifiBoard*>(&self->GetCurrentBoard());
                        if (wifi) wifi->SwitchConfigMode();
                        else ESP_LOGW(TAG, "SwitchConfigMode skip: current board is not WifiBoard");
                        switching.clear();
                        vTaskDelete(nullptr);
                    }, "config_switch", 4096, this, 3, nullptr, 1);
                }
                return;
            }
#if CONFIG_USE_DEVICE_AEC
            // ③ 在线态：AEC 模式 toggle
            if (status == kDeviceStateIdle || status == kDeviceStateListening || status == kDeviceStateSpeaking) {
                AbortIfSpeaking();
                app.SetDeviceState(kDeviceStateIdle);
                app.SetAecMode(app.GetAecMode() == kAecOff ? kAecOnDeviceSide : kAecOff);
                WakeUp();
            }
#endif
        });
        // 3 连击：4G ↔ WiFi ↔ 配网 三态切换
        boot_button_.OnMultipleClick([this]() {
            auto& app = Application::GetInstance();
            PauseAudioAndChatBeforeSwitch();
            if (app.GetDeviceState() == kDeviceStateWifiConfiguring) {
                app.Alert(Lang::Strings::WIFI_CONFIG_MODE, "切换到4G", "logo", Lang::Sounds::OGG_NETWORK_4G);
                vTaskDelay(pdMS_TO_TICKS(1500));
                SwitchNetworkType();
            } else if (GetNetworkType() == NetworkType::ML307) {
                app.Alert(Lang::Strings::WIFI_CONFIG_MODE, "切换到WiFi", "logo", Lang::Sounds::OGG_NETWORK_WIFI);
                vTaskDelay(pdMS_TO_TICKS(1500));
                SwitchNetworkType();
            } else {
                app.Alert(Lang::Strings::WIFI_CONFIG_MODE, "切换到配网", "logo", Lang::Sounds::OGG_WIFI_CONFIG);
                vTaskDelay(pdMS_TO_TICKS(1500));
                app.Schedule([this]() {
                    static_cast<WifiBoard&>(GetCurrentBoard()).ResetWifiConfiguration();
                });
            }
        }, 3);

        // 4 连击关机：用户主动关机视角说"再见"，仅按键唤醒（防陀螺仪误开机）
        boot_button_.OnMultipleClick([this]() {
            ShutdownOrSleep("再见", "", Lang::Sounds::OGG_SHUTDOWN, 2500, false);
        }, 4);

        // 9 连击进入恢复出厂确认（10s 内再双击确认，超时由 OnClick/OnDoubleClick lambda 自动清）
        boot_button_.OnMultipleClick([this]() {
            AbortIfSpeaking();
            waiting_factory_reset_confirm_.store(true);
            factory_reset_request_time_ = esp_timer_get_time();
            Application::GetInstance().Alert("恢复出厂设置", "10秒内双击确认", "logo", Lang::Sounds::OGG_FACTORY_RESET);
        }, 9);

        boot_button_.OnLongPress([this]() {
            if (boot_hold_grace_active_.load()) {
                ESP_LOGI(TAG, "开机长按 grace 期：忽略 3s 关机");
                return;
            }
            if (shutdown_armed_.exchange(true)) return;  // 防重入
            ESP_LOGI(TAG, "长按 3 秒：800ms 后关机（中途松开取消）");
            Application::GetInstance().Alert("关机中", "", "logo", Lang::Sounds::OGG_REBOOT);

            // lazy 创建 timer · 整个生命周期复用（无内存泄漏）
            if (!shutdown_delay_timer_) {
                esp_timer_create_args_t args = {
                    .callback = [](void* arg) {
                        // ESP_TIMER_TASK 高优先级共享任务，禁止阻塞（ShutdownOrSleep 含 2-3s vTaskDelay）
                        // → 用 Schedule 派发到 main_app 任务执行，timer 回调立刻返回
                        auto* self = static_cast<MyDazyP30_4GBoard*>(arg);
                        if (self->shutdown_armed_.exchange(false)) {
                            ESP_LOGI(TAG, "800ms 缓冲到 · 派发关机到 main_app");
                            Application::GetInstance().Schedule([self]() {
                                self->ShutdownOrSleep("再见", "", "", 1500, false);
                            });
                        }
                    },
                    .arg = this,
                    .dispatch_method = ESP_TIMER_TASK,
                    .name = "shutdown_delay",
                    .skip_unhandled_events = true,
                };
                esp_timer_create(&args, &shutdown_delay_timer_);
            }
            esp_timer_stop(shutdown_delay_timer_);
            esp_timer_start_once(shutdown_delay_timer_, 800 * 1000);  // 800ms
        }, 3000);

        boot_button_.OnPressUp([this]() {
            // 首次 PressUp 自动清 grace（用户终于松手 → 后续按键回归正常语义）
            if (boot_hold_grace_active_.exchange(false)) {
                ESP_LOGI(TAG, "开机长按 grace 期已结束（用户松手）");
            }
            if (shutdown_armed_.exchange(false)) {
                if (shutdown_delay_timer_) esp_timer_stop(shutdown_delay_timer_);
                ESP_LOGI(TAG, "关机取消（用户在 800ms 缓冲内松开）");
            }
        });

        volume_up_button_.OnClick  ([this]() { ApplyVolume(+10); });
        volume_down_button_.OnClick([this]() { ApplyVolume(-10); });
    }

    // 应用一次音量增量；clamp 到 [0,100] 并刷新状态栏 + 唤醒省电定时器
    void ApplyVolume(int delta) {
        auto* codec = GetAudioCodec();
        if (!codec) return;
        int v = codec->output_volume() + delta;
        if (v > 100) v = 100;
        if (v < 0) v = 0;
        codec->SetOutputVolume(v);
        char buf[32];
        snprintf(buf, sizeof(buf), "%s %d", Lang::Strings::VOLUME, v);
        GetDisplay()->ShowNotification(buf, 1500);
        WakeUp();
    }

    // ========================================================
    // 状态上报（仅唤醒事件触发：进省电前 PowerSaveTimer::OnEnterSleepMode 调一次）
    // 字段拼装 / idle / music 防御全部在 Ota::ReportStatus()，板级仅负责调用时机。
    // ========================================================

    void ReportStatus() {
        Application::GetInstance().Schedule([]() {
            Ota ota; ota.ReportStatus();
        });
    }


    // ========================================================
    // 初始化业务逻辑
    // ========================================================

    void ApplyDefaultSettings() {
        // 音量范围修正（50-100）
        Settings audio_settings("audio", true);
        constexpr int DEFAULT_VOLUME = 80;
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

        // 闹钟 TIMER 唤醒：跳过欢迎音 · AlarmRinger 接管响铃 · 不抢屏不抢音
        if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER) {
            ESP_LOGI(TAG, "TIMER 唤醒(闹钟模式)· 跳过欢迎音");
            vTaskDelete(NULL);
            return;
        }

        if (app.GetDeviceState() != kDeviceStateWifiConfiguring && self->first_boot_) {
            app.RequestAutoChatOnIdle();
            app.PlaySound(Lang::Sounds::OGG_WELCOME);
        }
        vTaskDelete(NULL);
    }

    void StartWelcomeTask() {
        if (!first_boot_) return;
        xTaskCreatePinnedToCore(&MyDazyP30_4GBoard::WelcomeTaskEntry,
                                "welcome_init", 3072, this, 3, nullptr, 1);
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
        HandleWakeupCause();
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

        // 触发 audio_codec_ 懒构造（codec 必须在 ApplyDefaultSettings 写音量前就绪）
        GetAudioCodec();

        ApplyDefaultSettings();

        // 注册板专属 MCP 工具（教育卡 show_stroke/show_card）— 必须在 Display 初始化之后
        // 修复 BUG：v32 之前 P30-4G 完全没注册 show_stroke，导致 LLM 看不到工具 → 用户感知"识字 GIF 不工作"
        InitializeTools();

        ESP_LOGI(TAG, "MyDazy P30 4G 初始化完成 (ES8311+ES7210, 支持4G、电源管理、触摸屏)");

        StartWelcomeTask();
    }

    // MCP 工具注册（板专属能力）— 通用工具由 McpServer::AddCommonTools 自动注册
    void InitializeTools() {
        auto& mcp = McpServer::GetInstance();
        // 教育卡 MCP 工具集：show_stroke 笔画 GIF + show_card 教育卡
        // 4G/WiFi 两种网络模式下都开启 show_stroke：
        //   完整性校验四重防护（1KB min + GIF89a/87a magic + 末字节 0x3B Trailer + Schedule state 守护）
        //   弱网失败时端侧自动弹大字 EduCard 兜底，不会显示脏帧或崩溃
        RegisterEducationMcpTools(mcp, dynamic_cast<UiDisplay*>(GetDisplay()));
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

    // 出厂复位任务（B2 修复）· 9 连击双击确认时由 OnDoubleClick 派发
    // 任务: Core 0 · stack 6 KB(HTTP+TLS+nvs_erase 用量) · P3 · 一次性 · SystemReset::CheckButtons 内 esp_restart 不返回
    // 异步化目的: 避免 button 内部 task 同步阻塞 8-9 s 导致 esp_timer 调度冻结 / LVGL 卡死
    static void FactoryResetTaskEntry(void* /*arg*/) {
        ESP_LOGI("P30_4G", "factory_reset task: 开始");
        RequestServerUnbind();             // 最多 1.5s（HTTP timeout）· 失败不阻塞
        SystemReset::CheckButtons(true);   // 内部: nvs_flash_erase + partition_erase + RestartInSeconds(3) + esp_restart
        // 兜底: 理论不会执行，因 esp_restart 不返回
        vTaskDelete(nullptr);
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
        // 1.5s 超时（B2 修复：原 5s）· 出厂复位路径已在独立 factory_reset 任务跑
        // 4G CSQ 差时快速失败，避免阻塞后续 SystemReset::CheckButtons + esp_restart
        http->SetTimeout(1500);

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

    // 切换网络：优雅断 MQTT/WS → 停 audio → 写 NVS → esp_restart() · 电源复位由 ShutdownHandler 自动接管
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
        Application::GetInstance().ResetProtocol();  // 与 EnterDeepSleep 对称：优雅断 MQTT/WS 避免服务端残留 session
        vTaskDelay(pdMS_TO_TICKS(500));              // 等异步 Schedule lambda 完成 close + 析构
        Application::GetInstance().GetAudioService().Stop();
        esp_restart();   // ShutdownHandler 自动断 LDO 复位 LCD
    }
};

DECLARE_BOARD(MyDazyP30_4GBoard);
