#include "wifi_board.h"

#include "display.h"
#include "application.h"
#include "power_manager.h"
#include "flow_engine.h"
#include "system_info.h"
#include "settings.h"
#include "assets/lang_config.h"
#include "audio/music_player.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_network.h>
#include <esp_log.h>
#include <esp_lvgl_port.h>
#include <algorithm>

#include <font_awesome.h>
#include "wifi_station.h"
#include "wifi_ap.h"
#include "ssid_manager.h"
#include "blufi/blufi.h"

static const char *TAG = "WifiBoard";

// 智能联网参数（P0：重试 2→4 + 首次 3s→5s · 防弱信号家庭场景频繁误进配网）
static constexpr int MAX_SCAN_RETRY = 4;        // 最大扫描重试次数（双频开启扫描 1.3s · 2 次太少）
static constexpr int SCAN_WAIT_MS = 5000;       // 单次扫描等待时间（含扫描 + 连接 buffer）
static constexpr int CONNECT_TIMEOUT_MS = 10000; // 连接超时

WifiBoard::WifiBoard() {
    Settings settings("wifi", true);
    wifi_config_mode_ = settings.GetInt("force_ap") == 1;
    if (wifi_config_mode_) {
        ESP_LOGI(TAG, "force_ap is set, reset to 0");
        settings.SetInt("force_ap", 0);
    }
}

std::string WifiBoard::GetBoardType() {
    return "wifi";
}

// 获取设备显示名称（品牌名-MAC后4位）
static std::string GetDeviceShowName() {
    std::string mac = SystemInfo::GetMacAddress();
    std::string suffix = mac.substr(mac.length() - 5);
    suffix.erase(std::remove(suffix.begin(), suffix.end(), ':'), suffix.end());
    std::transform(suffix.begin(), suffix.end(), suffix.begin(), ::toupper);

    auto& board = Board::GetInstance();
    return board.GetBrandName() + "-" + suffix;
}

// ============ 智能联网 ============

bool WifiBoard::SmartConnect() {
    auto& wifi_station = WifiStation::GetInstance();
    auto display = Board::GetInstance().GetDisplay();

    // 设置连接回调
    wifi_station.OnScanBegin([display]() {
        display->ShowNotification(Lang::Strings::SCANNING_WIFI, 10000);
    });
    wifi_station.OnConnect([display](const std::string& ssid) {
        std::string msg = std::string(Lang::Strings::CONNECT_TO) + ssid + "...";
        display->ShowNotification(msg.c_str(), 15000);
    });
    wifi_station.OnConnected([display](const std::string& ssid) {
        std::string msg = std::string(Lang::Strings::CONNECTED_TO) + ssid;
        display->ShowNotification(msg.c_str(), 3000);
    });

    // 启动 WiFi（会自动扫描并尝试连接）
    ESP_LOGI(TAG, "启动智能联网...");
    wifi_station.Start();

    // 等待连接成功（最多重试 MAX_SCAN_RETRY 次）
    for (int retry = 0; retry < MAX_SCAN_RETRY; retry++) {
        ESP_LOGI(TAG, "等待WiFi连接 (尝试 %d/%d)", retry + 1, MAX_SCAN_RETRY);

        // 等待扫描完成 + 连接（最多 SCAN_WAIT_MS）
        if (wifi_station.WaitForConnected(SCAN_WAIT_MS)) {
            ESP_LOGI(TAG, "WiFi连接成功: %s", wifi_station.GetSsid().c_str());
            if (network_event_callback_) {
                network_event_callback_(NetworkEvent::Connected, wifi_station.GetSsid());
            }
            return true;
        }

        // 检查是否有匹配的热点
        auto matched = wifi_station.GetMatchedAccessPoints(false);  // 使用缓存
        if (matched.empty()) {
            ESP_LOGW(TAG, "未找到匹配热点，立即重新扫描...");
            wifi_station.TriggerScan();
            continue;  // 直接进入下一次循环
        }

        ESP_LOGI(TAG, "找到 %d 个匹配热点，等待连接...", (int)matched.size());
        if (wifi_station.WaitForConnected(CONNECT_TIMEOUT_MS)) {
            ESP_LOGI(TAG, "WiFi连接成功: %s", wifi_station.GetSsid().c_str());
            if (network_event_callback_) {
                network_event_callback_(NetworkEvent::Connected, wifi_station.GetSsid());
            }
            return true;
        }
    }

    ESP_LOGW(TAG, "多次尝试连接失败");
    wifi_station.Stop();
    return false;
}

