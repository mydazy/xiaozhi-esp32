#include "dual_network_board.h"
#include "ml307_board.h"
#include "wifi_board.h"
#include "assets/lang_config.h"
#include "codecs/es7111_audio_codec.h"
#include "display/display.h"
#include "display/emote_display.h"
#include "display/lcd_display.h"
#include "lcd_driver_factory.h"
#include "esp_lcd_jd9853.h"
#include "axs5106l_touch.h"
#include "sc7a20h.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "settings.h"
#include "system_info.h"
#include "system_reset.h"   // 添加恢复出厂设置支持
#include "power_save_timer.h"
#include "power_manager.h"
#include "mcp_server.h"
#include "rtc_wake_stub.h"
#include "ota.h"
#include "assets.h"
#include "esp_nfc_ws1850s.h"
#include "typec_headset.h"
#include "ibeacon.h"
#include "ml307_gnss.h"
#include <font_awesome.h>
#include <sys/stat.h>       // 文件属性头文件
#include <sys/unistd.h>
#include <dirent.h>         // 目录操作头文件
#include "mbedtls/md5.h"    // MD5计算库
#include "esp_partition.h"
//#include "alarm_clock.h"

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

#define TAG "MyDazyP31Board"

// 耳机检测全局状态（audio_service.cc 访问）
bool headset_present = false;

// ADC 校准辅助
static bool AdcCalibrationInit(adc_unit_t unit, adc_channel_t channel, adc_atten_t atten, adc_cali_handle_t* out_handle) {
    adc_cali_handle_t handle = NULL;
    bool calibrated = false;
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cali_config = {
        .unit_id = unit, .chan = channel, .atten = atten, .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali_config, &handle) == ESP_OK) calibrated = true;
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_line_fitting_config_t cali_config = {
        .unit_id = unit, .atten = atten, .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_line_fitting(&cali_config, &handle) == ESP_OK) calibrated = true;
#endif
    *out_handle = handle;
    return calibrated;
}

static bool ReadAdcMv(adc_oneshot_unit_handle_t handle, adc_cali_handle_t cali_handle, bool calibrated, adc_channel_t channel, int& out_mv) {
    int raw = 0;
    if (adc_oneshot_read(handle, channel, &raw) != ESP_OK) return false;
    if (calibrated && cali_handle && adc_cali_raw_to_voltage(cali_handle, raw, &out_mv) == ESP_OK) return true;
    out_mv = raw * 3300 / 4095;
    return true;
}


class MyDazyP31Board : public DualNetworkBoard {
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

    bool first_boot_ = false;
    // 闹钟功能
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

    // 欢迎音异步任务句柄（使用 volatile 保证可见性，FreeRTOS API 不支持 std::atomic）
    volatile TaskHandle_t welcome_task_handle_ = nullptr;

    // 恢复出厂设置确认状态
    volatile bool waiting_factory_reset_confirm_ = false;
    uint64_t factory_reset_request_time_ = 0;  // 恢复出厂设置请求时间（用于超时检测）

