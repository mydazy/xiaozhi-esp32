#include "wifi_board.h"
#include "assets/lang_config.h"
#include "codecs/box_audio_codec.h"
#include "display/display.h"
#include "display/emote_display.h"
#include "display/lcd_display.h"
#include "esp_lcd_jd9853.h"
#include "axs5106l_touch.h"
#include "sc7a20h.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "settings.h"
#include "system_info.h"
#include "system_reset.h"

#include <atomic>
#include "power_save_timer.h"
#include "power_manager.h"
#include "mcp_server.h"
#include "ota.h"
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
#include <wifi_manager.h>
#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <driver/rtc_io.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>

#define TAG "MyDazyP30_WifiBoard"

class MyDazyP30_WifiBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    Button boot_button_;
    Button volume_up_button_;
    Button volume_down_button_;

    // SC7A20H 三轴加速度传感器
    Sc7a20h* sc7a20h_sensor_ = nullptr;
    bool sc7a20h_initialized_ = false;

    // 显示
    Display* display_ = nullptr;
    PowerManager* power_manager_ = nullptr;
    PowerSaveTimer* power_save_timer_ = nullptr;

    // 触摸屏
    Axs5106lTouch* touch_driver_ = nullptr;

    // 音频编解码器
    BoxAudioCodec* audio_codec_ = nullptr;

    bool first_boot_ = false;
    bool is_alarm_clock_ = false;
    time_t start_time_;

    // 音量调节任务
    TaskHandle_t vol_up_task_ = nullptr;
    TaskHandle_t vol_down_task_ = nullptr;
    volatile bool vol_up_running_ = false;
    volatile bool vol_down_running_ = false;

    // 长按录音状态跟踪
    volatile bool is_recording_for_test_ = false;
    volatile bool is_recording_for_send_ = false;

    // 长按关机状态机：3s 警告 → 5s 执行；5s 前松开取消
    std::atomic<bool> shutdown_armed_{false};

    // 欢迎音异步任务句柄
    volatile TaskHandle_t welcome_task_handle_ = nullptr;

    // 恢复出厂设置确认状态
    volatile bool waiting_factory_reset_confirm_ = false;
    uint64_t factory_reset_request_time_ = 0;

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

    void InitializeGpio() {
        // 先关闭背光，防止重启后显示随机GRAM数据
        gpio_reset_pin(DISPLAY_BACKLIGHT);
        gpio_set_direction(DISPLAY_BACKLIGHT, GPIO_MODE_OUTPUT);
        gpio_set_level(DISPLAY_BACKLIGHT, 0);

        // PA_EN 置高
        gpio_reset_pin(AUDIO_CODEC_PA_PIN);
        gpio_set_direction(AUDIO_CODEC_PA_PIN, GPIO_MODE_OUTPUT);
        gpio_set_level(AUDIO_CODEC_PA_PIN, 1);

        // 配置音频电源GPIO为输出模式
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
        ESP_LOGI(TAG, "音频电源稳定，I2C通信就绪");

        // 配置TE输入GPIO
        gpio_config_t input_conf = {
            .pin_bit_mask = (1ULL << DISPLAY_LCD_TE),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&input_conf);

        esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
        switch (wakeup_reason) {
            case ESP_SLEEP_WAKEUP_EXT0:
                ESP_LOGI(TAG, "从开机键唤醒");
                first_boot_ = true;
                break;
            case ESP_SLEEP_WAKEUP_EXT1:
                ESP_LOGI(TAG, "从陀螺仪唤醒");
                first_boot_ = true;
                break;
            case ESP_SLEEP_WAKEUP_TIMER:
                ESP_LOGI(TAG, "从定时器唤醒");
                is_alarm_clock_ = true;
                break;
            default:
                ESP_LOGI(TAG, "首次启动或复位 (原因=%u)", wakeup_reason);
                first_boot_ = true;
                break;
        }
    }

    void InitializeSc7a20h() {
        sc7a20h_sensor_ = new Sc7a20h(i2c_bus_, 0x19);

        if (sc7a20h_sensor_->Initialize()) {
            ESP_LOGI(TAG, "SC7A20H传感器初始化成功");
            sc7a20h_sensor_->SetMotionDetection(true);
            sc7a20h_initialized_ = true;
        } else {
            ESP_LOGE(TAG, "SC7A20H传感器初始化失败");
            delete sc7a20h_sensor_;
            sc7a20h_sensor_ = nullptr;
            sc7a20h_initialized_ = false;
        }
    }

    void PrepareTouchHardware() {
        touch_driver_ = new Axs5106lTouch(
            i2c_bus_,
            TOUCH_RST_NUM,
            TOUCH_INT_NUM,
            DISPLAY_WIDTH,
            DISPLAY_HEIGHT,
            TOUCH_SWAP_XY,
            TOUCH_MIRROR_X,
            TOUCH_MIRROR_Y
        );

        if (!touch_driver_->InitializeHardware()) {
            ESP_LOGE(TAG, "触摸屏硬件初始化失败");
            delete touch_driver_;
            touch_driver_ = nullptr;
        }
    }

    void InitializeTouch() {
        if (touch_driver_ == nullptr) return;

        if (!touch_driver_->InitializeInput()) {
            ESP_LOGE(TAG, "触摸屏输入初始化失败");
            delete touch_driver_;
            touch_driver_ = nullptr;
            return;
        }

        touch_driver_->SetWakeCallback([this]() {
            WakeUp();
        });

        touch_driver_->SetGestureCallback([this](TouchGesture gesture, int16_t x, int16_t y) {
            WakeUp();

            switch (gesture) {
                case TouchGesture::SingleClick: {
                    Settings settings("status", false);
                    int touch_interrupt = settings.GetInt("touchInterrupt", 1);
                    if (!touch_interrupt) break;

                    auto& app = Application::GetInstance();
                    auto state = app.GetDeviceState();

                    if (state == kDeviceStateIdle) {
                        ESP_LOGI(TAG, "单击唤醒对话");
                        app.PlaySound(Lang::Sounds::OGG_WAKEUP);
                        vTaskDelay(pdMS_TO_TICKS(800));
                        app.ToggleChatState();
                    } else if (state == kDeviceStateSpeaking) {
                        ESP_LOGI(TAG, "单击打断TTS");
                        app.AbortSpeaking(kAbortReasonNone);
                    }
                    break;
                }
                default:
                    break;
            }
        });

        ESP_LOGI(TAG, "触摸屏初始化完成");
    }

    void InitializePowerManager() {
        power_manager_ = new PowerManager(POWER_MANAGER_GPIO);
        power_manager_->OnChargingStatusChanged([this](bool is_charging) {
            Settings settings("status", false);
            int deep_sleep_enabled = settings.GetInt("deepSleep", 1);
            if (deep_sleep_enabled) {
                power_save_timer_->SetEnabled(true);
            } else {
                power_save_timer_->SetEnabled(false);
            }
        });

        power_manager_->OnLowBatteryStatusChanged([this](bool is_low_battery) {
            if (is_low_battery) {
                auto& app = Application::GetInstance();
                app.Alert("电量不足", "请充电", "", Lang::Sounds::OGG_CHARGE);
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

        power_save_timer_->OnShutdownRequest([this]() {
            Settings settings("status", false);
            int deep_sleep_enabled = settings.GetInt("deepSleep", 1);
            if (deep_sleep_enabled) {
                ESP_LOGI(TAG, "5分钟无操作，进入深度睡眠");
                EnterDeepSleep(true);
            }
        });

        if (deep_sleep_enabled) {
            power_save_timer_->SetEnabled(true);
        } else {
            power_save_timer_->SetEnabled(false);
        }
    }

    void EnterDeepSleep(bool enable_gyro_wakeup = true) {
        ESP_LOGI(TAG, "====== 开始进入深度睡眠流程 ======");

        // 先关闭触摸屏
        if (touch_driver_) {
            touch_driver_->Cleanup();
            delete touch_driver_;
            touch_driver_ = nullptr;
            vTaskDelay(pdMS_TO_TICKS(100));
        }

        ESP_LOGI(TAG, "关闭音频电源");
        gpio_set_level(AUDIO_PWR_EN_GPIO, 0);
        rtc_gpio_hold_en(AUDIO_PWR_EN_GPIO);

        // 按键唤醒
        ESP_ERROR_CHECK(esp_sleep_enable_ext0_wakeup(BOOT_BUTTON_GPIO, 0));
        ESP_ERROR_CHECK(rtc_gpio_pullup_en(BOOT_BUTTON_GPIO));
        ESP_ERROR_CHECK(rtc_gpio_pulldown_dis(BOOT_BUTTON_GPIO));

        vTaskDelay(pdMS_TO_TICKS(200));

        // 陀螺仪唤醒
        if (enable_gyro_wakeup) {
            Settings settings("status", false);
            int32_t pickup_wake = settings.GetInt("pickupWake", 1);
            if (pickup_wake && sc7a20h_initialized_) {
                ESP_ERROR_CHECK(esp_sleep_enable_ext1_wakeup_io((1ULL << SC7A20H_GPIO_INT1), ESP_EXT1_WAKEUP_ANY_LOW));
                ESP_ERROR_CHECK(rtc_gpio_pullup_en(SC7A20H_GPIO_INT1));
                ESP_ERROR_CHECK(rtc_gpio_pulldown_dis(SC7A20H_GPIO_INT1));
            }
        }

        // 关闭 WiFi
        WifiManager::GetInstance().StopStation();

        if (power_save_timer_) {
            power_save_timer_->SetEnabled(false);
        }

        gpio_set_level(DISPLAY_BACKLIGHT, 0);

        // 重置音频GPIO
        gpio_reset_pin(AUDIO_I2S_GPIO_MCLK);
        gpio_reset_pin(AUDIO_I2S_GPIO_BCLK);
        gpio_reset_pin(AUDIO_I2S_GPIO_WS);
        gpio_reset_pin(AUDIO_I2S_GPIO_DIN);
        gpio_reset_pin(AUDIO_I2S_GPIO_DOUT);
        gpio_reset_pin(AUDIO_CODEC_I2C_SDA_PIN);
        gpio_reset_pin(AUDIO_CODEC_I2C_SCL_PIN);
        gpio_reset_pin(AUDIO_CODEC_PA_PIN);

        // 显示GPIO
        gpio_reset_pin(DISPLAY_SPI_MOSI);
        gpio_reset_pin(DISPLAY_SPI_SCLK);
        gpio_reset_pin(DISPLAY_LCD_DC);
        gpio_reset_pin(DISPLAY_LCD_CS);
        gpio_reset_pin(DISPLAY_BACKLIGHT);

        // 触摸屏GPIO
        gpio_reset_pin(TOUCH_RST_NUM);
        gpio_reset_pin(TOUCH_INT_NUM);

        // 配置GPIO为输入模式降低功耗
        gpio_config_t input_conf = {
            .pin_bit_mask = (1ULL << AUDIO_I2S_GPIO_MCLK) | (1ULL << AUDIO_I2S_GPIO_BCLK) |
                            (1ULL << AUDIO_I2S_GPIO_WS) | (1ULL << AUDIO_I2S_GPIO_DIN) |
                            (1ULL << AUDIO_I2S_GPIO_DOUT) | (1ULL << AUDIO_CODEC_I2C_SDA_PIN) |
                            (1ULL << AUDIO_CODEC_I2C_SCL_PIN) | (1ULL << AUDIO_CODEC_PA_PIN) |
                            (1ULL << DISPLAY_SPI_MOSI) | (1ULL << DISPLAY_SPI_SCLK) |
                            (1ULL << DISPLAY_LCD_DC) | (1ULL << DISPLAY_LCD_CS) |
                            (1ULL << DISPLAY_BACKLIGHT) | (1ULL << TOUCH_RST_NUM) |
                            (1ULL << TOUCH_INT_NUM),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE
        };
        gpio_config(&input_conf);

        ESP_LOGI(TAG, "准备进入深度睡眠");
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_deep_sleep_start();
    }

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
        ESP_LOGI(TAG, "LVGL显示模式已启用");
#endif

        SystemInfo::PrintHeapStats();
    }

    // 长按 3 秒：提醒"再按 2 秒关机"，同时停止录音避免冲突
    void HandleBootLongPress3s_ShutdownWarn() {
        auto& app = Application::GetInstance();
        if (is_recording_for_send_ || is_recording_for_test_) {
            is_recording_for_send_ = false;
            is_recording_for_test_ = false;
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
        auto& app = Application::GetInstance();
        if (app.GetDeviceState() == kDeviceStateSpeaking) {
            app.AbortSpeaking(kAbortReasonNone);
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
        EnterDeepSleep(false);
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            auto status = app.GetDeviceState();
            ESP_LOGI(TAG, "单击 button 状态: %u", status);
            waiting_factory_reset_confirm_ = false;
            if (status == kDeviceStateIdle || status == kDeviceStateListening || status == kDeviceStateSpeaking) {
                if (status == kDeviceStateIdle) {
                    app.PlaySound(Lang::Sounds::OGG_WAKEUP);
                    vTaskDelay(pdMS_TO_TICKS(1500));
                } else if (status == kDeviceStateListening) {
                    app.PlaySound(Lang::Sounds::OGG_EXITCHAT);
                }
                app.ToggleChatState();
            }
        });

        boot_button_.OnDoubleClick([this]() {
            auto& app = Application::GetInstance();
            auto status = app.GetDeviceState();

            if (waiting_factory_reset_confirm_) {
                uint64_t now = esp_timer_get_time();
                if (now - factory_reset_request_time_ > 15000000) {
                    waiting_factory_reset_confirm_ = false;
                    return;
                }
                waiting_factory_reset_confirm_ = false;
                if (status == kDeviceStateSpeaking) {
                    app.AbortSpeaking(kAbortReasonNone);
                }
                app.Alert("确认恢复", "开始执行", "logo", Lang::Sounds::OGG_START_RESET);
                vTaskDelay(pdMS_TO_TICKS(3000));
                // 服务器解绑 + NVS 擦除 + otadata 擦除；app.Reboot() 内部会切 LDO 复位 LCD/音频
                SystemReset::DoFactoryReset();
                app.Reboot();
                return;
            }

            #if CONFIG_USE_DEVICE_AEC
            if (status == kDeviceStateIdle || status == kDeviceStateListening || status == kDeviceStateSpeaking) {
                if (status == kDeviceStateSpeaking) {
                    app.AbortSpeaking(kAbortReasonNone);
                }
                app.SetDeviceState(kDeviceStateIdle);
                if (app.GetAecMode() == kAecOff) {
                    app.SetAecMode(kAecOnDeviceSide);
                } else {
                    app.SetAecMode(kAecOff);
                }
                WakeUp();
            }
            #endif
        });

        // 连按3次进入/退出配网模式（WiFi 版本无 4G 切换）
        boot_button_.OnMultipleClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateSpeaking) {
                app.AbortSpeaking(kAbortReasonNone);
            }
            app.Alert(Lang::Strings::WIFI_CONFIG_MODE, "开始配网", "logo", Lang::Sounds::OGG_NETWORK_WIFI);
            vTaskDelay(pdMS_TO_TICKS(1500));
            app.Schedule([this]() {
                EnterWifiConfigMode();
            });
        }, 3);

        // 连按4次关机
        boot_button_.OnMultipleClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateSpeaking) {
                app.AbortSpeaking(kAbortReasonNone);
            }
            app.Alert("连按4次关机", "拜拜^-^", "", Lang::Sounds::OGG_SHUTDOWN);
            vTaskDelay(pdMS_TO_TICKS(3000));
            EnterDeepSleep(false);
        }, 4);

        // 连按6次：音频测试模式
        boot_button_.OnMultipleClick([this]() {
            auto& app = Application::GetInstance();
            auto status = app.GetDeviceState();
            if (status == kDeviceStateSpeaking) {
                app.AbortSpeaking(kAbortReasonNone);
            }
            app.SetDeviceState(kDeviceStateWifiConfiguring);
            app.StartListening();
            app.Alert("音频测试", "", "", Lang::Sounds::OGG_AUDIO_TEST);
            vTaskDelay(pdMS_TO_TICKS(3000));
            app.SetDeviceState(kDeviceStateAudioTesting);
            app.StopListening();
        }, 6);

        // 连按9次：恢复出厂设置确认
        boot_button_.OnMultipleClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateSpeaking) {
                app.AbortSpeaking(kAbortReasonNone);
            }
            waiting_factory_reset_confirm_ = true;
            factory_reset_request_time_ = esp_timer_get_time();
            app.Alert("恢复出厂设置", "10秒内双击确认", "logo", Lang::Sounds::OGG_FACTORY_RESET);
        }, 9);

        // 长按多段：0.7s 录音、3s 关机提醒、5s 真正关机（与 P30-4G 对齐）
        boot_button_.OnLongPress([this]() {
            auto& app = Application::GetInstance();
            auto status = app.GetDeviceState();
            if (status == kDeviceStateIdle || status == kDeviceStateSpeaking || status == kDeviceStateListening) {
                if (status != kDeviceStateListening) {
                    app.StartListening();
                }
                is_recording_for_send_ = true;
            } else if (status == kDeviceStateStarting || status == kDeviceStateActivating || status == kDeviceStateWifiConfiguring) {
                is_recording_for_test_ = true;
                app.SetDeviceState(kDeviceStateWifiConfiguring);
                vTaskDelay(pdMS_TO_TICKS(100));
                app.StartListening();
            }
        }, 700);
        boot_button_.OnLongPress([this]() { HandleBootLongPress3s_ShutdownWarn(); }, 3000);
        boot_button_.OnLongPress([this]() { HandleBootLongPress5s_ShutdownConfirm(); }, 5000);

        boot_button_.OnPressUp([this]() {
            auto& app = Application::GetInstance();
            // 关机倒计时未到 5 秒就松开 → 取消
            if (shutdown_armed_.exchange(false)) {
                ESP_LOGI(TAG, "关机倒计时取消");
                app.PlaySound(Lang::Sounds::OGG_POPUP);
                return;
            }
            if (is_recording_for_test_) {
                is_recording_for_test_ = false;
                app.SetDeviceState(kDeviceStateAudioTesting);
                app.StopListening();
            } else if (is_recording_for_send_) {
                is_recording_for_send_ = false;
                app.StopListening();
                vTaskDelay(pdMS_TO_TICKS(100));
                app.PlaySound(Lang::Sounds::OGG_POPUP);
            }
        });

        // 音量按钮
        volume_up_button_.OnClick([this]() { AdjustVolume(+10); });
        volume_down_button_.OnClick([this]() { AdjustVolume(-10); });

        volume_up_button_.OnLongPress([this]() {
            StartVolumeTask(+5, &vol_up_task_, &vol_up_running_);
        });
        volume_up_button_.OnPressUp([this]() { vol_up_running_ = false; });

        volume_down_button_.OnLongPress([this]() {
            StartVolumeTask(-5, &vol_down_task_, &vol_down_running_);
        });
        volume_down_button_.OnPressUp([this]() { vol_down_running_ = false; });
    }

    // 状态定时上报定时器
    esp_timer_handle_t status_timer_ = nullptr;

    void ReportStatus() {
        // 仅在 idle / listening 上报（与 P30-4G / P31 一致）：
        // speaking 时让位 TTS 带宽 + connecting/activating 瞬态无上报意义
        auto state = Application::GetInstance().GetDeviceState();
        if (state != kDeviceStateIdle && state != kDeviceStateListening) {
            ESP_LOGD(TAG, "skip status report, state=%d (仅 idle/listening 上报)", (int)state);
            return;
        }

        int battery = power_manager_ ? power_manager_->GetBatteryLevel() : -1;
        bool charging = power_manager_ ? power_manager_->IsCharging() : false;
        size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);

        // WiFi 版本只上报 WiFi 信号强度
        int rssi = 0;
        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            rssi = ap_info.rssi;
        }

        Application::GetInstance().Schedule(
            [battery, charging, free_heap, rssi]() {
            cJSON* p = cJSON_CreateObject();
            cJSON_AddNumberToObject(p, "battery", battery);
            cJSON_AddBoolToObject(p, "charging", charging);

            cJSON* net = cJSON_CreateObject();
            cJSON_AddStringToObject(net, "type", "wifi");
            if (rssi < 0) cJSON_AddNumberToObject(net, "rssi", rssi);
            cJSON_AddItemToObject(p, "network", net);

            cJSON_AddNumberToObject(p, "free_heap", (double)free_heap);
            Ota::ReportStatus(p);
        });
    }

    void StartStatusTimer() {
        esp_timer_create_args_t args = {
            .callback = [](void* arg) {
                static_cast<MyDazyP30_WifiBoard*>(arg)->ReportStatus();
            },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "status_report",
        };
        esp_timer_create(&args, &status_timer_);
        esp_timer_start_periodic(status_timer_, 90 * 1000000ULL);
    }

    void AdjustVolume(int delta) {
        auto codec = GetAudioCodec();
        if (codec) {
            int volume = codec->output_volume() + delta;
            if (volume > 100) volume = 100;
            if (volume < 0) volume = 0;
            codec->SetOutputVolume(volume);
            char volume_text[64];
            snprintf(volume_text, sizeof(volume_text), "%s %d", Lang::Strings::VOLUME, volume);
            GetDisplay()->SetStatus(volume_text);
            WakeUp();
        }
    }

    void StartVolumeTask(int delta, TaskHandle_t* task_handle, volatile bool* running) {
        if (*task_handle != NULL) return;
        *running = true;
        xTaskCreatePinnedToCore([](void* arg) {
            auto [delta, running, task_handle] = *static_cast<std::tuple<int, volatile bool*, TaskHandle_t*>*>(arg);
            auto* params = static_cast<std::tuple<int, volatile bool*, TaskHandle_t*>*>(arg);
            delete params;

            while (*running) {
                auto codec = Board::GetInstance().GetAudioCodec();
                if (codec) {
                    int v = codec->output_volume() + delta;
                    if (v > 100) v = 100;
                    if (v < 0) v = 0;
                    codec->SetOutputVolume(v);
                    char volume_text[64];
                    snprintf(volume_text, sizeof(volume_text), "%s %d", Lang::Strings::VOLUME, v);
                    Board::GetInstance().GetDisplay()->SetStatus(volume_text);
                    static_cast<MyDazyP30_WifiBoard&>(Board::GetInstance()).WakeUp();
                }
                vTaskDelay(pdMS_TO_TICKS(200));
            }

            *task_handle = NULL;
            vTaskDelete(NULL);
        }, "vol_adjust", 2048, new std::tuple<int, volatile bool*, TaskHandle_t*>(delta, running, task_handle), 5, task_handle, 1);
    }