// ============ 配网模式 ============

// 统一凭证验证器
static auto credential_validator = [](const std::string& ssid, const std::string& password, std::string& error) {
    auto result = WifiStation::GetInstance().TryConnectAndSave(ssid, password, 10000);
    if (!result.success) {
        error = "Connection failed";
        return false;
    }
    return true;
};

bool WifiBoard::StartConfigMode(ConfigMode mode, bool is_switch) {
    std::string show_name = GetDeviceShowName();
    bool success = false;

    ESP_LOGI(TAG, "启动%s配网: %s", mode == ConfigMode::BLUFI ? "蓝牙" : "热点", show_name.c_str());
    if (mode == ConfigMode::BLUFI) {
        Application::GetInstance().Alert(Lang::Strings::WIFI_CONFIG_MODE, "", "", Lang::Sounds::OGG_BLECONFIG);

        auto& blufi = Blufi::GetInstance();
        blufi.SetCredentialValidator(credential_validator);
        // 注意：捕获 this 而不是依赖 Board::GetInstance()
        // DualNetworkBoard 场景下 Board::GetInstance() 返回的不是 WifiBoard
        blufi.OnConfigSuccess([this]() {
            std::string data = Board::GetInstance().GetBoardJson();
            Blufi::GetInstance().SendData(data.c_str(), data.length());
            this->OnConfigSuccess();
        });

        bool lvgl_locked = lvgl_port_lock(1000);
        if (!lvgl_locked) {
            ESP_LOGW(TAG, "LVGL 锁超时，仍继续 Blufi 启动（有死锁风险）");
        }
        success = blufi.Start(show_name);
        if (lvgl_locked) lvgl_port_unlock();
    } else {
        Application::GetInstance().Alert(Lang::Strings::WIFI_CONFIG_MODE, "", "", Lang::Sounds::OGG_WIFICONFIG);

        auto& wifi_ap = WifiAp::GetInstance();
        wifi_ap.SetCredentialValidator(credential_validator);
        wifi_ap.OnConfigSuccess([this]() {
            this->OnConfigSuccess();
        });
        show_name = "Ai-" + show_name;
        wifi_ap.Start(show_name, Lang::CODE);
        success = true;
    }

    if (success) {
        current_config_mode_ = mode;
        config_initialized_ = true;

        // 显示配网 UI 页面
        auto display = Board::GetInstance().GetDisplay();
        if (display) {
            if (mode == ConfigMode::BLUFI) {
                // 蓝牙配网：二维码扫码跳转 H5/小程序，设备名通过蓝牙广播匹配
                display->ShowQrCode("https://mydazy.cn/ota/blufi",
                                    show_name.c_str(),         // highlight：设备名（蓝色加亮）
                                    "双击切换模式",            // top
                                    "微信扫码配网",      // bottom：操作引导
                                    "蓝牙配网", "热点配网", true);
            } else {
                // 热点配网：调用方拼接标准 WiFi 二维码格式
                std::string wifi_qr = "WIFI:T:nopass;S:" + show_name + ";;";
                display->ShowQrCode(wifi_qr.c_str(),
                                    show_name.c_str(),         // highlight：设备名（蓝色加亮）
                                    "双击切换模式",            // top
                                    "连接WiFi热点",       // bottom：辅助说明
                                    "蓝牙配网", "热点配网", false);
            }
        }
    }

    return success;
}

void WifiBoard::StopConfigMode() {
    if (!config_initialized_) return;

    ESP_LOGI(TAG, "停止配网模式");

    if (current_config_mode_ == ConfigMode::AP) {
        WifiAp::GetInstance().Stop();
    } else {
        Blufi::GetInstance().Stop();
    }

    config_initialized_ = false;
    vTaskDelay(pdMS_TO_TICKS(300));
}

