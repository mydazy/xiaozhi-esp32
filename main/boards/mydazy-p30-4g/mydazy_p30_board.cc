// 项目头
#include "dual_network_board.h"
#include "assets/lang_config.h"
#include "codecs/box_audio_codec.h"
#include "display/display.h"
#include "display/emote_display.h"
#include "display/lcd_display.h"
#include "esp_lcd_jd9853.h"
#include "axs5106l_touch.h"
#include "sc7a20h.h"
#include "application.h"
#include "audio/music_player.h"
#include "button.h"
#include "config.h"
#include "mcp_server.h"
#include "p30_backlight.h"
#include "p30_button.h"
#include "settings.h"
#include "system_info.h"
#include "power_save_timer.h"
#include "power_manager.h"

// ESP-IDF
#include <esp_log.h>
#include <esp_err.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <esp_rom_sys.h>
#include <wifi_manager.h>
#include <nvs_flash.h>
#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <driver/rtc_io.h>

#include <atomic>

#define TAG "MyDazyP30_4GBoard"

namespace {
// 小工具：处于 Speaking 时先 Abort，避免状态冲突
inline void AbortIfSpeaking() {
    auto& app = Application::GetInstance();
    if (app.GetDeviceState() == kDeviceStateSpeaking) {
        app.AbortSpeaking(kAbortReasonNone);
    }
}
}  // namespace

class MyDazyP30_4GBoard : public DualNetworkBoard {
private:
    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    P30Button boot_button_;
    Button volume_up_button_;
    Button volume_down_button_;

    // SC7A20H 三轴加速度传感器
    sc7a20h_handle_t sc7a20h_sensor_ = nullptr;
    bool sc7a20h_initialized_ = false;

    // 显示
    Display* display_ = nullptr;
    PowerManager* power_manager_ = nullptr;
    PowerSaveTimer* power_save_timer_ = nullptr;

    // 触摸屏（C handle，opaque struct pointer）
    axs5106l_touch_handle_t touch_driver_ = nullptr;

    // 音频编解码器
    BoxAudioCodec* audio_codec_ = nullptr;

    bool first_boot_ = false;

    // 音量长按递增任务（多核共享，用 atomic）
    TaskHandle_t vol_up_task_ = nullptr;
    TaskHandle_t vol_down_task_ = nullptr;
    std::atomic<bool> vol_up_running_{false};
    std::atomic<bool> vol_down_running_{false};

    // 长按录音状态跟踪（按键线程 vs 主循环）
    std::atomic<bool> is_recording_for_send_{false};

    // 关机三段长按状态（3秒提醒、5秒执行、PressUp 取消）
    std::atomic<bool> shutdown_armed_{false};

    // 9 连击恢复出厂 → 等双击确认
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
            .glitch_ignore_cnt = 7,
            .intr_priority = 3,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 0,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
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
                ESP_LOGI(TAG, "从陀螺仪唤醒");
                first_boot_ = true;
                break;
            case ESP_SLEEP_WAKEUP_TIMER:
                ESP_LOGI(TAG, "从定时器唤醒（闹钟）");
                break;
            default:
                ESP_LOGI(TAG, "首次启动或复位 (原因=%u)", wakeup_reason);
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

        // 配置音频电源GPIO为输出模式（LDO 总开关，控制 LCD+音频+4G VDD_EXT）
        gpio_config_t output_conf = {
            .pin_bit_mask = (1ULL << AUDIO_PWR_EN_GPIO),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&output_conf);
        rtc_gpio_hold_dis(AUDIO_PWR_EN_GPIO);