public:
    MyDazyP30_WifiBoard() :
        boot_button_(BOOT_BUTTON_GPIO, false, 1500, 400),
        volume_up_button_(VOLUME_UP_BUTTON_GPIO),
        volume_down_button_(VOLUME_DOWN_BUTTON_GPIO) {

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

        // 定时状态上报（90 秒间隔）
        StartStatusTimer();

        // 初始化音频编解码器
        GetAudioCodec();

        // 灯光控制
        GetBacklight()->RestoreBrightness();

        // 音量初始化：开机检查并修正音量范围（50-100）
        Settings audio_settings("audio", true);
        int DEFAULT_VOLUME = 80;
        int original_volume = audio_settings.GetInt("output_volume", DEFAULT_VOLUME);
        if (original_volume < 50) {
            audio_settings.SetInt("output_volume", DEFAULT_VOLUME);
            ESP_LOGI(TAG, "检测到音量%d小于50，自动调整为%d", original_volume, DEFAULT_VOLUME);
        }

        // WiFi配网模式初始化：确保默认使用蓝牙配网
        Settings wifi_settings("wifi", true);
        wifi_settings.SetInt("blufi", 1);

        ESP_LOGI(TAG, "MyDazy P30 WiFi 初始化完成 (ES8311+ES7210, 纯WiFi, 电源管理, 触摸屏)");

        // 首次开机欢迎音
        if (first_boot_) {
            TaskHandle_t temp_handle = nullptr;
            xTaskCreatePinnedToCore([](void* arg) {
                auto self = static_cast<MyDazyP30_WifiBoard*>(arg);
                auto& app = Application::GetInstance();
                vTaskDelay(pdMS_TO_TICKS(1500));

                if (app.GetDeviceState() == kDeviceStateWifiConfiguring) {
                    self->welcome_task_handle_ = nullptr;
                    vTaskDelete(NULL);
                    return;
                }

                Settings audio_settings("audio", false);
                int enabled = audio_settings.GetInt("playWelcome", 1);
                if (self->first_boot_ && enabled) {
                    app.PlaySound(Lang::Sounds::OGG_WELCOME);
                    vTaskDelay(pdMS_TO_TICKS(3000));
                    if (app.GetDeviceState() == kDeviceStateIdle) {
                        app.ToggleChatState();
                    }
                }

                self->welcome_task_handle_ = nullptr;
                vTaskDelete(NULL);
            }, "welcome_init", 3072, this, 3, &temp_handle, 1);
            welcome_task_handle_ = temp_handle;
        }
    }

    ~MyDazyP30_WifiBoard() {
        if (status_timer_) {
            esp_timer_stop(status_timer_);
            esp_timer_delete(status_timer_);
            status_timer_ = nullptr;
        }
        vol_up_running_ = false;
        vol_down_running_ = false;
        if (vol_up_task_) { vTaskDelete(vol_up_task_); vol_up_task_ = NULL; }
        if (vol_down_task_) { vTaskDelete(vol_down_task_); vol_down_task_ = NULL; }

        if (sc7a20h_sensor_) { delete sc7a20h_sensor_; sc7a20h_sensor_ = nullptr; }
        if (audio_codec_) { delete audio_codec_; audio_codec_ = nullptr; }
        if (power_manager_) { delete power_manager_; power_manager_ = nullptr; }
        if (power_save_timer_) { delete power_save_timer_; power_save_timer_ = nullptr; }
        if (display_) { delete display_; display_ = nullptr; }
    }

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

    i2c_master_bus_handle_t GetI2cBus() {
        return i2c_bus_;
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

    // 重启前硬件清理：切 AUDIO_PWR_EN_GPIO (GPIO9) 让 LCD + 音频 CODEC 真正下电复位
    virtual void PrepareForReboot() override {
        ESP_LOGI(TAG, "PrepareForReboot: 关 LDO 复位 LCD/音频");
        if (audio_codec_) {
            audio_codec_->EnableOutput(false);
        }
        auto* bl = GetBacklight();
        if (bl) {
            bl->SetBrightness(0);
        }
        gpio_set_level(AUDIO_PWR_EN_GPIO, 0);
        rtc_gpio_hold_en(AUDIO_PWR_EN_GPIO);
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    void WakeUp() {
        if (power_save_timer_) {
            power_save_timer_->WakeUp();
        }
    }

    void CleanupDisplay() {
        auto backlight = GetBacklight();
        if (backlight) {
            backlight->SetBrightness(0);
        }
        vTaskDelay(pdMS_TO_TICKS(100));

        if (touch_driver_) {
            touch_driver_->Cleanup();
            delete touch_driver_;
            touch_driver_ = nullptr;
        }

        gpio_set_level(AUDIO_PWR_EN_GPIO, 0);
        rtc_gpio_hold_en(AUDIO_PWR_EN_GPIO);

        vTaskDelay(pdMS_TO_TICKS(100));

        if (display_ != nullptr) {
            delete display_;
            display_ = nullptr;
        }
    }
};

DECLARE_BOARD(MyDazyP30_WifiBoard);