void WifiBoard::SwitchConfigMode() {
    auto& app = Application::GetInstance();
    if (app.GetDeviceState() != kDeviceStateWifiConfiguring) {
        ESP_LOGW(TAG, "非配网状态，无法切换");
        return;
    }

    ConfigMode new_mode = (current_config_mode_ == ConfigMode::BLUFI) ? ConfigMode::AP : ConfigMode::BLUFI;
    ConfigMode old_mode = current_config_mode_;

    ESP_LOGI(TAG, "===== 切换配网模式: %s -> %s =====",
        old_mode == ConfigMode::BLUFI ? "蓝牙" : "热点",
        new_mode == ConfigMode::BLUFI ? "蓝牙" : "热点");

    // 1. 停止当前配网模式
    ESP_LOGI(TAG, "[1/4] 停止当前配网...");
    StopConfigMode();

    auto& wifi = WifiStation::GetInstance();

    // 2. 切换到蓝牙前，先初始化 BLE 控制器和切换 WiFi 模式
    if (new_mode == ConfigMode::BLUFI) {
        // 只播音效，不调 Alert（Alert 会在非 LVGL 线程获取 display 锁超时）
        Application::GetInstance().PlaySound(Lang::Sounds::OGG_BLE_CONFIG);

        // 2.1 初始化 BLE 控制器（如果未初始化）
        auto& blufi = Blufi::GetInstance();
        if (!blufi.IsControllerInitialized()) {
            ESP_LOGI(TAG, "[2/5] 初始化 BLE 控制器");
            // ⚠️ 关键：BLE 控制器初始化涉及 flash read（加载 firmware）→ 禁用 cache
            // 期间 LVGL 若在渲染，访问 PSRAM buffer 会阻塞 10+ 秒 → taskLVGL 持锁饿死 IDLE1 → Task WDT
            // 修复：lvgl_port_lock 强制 LVGL 暂停渲染，让它在等锁而不是在 PSRAM 阻塞
            bool lvgl_locked = lvgl_port_lock(1000);  // 1s 获取锁
            if (!lvgl_locked) {
                ESP_LOGW(TAG, "LVGL 锁超时，仍继续 BLE 初始化（有死锁风险）");
            }
            bool ok = blufi.InitializeController();
            if (lvgl_locked) lvgl_port_unlock();

            if (!ok) {
                ESP_LOGE(TAG, "BLE 控制器初始化失败，无法切换到 Blufi");
                new_mode = ConfigMode::AP;  // 降级到 AP 模式
            } else {
                vTaskDelay(pdMS_TO_TICKS(200));
                ESP_LOGI(TAG, "✅ BLE 控制器就绪");
            }
        }

        // 2.2 切换 WiFi 到 STA 模式（避免 APSTA 与蓝牙冲突）
        if (new_mode == ConfigMode::BLUFI && wifi.GetMode() == WifiMode::APSTA) {
            ESP_LOGI(TAG, "[3/5] WiFi切换到STA模式");
            wifi.SetMode(WifiMode::STA);
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    } else {
        Application::GetInstance().PlaySound(Lang::Sounds::OGG_WIFI_CONFIG);

        // 切换到AP模式：确保WiFi已初始化（BLUFI模式下WiFi可能未启动）
        if (!wifi.IsInitialized()) {
            ESP_LOGI(TAG, "[2/5] 初始化 WiFi（从蓝牙切换）");
            wifi.SetScanOnlyMode(true);
            // 同理：WiFi 初始化涉及 phy_init flash 读取 → 禁用 cache → LVGL 阻塞
            bool lvgl_locked = lvgl_port_lock(1000);
            if (!lvgl_locked) {
                ESP_LOGW(TAG, "LVGL 锁超时，仍继续 WiFi 初始化");
            }
            wifi.Start();
            if (lvgl_locked) lvgl_port_unlock();
            vTaskDelay(pdMS_TO_TICKS(500));
            if (!wifi.IsInitialized()) {
                ESP_LOGE(TAG, "WiFi 初始化失败，无法切换到 AP 模式");
                // 回退到蓝牙模式
                new_mode = old_mode;
            }
        } else {
            ESP_LOGI(TAG, "[2/5] WiFi已就绪");
        }
    }

    // 3. 保存配网模式设置
    ESP_LOGI(TAG, "[4/5] 保存配网设置");
    Settings settings("wifi", true);
    settings.SetInt("blufi", new_mode == ConfigMode::BLUFI ? 1 : 0);

    // 4. 启动新配网模式
    ESP_LOGI(TAG, "[5/5] 启动%s配网", new_mode == ConfigMode::BLUFI ? "Blufi" : "AP");
    if (!StartConfigMode(new_mode, true)) {
        ESP_LOGW(TAG, "切换失败，回退到%s", old_mode == ConfigMode::BLUFI ? "Blufi" : "AP");
        settings.SetInt("blufi", old_mode == ConfigMode::BLUFI ? 1 : 0);
        vTaskDelay(pdMS_TO_TICKS(300));
        StartConfigMode(old_mode, true);
    }

    ESP_LOGI(TAG, "配网切换完成");
}

void WifiBoard::EnterWifiConfigMode() {
    auto& app = Application::GetInstance();
    app.SetDeviceState(kDeviceStateWifiConfiguring);

    Settings settings("wifi", true);
    ConfigMode mode = settings.GetInt("blufi", 1) ? ConfigMode::BLUFI : ConfigMode::AP;

    // 【架构优化】BLUFI模式：BLE先广播 → WiFi延迟到BLE连接后
    // AP模式：WiFi先初始化 → 启动AP热点

    if (mode == ConfigMode::BLUFI) {
        // BLUFI模式：仅初始化BLE，WiFi延迟到BLE_CONNECT回调中
        ESP_LOGI(TAG, "【1/2】初始化 BLE 控制器");
        auto& blufi = Blufi::GetInstance();
        if (!blufi.IsControllerInitialized()) {
            // ⚠️ 锁 LVGL 避免 BLE firmware flash 加载期间 PSRAM 访问死锁（同 SwitchConfigMode）
            bool lvgl_locked = lvgl_port_lock(1000);
            bool ok = blufi.InitializeController();
            if (lvgl_locked) lvgl_port_unlock();

            if (!ok) {
                ESP_LOGE(TAG, "BLE 控制器初始化失败，切换到 AP 模式");
                mode = ConfigMode::AP;
            } else {
                vTaskDelay(pdMS_TO_TICKS(200));
            }
        }

        if (mode == ConfigMode::BLUFI) {
            // 立即启动BLE广播，手机可以快速发现设备
            ESP_LOGI(TAG, "【2/2】启动 Blufi 广播（WiFi将在手机连接后初始化）");
            if (!StartConfigMode(mode)) {
                ESP_LOGW(TAG, "Blufi启动失败，切换到AP");
                mode = ConfigMode::AP;
            }
        }
    }

    if (mode == ConfigMode::AP) {
        // AP模式：需要先初始化WiFi
        ESP_LOGI(TAG, "【1/2】初始化 WiFi");
        auto& wifi = WifiStation::GetInstance();
        wifi.SetScanOnlyMode(true);
        if (!wifi.IsInitialized()) {
            wifi.Start();
            vTaskDelay(pdMS_TO_TICKS(500));
            if (!wifi.IsInitialized()) {
                ESP_LOGE(TAG, "WiFi 初始化失败，无法进入配网模式");
                app.Alert("WiFi 错误", "初始化失败", "", "");
                return;
            }
        }
        ESP_LOGI(TAG, "【2/2】启动 AP 配网");
        StartConfigMode(ConfigMode::AP);
    }

    // 【P1改进】添加配网超时机制（10分钟）
    const int CONFIG_TIMEOUT_SECONDS = 600;  // 10 分钟
    int elapsed_seconds = 0;

    // 等待配网完成或超时
    while (wifi_config_mode_ && elapsed_seconds < CONFIG_TIMEOUT_SECONDS) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        elapsed_seconds++;

        // 每分钟打印一次进度
        if (elapsed_seconds % 60 == 0) {
            ESP_LOGI(TAG, "配网进行中... (%d/%d 分钟)",
                     elapsed_seconds / 60, CONFIG_TIMEOUT_SECONDS / 60);
        }
    }

    if (wifi_config_mode_) {
        ESP_LOGW(TAG, "配网超时（%d 分钟），退出配网模式", CONFIG_TIMEOUT_SECONDS / 60);
        StopConfigMode();
        wifi_config_mode_ = false;
        auto& wifi = WifiStation::GetInstance();
        wifi.SetScanOnlyMode(false);
        if (wifi.IsInitialized()) {
            wifi.SetMode(WifiMode::STA);
        }
        app.Alert("配网超时", "请重试", "", "");

        // 配网超时，进入深度休眠（避免无WiFi连接下继续启动流程）
        ESP_LOGW(TAG, "配网超时，3秒后进入深度休眠");
        vTaskDelay(pdMS_TO_TICKS(3000));  // 等待提示音播放
        Board::GetInstance().EnterDeepSleep(true);
        // 不会执行到这里（EnterDeepSleep 内部调用 esp_deep_sleep_start）
    } else {
        ESP_LOGI(TAG, "配网完成");
    }
}