        // 软启动音频电源
        gpio_set_level(AUDIO_PWR_EN_GPIO, 0);
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
        sc7a20h_config_t cfg = SC7A20H_DEFAULT_CONFIG();
        cfg.i2c_addr = 0x19;
        if (sc7a20h_create_with_motion_detection(i2c_bus_, &cfg, NULL, &sc7a20h_sensor_) == ESP_OK) {
            ESP_LOGI(TAG, "SC7A20H传感器初始化成功（运动检测+防抖已启用）");
            sc7a20h_initialized_ = true;
        } else {
            ESP_LOGE(TAG, "SC7A20H传感器初始化失败");
            sc7a20h_sensor_ = nullptr;
            sc7a20h_initialized_ = false;
        }
    }

    void PrepareTouchHardware() {
        axs5106l_touch_config_t cfg = AXS5106L_TOUCH_DEFAULT_CONFIG(
            i2c_bus_, TOUCH_RST_NUM, TOUCH_INT_NUM, DISPLAY_WIDTH, DISPLAY_HEIGHT);
        cfg.swap_xy  = TOUCH_SWAP_XY;
        cfg.mirror_x = TOUCH_MIRROR_X;
        cfg.mirror_y = TOUCH_MIRROR_Y;

        if (axs5106l_touch_new(&cfg, &touch_driver_) != ESP_OK) {
            ESP_LOGE(TAG, "触摸屏硬件初始化失败");
            touch_driver_ = nullptr;
        }
    }

    // C 回调蹦床：把 void* user_ctx 还原为 board 实例
    static void OnTouchWake(void *ctx) {
        static_cast<MyDazyP30_4GBoard*>(ctx)->WakeUp();
    }

    static void OnTouchGesture(axs5106l_gesture_t g, int16_t /*x*/, int16_t /*y*/, void *ctx) {
        auto* self = static_cast<MyDazyP30_4GBoard*>(ctx);
        self->WakeUp();
        if (g == AXS5106L_GESTURE_SINGLE_CLICK) {
            self->HandleTouchSingleClick();
        }
    }

    void InitializeTouch() {
        if (touch_driver_ == nullptr) return;

        if (axs5106l_touch_attach_lvgl(touch_driver_) != ESP_OK) {
            ESP_LOGE(TAG, "触摸屏输入初始化失败");
            axs5106l_touch_del(touch_driver_);
            touch_driver_ = nullptr;
            return;
        }

        axs5106l_touch_set_wake_callback(touch_driver_, &MyDazyP30_4GBoard::OnTouchWake, this);
        axs5106l_touch_set_gesture_callback(touch_driver_, &MyDazyP30_4GBoard::OnTouchGesture, this);

        ESP_LOGI(TAG, "触摸屏初始化完成");
    }

    void HandleTouchSingleClick() {
        Settings settings("status", false);
        int touch_interrupt = settings.GetInt("touchInterrupt", 1);
        if (!touch_interrupt) return;

        auto& app = Application::GetInstance();
        auto state = app.GetDeviceState();

        // MP3 播放中：单击 = 停音乐，不进入对话
        if (MusicPlayer::GetInstance().IsPlaying()) {
            ESP_LOGI(TAG, "单击停止 MP3 播放");
            MusicPlayer::GetInstance().Stop();
            return;
        }

        if (state == kDeviceStateIdle) {
            ESP_LOGI(TAG, "单击唤醒对话");
            app.PlaySound(Lang::Sounds::OGG_POPUP);
            vTaskDelay(pdMS_TO_TICKS(800));
            app.ToggleChatState();
        } else if (state == kDeviceStateSpeaking) {
            ESP_LOGI(TAG, "单击打断TTS");
            app.AbortSpeaking(kAbortReasonNone);
        }
    }

    void InitializePowerManager() {
        power_manager_ = new PowerManager(POWER_MANAGER_GPIO);

        power_manager_->OnLowBatteryStatusChanged([this](bool is_low_battery) {
            if (is_low_battery) {
                Application::GetInstance().Alert("电量不足", "请充电", "", Lang::Sounds::OGG_LOW_BATTERY);
            }
        });
        vTaskDelay(pdMS_TO_TICKS(200));

        int battery_level = power_manager_->GetBatteryLevel();
        bool is_charging = power_manager_->IsCharging();
        ESP_LOGI(TAG, "电池监控器初始化完成，当前电量: %d%%, 充电状态: %s",
                 battery_level, is_charging ? "充电中" : "未充电");

        if (power_manager_->IsOffBatteryLevel() && battery_level > 0 && !is_charging) {
            ESP_LOGE(TAG, "电量过低，强制关机");
            auto& app = Application::GetInstance();
            app.Alert("电量过低", "强制关机", "", Lang::Sounds::OGG_LOW_BATTERY);
            vTaskDelay(pdMS_TO_TICKS(3000));
            EnterDeepSleep(false);
        }
    }

    void InitializePowerSaveTimer() {
        Settings settings("status", false);
        int deep_sleep_enabled = settings.GetInt("deepSleep", 1);

        power_save_timer_ = new PowerSaveTimer(120, 60, 300);
        power_save_timer_->OnEnterSleepMode([this]() {
            ESP_LOGI(TAG, "进入省电模式");
            GetBacklight()->SetBrightness(15);
        });
        power_save_timer_->OnExitSleepMode([this]() {
            ESP_LOGI(TAG, "退出省电模式");
            GetBacklight()->RestoreBrightness();
        });
        power_save_timer_->OnShutdownRequest([this, deep_sleep_enabled]() {
            if (deep_sleep_enabled) {
                ESP_LOGI(TAG, "5分钟无操作，进入深度睡眠");
                EnterDeepSleep(true);
            }
        });

        power_save_timer_->SetEnabled(deep_sleep_enabled != 0);
    }

    // ========================================================
    // 深度睡眠（拆分子步骤，保证每个函数 ≤ 50 行）
    // ========================================================

    void ShutdownTouchAndAudioForSleep() {
        if (touch_driver_) {
            axs5106l_touch_del(touch_driver_);
            touch_driver_ = nullptr;
            vTaskDelay(pdMS_TO_TICKS(100));
        }

        ESP_LOGI(TAG, "关闭音频电源");
        gpio_set_level(AUDIO_PWR_EN_GPIO, 0);
        rtc_gpio_hold_en(AUDIO_PWR_EN_GPIO);
    }

    void ConfigureDeepSleepWakeupSources(bool enable_gyro_wakeup) {
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

    void EnterDeepSleep(bool enable_gyro_wakeup = true) {
        ESP_LOGI(TAG, "====== 开始进入深度睡眠流程 ======");

        ShutdownTouchAndAudioForSleep();
        ConfigureDeepSleepWakeupSources(enable_gyro_wakeup);

        if (GetNetworkType() == NetworkType::WIFI) {
            WifiManager::GetInstance().StopStation();
        }
        if (power_save_timer_) {
            power_save_timer_->SetEnabled(false);
        }
        gpio_set_level(DISPLAY_BACKLIGHT, 0);

        ResetAllGpiosForSleep();

        ESP_LOGI(TAG, "准备进入深度睡眠");
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_deep_sleep_start();
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
        display_ = new SpiLcdDisplay(panel_io_, panel_,
                                     DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
                                     DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
        ESP_LOGI(TAG, "SpiLcdDisplay 已启用");
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
        // 单击撤销待确认的恢复出厂请求
        waiting_factory_reset_confirm_.store(false);
        if (status != kDeviceStateIdle && status != kDeviceStateListening && status != kDeviceStateSpeaking) {
            return;
        }
        if (status == kDeviceStateIdle) {
            app.PlaySound(Lang::Sounds::OGG_POPUP);
            vTaskDelay(pdMS_TO_TICKS(1500));
        } else if (status == kDeviceStateListening) {
            app.PlaySound(Lang::Sounds::OGG_POPUP);
        }
        app.ToggleChatState();
    }

    void HandleBootDoubleClick() {
        auto& app = Application::GetInstance();

        // 优先处理：9 连击触发后 15 秒内的双击 = 确认恢复出厂
        if (waiting_factory_reset_confirm_.load()) {
            uint64_t now = esp_timer_get_time();
            if (now - factory_reset_request_time_ > 15000000) {
                waiting_factory_reset_confirm_.store(false);
                return;
            }
            waiting_factory_reset_confirm_.store(false);
            AbortIfSpeaking();
            app.Alert("确认恢复", "开始执行", "logo", Lang::Sounds::OGG_EXCLAMATION);
            vTaskDelay(pdMS_TO_TICKS(3000));
            // 擦 NVS 后 esp_restart()，ShutdownHandler 自动断 LDO 复位 LCD
            nvs_flash_erase();
            esp_restart();
            return;
        }

#if CONFIG_USE_DEVICE_AEC
        auto status = app.GetDeviceState();
        if (status == kDeviceStateIdle || status == kDeviceStateListening || status == kDeviceStateSpeaking) {
            AbortIfSpeaking();
            app.SetDeviceState(kDeviceStateIdle);
            app.SetAecMode(app.GetAecMode() == kAecOff ? kAecOnDeviceSide : kAecOff);
            WakeUp();
        }
#endif
    }

    void HandleBootMultiClick9_FactoryReset() {
        auto& app = Application::GetInstance();
        AbortIfSpeaking();
        waiting_factory_reset_confirm_.store(true);
        factory_reset_request_time_ = esp_timer_get_time();
        app.Alert("恢复出厂设置", "15秒内双击确认", "logo", Lang::Sounds::OGG_EXCLAMATION);
    }

    void HandleBootMultiClick3_SwitchNetwork() {
        auto& app = Application::GetInstance();
        if (app.GetDeviceState() == kDeviceStateWifiConfiguring) {
            app.Alert(Lang::Strings::WIFI_CONFIG_MODE, "切换到4G", "logo", Lang::Sounds::OGG_WIFICONFIG);
            vTaskDelay(pdMS_TO_TICKS(1500));
            SwitchNetworkType();
            return;
        }
        AbortIfSpeaking();
        if (GetNetworkType() == NetworkType::ML307) {
            app.Alert(Lang::Strings::WIFI_CONFIG_MODE, "切换到WiFi", "logo", Lang::Sounds::OGG_WIFICONFIG);
            vTaskDelay(pdMS_TO_TICKS(1500));
            SwitchNetworkType();
        } else {
            app.Schedule([this]() {
                auto& wifi_board = static_cast<WifiBoard&>(GetCurrentBoard());
                wifi_board.EnterWifiConfigMode();
            });
        }
    }

    void HandleBootLongPress() {
        auto& app = Application::GetInstance();
        auto status = app.GetDeviceState();
        if (status == kDeviceStateIdle || status == kDeviceStateSpeaking || status == kDeviceStateListening) {
            if (status != kDeviceStateListening) {
                app.StartListening();
            }
            is_recording_for_send_.store(true);
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
        if (is_recording_for_send_.exchange(false)) {
            app.StopListening();
            vTaskDelay(pdMS_TO_TICKS(100));
            app.PlaySound(Lang::Sounds::OGG_POPUP);
        }
    }

    // 长按 3 秒：提醒"再按 2 秒关机"，同时停止录音避免冲突
    void HandleBootLongPress3s_ShutdownWarn() {
        auto& app = Application::GetInstance();
        if (is_recording_for_send_.exchange(false)) {
            app.StopListening();
        }
        shutdown_armed_.store(true);
        ESP_LOGI(TAG, "长按 3 秒：关机倒计时开始");
        app.Alert("长按 5 秒关机", "继续按住...", "logo", Lang::Sounds::OGG_VIBRATION);
    }

    // 长按 5 秒：真正关机
    void HandleBootLongPress5s_ShutdownConfirm() {
        if (!shutdown_armed_.load()) return;
        shutdown_armed_.store(false);
        ESP_LOGI(TAG, "长按 5 秒：执行关机");
        AbortIfSpeaking();
        vTaskDelay(pdMS_TO_TICKS(2000));
        EnterDeepSleep(false);
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() { HandleBootClick(); });
        boot_button_.OnDoubleClick([this]() { HandleBootDoubleClick(); });
        boot_button_.OnMultipleClick([this]() { HandleBootMultiClick3_SwitchNetwork(); }, 3);
        boot_button_.OnMultipleClick([this]() { HandleBootMultiClick9_FactoryReset(); }, 9);
        // 长按多段：0.7s 录音、3s 关机提醒、5s 真正关机（关机统一走长按，删除 4 连击关机入口）
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
        GetDisplay()->SetStatus(buf);
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

        // 首次启动默认值：黑主题 + 35% 亮度 + 4G（用户主动切换后的偏好保留）
        Settings display_settings("display", true);
        if (display_settings.GetString("theme", "").empty()) {
            display_settings.SetString("theme", "dark");
        }
        if (display_settings.GetInt("brightness", -1) == -1) {
            display_settings.SetInt("brightness", 35);
        }
        Settings net_settings("network", true);
        if (net_settings.GetInt("type", -1) == -1) {
            net_settings.SetInt("type", 1);  // 1=ML307(4G), 0=WiFi
        }
    }

    // 欢迎音任务：开机 1.5s 后异步播放，避免阻塞构造函数；first_boot 为 false 时直接跳过
    // 任务内部 vTaskDelete(NULL) 自删，无外部 join，因此不需要保留 handle
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
    }

public:
    MyDazyP30_4GBoard() :
        DualNetworkBoard(ML307_TX_PIN, ML307_RX_PIN, GPIO_NUM_NC, 1),
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
        InitializePowerManager();
        InitializePowerSaveTimer();
        InitializeButtons();
        InitializeTools();

        GetAudioCodec();
        GetBacklight()->SetBrightness(20);

        ApplyDefaultSettings();

        ESP_LOGI(TAG, "MyDazy P30 4G 初始化完成 (ES8311+ES7210, 支持4G、电源管理、触摸屏)");

        StartWelcomeTask();
    }

    // Board 实例由 DECLARE_BOARD 单例持有，进程生命周期 = 设备运行周期
    // → 不写析构（与上游 70+ board 一致）。下电流程由 ShutdownHandler 接管。

    virtual AudioCodec* GetAudioCodec() override {
        if (audio_codec_ == nullptr) {
            audio_codec_ = new BoxAudioCodec(
                i2c_bus_,
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
        // P30 专属：duty 钳位 ~30% 防 BL 过流 + 最低亮度 15% 防闪烁
        static P30Backlight backlight(DISPLAY_BACKLIGHT, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }

    void WakeUp() {
        if (power_save_timer_) {
            power_save_timer_->WakeUp();
        }
    }

    // ESP-IDF 标准 shutdown hook：esp_restart() 调用前自动执行（OTA 升级后/恢复出厂/SwitchNetworkType 等所有路径）。
    // 切 AUDIO_PWR_EN_GPIO=GPIO9 让 LCD (JD9853) + 音频 + 4G 完整电源复位，避免重启后 LCD GRAM 残留导致黑屏/花屏。
    // 用 esp_register_shutdown_handler 注册，**完全无需修改 base Board 类或 application.cc/system_reset.cc**。
    static void ShutdownHandler() {
        gpio_set_level(AUDIO_PWR_EN_GPIO, 0);
        rtc_gpio_hold_en(AUDIO_PWR_EN_GPIO);    // 穿越 esp_restart() 保持 LOW
        esp_rom_delay_us(500 * 1000);           // 等 22uF 电容放电（shutdown 上下文用 ROM delay 更稳妥）
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
