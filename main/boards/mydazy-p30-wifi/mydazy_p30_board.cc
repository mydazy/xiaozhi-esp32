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
#include "pomodoro_manager.h"
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

#define TAG "MyDazyP30_WifiBoard"

#include <sys/time.h>
RTC_DATA_ATTR static uint64_t s_last_sleep_us = 0;
static constexpr uint64_t kDeadHoldWindowUs = 500 * 1000;
static constexpr int kShutdownReleaseWaitMaxMs = 30000;  // 关机前等松手兜底上限
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

class MyDazyP30_WifiBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    i2c_worker_handle_t     i2c_worker_ = nullptr;   /* v3.0+ 共享总线串行化 worker */
    Button boot_button_;
    Button volume_up_button_;
    Button volume_down_button_;

    // SC7A20H 三轴加速度传感器（mydazy/esp_sc7a20h component）
    sc7a20h_handle_t sc7a20h_sensor_ = nullptr;

    // 显示
    Display* display_ = nullptr;
    PowerManager* power_manager_ = nullptr;
    PowerSaveTimer* power_save_timer_ = nullptr;
    esp_timer_handle_t wake_chat_timer_ = nullptr;
    axs5106l_touch_handle_t touch_driver_ = nullptr;
    BoxAudioCodec* audio_codec_ = nullptr;

    bool first_boot_ = false;

    std::atomic<bool> shutdown_armed_{false};
    std::atomic<bool> boot_hold_grace_active_{false};
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
        ESP_ERROR_CHECK(spi_bus_initialize(DISPLAY_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void HandleWakeupCause() {
        esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
        esp_reset_reason_t reset_reason = esp_reset_reason();
        ESP_LOGI(TAG, "Boot diag: wakeup_cause=%d, reset_reason=%d", (int)wakeup_reason, (int)reset_reason);

        switch (wakeup_reason) {
            case ESP_SLEEP_WAKEUP_EXT0:
                ESP_LOGI(TAG, "从开机键唤醒");
                if (s_last_sleep_us > 0) {
                    uint64_t since_us = NowRtcUs() - s_last_sleep_us;
                    if (since_us < kDeadHoldWindowUs && gpio_get_level(BOOT_BUTTON_GPIO) == 0) {
                        ESP_LOGW(TAG, "距 sleep %llu ms · 用户未松手 · 短路 sleep", since_us / 1000);
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
                AlarmManager::MarkTimerWakeup();
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

        // VBAT 放电窗口：USB 持续供电的冷启动场景下，VT3 P-MOS 漏电流 + D2 TVS
        vTaskDelay(pdMS_TO_TICKS(1000));
        gpio_set_level(AUDIO_PWR_EN_GPIO, 1);
        ESP_LOGI(TAG, "音频电源已启用 (GPIO%d)", AUDIO_PWR_EN_GPIO);

        // ES8311/ES7210 上电稳定 ≥300ms：消除首次 esp_codec_dev_open 的 I2C INVALID_STATE 偶发 ERROR
        vTaskDelay(pdMS_TO_TICKS(300));

    }

    void InitializeSc7a20h() {
        sc7a20h_sensor_ = sc7a20h_init(i2c_worker_, 320 /*mg*/, 100 /*ms*/);
        if (!sc7a20h_sensor_) { ESP_LOGE(TAG, "SC7A20H 初始化失败"); return; }
        sc7a20h_shake (sc7a20h_sensor_, 1500, 1000, 4, 1500, &OnShake,  this);
    }

    // 摇一摇 — 仅用于闹钟摇停（日常摇→AI互动已砍 · 走路/拿起易误触发 · 两板统一）
    static void OnShake(void* /*ctx*/) {
        Application::GetInstance().Schedule([] {
            AlarmRinger::GetInstance().ShakeStop(6);   // 6 次累计才停闹铃（防误关）；非响铃时 ShakeStop 直接返回，无副作用
        });
    }

    // 桌面双击 — 唤醒屏幕（与触摸唤醒同语义 · 不进 AI 对话）
    static void OnStrike(void* ctx) {
        auto* self = static_cast<MyDazyP30_WifiBoard*>(ctx);
        Application::GetInstance().Schedule([self] {
            if (TryStopAlarmRinger("strike")) return;
            ESP_LOGI(TAG, "strike → wakeup");
            self->WakeUp();
        });
    }

    void PrepareTouchHardware() {
        axs5106l_touch_config_t cfg = {
            .worker          = i2c_worker_,
            .rst_gpio        = TOUCH_RST_NUM,
            .int_gpio        = TOUCH_INT_NUM,
            .width           = DISPLAY_WIDTH,
            .height          = DISPLAY_HEIGHT,
            .rf_mode         = AXS5106L_RF_NORMAL,
            .cb_ctx          = this,
            .on_wake         = &OnTouchWake,
            .on_click        = &OnTouchClick,
            .on_double_click = &OnTouchDoubleClick,
            .on_swipe        = &OnTouchSwipe,        /* 下滑唤起控制中心 · 上滑收回 */
        };
        if (axs5106l_touch_init(&cfg, &touch_driver_) != ESP_OK) {
            ESP_LOGE(TAG, "触摸屏硬件初始化失败");
            touch_driver_ = nullptr;
        }
    }

    static void OnTouchWake(void *ctx) {
        auto* self = static_cast<MyDazyP30_WifiBoard*>(ctx);
        if (self->power_save_timer_ && self->power_save_timer_->IsInSleepMode() && self->touch_driver_) {
            axs5106l_touch_swallow_current_press(self->touch_driver_);
        }
        self->WakeUp();
    }

    // 控制中心可见时：点击交给 LVGL 处理其内部控件，不触发业务唤醒/打断
    static bool ControlCenterAbsorbs() {
        auto* ui = dynamic_cast<UiDisplay*>(Board::GetInstance().GetDisplay());
        return ui && ui->IsControlCenterVisible();
    }

    static void OnTouchClick(int16_t /*x*/, int16_t y, void *ctx) {
        auto* self = static_cast<MyDazyP30_WifiBoard*>(ctx);
        self->WakeUp();
        if (ControlCenterAbsorbs()) return;
        if (y < 36) {
            if (auto* ui = dynamic_cast<UiDisplay*>(Board::GetInstance().GetDisplay())) {
                if (!ui->IsControlCenterVisible()) ui->ShowControlCenter();
            }
            return;
        }
        self->HandleTouchSingleClick();
    }

    static void OnTouchDoubleClick(int16_t /*x*/, int16_t y, void *ctx) {
        auto* self = static_cast<MyDazyP30_WifiBoard*>(ctx);
        self->WakeUp();
        if (ControlCenterAbsorbs()) return;
        if (y < 36) return;
        self->HandleTouchDoubleClick();
    }

    // 上滑收回控制中心（打开走单击状态栏 · 下滑/横滑不再处理）
    static void OnTouchSwipe(int16_t dx, int16_t dy, void *ctx) {
        int adx = dx < 0 ? -dx : dx;
        int ady = dy < 0 ? -dy : dy;
        if (adx >= ady) return;                 // 横滑不处理
        if (dy >= 0) return;                    // 下滑不再唤起 ControlCenter
        auto* self = static_cast<MyDazyP30_WifiBoard*>(ctx);
        self->WakeUp();
        if (auto* ui = dynamic_cast<UiDisplay*>(Board::GetInstance().GetDisplay())) {
            if (ui->IsControlCenterVisible()) ui->HideControlCenter();
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
        // 触屏不参与闹钟关停（戳屏误关风险 · 关停走按键/摇晃/语音）
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
            ScheduleWakeChatToggle(800);
        } else if (state == kDeviceStateSpeaking) {
            ESP_LOGI(TAG, "单击打断TTS");
            app.AbortSpeaking(kAbortReasonNone);
        }
    }

    // 触摸双击：退出对话回 Idle（孩子最熟悉的"退出"心智）
    void HandleTouchDoubleClick() {
        // 触屏不参与闹钟关停（同 SingleClick）
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

        // 运行期过放闭环：连续确认过放 → 切主线程 + 二次确认未充电 → 强制关机
        power_manager_->OnShutdownRequest([this]() {
            Application::GetInstance().Schedule([this]() {
                if (!power_manager_->IsCharging()) {
                    ESP_LOGE(TAG, "运行期电量过低，强制关机");
                    ShutdownOrSleep("电量过低", "强制关机", Lang::Sounds::OGG_LOW_BATTERY, 3000, false);
                }
            });
        });
        // 充电时复位过放守卫，防充放电循环后保护哑火
        power_manager_->OnChargingStatusChanged([this](bool is_charging) {
            if (is_charging) {
                power_manager_->ResetOffBatteryGuard();
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

        // 回调注入完毕，启动周期检测 + 启用运行期过放触发
        power_manager_->Start();
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
            if (PowerManager::IsChargingGlobal()) {
                ESP_LOGI(TAG, "充电中，跳过深度睡眠 · LCD 保持降亮状态");
                return;
            }
            if (deep_sleep_enabled) {
                ESP_LOGI(TAG, "5分钟无操作，进入深度睡眠");
                bool pickup = Settings("status", false).GetInt("pickupWake", 0) == 1;
                ShutdownOrSleep("休眠中", pickup ? "拿起唤醒" : "按键唤醒", "", 1500, true);
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

    // 拿起唤醒
    void ArmGyroWakeup() {
        if (!sc7a20h_sensor_) return;
        if (Settings("status", false).GetInt("pickupWake", 0) == 0) return;
        esp_err_t r = sc7a20h_wakeup(sc7a20h_sensor_, SC7A20H_GPIO_INT1);
        if (r != ESP_OK) ESP_LOGW(TAG, "sc7a20h_wakeup failed: %s", esp_err_to_name(r));
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

    void EnterDeepSleep(bool enable_gyro_wakeup = true) {
        if (enable_gyro_wakeup) {
            int next_alarm = AlarmManager::GetInstance().GetSecondsToNextAlarm();
            if (next_alarm > 0 && next_alarm <= 900) {
                ESP_LOGW(TAG, "闹钟 %d 秒后到达，放弃本次深睡等闹钟", next_alarm);
                if (power_save_timer_) power_save_timer_->WakeUp();  // 重置计时，防每秒重试
                return;
            }
        }

        ESP_LOGI(TAG, "====== 开始进入深度睡眠流程 ======");

        ESP_LOGI(TAG, "停止 AudioService（释放 codec / 退出 audio_* 任务）");
        Application::GetInstance().GetAudioService().Stop();
        vTaskDelay(pdMS_TO_TICKS(100));

        if (power_save_timer_) {
            power_save_timer_->SetEnabled(false);
        }
        gpio_set_level(DISPLAY_BACKLIGHT, 0);

        ESP_LOGI(TAG, "主动断开 MQTT/WS 长连接（优雅 close）");
        Application::GetInstance().ResetProtocol();
        vTaskDelay(pdMS_TO_TICKS(500));

        if (enable_gyro_wakeup) ArmGyroWakeup();         // EXT1 · 必须在音频断电之前
        ShutdownTouchAndAudioForSleep();                 // AUDIO_PWR_EN=0（之后 I²C 不可用）
        ESP_ERROR_CHECK(esp_sleep_enable_ext0_wakeup(BOOT_BUTTON_GPIO, 0));   // EXT0 BOOT 键
        rtc_gpio_pullup_en(BOOT_BUTTON_GPIO);
        rtc_gpio_pulldown_dis(BOOT_BUTTON_GPIO);

        WifiStation::GetInstance().Stop();
        // STA_STOP 事件链（典型 ~150-300ms），再断电源。
        vTaskDelay(pdMS_TO_TICKS(300));

        // 闹钟：arm RTC 定时唤醒（与 EXT0/EXT1 三源并存 · 含分段睡眠策略）
        AlarmManager::GetInstance().FlushNvs();
        AlarmManager::GetInstance().ConfigureTimerWakeup();

        ESP_LOGI(TAG, "准备进入深度睡眠");
        vTaskDelay(pdMS_TO_TICKS(200));

        ResetAllGpiosForSleep();

        if (!enable_gyro_wakeup) {
            gpio_config_t boot_in = {
                .pin_bit_mask = (1ULL << BOOT_BUTTON_GPIO),
                .mode = GPIO_MODE_INPUT,
                .pull_up_en = GPIO_PULLUP_ENABLE,
                .pull_down_en = GPIO_PULLDOWN_DISABLE,
                .intr_type = GPIO_INTR_DISABLE,
            };
            gpio_config(&boot_in);
            int waited = 0;
            while (gpio_get_level(BOOT_BUTTON_GPIO) == 0 && waited < kShutdownReleaseWaitMaxMs) {
                vTaskDelay(pdMS_TO_TICKS(50));
                waited += 50;
            }
            if (waited > 0) ESP_LOGI(TAG, "关机前等松手 %dms", waited);
        }

        s_last_sleep_us = NowRtcUs();
        esp_deep_sleep_start();
    }

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
        // 开机时 GPIO 仍按下 → 置 grace · OnLongPress 跳过 3s 关机 · OnPressUp 自动清
        if (first_boot_ && gpio_get_level(BOOT_BUTTON_GPIO) == 0) {
            boot_hold_grace_active_.store(true);
            ESP_LOGI(TAG, "开机长按 grace 已置位");
        }

        boot_button_.OnClick([this]() {
            if (TryStopAlarmRinger("button")) return;
            auto& app = Application::GetInstance();
            auto status = app.GetDeviceState();
            ESP_LOGI(TAG, "单击 button 状态: %u", status);
            waiting_factory_reset_confirm_.store(false);
            if (PomodoroManager::GetInstance().IsActive()) {
                ESP_LOGI(TAG, "按键 → Stop 番茄钟");
                app.PlaySound(Lang::Sounds::OGG_EXITCHAT);
                PomodoroManager::GetInstance().Stop();
                return;
            }
            if (MusicPlayer::GetInstance().IsPlaying()) {
                ESP_LOGI(TAG, "按键打断 MP3 → 唤醒对话");
                StopMp3AndExitPlayerUi();
            }
            // Connecting：按键打断握手 → 关 channel + 兜底切回 idle（防卡死在"连接中"）
            if (status == kDeviceStateConnecting) {
                ESP_LOGI(TAG, "按键打断 Connecting → 退出");
                app.PlaySound(Lang::Sounds::OGG_EXITCHAT);
                app.CloseAudioChannel();   // channel 真打开 → OnAudioChannelClosed 回调切 idle
                app.Schedule([]() {
                    auto& a = Application::GetInstance();
                    if (a.GetDeviceState() == kDeviceStateConnecting) {
                        a.SetDeviceState(kDeviceStateIdle);  // 兜底：channel 未真打开时回调不触发
                    }
                });
                return;
            }
            if (status != kDeviceStateIdle && status != kDeviceStateListening && status != kDeviceStateSpeaking) return;
            if (status == kDeviceStateIdle) {
                app.PlaySound(Lang::Sounds::OGG_WAKEUP);
                ScheduleWakeChatToggle(1500);
                return;
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
                vTaskDelay(pdMS_TO_TICKS(1500));
                RequestServerUnbind();
                SystemReset::CheckButtons(true);
                return;
            }
            // ② 配网态：物理按键双击 BLUFI ↔ AP 切换（触屏双击不参与）
            if (status == kDeviceStateWifiConfiguring) {
                static std::atomic_flag switching = ATOMIC_FLAG_INIT;
                if (!switching.test_and_set()) {
                    xTaskCreatePinnedToCore([](void*) {
                        static_cast<WifiBoard&>(Board::GetInstance()).SwitchConfigMode();
                        switching.clear();
                        vTaskDelete(nullptr);
                    }, "config_switch", 4096, nullptr, 3, nullptr, 1);
                }
                return;
            } else {
                ToggleAecMode();
            }
        });
        // 3 连击：进/退配网（控制中心点"切换"按钮走同一路径）
        boot_button_.OnMultipleClick([this]() { SwitchNetwork(); }, 3);

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
                ESP_LOGI(TAG, "grace 期：忽略 3s 关机");
                return;
            }
            if (shutdown_armed_.exchange(true)) return;
            ESP_LOGI(TAG, "长按 3 秒 → 立即播再见音（提示音=松手信号）→ 关机");
            Application::GetInstance().Schedule([this]() {
                ShutdownOrSleep("再见", "", Lang::Sounds::OGG_REBOOT, 2500, false);
            });
        }, 3000);

        boot_button_.OnPressUp([this]() {
            if (boot_hold_grace_active_.exchange(false)) {
                ESP_LOGI(TAG, "grace 已清");
            }
        });

        // PressDown 而非 OnClick：单击事件有 ~300ms 双击判定窗，快按第二下会被
        // 归类成"双击"（未注册）而吞掉。按下即响应：每按一下立即 ±10，零延迟。
        volume_up_button_.OnPressDown  ([this]() { ApplyVolume(+10); });
        volume_down_button_.OnPressDown([this]() { ApplyVolume(-10); });
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
        constexpr int DEFAULT_VOLUME = 50;
        int original_volume = audio_settings.GetInt("output_volume", DEFAULT_VOLUME);
        if (original_volume < 30) {
            audio_settings.SetInt("output_volume", DEFAULT_VOLUME);
            ESP_LOGI(TAG, "检测到音量%d小于50，自动调整为%d", original_volume, DEFAULT_VOLUME);
        }
        // 默认使用蓝牙配网
        Settings wifi_settings("wifi", true);
        wifi_settings.SetInt("blufi", 1);
    }

    static void WelcomeTaskEntry(void* arg) {
        auto* self = static_cast<MyDazyP30_WifiBoard*>(arg);
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
        xTaskCreatePinnedToCore(&MyDazyP30_WifiBoard::WelcomeTaskEntry,
                                "welcome_init", 3072, this, 3, nullptr, 1);
    }

public:
    MyDazyP30_WifiBoard() :
        boot_button_(BOOT_BUTTON_GPIO, false, 800, 400),
        volume_up_button_(VOLUME_UP_BUTTON_GPIO),
        volume_down_button_(VOLUME_DOWN_BUTTON_GPIO) {

        esp_register_shutdown_handler(ShutdownHandler);

        InitializeGpio();
        HandleWakeupCause();
        InitializeI2c();
        InitializeSpi();
        InitializeDisplay();
        vTaskDelay(pdMS_TO_TICKS(500));
        PrepareTouchHardware();
        InitializeTouch();
        InitializeSc7a20h();
        InitializePowerManager();
        InitializePowerSaveTimer();
        InitializeButtons();
        GetAudioCodec();
        ApplyDefaultSettings();
        InitializeTools();

        ESP_LOGI(TAG, "MyDazy P30 WiFi 初始化完成 (ES8311+ES7210, 纯WiFi、电源管理、触摸屏)");

        StartWelcomeTask();
    }

    // MCP 工具注册（板专属能力）— 通用工具由 McpServer::AddCommonTools 自动注册
    void InitializeTools() {
        auto& mcp = McpServer::GetInstance();
        RegisterEducationMcpTools(mcp, dynamic_cast<UiDisplay*>(GetDisplay()));
    }


    virtual AudioCodec* GetAudioCodec() override {
        if (audio_codec_ == nullptr) {
            audio_codec_ = new BoxAudioCodec(
                i2c_worker_,
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
        WifiBoard::SetPowerSaveLevel(level);
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

    // AEC 模式切换：按键双击③ 与控制中心触屏共用同一路径（保证两者行为/提示音一致）
    void ToggleAecMode() override {
#if CONFIG_USE_DEVICE_AEC
        auto& app = Application::GetInstance();
        auto status = app.GetDeviceState();
        ESP_LOGI(TAG, "ToggleAecMode: status=%d cur_aec=%d", (int)status, (int)app.GetAecMode());
        if (status == kDeviceStateIdle || status == kDeviceStateListening || status == kDeviceStateSpeaking) {
            AbortIfSpeaking();
            app.SetDeviceState(kDeviceStateIdle);
            app.SetAecMode(app.GetAecMode() == kAecOff ? kAecOnDeviceSide : kAecOff);
            WakeUp();
        }
#endif
    }

    // 控制中心"切换"/3 连击 → 进/退配网（同语义）。
    // 量产版 65f8dcd0 重构时误删此 override，致三连击落到 Board 空实现、无法进配网，此处恢复。
    bool CanSwitchNetwork() const override { return true; }
    void SwitchNetwork() override {
        auto& app = Application::GetInstance();
        PauseAudioAndChatBeforeSwitch();
        if (app.GetDeviceState() == kDeviceStateWifiConfiguring) {
            Settings settings("wifi", true);
            settings.SetInt("force_ap", 0);
            app.Alert(Lang::Strings::WIFI_CONFIG_MODE, "退出配网", "logo", Lang::Sounds::OGG_NETWORK_WIFI);
            vTaskDelay(pdMS_TO_TICKS(1500));
            app.Reboot();
        } else {
            app.Alert(Lang::Strings::WIFI_CONFIG_MODE, "进入配网", "logo", Lang::Sounds::OGG_BLE_CONFIG);
            vTaskDelay(pdMS_TO_TICKS(1500));
            ResetWifiConfiguration();   // 内部 force_ap=1 + skip_welcome=1 + Reboot
        }
    }

    // 异步等提示音播完再 ToggleChatState：
    void ScheduleWakeChatToggle(int delay_ms) {
        if (!wake_chat_timer_) {
            const esp_timer_create_args_t args = {
                .callback = [](void*) {
                    Application::GetInstance().Schedule([]() {
                        Application::GetInstance().ToggleChatState();
                    });
                },
                .arg = nullptr,
                .dispatch_method = ESP_TIMER_TASK,
                .name = "wake_chat",
                .skip_unhandled_events = true,
            };
            ESP_ERROR_CHECK(esp_timer_create(&args, &wake_chat_timer_));
        }
        esp_timer_stop(wake_chat_timer_);
        esp_timer_start_once(wake_chat_timer_, (uint64_t)delay_ms * 1000);
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
            ESP_LOGI("P30_WIFI", "Not mydazy server, skip unbind request");
            return false;
        }
        url += "/reset";

        if (!WifiStation::GetInstance().IsConnected()) {
            ESP_LOGW("P30_WIFI", "WiFi not connected, skip server unbind");
            return false;
        }

        auto network = Board::GetInstance().GetNetwork();
        if (!network) {
            ESP_LOGW("P30_WIFI", "Network not available, skip server unbind");
            return false;
        }
        auto http = network->CreateHttp(0);
        if (!http) {
            ESP_LOGW("P30_WIFI", "Failed to create HTTP client");
            return false;
        }
        http->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
        http->SetHeader("Client-Id", Board::GetInstance().GetUuid().c_str());
        http->SetHeader("User-Agent", SystemInfo::GetUserAgent().c_str());
        http->SetTimeout(5000);

        if (!http->Open("GET", url)) {
            ESP_LOGW("P30_WIFI", "Failed to connect to server for unbind");
            return false;
        }
        int status_code = http->GetStatusCode();
        std::string response = http->ReadAll();
        http->Close();
        ESP_LOGI("P30_WIFI", "Server unbind response: code=%d, body=%s", status_code, response.c_str());
        return status_code == 200 || status_code == 204;
    }

};

DECLARE_BOARD(MyDazyP30_WifiBoard);