// ============ 网络启动 ============

void WifiBoard::StartNetwork() {
    // 强制进入配网模式
    if (wifi_config_mode_) {
        EnterWifiConfigMode();
        return;
    }

    // 无凭证直接进入配网
    auto& ssid_manager = SsidManager::GetInstance();
    if (ssid_manager.GetSsidList().empty()) {
        ESP_LOGI(TAG, "无WiFi凭证，进入配网");
        wifi_config_mode_ = true;
        EnterWifiConfigMode();
        return;
    }

    // 智能联网（扫描+匹配+重试）
    if (!SmartConnect()) {
        ESP_LOGW(TAG, "智能联网失败，进入配网");
        wifi_config_mode_ = true;
        EnterWifiConfigMode();
    } else {
        // WiFi 连通，本次启动肯定不再进配网 → 释放 BT 静态 .bss/.data 约 30-50KB 内部 RAM
        // （BT 从未 init，直接 release；若后续用户触发进配网会先 esp_restart，安全）
        Blufi::ReleaseStaticMem();
    }
}

// ============ 配网成功 ============

void WifiBoard::OnConfigSuccess() {
    // 防重入：SwitchConfigMode 或多次回调可能同时触发
    if (!config_initialized_) {
        ESP_LOGW(TAG, "配网已停止，忽略重复回调");
        return;
    }

    ESP_LOGI(TAG, "===== 配网成功 =====");

    auto& app = Application::GetInstance();
    auto& wifi = WifiStation::GetInstance();

    // 检查连接状态
    if (!wifi.IsConnected()) {
        ESP_LOGW(TAG, "WiFi未连接");
    } else {
        ESP_LOGI(TAG, "已连接: %s (%s)", wifi.GetSsid().c_str(), wifi.GetIpAddress().c_str());
    }

    app.Alert(Lang::Strings::WIFI_CONFIG_MODE, "", "", Lang::Sounds::OGG_SUCCESS);

    // Pin Core 0：与 BT controller / WiFi stack 同核 · 防漂到 Core 1 撞实时音频
    xTaskCreatePinnedToCore([](void* arg) {
        auto* self = static_cast<WifiBoard*>(arg);

        // 1. 等待提示音 + BLE数据发送完成
        vTaskDelay(pdMS_TO_TICKS(2000));

        // 2. 停止配网服务（BLE 主机栈 + 控制器清理，内部幂等）
        ESP_LOGI(TAG, "停止配网服务");
        self->StopConfigMode();

        // 3. 额外等待让 BLE 资源彻底释放
        vTaskDelay(pdMS_TO_TICKS(500));

        // 4. 重启设备
        //    原因：配网期间 BLE + WiFi + 提示音等模块已占用大量 RAM，继续走
        //    SetDeviceState(idle) → OTA → MQTT 流程时内部 RAM 会跌到 ~57KB，
        //    触达 60KB 红线；NVS flash op 禁用 cache 时 PSRAM 栈任务
        //    （opus_codec/cdc_console/audio_detection）会 Double exception
        //    (SP=0x60100000)。重启后 WiFi 凭证已保存，走正常 SmartConnect
        //    流程，RAM 干净不会崩溃。
        ESP_LOGI(TAG, "===== 配网成功，重启设备以释放资源 =====");
        self->wifi_config_mode_ = false;
        Application::GetInstance().Reboot();  // 内部 esp_restart() 不返回

        vTaskDelete(NULL);
    }, "config_done", 8192, this, 5, NULL, 0 /* Core 0 */);
}