    void InitializeI2c() {
        // Initialize I2C peripheral for audio codec
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
            .max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t), // 284×48×2=27.3KB
            .flags = SPICOMMON_BUSFLAG_MASTER,
        };
        ESP_ERROR_CHECK(spi_bus_initialize(DISPLAY_SPI_HOST, &buscfg, SPI_DMA_DISABLED));
    }

    void InitializeGpio() {
        // 【修复1】先关闭背光，防止重启后显示随机GRAM数据
        gpio_reset_pin(DISPLAY_BACKLIGHT);
        gpio_set_direction(DISPLAY_BACKLIGHT, GPIO_MODE_OUTPUT);
        gpio_set_level(DISPLAY_BACKLIGHT, 0);  // 背光关闭

        //ADD YZT PA_EN SET HIGH
        gpio_reset_pin(GPIO_NUM_10);
        gpio_set_direction(GPIO_NUM_10, GPIO_MODE_OUTPUT);
        gpio_set_level(GPIO_NUM_10, 1);  // PA_EN置高


        // 1. 配置音频电源GPIO为输出模式（触摸屏复位引脚由esp_lcd驱动管理，不在此配置）
        gpio_config_t output_conf = {
            .pin_bit_mask = (1ULL << AUDIO_PWR_EN_GPIO),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&output_conf);
        // 解除深睡保持，避免电平被保持导致配置失效
        rtc_gpio_hold_dis(AUDIO_PWR_EN_GPIO);

        // 2. 设置音频电源GPIO为高电平（软启动，减少电源冲击）
        gpio_set_level(AUDIO_PWR_EN_GPIO, 0); // 先确保关闭
        vTaskDelay(pdMS_TO_TICKS(10)); // 短暂延时10ms
        gpio_set_level(AUDIO_PWR_EN_GPIO, 1); // 启用音频电源
        ESP_LOGI(TAG, "音频电源已启用 (GPIO%d)", AUDIO_PWR_EN_GPIO);

        // 音频芯片上电稳定时间 200ms（ES8311/ES7210 datasheet 建议）
        vTaskDelay(pdMS_TO_TICKS(200));
        ESP_LOGI(TAG, "音频电源稳定，I2C通信就绪");

        //配置输入GPIO - 按钮需要上拉电阻
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
                first_boot_ = true;  // 开机键唤醒也算首次启动，播放欢迎音
                break;
            case ESP_SLEEP_WAKEUP_EXT1:
                ESP_LOGI(TAG, "从陀螺仪唤醒");
                first_boot_ = true;
                break;
            case ESP_SLEEP_WAKEUP_TIMER:
                ESP_LOGI(TAG, "从定时器唤醒");
                is_alarm_clock_ = true;
                // 预留：闹钟功能
                break;
            case ESP_SLEEP_WAKEUP_ULP:
                ESP_LOGI(TAG, "从 ULP 唤醒");
                break;
            default:
                ESP_LOGI(TAG, "首次启动或复位 (原因=%u)", wakeup_reason);
                first_boot_ = true;
                break;
        }
    }



    void InitializeSc7a20h() {
        // 初始化SC7A20H传感器
        sc7a20h_sensor_ = new Sc7a20h(i2c_bus_, 0x19); // 默认I2C地址

        if (sc7a20h_sensor_->Initialize()) {
            ESP_LOGI(TAG, "SC7A20H传感器初始化成功");

            // 设置唤醒回调，添加防抖处理
            sc7a20h_sensor_->SetWakeupCallback([this]() {
                static uint64_t last_wakeup_time = 0;
                uint64_t current_time = esp_timer_get_time();

                // 防抖：500ms内只允许一次唤醒
                if (current_time - last_wakeup_time > 500000) {
                    last_wakeup_time = current_time;
                    WakeUp();
                    ESP_LOGI(TAG, "SC7A20H触发设备唤醒");
                }
            });

            // 启用运动检测，设置更严格的阈值
            sc7a20h_sensor_->SetMotionDetection(true);
            sc7a20h_initialized_ = true;
            ESP_LOGI(TAG, "SC7A20H传感器初始化完成，已启用防抖处理和运动检测");
        } else {
            ESP_LOGE(TAG, "SC7A20H传感器初始化失败");
            delete sc7a20h_sensor_;
            sc7a20h_sensor_ = nullptr;
            sc7a20h_initialized_ = false;
        }
    }


    void PrepareTouchHardware() {
        // 共享 LCD/Touch 复位线时，必须先完成触摸芯片的硬件复位和固件检查，
        // 再初始化 LCD，避免触摸再次拉低复位脚把已经点亮的 LCD 一起复位。
        // 这里故意只做触摸芯片 bring-up，不注册 LVGL 输入设备。
        touch_driver_ = new Axs5106lTouch(
            i2c_bus_,
            TOUCH_RST_NUM,
            TOUCH_INT_NUM,
            DISPLAY_WIDTH,
            DISPLAY_HEIGHT,
            DISPLAY_SWAP_XY,
            DISPLAY_MIRROR_X,
            DISPLAY_MIRROR_Y
        );

        if (!touch_driver_->InitializeHardware()) {
            ESP_LOGE(TAG, "触摸屏硬件初始化失败");
            delete touch_driver_;
            touch_driver_ = nullptr;
        }
    }

    void InitializeTouch() {
        if (touch_driver_ == nullptr) {
            return;
        }

        // 到这里 LCD 和 LVGL 都已经准备好，才能安全注册触摸输入设备。
        if (!touch_driver_->InitializeInput()) {
            ESP_LOGE(TAG, "触摸屏输入初始化失败");
            delete touch_driver_;
            touch_driver_ = nullptr;
            return;
        }

        // 设置触摸回调（唤醒设备）
        touch_driver_->SetTouchCallback([this]() {
            WakeUp();
        });

        // 设置手势回调：单击唤醒/打断/退出对话，滑动调节音量
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
                        // 空闲状态：单击唤醒并开始对话
                        ESP_LOGI(TAG, "单击唤醒对话");
                        app.PlaySound(Lang::Sounds::OGG_WAKEUP);
                        vTaskDelay(pdMS_TO_TICKS(800));
                        app.ToggleChatState();
                    }  else if (state == kDeviceStateSpeaking) {
                        // 播放状态：单击打断TTS
                        ESP_LOGI(TAG, "单击打断TTS");
                        app.AbortSpeaking(kAbortReasonNone);
                    }
                    break;
                }
                default:
                    break;
            }
        });

        ESP_LOGI(TAG, "✅ 触摸屏初始化完成（使用新驱动，支持手势识别，抗干扰能力更强）");
    }

    void InitializePowerManager() {
        power_manager_ = new PowerManager(POWER_MANAGER_GPIO);
        power_manager_->OnChargingStatusChanged([this](bool is_charging) {
            Settings settings("status", false);
            int deep_sleep_enabled = settings.GetInt("deepSleep", 1);
            if (deep_sleep_enabled) {
                power_save_timer_->SetEnabled(true);
                ESP_LOGI(TAG, "省电定时器已启用（充电状态=%d, deepSleep=%d）", is_charging, deep_sleep_enabled);
            } else {
                power_save_timer_->SetEnabled(false);
                ESP_LOGI(TAG, "省电定时器已禁用（deepSleep=%d）", deep_sleep_enabled);
            }
        });

        power_manager_->OnLowBatteryStatusChanged([this](bool is_low_battery) {
            if (is_low_battery) {
                auto& app = Application::GetInstance();
                app.Alert("电量不足", "请充电", "", Lang::Sounds::OGG_CHARGE);
            }
        });
        vTaskDelay(pdMS_TO_TICKS(200));

        // 检查电量是否过低
        int battery_level = power_manager_->GetBatteryLevel();
        bool is_charging = power_manager_->IsCharging();
        ESP_LOGI(TAG, "电池监控器初始化完成，当前电量: %d%%, 充电状态: %s", battery_level, is_charging ? "充电中" : "未充电");

        // 优化：延迟低电关机到初始化完成后，避免用户无法充电
        if(power_manager_->IsOffBatteryLevel() && battery_level > 0 && !is_charging){
            ESP_LOGE(TAG, "电量过低，强制关机");
            auto& app = Application::GetInstance();
            app.Alert("电量过低", "强制关机", "", Lang::Sounds::OGG_LOW_BATTERY);
            vTaskDelay(pdMS_TO_TICKS(3000));
            EnterDeepSleep(false);
        }
    }

    void InitializePowerSaveTimer() {
        // 检查是否启用深度睡眠功能
        Settings settings("status", false);
        int deep_sleep_enabled = settings.GetInt("deepSleep", 1);
        ESP_LOGI(TAG, "✅ 深度睡眠配置：%s（deepSleep=%d）", deep_sleep_enabled ? "已启用" : "已禁用", deep_sleep_enabled);

        // 睡眠模式：CPU 动态 40-80MHz（✅ 保留唤醒词检测和麦克风输入，仅降频+降低屏幕亮度，最低功耗）
        power_save_timer_ = new PowerSaveTimer(120, 60, 300);
        // power_save_timer_ = new PowerSaveTimer(120, 3, 5); // hsf 测试灵敏度
        power_save_timer_->OnEnterSleepMode([this]() {
            ESP_LOGI(TAG, "进入省电模式（降频+降亮度）");
            GetBacklight()->SetBrightness(15);
        });
        power_save_timer_->OnExitSleepMode([this]() {
            ESP_LOGI(TAG, "退出省电模式（恢复性能）");
            GetBacklight()->RestoreBrightness();
        });

        power_save_timer_->OnShutdownRequest([this]() {
            Settings settings("status", false);
            int deep_sleep_enabled = settings.GetInt("deepSleep", 1);
            if (deep_sleep_enabled) {
                ESP_LOGI(TAG, "✅ 深度睡眠已启用，5分钟无操作后进入深度睡眠");
                EnterDeepSleep(true);
            }
        });

        // 根据deepSleep配置决定是否启用定时器
        if (deep_sleep_enabled) {
            power_save_timer_->SetEnabled(true);
            ESP_LOGI(TAG, "省电定时器已启用（60秒后降频，300秒后深度睡眠）");
        } else {
            power_save_timer_->SetEnabled(false);
            ESP_LOGI(TAG, "省电定时器已禁用（深度睡眠功能关闭）");
        }
    }

    void EnterDeepSleep(bool enable_gyro_wakeup = true) {
        ESP_LOGI(TAG, "====== 开始进入深度睡眠流程 ======");

        // 闹钟定时唤醒功能
//        int intervalue = alarm_clock_get_next_task();
//        if(intervalue > 0){
//            esp_sleep_enable_timer_wakeup((uint64_t) intervalue * 1000 * 1000);
//            ESP_LOGI(TAG, "设置定时器唤醒: %d秒", intervalue);
//        }

        // 2. 先关闭触摸屏，避免I2C错误
        if (touch_driver_) {
            touch_driver_->Cleanup();
            delete touch_driver_;
            touch_driver_ = nullptr;
            vTaskDelay(pdMS_TO_TICKS(100)); // 等待触摸屏完全关闭
        }

        ESP_LOGI(TAG, "关闭音频电源");
        gpio_set_level(AUDIO_PWR_EN_GPIO, 0);
        rtc_gpio_hold_en(AUDIO_PWR_EN_GPIO);

        // hsf
        // 按键2（中间）唤醒设备
        ESP_ERROR_CHECK(esp_sleep_enable_ext0_wakeup(BOOT_BUTTON_GPIO, 0));
        ESP_ERROR_CHECK(rtc_gpio_pullup_en(BOOT_BUTTON_GPIO));
        ESP_ERROR_CHECK(rtc_gpio_pulldown_dis(BOOT_BUTTON_GPIO));
        ESP_LOGI(TAG, "BOOT wake source configured, GPIO%d", BOOT_BUTTON_GPIO);



        vTaskDelay(pdMS_TO_TICKS(200)); // 等待音频芯片完全断电

        // 3. 陀螺仪唤醒配置（仅在自动休眠时启用）
        if (enable_gyro_wakeup) {
            Settings settings("status", false);
            int32_t pickup_wake = settings.GetInt("pickupWake", 1); // 默认启用拿起唤醒（陀螺仪唤醒）
            if(pickup_wake && sc7a20h_initialized_){
                ESP_ERROR_CHECK(esp_sleep_enable_ext1_wakeup_io((1ULL << SC7A20H_GPIO_INT1), ESP_EXT1_WAKEUP_ANY_LOW));
                ESP_ERROR_CHECK(rtc_gpio_pullup_en(SC7A20H_GPIO_INT1));
                ESP_ERROR_CHECK(rtc_gpio_pulldown_dis(SC7A20H_GPIO_INT1));
                ESP_LOGI(TAG, "SC7A20H唤醒源配置完成，GPIO%d", SC7A20H_GPIO_INT1);
            }
        }

        // 5. 处理WiFi连接（WiFi版本固定断开WiFi）
        if (GetNetworkType() == NetworkType::WIFI) {
            WifiManager::GetInstance().StopStation();
        }

        // 6. 停止电源管理定时器
        if (power_save_timer_) {
            power_save_timer_->SetEnabled(false);
            ESP_LOGI(TAG, "电源管理定时器已停止");
        }

        // 6. 关闭背光
        ESP_LOGI(TAG, "[3/8] 关闭显示背光");
        gpio_set_level(DISPLAY_BACKLIGHT, 0);

        // 8. 重置音频相关GPIO
        ESP_LOGI(TAG, "重置音频GPIO");
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

        // 配置GPIO为输入模式
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
        esp_err_t gpio_err = gpio_config(&input_conf);
        if (gpio_err != ESP_OK) {
            ESP_LOGE(TAG, "GPIO配置失败: %s", esp_err_to_name(gpio_err));
        }

        ESP_LOGI(TAG, "[6/8] 音频和显示系统已完全关闭");

        // 9. 设置 Wake Stub
        esp_set_deep_sleep_wake_stub(&wake_stub);
        // 10. 最终等待，确保所有操作完成
        ESP_LOGI(TAG, "准备进入深度睡眠");
        vTaskDelay(pdMS_TO_TICKS(200)); // 确保所有日志输出完成

        // 进入深度休眠
        esp_deep_sleep_start();
    }

    void InitializeDisplay() {
        esp_lcd_panel_io_handle_t panel_io_ = nullptr;
        esp_lcd_panel_handle_t panel_ = nullptr;

        display_panel_spi_params_t params = {
            .host = DISPLAY_SPI_HOST,
            .cs_gpio_num = DISPLAY_LCD_CS,
            .dc_gpio_num = DISPLAY_LCD_DC,
            .reset_gpio_num = DISPLAY_LCD_RESET,    
            .pclk_hz = (50 * 1000 * 1000),
            .trans_queue_depth = 10,
            .lcd_cmd_bits = 8,
            .lcd_param_bits = 8,
            .width = DISPLAY_WIDTH,
            .height = DISPLAY_HEIGHT,
            .offset_x = DISPLAY_OFFSET_X,
            .offset_y = DISPLAY_OFFSET_Y,
            .swap_xy = DISPLAY_SWAP_XY,
            .mirror_x = DISPLAY_MIRROR_X,
            .mirror_y = DISPLAY_MIRROR_Y,
            .invert_color = DISPLAY_INVERT_COLOR,
        };
        display_panel_result_t result = {};
        ESP_ERROR_CHECK(display_panel_create_jd9853(&params, &result));
        // 保存句柄用于动态切换
        panel_io_ = result.io_handle;
        panel_ = result.panel_handle;

#if CONFIG_USE_EMOTE_MESSAGE_STYLE
        display_ = new emote::EmoteDisplay(panel_, panel_io_, DISPLAY_WIDTH, DISPLAY_HEIGHT);
        ESP_LOGI(TAG, "表情包显示模式已启用");
#else
        display_ = new SpiLcdDisplay(panel_io_, panel_,
                                     DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
                                     DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
        ESP_LOGI(TAG, "LVGL显示模式已启用");
#endif

        // 关键节点：显示初始化完成后打印内存
        SystemInfo::PrintHeapStats();
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            auto status = app.GetDeviceState();
            ESP_LOGI(TAG, "单击 button 状态： %u", status);
            waiting_factory_reset_confirm_ = false; // 重置确认状态
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

        // 双击：确认恢复出厂设置或切换聊天模式
        boot_button_.OnDoubleClick([this]() {
            auto& app = Application::GetInstance();
            auto status = app.GetDeviceState();
            ESP_LOGI(TAG, "双击 button 状态： %u", status);

            if (waiting_factory_reset_confirm_) {
                // 检查是否超时（10秒超时）
                uint64_t now = esp_timer_get_time();
                if (now - factory_reset_request_time_ > 15000000) {  // 10秒 = 10000000微秒
                    ESP_LOGW(TAG, "恢复出厂设置确认超时，已自动取消");
                    waiting_factory_reset_confirm_ = false;
                    return;
                }

                ESP_LOGI(TAG, "双击确认：执行恢复出厂设置");
                waiting_factory_reset_confirm_ = false;  // 重置确认状态
                if (status == kDeviceStateSpeaking) {
                    app.AbortSpeaking(kAbortReasonNone);
                }
                app.Alert("确认恢复", "开始执行", "logo", Lang::Sounds::OGG_START_RESET);
                vTaskDelay(pdMS_TO_TICKS(3000));
                // NVS 清除 + 重启实现恢复出厂
                nvs_flash_erase();
                nvs_flash_init();
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
                } else{
                    app.SetAecMode(kAecOff);
                }
                WakeUp();
            }
            #endif
        });

        // 连按3次进入配网模式
        boot_button_.OnMultipleClick([this]() {
            ESP_LOGI(TAG, "连按3次进入配网模式");
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() != kDeviceStateWifiConfiguring){
                if (app.GetDeviceState() == kDeviceStateSpeaking) {
                    app.AbortSpeaking(kAbortReasonNone);
                }
                if (GetNetworkType() == NetworkType::ML307) {
                    ESP_LOGI(TAG, "当前是4G模式,切换到WiFi板卡");
                    app.Alert(Lang::Strings::WIFI_CONFIG_MODE, "切换到WiFi", "logo", Lang::Sounds::OGG_NETWORK_WIFI);
                    vTaskDelay(pdMS_TO_TICKS(1500));
                    SwitchNetworkType();
                } else {
                    // WiFi模式下，进入配网模式
                    app.Schedule([this]() {
                        auto& wifi_board = static_cast<WifiBoard&>(GetCurrentBoard());
                        wifi_board.EnterWifiConfigMode();
                    });
                }
            } else {
                app.Alert(Lang::Strings::WIFI_CONFIG_MODE, "切换到4G", "logo", Lang::Sounds::OGG_NETWORK_4G);
                vTaskDelay(pdMS_TO_TICKS(1500));
                SwitchNetworkType();
            }
        }, 3);

        // 连按4次关机
        boot_button_.OnMultipleClick([this]() {
            ESP_LOGI(TAG, "连按4次关机");
            auto& app = Application::GetInstance();
            if(app.GetDeviceState() == kDeviceStateSpeaking){
                app.AbortSpeaking(kAbortReasonNone);
            }
            app.Alert("连按4次关机", "拜拜^-^", "", Lang::Sounds::OGG_SHUTDOWN);
            vTaskDelay(pdMS_TO_TICKS(3000));

            // 进入休眠（按键关机禁用陀螺仪唤醒）
            EnterDeepSleep(false);
        }, 4);

        // 连按6次：进入音频测试模式（任意状态下都可用）
        boot_button_.OnMultipleClick([this]() {
            auto& app = Application::GetInstance();
            auto status = app.GetDeviceState();
            ESP_LOGI(TAG, "连按6次：进入音频测试模式（当前状态：%u）", status);
            // 如果正在播放，先中断
            if (status == kDeviceStateSpeaking) {
                app.AbortSpeaking(kAbortReasonNone);
            }

            // 切换到配网模式（允许测试录音）
            app.SetDeviceState(kDeviceStateWifiConfiguring);
            app.StartListening();

            // 播放提示音并语音提示
            app.Alert("音频测试", "", "", Lang::Sounds::OGG_AUDIO_TEST);
            vTaskDelay(pdMS_TO_TICKS(3000));
            app.SetDeviceState(kDeviceStateAudioTesting);
            app.StopListening();

            ESP_LOGI(TAG, "联按6次进入音频测试模式");
        }, 6);

        // 连按9次：进入恢复出厂设置确认状态
        boot_button_.OnMultipleClick([this]() {
            ESP_LOGI(TAG, "连按9次：进入恢复出厂设置确认状态");
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateSpeaking) {
                app.AbortSpeaking(kAbortReasonNone);
            }
            // 设置等待确认状态
            waiting_factory_reset_confirm_ = true;
            factory_reset_request_time_ = esp_timer_get_time();

            // 播放警告提示音并提示用户双击确认
            app.Alert("恢复出厂设置", "10秒内双击确认", "logo", Lang::Sounds::OGG_FACTORY_RESET);
            ESP_LOGI(TAG, "等待双击确认恢复出厂设置（10秒超时）...");
        }, 9);

        // 长按按钮处理
        boot_button_.OnLongPress([this]() {
            auto& app = Application::GetInstance();
            auto status = app.GetDeviceState();
            if (status == kDeviceStateIdle || status == kDeviceStateSpeaking || status == kDeviceStateListening) {
                ESP_LOGI(TAG, "✅ 按键对话：开始倾听（统一处理）");
                if(status != kDeviceStateListening){
                    app.StartListening();
                }
                is_recording_for_send_ = true;
            }
            // 配网/启动模式：录音测试
            else if (status == kDeviceStateStarting || status == kDeviceStateActivating || status == kDeviceStateWifiConfiguring) {
                ESP_LOGI(TAG, "配网模式：开始录音测试");
                is_recording_for_test_ = true;
                app.SetDeviceState(kDeviceStateWifiConfiguring);
                vTaskDelay(pdMS_TO_TICKS(100));
                app.StartListening();
            }
        });

        // 按钮松开处理
        boot_button_.OnPressUp([this]() {
            auto& app = Application::GetInstance();
            if (is_recording_for_test_) {
                is_recording_for_test_ = false;
                ESP_LOGI(TAG, "配网模式：停止录音，开始播放");
                app.SetDeviceState(kDeviceStateAudioTesting);
                app.StopListening();
            } else if (is_recording_for_send_) {
                is_recording_for_send_ = false;
                ESP_LOGI(TAG, "聊天模式：发送音频");
                app.StopListening();
                vTaskDelay(pdMS_TO_TICKS(100));
                app.PlaySound(Lang::Sounds::OGG_POPUP);
            }
        });

        // 音量按钮点击处理
        volume_up_button_.OnClick([this]() { AdjustVolume(+10); });
        volume_down_button_.OnClick([this]() { AdjustVolume(-10); });

        // 音量长按：连续调节（每100ms步进5）
        volume_up_button_.OnLongPress([this]() {
            StartVolumeTask(+5, &vol_up_task_, &vol_up_running_);
        });
        volume_up_button_.OnPressUp([this]() { vol_up_running_ = false; });

        volume_down_button_.OnLongPress([this]() {
            StartVolumeTask(-5, &vol_down_task_, &vol_down_running_);
        });
        volume_down_button_.OnPressUp([this]() { vol_down_running_ = false; });
    }

    // NFC 读写器
    NfcWs1850s* nfc_ = nullptr;

    // GNSS 定位
    Ml307Gnss* gnss_ = nullptr;

    // Type-C 耳机检测
    TypecHeadset* headset_ = nullptr;
    std::atomic<int> gnss_satellites_{0};
    std::atomic<bool> gnss_fixed_{false};
    double gnss_lat_ = 0.0, gnss_lon_ = 0.0;

    static const char* NfcTypeName(NfcCardType type) {
        switch (type) {
            case NfcCardType::kMifareClassic1K: return "M1-1K";
            case NfcCardType::kMifareClassic4K: return "M1-4K";
            case NfcCardType::kUltralight:      return "NTAG";
            case NfcCardType::kMifarePlus:      return "Plus";
            case NfcCardType::kIso14443A4:      return "CPU";
            default:                            return "NFC";
        }
    }

    // NFC → POST OTA_URL/switch {"type":"nfc","uid":"...","card_type":"..."}
    void NfcRequestSwitch(NfcCardType type, const NfcUid& uid) {
        auto uid_str = uid.ToString();
        auto type_name = std::string(NfcTypeName(type));
        Application::GetInstance().Schedule([uid_str, type_name]() {
            cJSON* p = cJSON_CreateObject();
            cJSON_AddStringToObject(p, "uid", uid_str.c_str());
            cJSON_AddStringToObject(p, "card_type", type_name.c_str());
            Ota::RequestSwitch("nfc", p);
        });
    }

    // iBeacon → POST OTA_URL/switch {"type":"ibeacon","uuid":"...","major":1,"minor":2,"rssi":-60}
    void IBeaconRequestSwitch(const IBeaconInfo& beacon) {
        auto uuid = beacon.uuid;
        uint16_t major = beacon.major, minor = beacon.minor;
        int8_t rssi = beacon.rssi;
        Application::GetInstance().Schedule([uuid, major, minor, rssi]() {
            cJSON* p = cJSON_CreateObject();
            cJSON_AddStringToObject(p, "uuid", uuid.c_str());
            cJSON_AddNumberToObject(p, "major", major);
            cJSON_AddNumberToObject(p, "minor", minor);
            cJSON_AddNumberToObject(p, "rssi", rssi);
            Ota::RequestSwitch("ibeacon", p);
        });
    }

    // 状态定时上报定时器
    esp_timer_handle_t status_timer_ = nullptr;

    // 上报设备状态 → POST OTA_URL/status
    void ReportStatus() {
        int battery = power_manager_ ? power_manager_->GetBatteryLevel() : -1;
        bool charging = power_manager_ ? power_manager_->IsCharging() : false;
        bool fixed = gnss_fixed_.load();
        int sats = gnss_satellites_.load();
        double lat = gnss_lat_, lon = gnss_lon_;
        size_t free_heap = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);

        // 网络信息
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
            [battery, charging, fixed, sats, lat, lon, free_heap, csq, carrier]() {
            cJSON* p = cJSON_CreateObject();

            cJSON_AddNumberToObject(p, "battery", battery);
            cJSON_AddBoolToObject(p, "charging", charging);

            cJSON* gps = cJSON_CreateObject();
            cJSON_AddBoolToObject(gps, "fixed", fixed);
            cJSON_AddNumberToObject(gps, "satellites", sats);
            if (fixed) {
                cJSON_AddNumberToObject(gps, "latitude", lat);
                cJSON_AddNumberToObject(gps, "longitude", lon);
            }
            cJSON_AddItemToObject(p, "gps", gps);

            cJSON* net = cJSON_CreateObject();
            cJSON_AddStringToObject(net, "type", "cellular");
            if (!carrier.empty()) cJSON_AddStringToObject(net, "carrier", carrier.c_str());
            if (csq >= 0) cJSON_AddNumberToObject(net, "csq", csq);
            cJSON_AddItemToObject(p, "network", net);

            cJSON_AddNumberToObject(p, "free_heap", (double)free_heap);

            Ota::ReportStatus(p);
        });
    }

    // 启动 90 秒定时状态上报
    void StartStatusTimer() {
        esp_timer_create_args_t args = {
            .callback = [](void* arg) {
                static_cast<MyDazyP31Board*>(arg)->ReportStatus();
            },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "status_report",
        };
        esp_timer_create(&args, &status_timer_);
        esp_timer_start_periodic(status_timer_, 90 * 1000000ULL);
    }

    void InitializeNfc() {
        if (!i2c_bus_) return;

        nfc_ = new NfcWs1850s(i2c_bus_);
        if (nfc_->Initialize() != ESP_OK) {
            delete nfc_;
            nfc_ = nullptr;
            return;
        }

        nfc_->SetCardCallback([this](NfcCardType type, const NfcUid& uid) {
            auto display = GetDisplay();
            if (display) {
                char text[64];
                snprintf(text, sizeof(text), "%s: %s", NfcTypeName(type), uid.ToString().c_str());
                display->SetChatMessage("system", text);
            }

            // NFC 刷卡 → HTTP POST 到 OTA 地址，携带 /switch + UID
            NfcRequestSwitch(type, uid);
        });

        nfc_->StartDetection(300);
    }

    // GPS 初始化（4G 网络连接后调用）
    void InitializeHeadset() {
        TypecHeadsetConfig hcfg = {
            .usb_det_pin = USB_DET_GPIO,
            .cc_adc_pin = CC_ADC_PIN,
            .usb_sw_pin = USB_SW_GPIO,
            .cc_vdd_pin = CC_VDD_GPIO,
            .mic_select_pin = MIC_SELECT_GPIO,
            .pa_pin = AUDIO_CODEC_PA_PIN,
            .usb_mic_adc_pin = USB_MIC_ADC_GPIO,
            .cc_adc_unit = CC_ADC_UNIT,
            .cc_adc_channel = CC_ADC_CHANNEL,
            .mic_adc_unit = USB_MIC_ADC_UNIT,
            .mic_adc_channel = USB_MIC_ADC_CHANNEL,
            .cc_headset_mv = CC_ADC_HEADSET_MV,
        };
        headset_ = new TypecHeadset(hcfg);

        headset_->SetCallback([this](bool inserted) {
            // 切换 codec 耳机增益模式
            auto* codec = dynamic_cast<Es7111AudioCodec*>(GetAudioCodec());
            if (codec) codec->SetHeadsetMode(inserted);

            auto display = GetDisplay();
            if (display) {
                display->ShowNotification(inserted ? "耳机已插入" : "耳机已拔出", 3000);
            }
            WakeUp();
        });

        // 共享 PowerManager 的 ADC handle（同一个 ADC1）
        headset_->Start(PowerManager::GetSharedAdcHandle());
    }

    void StartGnss() {
        if (GetNetworkType() != NetworkType::ML307) return;

        auto& ml307_board = dynamic_cast<Ml307Board&>(GetCurrentBoard());
        auto* modem = ml307_board.GetModem();
        if (!modem) {
            ESP_LOGW(TAG, "GNSS: modem not ready");
            return;
        }

        gnss_ = new Ml307Gnss(modem->GetAtUart());

        // 搜星回调 → 更新状态栏
        gnss_->SetSatCallback([this](int sats) {
            gnss_satellites_ = sats;
            auto display = GetDisplay();
            if (display) {
                char text[32];
                snprintf(text, sizeof(text), FONT_AWESOME_LOCATION_DOT " %d", sats);
                display->SetStatus(text);
            }
        });

        // 定位成功回调
        gnss_->SetFixCallback([this](const GnssFix& fix) {
            gnss_fixed_ = fix.valid;
            gnss_satellites_ = fix.satellites;
            gnss_lat_ = fix.latitude;
            gnss_lon_ = fix.longitude;
            auto display = GetDisplay();
            if (display) {
                char text[48];
                snprintf(text, sizeof(text), FONT_AWESOME_LOCATION_DOT " %d  %.4f,%.4f",
                         fix.satellites, fix.latitude, fix.longitude);
                display->SetStatus(text);
            }
            // 定位成功时上报状态
            if (fix.valid) ReportStatus();
        });

        gnss_->Start(kGnssGps | kGnssBds);
        ESP_LOGI(TAG, "GNSS started (GPS+BDS)");
    }

    // iBeacon 扫描器
    void InitializeIBeacon() {
        auto& ibeacon = IBeacon::GetInstance();
        ibeacon.OnDetected([this](const IBeaconInfo& beacon) {
            auto display = GetDisplay();
            if (display) {
                char text[128];
                snprintf(text, sizeof(text), "iBeacon: M=%u m=%u RSSI=%d %.1fm",
                         beacon.major, beacon.minor, beacon.rssi,
                         beacon.CalculateDistance());
                display->ShowNotification(text, 10000);
            }
            IBeaconRequestSwitch(beacon);
        });

        // 延迟启动：等 BLE 协议栈就绪后再开始扫描
        ibeacon.StartDeferred(30000);
    }

    // 音量调节函数（单次调节）
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

    // 启动连续音量调节任务
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
                    static_cast<MyDazyP31Board&>(Board::GetInstance()).WakeUp();
                }
                vTaskDelay(pdMS_TO_TICKS(200)); // 200ms间隔，每秒调节5次
            }

            *task_handle = NULL;
            vTaskDelete(NULL);
        }, "vol_adjust", 2048, new std::tuple<int, volatile bool*, TaskHandle_t*>(delta, running, task_handle), 5, task_handle, 1);
    }

