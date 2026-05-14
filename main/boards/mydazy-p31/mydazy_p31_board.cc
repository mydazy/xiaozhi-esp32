#include "dual_network_board.h"
#include "alarm_manager.h"
#include "audio/alarm_ringer.h"
#include "ml307_board.h"
#include "wifi_board.h"
#include "assets/lang_config.h"
#include "codecs/es7111_audio_codec.h"
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
#include "system_reset.h"   // 添加恢复出厂设置支持
#include "power_save_timer.h"
#include "power_manager.h"
#include "mcp_server.h"
#include "education_mcp_tools.h"
#include "ota.h"
#include "assets.h"
#include "esp_nfc_ws1850s.h"
#include "typec_headset.h"
#include "ibeacon.h"
#include "ml307_gnss.h"
#include "device_state_event.h"
#include <font_awesome.h>
#include <mutex>
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
#include "wifi_station.h"
#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <driver/rtc_io.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>

#define TAG "MyDazyP31Board"

// 距上次 sleep < 500ms + GPIO 低 = 死按延续 · 短路再 sleep
// 用 gettimeofday（POSIX 标准 · ESP-IDF 内部基于 RTC · 跨 deep sleep 持续 · 无组件依赖）
#include <sys/time.h>
RTC_DATA_ATTR static uint64_t s_last_sleep_us = 0;
static constexpr uint64_t kDeadHoldWindowUs = 500 * 1000;
static inline uint64_t NowRtcUs() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return (uint64_t)tv.tv_sec * 1000000ULL + tv.tv_usec;
}

// 耳机检测全局状态（audio_service.cc 访问）
bool headset_present = false;