// ============ 其他方法 ============

NetworkInterface* WifiBoard::GetNetwork() {
    static EspNetwork network;
    return &network;
}

const char* WifiBoard::GetNetworkStateIcon() {
    if (wifi_config_mode_) {
        return FONT_AWESOME_WIFI;
    }

    auto& wifi_station = WifiStation::GetInstance();
    if (!wifi_station.IsConnected()) {
        return FONT_AWESOME_WIFI_SLASH;
    }

    int8_t rssi = wifi_station.GetRssi();
    if (rssi >= -60) return FONT_AWESOME_WIFI;
    if (rssi >= -70) return FONT_AWESOME_WIFI_FAIR;
    return FONT_AWESOME_WIFI_WEAK;
}

std::string WifiBoard::GetBoardJson() {
    auto& wifi_station = WifiStation::GetInstance();
    std::string json = R"({"type":")" + std::string(BOARD_TYPE) + R"(",)";
    json += R"("name":")" + std::string(BOARD_NAME) + R"(",)";

    if (!wifi_config_mode_) {
        json += R"("ssid":")" + wifi_station.GetSsid() + R"(",)";
        json += R"("rssi":)" + std::to_string(wifi_station.GetRssi()) + R"(,)";
        json += R"("channel":)" + std::to_string(wifi_station.GetChannel()) + R"(,)";
        json += R"("ip":")" + wifi_station.GetIpAddress() + R"(",)";
    }

    json += R"("mac":")" + SystemInfo::GetMacAddress() + R"(")";


    json += R"(})";
    return json;
}