public:
    MyDazyP31Board() :
        DualNetworkBoard(ML307_TX_PIN, ML307_RX_PIN, GPIO_NUM_NC, 1),
        boot_button_(BOOT_BUTTON_GPIO, false, 1500, 400),
        volume_up_button_(VOLUME_UP_BUTTON_GPIO),
        volume_down_button_(VOLUME_DOWN_BUTTON_GPIO) {

        InitializeGpio();           // 1. 先启用音频电源
        InitializeI2c();            // 2. 初始化I2C总线 (音频编解码器需要)
        PrepareTouchHardware();     // 3. 先完成共享复位线上的触摸硬件初始化
        InitializeSpi();            // 4. 初始化SPI总线 (显示需要)
        InitializeDisplay();        // 5. 初始化显示 (依赖SPI)
        InitializeTouch();          // 6. LCD/LVGL 就绪后再注册触摸输入
                                     //    这样触摸初始化不会在显示点亮后再拉共享 RST。
// #if !MYDAZY_TOUCH_I2C_ONLY_TEST 
        InitializeSc7a20h();        // 7. 初始化SC7A20H传感器 (依赖I2C)
// #endif
        InitializePowerManager();  // 8. 初始化电池监控
        InitializePowerSaveTimer(); // 9. 初始化省电定时器
        InitializeButtons();        // 10. 初始化按钮 (最后初始化)
        InitializeNfc();            // 11. 初始化 NFC（WS1850S I2C）
        InitializeHeadset();        // 12. Type-C 耳机动态检测
        // InitializeIBeacon();     // TODO: iBeacon 需等 BLE host sync 后启动，暂禁用

        // 13. GPS 延迟启动（等 modem 检测到即可，不依赖网络注册）
        xTaskCreatePinnedToCore([](void* arg) {
            auto* self = static_cast<MyDazyP31Board*>(arg);
            if (self->GetNetworkType() != NetworkType::ML307) {
                vTaskDelete(NULL);
                return;
            }
            // 等 modem 初始化完成（最多 30 秒）
            for (int i = 0; i < 15; i++) {
                vTaskDelay(pdMS_TO_TICKS(2000));
                auto& ml307 = dynamic_cast<Ml307Board&>(self->GetCurrentBoard());
                if (ml307.GetModem() != nullptr) {
                    self->StartGnss();
                    break;
                }
            }
            vTaskDelete(NULL);
        }, "gnss_init", 3072, this, 2, NULL, 0);

        // 14. 定时状态上报（90 秒间隔）
        StartStatusTimer();

        // 灯光控制
        GetBacklight()->RestoreBrightness();

        // 优化：音量初始化逻辑 - 开机检查并修正音量范围（50-100）
        Settings audio_settings("audio", true);  // 优化：直接以读写模式打开，避免重复打开
        int DEFAULT_VOLUME = 80;
        int original_volume = audio_settings.GetInt("output_volume", DEFAULT_VOLUME);
        if (original_volume < 50) {
            audio_settings.SetInt("output_volume", DEFAULT_VOLUME);
            ESP_LOGI(TAG, "检测到音量%d小于%d，自动调整为%d", original_volume, 50, DEFAULT_VOLUME);
        }

        // WiFi配网模式初始化：确保默认使用蓝牙配网（blufi=1）
        Settings wifi_settings("wifi", true);
        wifi_settings.SetInt("blufi", 1);

        ESP_LOGI(TAG, "MyDazy P30 4G 初始化完成 (支持4G、ULP、电源管理、触摸屏)");

        // 首次开机欢迎音（可开关：Settings("audio").playWelcome 默认开启）
         if (first_boot_) {
            TaskHandle_t temp_handle = nullptr;
            xTaskCreatePinnedToCore([](void* arg){
                auto self = static_cast<MyDazyP31Board*>(arg);
                auto& app = Application::GetInstance();
                vTaskDelay(pdMS_TO_TICKS(1500));

                // 检查是否处于配网模式,如果是则跳过logo和欢迎音
                if (app.GetDeviceState() == kDeviceStateWifiConfiguring) {
                    ESP_LOGI(TAG, "配网模式:跳过开机logo和欢迎音");
                    self->welcome_task_handle_ = nullptr;
                    vTaskDelete(NULL);
                    return;
                }

                // 检查是否启用欢迎音（音频就绪后立即播放，无需等待联网）
                Settings audio_settings("audio", false);
                int enabled = audio_settings.GetInt("playWelcome", 1);
                if (self->first_boot_ && enabled) {
                    ESP_LOGI(TAG, "✅ 播放欢迎音（音频就绪，开机即播）");
                    app.PlaySound(Lang::Sounds::OGG_WELCOME);
                    vTaskDelay(pdMS_TO_TICKS(3000));
                    if (app.GetDeviceState() == kDeviceStateIdle){
                        ESP_LOGI(TAG, "欢迎音播放完成，自动开始对话");
                        app.ToggleChatState();
                    }
                } else {
                    ESP_LOGI(TAG, "跳过欢迎音播放（first_boot=%d, enabled=%d）",
                             self->first_boot_, enabled);
                }

                // 任务完成，清空句柄
                self->welcome_task_handle_ = nullptr;
                vTaskDelete(NULL);
            }, "welcome_init", 3072, this, 3, &temp_handle, 1);
            welcome_task_handle_ = temp_handle;
        }
    }

    ~MyDazyP31Board() {
        // 清理 iBeacon
        IBeacon::GetInstance().Stop();

        // 清理状态上报定时器
        if (status_timer_) {
            esp_timer_stop(status_timer_);
            esp_timer_delete(status_timer_);
            status_timer_ = nullptr;
        }
        // 清理耳机检测
        if (headset_) { headset_->Stop(); delete headset_; headset_ = nullptr; }
        // 清理 GNSS
        if (gnss_) {
            gnss_->Stop();
            delete gnss_;
            gnss_ = nullptr;
        }
        // 清理 NFC
        if (nfc_) {
            nfc_->StopDetection();
            delete nfc_;
            nfc_ = nullptr;
        }
        // 清理音量调节任务
        vol_up_running_ = false;
        vol_down_running_ = false;
        if (vol_up_task_) {
            vTaskDelete(vol_up_task_);
            vol_up_task_ = NULL;
        }
        if (vol_down_task_) {
            vTaskDelete(vol_down_task_);
            vol_down_task_ = NULL;
        }

        // 清理SC7A20H传感器
        if (sc7a20h_sensor_) {
            delete sc7a20h_sensor_;
            sc7a20h_sensor_ = nullptr;
        }

        // 清理其他资源
        if (power_manager_) {
            delete power_manager_;
            power_manager_ = nullptr;
        }

        if (power_save_timer_) {
            delete power_save_timer_;
            power_save_timer_ = nullptr;
        }

        if (display_) {
            delete display_;
            display_ = nullptr;
        }
    }

    virtual AudioCodec* GetAudioCodec() override {
        // ES7111(DAC,纯I2S无I2C) + ES7210(ADC,I2C初始化) 共享 duplex 总线
        static Es7111AudioCodec audio_codec(
            i2c_bus_,
            AUDIO_INPUT_SAMPLE_RATE,
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK,
            AUDIO_I2S_GPIO_BCLK,
            AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT,
            AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN,
            AUDIO_CODEC_ES7210_ADDR,
            AUDIO_INPUT_REFERENCE);
        return &audio_codec;
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
        DualNetworkBoard::SetPowerSaveLevel(level);
        ESP_LOGI(TAG, "SET power save level: %d", static_cast<int>(level));
    }

    // 获取电池电量（兼容原有接口）
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

    // 唤醒设备（内部方法，非虚函数）
    void WakeUp() {
        if (power_save_timer_) {
            power_save_timer_->WakeUp();
        }
    }

    // 清理显示资源（重启前调用）
    void CleanupDisplay() {
        ESP_LOGI(TAG, "====== 清理显示资源（简化版）======");

        // 1. 立即关闭背光，防止用户看到清理过程（避免白屏/雪花屏）
        // 注意：这里先黑屏并不等于已经重启，真正重启要等 CleanupDisplay() 走完并回到 esp_restart()。
        ESP_LOGI(TAG, "关闭背光（避免重启时闪屏）");
        auto backlight = GetBacklight();
        if (backlight) {
            backlight->SetBrightness(0);  // 通过PWM关闭背光
        }
        vTaskDelay(pdMS_TO_TICKS(100));  // 延长等待时间，确保PWM完全关闭

        // 2. 删除触摸驱动。
        // 先删 touch 再删 display，避免 touch cleanup 过程中还去访问已经销毁的 LVGL 对象。
        if (touch_driver_) {
            ESP_LOGI(TAG, "[cleanup] before touch cleanup");
            touch_driver_->Cleanup();
            ESP_LOGI(TAG, "[cleanup] after touch cleanup");
            ESP_LOGI(TAG, "[cleanup] before delete touch");
            delete touch_driver_;
            ESP_LOGI(TAG, "[cleanup] after delete touch");
            touch_driver_ = nullptr;
        }

        // 3. 关闭音频电源
        gpio_set_level(AUDIO_PWR_EN_GPIO, 0);
        rtc_gpio_hold_en(AUDIO_PWR_EN_GPIO);

        // 4. 删除显示对象（析构函数会统一清理 LVGL 和 LCD）
        // 之前“黑屏但没真正重启”的卡点就在这里：背光已经关了，但 delete display_ 没有顺利返回。
        // 关键：在删除前短暂延迟，确保主循环不再访问 UI
        vTaskDelay(pdMS_TO_TICKS(100));

        if (display_ != nullptr) {
            ESP_LOGI(TAG, "[cleanup] before delete display");
            delete display_;
            display_ = nullptr;
            ESP_LOGI(TAG, "[cleanup] after delete display");
            ESP_LOGI(TAG, "显示资源清理完成");
        }
    }

};

DECLARE_BOARD(MyDazyP31Board);