// 闹钟响铃中：任何用户输入（按键/触摸/摇晃）都优先关停闹钟，不进对话流程
static inline bool TryStopAlarmRinger(const char* reason) {
    if (AlarmRinger::GetInstance().IsRinging()) {
        AlarmRinger::GetInstance().Stop(reason);
        return true;
    }
    return false;
}

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
    i2c_worker_handle_t     i2c_worker_ = nullptr;   /* v4.0 SC7A20H 走串行 worker */
    Button boot_button_;
    Button volume_up_button_;
    Button volume_down_button_;

    // SC7A20H 三轴加速度传感器（v4.0 C 驱动 · 程序生命周期内不释放）
    sc7a20h_handle_t sc7a20h_sensor_ = nullptr;

    // 显示
    Display* display_ = nullptr;
    PowerManager* power_manager_ = nullptr;
    PowerSaveTimer* power_save_timer_ = nullptr;

    // 触摸屏
    Axs5106lTouch* touch_driver_ = nullptr;

    bool first_boot_ = false;
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
            // 4G RF 干扰场景下提高毛刺过滤阈值（~187ns，原值 7≈87ns）
            .glitch_ignore_cnt = 15,
            .intr_priority = 3,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 0,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));

        // v4.0 SC7A20H 驱动只接受 i2c_worker · codec / NFC / touch 仍直接走 i2c_bus_
        // （worker 仅串行化自己 add_device 的从机；与外部直接访问互不锁定，
        //   P31 上 SC7A20H 唯一访问者就是它本身，无并发风险）
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
            .max_transfer_sz = DISPLAY_WIDTH * 48 * sizeof(uint16_t),  // 48 行/次，DMA 描述符占内部 RAM
            .flags = SPICOMMON_BUSFLAG_MASTER,
        };
        // DMA 自动分配通道，SPI 传输由 DMA 搬运，CPU 不阻塞
        ESP_ERROR_CHECK(spi_bus_initialize(DISPLAY_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO));
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
                // 距上次 sleep < 500ms + GPIO 低 = 死按延续 · 短路再 sleep · 用户死按多久都安全
                if (s_last_sleep_us > 0) {
                    uint64_t since_us = NowRtcUs() - s_last_sleep_us;
                    if (since_us < kDeadHoldWindowUs && gpio_get_level(BOOT_BUTTON_GPIO) == 0) {
                        ESP_LOGW(TAG, "距 sleep %llu ms · 用户未松手 · 短路 sleep", since_us / 1000);
                        s_last_sleep_us = NowRtcUs();
                        esp_sleep_enable_ext0_wakeup(BOOT_BUTTON_GPIO, 0);
                        rtc_gpio_pullup_en(BOOT_BUTTON_GPIO);
                        esp_deep_sleep_start();
                    }
                }
                first_boot_ = true;  // 开机键唤醒也算首次启动，播放欢迎音
                break;
            case ESP_SLEEP_WAKEUP_EXT1:
                ESP_LOGI(TAG, "从陀螺仪唤醒");
                first_boot_ = true;
                break;
            case ESP_SLEEP_WAKEUP_TIMER:
                ESP_LOGI(TAG, "从定时器唤醒 · 闹钟模式");
                AlarmManager::MarkTimerWakeup();
                first_boot_ = true;
                break;
            default:
                ESP_LOGI(TAG, "首次启动或复位 (原因=%u, reset=%d)", wakeup_reason, (int)esp_reset_reason());
                // 仅正常启动放欢迎音：上电 / USB 接入 / 主动 esp_restart（OTA/出厂复位/切网）
                // panic / wdt / brownout 等异常重启不放，避免"崩溃后假装一切正常"误导用户
                {
                    esp_reset_reason_t rr = esp_reset_reason();
                    first_boot_ = (rr == ESP_RST_POWERON || rr == ESP_RST_USB || rr == ESP_RST_SW);
                }
                break;
        }
    }



    void InitializeSc7a20h() {
        // 拿起灵敏度 320mg/100ms 与 P30 对齐 · 双击 peak=2200（P31 整机 ~60g 含 NFC+GPS+耳机）
        sc7a20h_sensor_ = sc7a20h_init(i2c_worker_, 320 /*mg*/, 100 /*ms*/);
        if (!sc7a20h_sensor_) { ESP_LOGE(TAG, "SC7A20H 初始化失败"); return; }
        // shake target=2 · 上下 2 次即触发 · 闹钟摇停由 ShakeStop(3) 累计防误关
        sc7a20h_shake (sc7a20h_sensor_, 1500, 600, 2, 1500, &OnShake,  this);
        // 桌面双击唤醒 — 暂关 · 后续扩展（P31 整机重 peak 调至 2200）
        // sc7a20h_strike(sc7a20h_sensor_, 2200,  80, 400, 800, &OnStrike, this);
    }

    // 摇一摇 — 日常 AI 互动 · 闹钟响铃中累计 3 次才停（防走路误关）
    static void OnShake(void* /*ctx*/) {
        Application::GetInstance().Schedule([] {
            if (AlarmRinger::GetInstance().ShakeStop(3)) return;
            auto& app = Application::GetInstance();
            auto state = app.GetDeviceState();
            if (state != kDeviceStateIdle && state != kDeviceStateListening) return;
            app.PlaySound(Lang::Sounds::OGG_POPUP);
            ESP_LOGI(TAG, "shake → AI");
            app.SendTextToAI("摇一摇随机互动");
        });
    }

    // 桌面双击 — 唤醒屏幕（与触摸唤醒同语义 · 不进 AI 对话）
    static void OnStrike(void* ctx) {
        auto* self = static_cast<MyDazyP31Board*>(ctx);
        Application::GetInstance().Schedule([self] {
            if (TryStopAlarmRinger("strike")) return;
            ESP_LOGI(TAG, "strike → wakeup");
            self->WakeUp();
        });
    }

    // 拿起唤醒 arm — 必须在 AUDIO_PWR_EN=0 之前调
    // 否则失电的 ES8311/ES7210 通过 ESD 二极管把 SDA/SCL 钉死 → INT1_SRC 清 latch 失败
    // （配合驱动 v5.0 LIR_INT1=0 双保险锁死秒醒 · 详见 docs § 8.4）
    void ArmGyroWakeup() {
        if (!sc7a20h_sensor_) return;
        if (Settings("status", false).GetInt("pickupWake", 1) == 0) return;
        esp_err_t r = sc7a20h_wakeup(sc7a20h_sensor_, SC7A20H_GPIO_INT1);
        if (r != ESP_OK) ESP_LOGW(TAG, "sc7a20h_wakeup failed: %s", esp_err_to_name(r));
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
        touch_driver_->SetWakeCallback([this]() {
            WakeUp();
        });

        // 设置手势回调：单击唤醒/打断/退出对话，滑动调节音量
        touch_driver_->SetGestureCallback([this](TouchGesture gesture, int16_t x, int16_t y) {
            if (TryStopAlarmRinger("touch")) return;
            WakeUp();

            switch (gesture) {
                case TouchGesture::SingleClick: {
                    auto& app = Application::GetInstance();
                    auto state = app.GetDeviceState();

                    // MP3 播放中：单击 = 停音乐，不进入对话
                    if (MusicPlayer::GetInstance().IsPlaying()) {
                        ESP_LOGI(TAG, "单击停止 MP3 播放");
                        MusicPlayer::GetInstance().Stop();
                        break;
                    }

                    if (state == kDeviceStateIdle) {
                        // 空闲状态：
                        //  - 时钟页可见 → 进入主菜单（4 宫格）
                        //  - 菜单已在 → 点到图标由按钮吞掉；点空白由菜单容器自回退
                        //  - 其它场景 → 原单击唤醒对话路径兜底
                        auto* lcd = dynamic_cast<UiDisplay*>(GetDisplay());
                        if (lcd && lcd->IsBrainInfoVisible()) {
                            // 关于页：空白单击不处理，只能走左上 "<" 返回
                            break;
                        }
                        if (lcd && lcd->IsMenuVisible()) {
                            // 已在菜单页：图标/空白由 LVGL 事件处理，这里不再兜底
                            break;
                        }
                        if (lcd && lcd->IsClockMode()) {
                            ESP_LOGI(TAG, "单击时钟 → 主菜单");
                            lcd->ShowMenu();
                            break;
                        }
                        // 其它 idle 场景（例如配网/激活 overlay）维持原对话唤醒逻辑
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
                case TouchGesture::SwipeDown:
                    if (auto* lcd = dynamic_cast<UiDisplay*>(GetDisplay())) {
                        // 关于页不响应下滑，控制中心必须走右上角 "∨"
                        if (lcd->IsBrainInfoVisible()) break;
                        if (!lcd->IsControlCenterVisible()) lcd->ShowControlCenter();
                    }
                    break;
                case TouchGesture::SwipeUp:
                    if (auto* lcd = dynamic_cast<UiDisplay*>(GetDisplay())) {
                        // 优先级：控制中心 > 菜单。控制中心可见先关它，再次上滑才回时钟
                        // 关于页不响应上滑，必须走左上角 "<" 返回
                        if (lcd->IsBrainInfoVisible()) {
                            break;
                        }
                        if (lcd->IsControlCenterVisible()) {
                            lcd->HideControlCenter();
                        } else if (lcd->IsMenuVisible()) {
                            lcd->HideMenu();
                        }
                    }
                    break;
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
            ShutdownOrSleep("电量过低", "强制关机", Lang::Sounds::OGG_LOW_BATTERY, 3000, false);
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
            // 状态上报触发点：进入省电前打一次（替代周期轮询）
            ReportStatus();
        });
        power_save_timer_->OnExitSleepMode([this]() {
            ESP_LOGI(TAG, "退出省电模式（恢复性能）");
            GetBacklight()->RestoreBrightness();
        });

        power_save_timer_->OnShutdownRequest([this]() {
            // 充电中跳过深睡 · 软省电（OnEnterSleepMode 降亮）仍生效防发烫
            // 2026-05-12 落地"充电不休眠"
            if (PowerManager::IsChargingGlobal()) {
                ESP_LOGI(TAG, "充电中，跳过深度睡眠 · LCD 保持降亮状态");
                return;
            }
            Settings settings("status", false);
            int deep_sleep_enabled = settings.GetInt("deepSleep", 1);
            if (deep_sleep_enabled) {
                ESP_LOGI(TAG, "✅ 深度睡眠已启用，5分钟无操作后进入深度睡眠");
                // 自动休眠：仅屏幕提示 + 陀螺仪可唤醒（拿起即醒）
                // 不播提示音 —— 用户没主动操作，夜间/会议中突然响会打扰
                ShutdownOrSleep("休眠中", "拿起唤醒", "", 1500, true);
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

        // ⚠️ 必须最先停 AudioService：让 audio_input/AFE/encode/output 等任务退出，
        // 否则后面切 AUDIO_PWR_EN 关电源时任务仍在 I2S 读写已掉电的 codec → I2S timeout/panic。
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

        // 闹钟定时唤醒功能
//        int intervalue = alarm_clock_get_next_task();
//        if(intervalue > 0){
//            esp_sleep_enable_timer_wakeup((uint64_t) intervalue * 1000 * 1000);
//            ESP_LOGI(TAG, "设置定时器唤醒: %d秒", intervalue);
//        }

        // 1. 先停音频服务（关闭 I2S DMA），避免 xfer 期间被切断电源损坏 DAC/ADC 状态
        // 必须在断 AUDIO_PWR_EN 之前；P30-4G/WiFi 走 Application 层有此保护，P31 板级路径需自己补
        ESP_LOGI(TAG, "[1/8] 停止音频服务 (I2S DMA)");
        Application::GetInstance().GetAudioService().Stop();
        vTaskDelay(pdMS_TO_TICKS(100));

        // 2. 关闭触摸屏，避免I2C错误
        if (touch_driver_) {
            touch_driver_->Cleanup();
            delete touch_driver_;
            touch_driver_ = nullptr;
            vTaskDelay(pdMS_TO_TICKS(100)); // 等待触摸屏完全关闭
        }

        if (enable_gyro_wakeup) ArmGyroWakeup();   // EXT1 · 必须在 AUDIO_PWR_EN=0 之前

        ESP_LOGI(TAG, "[2/8] 关闭音频电源");
        gpio_set_level(AUDIO_PWR_EN_GPIO, 0);
        rtc_gpio_hold_en(AUDIO_PWR_EN_GPIO);

        // hsf
        // 按键2（中间）唤醒设备
        ESP_ERROR_CHECK(esp_sleep_enable_ext0_wakeup(BOOT_BUTTON_GPIO, 0));
        ESP_ERROR_CHECK(rtc_gpio_pullup_en(BOOT_BUTTON_GPIO));
        ESP_ERROR_CHECK(rtc_gpio_pulldown_dis(BOOT_BUTTON_GPIO));
        ESP_LOGI(TAG, "BOOT wake source configured, GPIO%d", BOOT_BUTTON_GPIO);



        vTaskDelay(pdMS_TO_TICKS(200)); // 等待音频芯片完全断电

        // 3. 陀螺仪唤醒：实际 arm 推迟到 GPIO reset 块之前 · 见下方注释

        // 5. 处理WiFi连接（WiFi版本固定断开WiFi）
        if (GetNetworkType() == NetworkType::WIFI) {
            WifiStation::GetInstance().Stop();
        }

        // 6. 停止电源管理定时器
        if (power_save_timer_) {
            power_save_timer_->SetEnabled(false);
            ESP_LOGI(TAG, "电源管理定时器已停止");
        }

        // 6. 关闭背光
        ESP_LOGI(TAG, "[3/8] 关闭显示背光");
        gpio_set_level(DISPLAY_BACKLIGHT, 0);

        // 200ms 让 AUDIO_PWR_EN=0 引发的机械咔哒振动衰减（LIR_INT1=0 后不会留 latch）
        vTaskDelay(pdMS_TO_TICKS(200));

        // 8. 重置音频相关GPIO（此时 SC7A20H 已 arm，I2C 总线可安全 reset）
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

        // 最终等待，确保所有操作完成
        // 闹钟：arm RTC 定时唤醒（与 EXT0/EXT1 三源并存）
        AlarmManager::GetInstance().FlushNvs();
        AlarmManager::GetInstance().ConfigureTimerWakeup();

        ESP_LOGI(TAG, "准备进入深度睡眠");
        vTaskDelay(pdMS_TO_TICKS(200));
        // 每次进 sleep 都记 RTC 时戳 · 唤醒后比较 < 500ms 即死按延续 · 直接短路 sleep
        s_last_sleep_us = NowRtcUs();
        esp_deep_sleep_start();
    }

    // 统一关机/休眠入口：先显示 Alert + 播提示音，等指定时长后进 deep sleep
    //   title/msg/sound 任一可空跳过；delay_ms 覆盖提示音 + 视觉感知（建议 ≥ 2000ms）
    //   enable_gyro_wakeup=true：陀螺仪可唤醒（自动休眠场景）/ false：仅按键唤醒（按键关机场景）
    void ShutdownOrSleep(const char* title, const char* msg, const std::string_view& sound,
                         int delay_ms, bool enable_gyro_wakeup) {
        auto& app = Application::GetInstance();
        if (app.GetDeviceState() == kDeviceStateSpeaking) {
            app.AbortSpeaking(kAbortReasonNone);
        }
        if (title) app.Alert(title, msg ? msg : "", "", sound);
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
        EnterDeepSleep(enable_gyro_wakeup);
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
        display_ = new UiDisplay(panel_io_, panel_,
                                 DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
                                 DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
        ESP_LOGI(TAG, "UiDisplay 已启用 (时钟 + 配网 + 激活 + 控制中心)");
#endif

        // 关键节点：显示初始化完成后打印内存
        SystemInfo::PrintHeapStats();
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            if (TryStopAlarmRinger("button")) return;
            auto& app = Application::GetInstance();
            auto status = app.GetDeviceState();
            ESP_LOGI(TAG, "单击 button 状态： %u", status);
            waiting_factory_reset_confirm_ = false; // 重置确认状态

            // MP3 播放期间：按键 = 先打断 MP3 + 退 Player UI，再走唤醒/对话流程
            if (MusicPlayer::GetInstance().IsPlaying()) {
                ESP_LOGI(TAG, "按键打断 MP3 → 唤醒对话");
                MusicPlayer::GetInstance().Stop();
                if (auto* ui = dynamic_cast<UiDisplay*>(Board::GetInstance().GetDisplay())) {
                    ui->SwitchOutPlayerMode();
                }
            }

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
            if (TryStopAlarmRinger("button")) return;
            auto& app = Application::GetInstance();
            auto status = app.GetDeviceState();
            ESP_LOGI(TAG, "双击 button 状态： %u", status);

            if (waiting_factory_reset_confirm_) {
                // 检查是否超时（10秒超时）
                uint64_t now = esp_timer_get_time();
                if (now - factory_reset_request_time_ > 10000000) {  // 10秒 = 10000000微秒
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
                // 9 连击+双击确认触发恢复出厂：NVS 全擦 + 3 秒倒计时 esp_restart
                // LDO 复位由 ShutdownHandler 接管（穿越 esp_restart 保持 GPIO LOW）
                SystemReset::CheckButtons(true);
                return;
            }

            // P31 硬件无 AEC 回采，双击不切换 AEC 模式
            ESP_LOGI(TAG, "P31: 双击（AEC 不可用，硬件无回采通道）");
        });

        // 连按3次进入配网模式
        boot_button_.OnMultipleClick([this]() {
            ESP_LOGI(TAG, "连按3次进入配网模式");
            auto& app = Application::GetInstance();

            // 切网前统一暂停：① 停 MP3 播放（含退 Player UI）② 打断对话（Speaking/Listening）
            // 避免重启或进配网时音频任务/协议通道未释放导致的资源残留与异响
            if (MusicPlayer::GetInstance().IsPlaying()) {
                ESP_LOGI(TAG, "切网前停止 MP3 播放");
                MusicPlayer::GetInstance().Stop();
                if (auto* ui = dynamic_cast<UiDisplay*>(Board::GetInstance().GetDisplay())) {
                    ui->SwitchOutPlayerMode();
                }
            }
            {
                auto state = app.GetDeviceState();
                if (state == kDeviceStateSpeaking || state == kDeviceStateListening) {
                    ESP_LOGI(TAG, "切网前打断对话 (state=%d)", (int)state);
                    app.AbortSpeaking(kAbortReasonNone);
                }
            }

            if (app.GetDeviceState() != kDeviceStateWifiConfiguring){
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
            // 用户主动关机：从用户视角说"再见"，仅按键唤醒防陀螺仪误开机
            ShutdownOrSleep("再见", "", Lang::Sounds::OGG_SHUTDOWN, 2500, false);
        }, 4);

        // 连按5次：GPS 演示模式（切换开关）
        boot_button_.OnMultipleClick([this]() {
            ToggleGpsDemo();
        }, 5);

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
    /* v2.0 极简：NFC 改用静态单例 C API（mydazy_nfc_init/pause/resume），无需实例字段 */

    // GNSS 定位
    Ml307Gnss* gnss_ = nullptr;

    // Type-C 耳机检测
    TypecHeadset* headset_ = nullptr;

    // GPS 状态（多核共享）
    // - 原子量适合单字段 boolean/int；double 在 32 位核上非原子，用 mutex 保护整组位置
    std::atomic<int> gnss_satellites_{0};       // 当前使用卫星数（来自 GGA）
    std::atomic<int> gnss_sats_in_view_{0};     // 可见卫星数（来自 GSV）
    std::atomic<bool> gnss_fixed_{false};
    std::atomic<int64_t> gnss_last_fix_us_{0};  // 最近一次成功 fix 的时间戳
    std::atomic<bool> gnss_started_{false};     // 当前是否运行中（可切换）
    std::atomic<bool> gnss_auto_started_{false};  // 首次 idle 自动启动标志
    std::atomic<bool> gnss_user_disabled_{false}; // MCP stop 后不再自动重启
    mutable std::mutex gnss_pos_mutex_;
    double gnss_lat_ = 0.0;
    double gnss_lon_ = 0.0;
    double gnss_hdop_ = 0.0;
    char gnss_utc_time_[16] = {};

    // 状态栏节流：仅在卫星数或 fix 状态变化时刷新
    std::atomic<int> last_status_sats_{-1};
    std::atomic<bool> last_status_fixed_{false};

    // GPS 演示模式（连按5次切换）
    std::atomic<bool> gps_demo_active_{false};
    esp_timer_handle_t gps_demo_timer_ = nullptr;
    esp_timer_handle_t gnss_watchdog_timer_ = nullptr;  // fix 超时降级

    // 快照读取，避免在持锁时访问 LCD/网络
    struct GnssPos { double lat; double lon; double hdop; char utc[16]; };
    GnssPos GetGnssPos() const {
        std::lock_guard<std::mutex> lock(gnss_pos_mutex_);
        GnssPos p{gnss_lat_, gnss_lon_, gnss_hdop_, {}};
        memcpy(p.utc, gnss_utc_time_, sizeof(p.utc));
        return p;
    }
    void SetGnssPos(double lat, double lon, double hdop, const char* utc) {
        std::lock_guard<std::mutex> lock(gnss_pos_mutex_);
        gnss_lat_ = lat;
        gnss_lon_ = lon;
        gnss_hdop_ = hdop;
        if (utc) {
            strncpy(gnss_utc_time_, utc, sizeof(gnss_utc_time_) - 1);
            gnss_utc_time_[sizeof(gnss_utc_time_) - 1] = '\0';
        }
    }

    void RefreshGpsDemo() {
        if (!gps_demo_active_.load()) return;
        auto display = GetDisplay();
        if (!display) return;

        bool fixed = gnss_fixed_.load();
        int sats = gnss_satellites_.load();

        // 顶部：卫星数 + 系统类型
        char top[48];
        snprintf(top, sizeof(top), FONT_AWESOME_LOCATION_DOT " GPS %d %s",
                 sats, fixed ? "OK" : "...");
        display->SetStatus(top);

        // 底部：定位信息（使用线程安全快照，不触碰 gnss_ 裸指针）
        char bot[128];
        if (fixed) {
            auto pos = GetGnssPos();
            snprintf(bot, sizeof(bot),
                "Lat %.6f  Lon %.6f\n"
                "HDOP %.1f  UTC %.6s",
                pos.lat, pos.lon, pos.hdop, pos.utc);
        } else {
            snprintf(bot, sizeof(bot), "Waiting for fix... (%d sats in view)",
                     gnss_sats_in_view_.load());
        }
        display->SetChatMessage("system", bot);
    }

    void ToggleGpsDemo() {
        bool was_active = gps_demo_active_.exchange(!gps_demo_active_.load());
        bool now_active = !was_active;
        ESP_LOGI(TAG, "GPS demo %s", now_active ? "ON" : "OFF");
        auto display = GetDisplay();

        if (now_active) {
            if (display) display->ShowNotification("GPS Demo ON", 2000);
            RefreshGpsDemo();
            if (!gps_demo_timer_) {
                esp_timer_create_args_t args = {
                    .callback = [](void* arg) {
                        static_cast<MyDazyP31Board*>(arg)->RefreshGpsDemo();
                    },
                    .arg = this,
                    .dispatch_method = ESP_TIMER_TASK,
                    .name = "gps_demo",
                    .skip_unhandled_events = true,
                };
                esp_timer_create(&args, &gps_demo_timer_);
            }
            esp_timer_start_periodic(gps_demo_timer_, 2000000);
        } else {
            if (gps_demo_timer_) {
                esp_timer_stop(gps_demo_timer_);
                // 复用 timer，不每次 delete（避免 demo 反复切换泄漏）
            }
            if (display) {
                display->ShowNotification("GPS Demo OFF", 2000);
                display->SetChatMessage("system", "");
                display->SetStatus("");
            }
        }
    }

    static const char* NfcTypeName(nfc_card_type_t type) {
        switch (type) {
            case NFC_CARD_MIFARE_1K:    return "M1-1K";
            case NFC_CARD_MIFARE_4K:    return "M1-4K";
            case NFC_CARD_ULTRALIGHT:   return "NTAG";
            case NFC_CARD_MIFARE_PLUS:  return "Plus";
            case NFC_CARD_ISO14443A4:   return "CPU";
            default:                    return "NFC";
        }
    }

    // NFC 静态 C callback（v2.0 极简 API 要求函数指针）→ 转发到实例方法
    static void OnNfcCard(nfc_card_type_t type, const nfc_uid_t* uid, void* ctx) {
        static_cast<MyDazyP31Board*>(ctx)->HandleNfcCard(type, uid);
    }

    void HandleNfcCard(nfc_card_type_t type, const nfc_uid_t* uid) {
        char uid_str[32];
        mydazy_nfc_uid_to_str(uid, uid_str, sizeof(uid_str));

        if (auto display = GetDisplay()) {
            char text[64];
            snprintf(text, sizeof(text), "%s: %s", NfcTypeName(type), uid_str);
            display->SetChatMessage("system", text);
        }
        NfcRequestSwitch(type, uid_str);
    }

    // NFC → POST OTA_URL/switch {"type":"nfc","uid":"...","card_type":"..."}
    void NfcRequestSwitch(nfc_card_type_t type, const char* uid_str) {
        std::string uid(uid_str);
        std::string type_name = NfcTypeName(type);
        Application::GetInstance().Schedule([uid, type_name]() {
            cJSON* p = cJSON_CreateObject();
            cJSON_AddStringToObject(p, "uid", uid.c_str());
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

    // 周期上报开关：默认关闭。唤醒事件触发的一次性上报不受此开关影响。
    // 改 true 重编即可恢复 90s 周期；后续可改成 NVS 设置项。
    static constexpr bool kEnablePeriodicStatusReport = false;

    // 上报设备状态 → POST OTA_URL/status
    // 字段拼装 / idle / music 防御全部在 Ota::ReportStatus()，板级仅负责调用时机。
    // GPS 通过覆写 GetDeviceStatusJson() 注入 status.gps（见下方 override）。
    void ReportStatus() {
        Application::GetInstance().Schedule([]() {
            Ota ota; ota.ReportStatus();
        });
    }

    // P31 独有：在标准 status JSON 上注入 gps 子对象。
    // 基类 DualNetworkBoard::GetDeviceStatusJson 委托给 current_board_（Ml307Board 或 WifiBoard），
    // 我们再 parse → 追加 gps → 重新序列化。
    std::string GetDeviceStatusJson() override {
        std::string base = DualNetworkBoard::GetDeviceStatusJson();
        cJSON* root = cJSON_Parse(base.c_str());
        if (root == nullptr) return base;

        cJSON* gps = cJSON_CreateObject();
        cJSON_AddBoolToObject(gps, "fixed", gnss_fixed_.load());
        cJSON_AddNumberToObject(gps, "satellites", gnss_satellites_.load());
        if (gnss_fixed_.load()) {
            auto pos = GetGnssPos();
            cJSON_AddNumberToObject(gps, "latitude", pos.lat);
            cJSON_AddNumberToObject(gps, "longitude", pos.lon);
        }
        cJSON_AddItemToObject(root, "gps", gps);

        char* str = cJSON_PrintUnformatted(root);
        std::string result(str ? str : "{}");
        cJSON_free(str);
        cJSON_Delete(root);
        return result;
    }

    // 启动 90 秒定时状态上报（默认关闭，唤醒触发一次性上报代替）
    void StartStatusTimer() {
        if (!kEnablePeriodicStatusReport) {
            ESP_LOGI(TAG, "Periodic status report disabled (wake-only mode)");
            return;
        }
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

        // v2.0 极简：1 行初始化（含底层 IIC + chip reset + 启动后台 detection task）
        if (mydazy_nfc_init(i2c_bus_, &MyDazyP31Board::OnNfcCard, this, 300) != ESP_OK) {
            ESP_LOGW(TAG, "NFC 初始化失败");
        }
    }

    void InitializeHeadset() {
#if TYPEC_HEADSET_ENABLED
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
            auto* codec = dynamic_cast<Es7111AudioCodec*>(GetAudioCodec());
            if (codec) codec->SetHeadsetMode(inserted);

            auto display = GetDisplay();
            if (display) {
                display->ShowNotification(inserted ? "耳机已插入" : "耳机已拔出", 3000);
            }
            WakeUp();
        });

        headset_->Start(PowerManager::GetSharedAdcHandle());
#else
        ESP_LOGW(TAG, "⚠️ Type-C headset detection DISABLED (TYPEC_HEADSET_ENABLED=0)");
#endif
    }

    // 状态栏节流：卫星数/fix 变化时才刷新，避免 1Hz 覆盖真正的业务状态
    void UpdateGpsStatusBar() {
        if (gps_demo_active_.load()) return;  // demo 模式由自己的定时器刷

        int sats = gnss_satellites_.load();
        bool fixed = gnss_fixed_.load();
        int prev_sats = last_status_sats_.exchange(sats);
        bool prev_fixed = last_status_fixed_.exchange(fixed);
        if (sats == prev_sats && fixed == prev_fixed) return;

        auto display = GetDisplay();
        if (!display) return;

        char text[48];
        if (fixed) {
            auto pos = GetGnssPos();
            snprintf(text, sizeof(text), FONT_AWESOME_LOCATION_DOT " %d  %.4f,%.4f",
                     sats, pos.lat, pos.lon);
        } else {
            snprintf(text, sizeof(text), FONT_AWESOME_LOCATION_DOT " %d", sats);
        }
        display->SetStatus(text);
    }

    // fix 超时降级：>10s 无新 fix → 清除 fixed 标志
    static void GnssWatchdogThunk(void* arg) {
        auto* self = static_cast<MyDazyP31Board*>(arg);
        if (!self->gnss_fixed_.load()) return;
        int64_t now = esp_timer_get_time();
        int64_t last = self->gnss_last_fix_us_.load();
        if (now - last > 10 * 1000000LL) {
            self->gnss_fixed_ = false;
            ESP_LOGW(TAG, "GNSS: fix stale (>10s), demoting to searching");
            self->UpdateGpsStatusBar();
        }
    }

    // 惰性创建 Ml307Gnss 实例（需要 modem 已 ready）；成功返回 true
    bool EnsureGnssInstance() {
        if (gnss_) return true;
        if (GetNetworkType() != NetworkType::ML307) return false;

        auto& ml307_board = dynamic_cast<Ml307Board&>(GetCurrentBoard());
        auto* modem = ml307_board.GetModem();
        if (!modem) {
            ESP_LOGW(TAG, "GNSS: modem not ready");
            return false;
        }

        gnss_ = new Ml307Gnss(modem->GetAtUart());

        // GSV: 可见卫星数（搜星指示）
        gnss_->SetSatCallback([this](int sats_in_view) {
            gnss_sats_in_view_ = sats_in_view;
            if (!gnss_fixed_.load()) {
                gnss_satellites_ = sats_in_view;
                UpdateGpsStatusBar();
            }
        });

        // GGA: 定位结果（fix + 使用中卫星数）
        gnss_->SetFixCallback([this](const GnssFix& fix) {
            if (fix.valid) {
                SetGnssPos(fix.latitude, fix.longitude, fix.hdop, fix.utc_time);
                gnss_satellites_ = fix.satellites;
                gnss_last_fix_us_ = esp_timer_get_time();
                bool was_fixed = gnss_fixed_.exchange(true);
                UpdateGpsStatusBar();
                if (!was_fixed) ReportStatus();  // 仅首次上报，避免 1Hz 轰炸后端
            } else {
                gnss_fixed_ = false;
                UpdateGpsStatusBar();
            }
        });

        return true;
    }

    // 启动 GPS（幂等）；返回是否当前处于运行状态
    bool StartGnss() {
        if (gnss_started_.load()) return true;
        if (!EnsureGnssInstance()) return false;

        if (!gnss_->Start(kGnssGps | kGnssBds)) {
            ESP_LOGE(TAG, "GNSS: Start() failed");
            return false;
        }
        gnss_started_ = true;
        ESP_LOGI(TAG, "GNSS started (GPS+BDS)");

        // fix watchdog: 每 3 秒检查一次；复用已创建的 timer
        if (!gnss_watchdog_timer_) {
            esp_timer_create_args_t args = {
                .callback = GnssWatchdogThunk,
                .arg = this,
                .dispatch_method = ESP_TIMER_TASK,
                .name = "gnss_wdt",
                .skip_unhandled_events = true,
            };
            esp_timer_create(&args, &gnss_watchdog_timer_);
        }
        esp_timer_start_periodic(gnss_watchdog_timer_, 3 * 1000000ULL);
        return true;
    }

    // 停止 GPS（幂等）；不销毁 gnss_ 实例以便再次 Start 复用 URC 回调
    void StopGnss() {
        if (!gnss_started_.exchange(false)) return;
        if (gnss_) gnss_->Stop();
        if (gnss_watchdog_timer_) esp_timer_stop(gnss_watchdog_timer_);

        gnss_fixed_ = false;
        gnss_satellites_ = 0;
        gnss_sats_in_view_ = 0;
        last_status_sats_ = -1;
        last_status_fixed_ = false;
        auto display = GetDisplay();
        if (display) display->SetStatus("");
        ESP_LOGI(TAG, "GNSS stopped");
    }

    // 注册 GPS 相关 MCP tools（独立于启动状态，Board 初始化时调一次即可）
    void InitializeGnssMcp() {
        auto& mcp = McpServer::GetInstance();

        // 开启 GPS
        mcp.AddTool("self.gps.start",
            "开启 GPS 定位。查位置前先调。需 4G 模块就绪。",
            PropertyList(),
            [this](const PropertyList&) -> ReturnValue {
                gnss_user_disabled_ = false;
                bool ok = StartGnss();
                cJSON* json = cJSON_CreateObject();
                cJSON_AddBoolToObject(json, "started", ok);
                cJSON_AddBoolToObject(json, "running", gnss_started_.load());
                if (!ok) {
                    cJSON_AddStringToObject(json, "reason",
                        GetNetworkType() == NetworkType::ML307 ? "modem_not_ready" : "not_4g_mode");
                }
                return json;
            });

        // 关闭 GPS
        mcp.AddTool("self.gps.stop",
            "关 GPS 省电。再次开启需调 self.gps.start。",
            PropertyList(),
            [this](const PropertyList&) -> ReturnValue {
                StopGnss();
                gnss_user_disabled_ = true;  // 禁止再自动启动
                cJSON* json = cJSON_CreateObject();
                cJSON_AddBoolToObject(json, "stopped", true);
                return json;
            });

        // 查询定位
        mcp.AddTool("self.gps.get_location",
            "查 GPS 经纬度。若 GPS 未开，先调 self.gps.start。",
            PropertyList(),
            [this](const PropertyList&) -> ReturnValue {
                cJSON* json = cJSON_CreateObject();
                bool running = gnss_started_.load();
                bool fixed = gnss_fixed_.load();
                int sats = gnss_satellites_.load();
                cJSON_AddBoolToObject(json, "running", running);
                cJSON_AddBoolToObject(json, "fixed", fixed);
                cJSON_AddNumberToObject(json, "satellites", sats);
                if (!running) {
                    cJSON_AddStringToObject(json, "status", "off");
                } else if (fixed) {
                    auto pos = GetGnssPos();
                    cJSON_AddNumberToObject(json, "latitude", pos.lat);
                    cJSON_AddNumberToObject(json, "longitude", pos.lon);
                    cJSON_AddNumberToObject(json, "hdop", pos.hdop);
                    if (pos.utc[0] != '\0') {
                        cJSON_AddStringToObject(json, "utc_time", pos.utc);
                    }
                } else {
                    cJSON_AddStringToObject(json, "status",
                        gnss_sats_in_view_.load() > 0 ? "searching" : "no_signal");
                }
                return json;
            });
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
            // 改用 ShowNotification（1.5s 自动消失） · clock/player 模式下也能可见
            GetDisplay()->ShowNotification(volume_text, 1500);
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
                    // 改用 ShowNotification（1.5s 自动消失） · clock/player 模式下也能可见
                    Board::GetInstance().GetDisplay()->ShowNotification(volume_text, 1500);
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

        // 注册重启钩子：任何 esp_restart() 调用前自动断 LDO 复位 LCD/音频
        esp_register_shutdown_handler(ShutdownHandler);

        InitializeGpio();           // 1. 先启用音频电源
        InitializeI2c();            // 2. 初始化I2C总线 (音频编解码器需要)
        PrepareTouchHardware();     // 3. 先完成共享复位线上的触摸硬件初始化
        InitializeSpi();            // 4. 初始化SPI总线 (显示需要)
        InitializeDisplay();        // 5. 初始化显示 (依赖SPI)
        InitializeTouch();          // 6. LCD/LVGL 就绪后再注册触摸输入
        InitializeSc7a20h();        // 7. 初始化SC7A20H传感器 (依赖I2C)
        InitializePowerManager();  // 8. 初始化电池监控
        InitializePowerSaveTimer(); // 9. 初始化省电定时器
        InitializeButtons();        // 10. 初始化按钮 (最后初始化)
        InitializeNfc();            // 11. 初始化 NFC（WS1850S I2C）
        InitializeHeadset();        // 12. Type-C 耳机动态检测
        // InitializeIBeacon();     // TODO: iBeacon 需等 BLE host sync 后启动，暂禁用

        // 13. GPS：注册 MCP tool（无论网络类型，tool 始终可调用）+ 订阅首次 Idle 事件
        //     首次进入 kDeviceStateIdle 表示激活已完成，4G 必然已联网，此时自动开 GPS。
        //     用户通过 self.gps.stop 关闭后，gnss_user_disabled_ 置位，不再自动重启。
        InitializeGnssMcp();

        // 13.b 教育卡 MCP 工具集：show_stroke 笔画 GIF + show_card 教育卡（必须在 Display 初始化之后）
        // 修复 BUG：v32 之前 P31 完全没注册 show_stroke，导致 LLM 看不到工具 → 识字 GIF 永远不触发
        // 4G/WiFi 两种网络模式下都开启 show_stroke：
        //   完整性校验四重防护（1KB min + GIF89a/87a magic + 末字节 0x3B Trailer + Schedule state 守护）
        //   弱网失败时端侧自动弹大字 EduCard 兜底，不会显示脏帧或崩溃
        RegisterEducationMcpTools(McpServer::GetInstance(),
                                  dynamic_cast<UiDisplay*>(GetDisplay()));
        DeviceStateEventManager::GetInstance().RegisterStateChangeCallback(
            [this](DeviceState /*prev*/, DeviceState curr) {
                if (curr != kDeviceStateIdle) return;
                if (gnss_auto_started_.load()) return;
                if (gnss_user_disabled_.load()) return;
                if (GetNetworkType() != NetworkType::ML307) return;  // WiFi 模式无 GPS
                if (StartGnss()) gnss_auto_started_ = true;
            });

        // 14. 定时状态上报（90 秒间隔）
        StartStatusTimer();

        // 背光开启移至 Application::Initialize（SetupUI 之后），
        // 避免 LVGL 首帧到达 GRAM 之前打开背光导致的开机白屏闪现。

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

        // P31 关闭 AEC 测试：排查断续问题
        Application::GetInstance().SetAecMode(kAecOff);
        ESP_LOGI(TAG, "P31: AEC 已关闭（排查断续问题）");

        ESP_LOGI(TAG, "MyDazy P31 初始化完成 (ES7111+ES7210, 4G, NFC, GPS, 触摸屏)");

        // 首次开机欢迎音
         if (first_boot_) {
            TaskHandle_t temp_handle = nullptr;
            xTaskCreatePinnedToCore([](void* arg){
                auto self = static_cast<MyDazyP31Board*>(arg);
                auto& app = Application::GetInstance();
                vTaskDelay(pdMS_TO_TICKS(1500));

                // 闹钟 TIMER 唤醒：跳过欢迎音 · AlarmRinger 接管响铃 · 不抢屏不抢音
                if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER) {
                    ESP_LOGI(TAG, "TIMER 唤醒(闹钟模式)· 跳过欢迎音");
                    self->welcome_task_handle_ = nullptr;
                    vTaskDelete(NULL);
                    return;
                }

                // 检查是否处于配网模式,如果是则跳过logo和欢迎音
                if (app.GetDeviceState() == kDeviceStateWifiConfiguring) {
                    ESP_LOGI(TAG, "配网模式:跳过开机logo和欢迎音");
                    self->welcome_task_handle_ = nullptr;
                    vTaskDelete(NULL);
                    return;
                }

                if (self->first_boot_) {
                    // A1 · 先 arm 自动对话（state listener 在首次进 Idle 时触发 ToggleChat）
                    app.RequestAutoChatOnIdle();
                    ESP_LOGI(TAG, "✅ 播放欢迎音（音频就绪，开机即播）");
                    app.PlaySound(Lang::Sounds::OGG_WELCOME);
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
        if (gnss_watchdog_timer_) {
            esp_timer_stop(gnss_watchdog_timer_);
            esp_timer_delete(gnss_watchdog_timer_);
            gnss_watchdog_timer_ = nullptr;
        }
        if (gps_demo_timer_) {
            esp_timer_stop(gps_demo_timer_);
            esp_timer_delete(gps_demo_timer_);
            gps_demo_timer_ = nullptr;
        }
        if (gnss_) {
            gnss_->Stop();
            delete gnss_;
            gnss_ = nullptr;
        }
        // 清理 NFC（v2.0 极简：pause 即可，单例无 handle）
        mydazy_nfc_pause();
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

        // v5.0 驱动按"程序生命周期内不释放"设计 · 无 del API · 仅清句柄
        sc7a20h_sensor_ = nullptr;

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

    // ESP-IDF 标准 shutdown hook：esp_restart() 调用前自动执行（OTA / 恢复出厂 / 双击确认等所有路径）。
    // 切 AUDIO_PWR_EN_GPIO (P31=GPIO15) 让 LCD + 音频 CODEC 真正下电复位，避免重启后 LCD 黑屏。
    // 用 esp_register_shutdown_handler 注册（构造函数中），不需要 base Board 虚函数。
    // 注：背光关闭由 application.cc Reboot() 在 esp_restart 前完成；static 方法不能访问 instance 成员。
    static void ShutdownHandler() {
        gpio_set_level(AUDIO_PWR_EN_GPIO, 0);
        rtc_gpio_hold_en(AUDIO_PWR_EN_GPIO);    // 穿越 esp_restart() 保持 LOW
        esp_rom_delay_us(500 * 1000);           // 等电容放电（shutdown 上下文用 ROM delay 更稳妥）
    }

    // 唤醒设备（内部方法，非虚函数）
    void WakeUp() {
        if (power_save_timer_) {
            power_save_timer_->WakeUp();
        }
    }

};

DECLARE_BOARD(MyDazyP31Board);