void WifiBoard::SetPowerSaveLevel(PowerSaveLevel level) {
    // MP3 播放期间禁止切回省电模式（弱网下省电会让 HTTP 流频繁超时 → 重试耗尽 → 播放中断）
    // 由 MusicPlayer::Stop / on_finished 主动恢复 LOW_POWER
    if (level != PowerSaveLevel::PERFORMANCE && MusicPlayer::GetInstance().IsPlaying()) {
        return;
    }
    WifiStation::GetInstance().SetPowerSaveMode(level != PowerSaveLevel::PERFORMANCE);
}

void WifiBoard::ResetWifiConfiguration() {
    {
        Settings settings("wifi", true);
        settings.SetInt("force_ap", 1);
    }
    {
        Settings boot_settings("boot", true);
        boot_settings.SetInt("skip_welcome", 1);
    }

    ESP_LOGI(TAG, "%s", Lang::Strings::ENTERING_WIFI_CONFIG_MODE);
    vTaskDelay(pdMS_TO_TICKS(500));
    Application::GetInstance().Reboot();
}

std::string WifiBoard::GetDeviceStatusJson() {
    auto& board = Board::GetInstance();
    auto root = cJSON_CreateObject();

    // Audio
    auto audio = cJSON_CreateObject();
    auto codec = board.GetAudioCodec();
    if (codec) {
        cJSON_AddNumberToObject(audio, "volume", codec->output_volume());
    }
    cJSON_AddItemToObject(root, "audio_speaker", audio);

    // Screen brightness
    auto backlight = board.GetBacklight();
    auto screen = cJSON_CreateObject();
    if (backlight) {
        cJSON_AddNumberToObject(screen, "brightness", backlight->brightness());
    }
    auto display = board.GetDisplay();
    if (display && display->height() > 64) {
        cJSON_AddStringToObject(screen, "theme",
            display->GetTheme() ? display->GetTheme()->name().c_str() : "");
    }
    cJSON_AddItemToObject(root, "screen", screen);

    // Battery
    int level = 0;
    bool charging = false, discharging = false;
    if (board.GetBatteryLevel(level, charging, discharging)) {
        auto battery = cJSON_CreateObject();
        cJSON_AddNumberToObject(battery, "level", level);
        cJSON_AddBoolToObject(battery, "charging", charging);
        cJSON_AddItemToObject(root, "battery", battery);
    }

    // Network
    auto& wifi = WifiStation::GetInstance();
    auto network = cJSON_CreateObject();
    cJSON_AddStringToObject(network, "type", "wifi");
    cJSON_AddStringToObject(network, "ssid", wifi.GetSsid().c_str());
    int rssi = wifi.GetRssi();
    cJSON_AddStringToObject(network, "signal", rssi >= -60 ? "strong" : (rssi >= -70 ? "medium" : "weak"));
    cJSON_AddItemToObject(root, "network", network);

    // Chip
    float temp = 0.0f;
    if (board.GetTemperature(temp)) {
        auto chip = cJSON_CreateObject();
        cJSON_AddNumberToObject(chip, "temperature", temp);
        cJSON_AddItemToObject(root, "chip", chip);
    }

    auto str = cJSON_PrintUnformatted(root);
    std::string result(str);
    cJSON_free(str);
    cJSON_Delete(root);
    return result;
}
