#include "wifi_board.h"

#include "display.h"
#include "application.h"
#include "system_info.h"
#include "settings.h"
#include "assets/lang_config.h"
#include <functional>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_network.h>
#include <esp_log.h>
#include <utility>

#include <font_awesome.h>
#include <wifi_manager.h>
#include <wifi_station.h>
#include <ssid_manager.h>
#ifdef CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING
#include "blufi.h"
#include <esp_lvgl_port.h>  // lvgl_port_lock: 保护 BLE flash-op × LVGL PSRAM buffer 死锁
#endif

static const char *TAG = "WifiBoard";

// Connection timeout in seconds
static constexpr int CONNECT_TIMEOUT_SEC = 60;

WifiBoard::WifiBoard() {
    // Create connection timeout timer
    esp_timer_create_args_t timer_args = {
        .callback = OnWifiConnectTimeout,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "wifi_connect_timer",
        .skip_unhandled_events = true
    };
    esp_timer_create(&timer_args, &connect_timer_);
}

WifiBoard::~WifiBoard() {
    if (connect_timer_) {
        esp_timer_stop(connect_timer_);
        esp_timer_delete(connect_timer_);
    }
}

std::string WifiBoard::GetBoardType() {
    return "wifi";
}

void WifiBoard::StartNetwork() {
    auto& wifi_manager = WifiManager::GetInstance();

    // Initialize WiFi manager
    WifiManagerConfig config;
    config.ssid_prefix = "Xiaozhi";
    config.language = Lang::CODE;
    wifi_manager.Initialize(config);

    // Set unified event callback - forward to NetworkEvent with SSID data
    wifi_manager.SetEventCallback([this](WifiEvent event, const std::string& data) {
        switch (event) {
            case WifiEvent::Scanning:
                OnNetworkEvent(NetworkEvent::Scanning);
                break;
            case WifiEvent::Connecting:
                OnNetworkEvent(NetworkEvent::Connecting, data);
                break;
            case WifiEvent::Connected:
                OnNetworkEvent(NetworkEvent::Connected, data);
                break;
            case WifiEvent::Disconnected:
                OnNetworkEvent(NetworkEvent::Disconnected);
                break;
            case WifiEvent::ConfigModeEnter:
                OnNetworkEvent(NetworkEvent::WifiConfigModeEnter);
                break;
            case WifiEvent::ConfigModeExit:
                OnNetworkEvent(NetworkEvent::WifiConfigModeExit);
                break;
        }
    });

    // Try to connect or enter config mode
    TryWifiConnect();
}

void WifiBoard::TryWifiConnect() {
    auto& ssid_manager = SsidManager::GetInstance();
    bool have_ssid = !ssid_manager.GetSsidList().empty();

    if (have_ssid) {
        // Start connection attempt with timeout
        ESP_LOGI(TAG, "Starting WiFi connection attempt");
        esp_timer_start_once(connect_timer_, CONNECT_TIMEOUT_SEC * 1000000ULL);
        WifiManager::GetInstance().StartStation();
    } else {
        // No SSID configured, enter config mode
        // Wait for the board version to be shown
        vTaskDelay(pdMS_TO_TICKS(1500));
        StartWifiConfigMode();
    }
}

void WifiBoard::OnNetworkEvent(NetworkEvent event, const std::string& data) {
    switch (event) {
        case NetworkEvent::Connected:
            // Stop timeout timer
            esp_timer_stop(connect_timer_);
            // 统一清理配网资源（BLUFI/AP 都走 StopConfigMode，幂等）
            if (in_config_mode_) {
                StopConfigMode();
            }
            in_config_mode_ = false;
            ESP_LOGI(TAG, "Connected to WiFi: %s", data.c_str());
            break;
        case NetworkEvent::Scanning:
            ESP_LOGI(TAG, "WiFi scanning");
            break;
        case NetworkEvent::Connecting:
            ESP_LOGI(TAG, "WiFi connecting to %s", data.c_str());
            break;
        case NetworkEvent::Disconnected:
            ESP_LOGW(TAG, "WiFi disconnected");
            break;
        case NetworkEvent::WifiConfigModeEnter:
            ESP_LOGI(TAG, "WiFi config mode entered");
            in_config_mode_ = true;
            break;
        case NetworkEvent::WifiConfigModeExit:
            ESP_LOGI(TAG, "WiFi config mode exited");
            in_config_mode_ = false;
            // Try to connect with the new credentials
            TryWifiConnect();
            break;
        default:
            break;
    }

    // Notify external callback if set
    if (network_event_callback_) {
        network_event_callback_(event, data);
    }
}

void WifiBoard::SetNetworkEventCallback(NetworkEventCallback callback) {
    network_event_callback_ = std::move(callback);
}

void WifiBoard::OnWifiConnectTimeout(void* arg) {
    auto* board = static_cast<WifiBoard*>(arg);
    ESP_LOGW(TAG, "WiFi connection timeout, entering config mode");

    WifiManager::GetInstance().StopStation();
    board->StartWifiConfigMode();
}

void WifiBoard::StartWifiConfigMode() {
    in_config_mode_ = true;
    // Transition to wifi configuring state
    Application::GetInstance().SetDeviceState(kDeviceStateWifiConfiguring);

    // 读取默认配网模式：Settings("wifi").blufi=1 走 BLUFI，=0 走 AP
    // 未编译 BLUFI 支持时强制 AP；首次启动默认 BLUFI 优先（小程序主流程）
    ConfigMode mode = ConfigMode::AP;
#ifdef CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING
    {
        Settings settings("wifi", false);
        mode = settings.GetInt("blufi", 1) ? ConfigMode::BLUFI : ConfigMode::AP;
    }
#endif

    if (!StartConfigMode(mode)) {
        // BLUFI 失败已在 StartConfigMode 内部降级到 AP；这里兜底处理编译缺失等极端场景
        ESP_LOGE(TAG, "StartConfigMode 失败，无可用配网模式");
    }
}

// 双击切换配网模式：在 Application 主循环里延迟执行 SwitchConfigMode，
// 避免在 LVGL 事件回调中直接调用（SwitchConfigMode 内有 vTaskDelay 300ms + flash op）。
std::function<void()> WifiBoard::MakeSwitchCallback() {
    return [this]() {
        Application::GetInstance().Schedule([this]() {
            SwitchConfigMode();
        });
    };
}

bool WifiBoard::StartConfigMode(ConfigMode mode) {
    current_config_mode_ = mode;
    auto& wifi_manager = WifiManager::GetInstance();
    // AP SSID 作为通用设备识别名（如 "MyDazy-ABCD"），BLUFI 广播名、QR 载荷同步
    std::string device_name = wifi_manager.GetApSsid();

    if (mode == ConfigMode::BLUFI) {
#ifdef CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING
        ESP_LOGI(TAG, "启动 BLUFI 配网，设备名: %s", device_name.c_str());
        // ⚠️ BLE controller init 触发 flash read → 禁用 cache；此时 LVGL 若在渲染
        //    PSRAM framebuffer 会阻塞 IDLE1 导致 Task WDT。lvgl_port_lock 强制 LVGL
        //    暂停渲染，让它等锁而不是卡在 PSRAM。超时 1s 后仍继续（记录警告）。
        bool lvgl_locked = lvgl_port_lock(1000);
        if (!lvgl_locked) {
            ESP_LOGW(TAG, "LVGL 锁超时，仍继续 BLUFI init（死锁风险）");
        }
        esp_err_t ret = Blufi::GetInstance().init();
        if (lvgl_locked) lvgl_port_unlock();

        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "BLUFI init 失败: %s，降级到 AP", esp_err_to_name(ret));
            current_config_mode_ = ConfigMode::AP;
            return StartConfigMode(ConfigMode::AP);  // 单次降级，AP 失败不再递归
        }

        // 展示配网 QR 页（设备名作为载荷，小程序扫码后 BLE 搜索同名设备）
        if (auto* display = GetDisplay()) {
            display->ShowWifiQrCode(device_name.c_str(), "双击切换配网方式",
                                    "蓝牙配网", "热点配网",
                                    /*active_left=*/true,
                                    MakeSwitchCallback());
        }
        return true;
#else
        ESP_LOGW(TAG, "未编译 BLUFI 支持，降级到 AP");
        current_config_mode_ = ConfigMode::AP;
        // 继续走下方 AP 分支
#endif
    }

    // AP 模式
#ifdef CONFIG_USE_HOTSPOT_WIFI_PROVISIONING
    ESP_LOGI(TAG, "启动 AP 热点配网，SSID: %s", device_name.c_str());
    wifi_manager.StartConfigAp();

    // QR 载荷 = SSID（UiDisplay 内部会按 WiFi QR 标准包装 WIFI:T:nopass;S:...）
    // hint 行显示 web 配网 URL，用户可手动输入
    Application::GetInstance().Schedule([this, device_name]() {
        if (auto* display = GetDisplay()) {
            std::string hint = WifiManager::GetInstance().GetApWebUrl();
            display->ShowWifiQrCode(device_name.c_str(), hint.c_str(),
                                    "蓝牙配网", "热点配网",
                                    /*active_left=*/false,
                                    MakeSwitchCallback());
        }
    });
    return true;
#else
    ESP_LOGE(TAG, "未编译 HOTSPOT 支持，无可用配网方式");
    return false;
#endif
}

void WifiBoard::StopConfigMode() {
    // 先隐藏 QR 页（解除 LVGL 双击监听，避免 stop 过程中触发切换回调）
    if (auto* display = GetDisplay()) {
        display->HideWifiQrCode();
    }

    if (current_config_mode_ == ConfigMode::BLUFI) {
#ifdef CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING
        ESP_LOGI(TAG, "停止 BLUFI 配网");
        // 同 init：deinit 也涉及 BT controller flash op，LVGL 锁保护
        bool lvgl_locked = lvgl_port_lock(1000);
        Blufi::GetInstance().deinit();
        if (lvgl_locked) lvgl_port_unlock();
#endif
    } else {
#ifdef CONFIG_USE_HOTSPOT_WIFI_PROVISIONING
        ESP_LOGI(TAG, "停止 AP 热点配网");
        WifiManager::GetInstance().StopConfigAp();
#endif
    }
}

void WifiBoard::SwitchConfigMode() {
    if (!in_config_mode_) {
        ESP_LOGW(TAG, "非配网状态，忽略切换请求");
        return;
    }

    ConfigMode old_mode = current_config_mode_;
    ConfigMode new_mode = (old_mode == ConfigMode::BLUFI) ? ConfigMode::AP : ConfigMode::BLUFI;

    ESP_LOGI(TAG, "===== 切换配网模式: %s -> %s =====",
             old_mode == ConfigMode::BLUFI ? "BLUFI" : "AP",
             new_mode == ConfigMode::BLUFI ? "BLUFI" : "AP");

    StopConfigMode();
    vTaskDelay(pdMS_TO_TICKS(300));  // 等 BLE/AP 资源彻底释放

    // 先持久化，启动失败时再回滚（避免启动过程中用户重启导致状态丢失）
    {
        Settings settings("wifi", true);
        settings.SetInt("blufi", new_mode == ConfigMode::BLUFI ? 1 : 0);
    }

    if (!StartConfigMode(new_mode)) {
        ESP_LOGW(TAG, "切换到 %s 失败，回退", new_mode == ConfigMode::BLUFI ? "BLUFI" : "AP");
        Settings settings("wifi", true);
        settings.SetInt("blufi", old_mode == ConfigMode::BLUFI ? 1 : 0);
        vTaskDelay(pdMS_TO_TICKS(300));
        StartConfigMode(old_mode);
    }
}

void WifiBoard::EnterWifiConfigMode() {
    ESP_LOGI(TAG, "EnterWifiConfigMode called");
    GetDisplay()->ShowNotification(Lang::Strings::ENTERING_WIFI_CONFIG_MODE);

    auto& app = Application::GetInstance();
    auto state = app.GetDeviceState();

    if (state == kDeviceStateSpeaking || state == kDeviceStateListening || state == kDeviceStateIdle) {
        // Reset protocol (close audio channel, reset protocol)
        Application::GetInstance().ResetProtocol();

        xTaskCreate([](void* arg) {
            auto* board = static_cast<WifiBoard*>(arg);

            // Wait for 1 second to allow speaking to finish gracefully
            vTaskDelay(pdMS_TO_TICKS(1000));

            // Stop any ongoing connection attempt
            esp_timer_stop(board->connect_timer_);
            WifiManager::GetInstance().StopStation();

            // Enter config mode
            board->StartWifiConfigMode();

            vTaskDelete(NULL);
        }, "wifi_cfg_delay", 4096, this, 2, NULL);
        return;
    }

    if (state != kDeviceStateStarting) {
        ESP_LOGE(TAG, "EnterWifiConfigMode called but device state is not starting or speaking, device state: %d", state);
        return;
    }

    // Stop any ongoing connection attempt
    esp_timer_stop(connect_timer_);
    WifiManager::GetInstance().StopStation();

    StartWifiConfigMode();
}

bool WifiBoard::IsInWifiConfigMode() const {
    return WifiManager::GetInstance().IsConfigMode();
}

NetworkInterface* WifiBoard::GetNetwork() {
    static EspNetwork network;
    return &network;
}

const char* WifiBoard::GetNetworkStateIcon() {
    auto& wifi = WifiManager::GetInstance();

    if (wifi.IsConfigMode()) {
        return FONT_AWESOME_WIFI;
    }
    if (!wifi.IsConnected()) {
        return FONT_AWESOME_WIFI_SLASH;
    }

    int rssi = wifi.GetRssi();
    if (rssi >= -65) {
        return FONT_AWESOME_WIFI;
    } else if (rssi >= -75) {
        return FONT_AWESOME_WIFI_FAIR;
    }
    return FONT_AWESOME_WIFI_WEAK;
}

std::string WifiBoard::GetBoardJson() {
    auto& wifi = WifiManager::GetInstance();
    std::string json = R"({"type":")" + std::string(BOARD_TYPE) + R"(",)";
    json += R"("name":")" + std::string(BOARD_NAME) + R"(",)";

    if (!wifi.IsConfigMode()) {
        json += R"("ssid":")" + wifi.GetSsid() + R"(",)";
        json += R"("rssi":)" + std::to_string(wifi.GetRssi()) + R"(,)";
        json += R"("channel":)" + std::to_string(wifi.GetChannel()) + R"(,)";
        json += R"("ip":")" + wifi.GetIpAddress() + R"(",)";
    }

    json += R"("mac":")" + SystemInfo::GetMacAddress() + R"("})";
    return json;
}

void WifiBoard::SetPowerSaveLevel(PowerSaveLevel level) {
    WifiPowerSaveLevel wifi_level;
    switch (level) {
        case PowerSaveLevel::LOW_POWER:
            wifi_level = WifiPowerSaveLevel::LOW_POWER;
            break;
        case PowerSaveLevel::BALANCED:
            wifi_level = WifiPowerSaveLevel::BALANCED;
            break;
        case PowerSaveLevel::PERFORMANCE:
        default:
            wifi_level = WifiPowerSaveLevel::PERFORMANCE;
            break;
    }
    WifiManager::GetInstance().SetPowerSaveLevel(wifi_level);
}

std::string WifiBoard::GetDeviceStatusJson() {
    auto& board = Board::GetInstance();
    auto root = cJSON_CreateObject();

    // Audio speaker
    auto audio_speaker = cJSON_CreateObject();
    if (auto codec = board.GetAudioCodec()) {
        cJSON_AddNumberToObject(audio_speaker, "volume", codec->output_volume());
    }
    cJSON_AddItemToObject(root, "audio_speaker", audio_speaker);

    // Screen
    auto screen = cJSON_CreateObject();
    if (auto backlight = board.GetBacklight()) {
        cJSON_AddNumberToObject(screen, "brightness", backlight->brightness());
    }
    if (auto display = board.GetDisplay(); display && display->height() > 64) {
        if (auto theme = display->GetTheme()) {
            cJSON_AddStringToObject(screen, "theme", theme->name().c_str());
        }
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
    auto& wifi = WifiManager::GetInstance();
    auto network = cJSON_CreateObject();
    cJSON_AddStringToObject(network, "type", "wifi");
    cJSON_AddStringToObject(network, "ssid", wifi.GetSsid().c_str());
    int rssi = wifi.GetRssi();
    const char* signal = rssi >= -60 ? "strong" : (rssi >= -70 ? "medium" : "weak");
    cJSON_AddStringToObject(network, "signal", signal);
    cJSON_AddItemToObject(root, "network", network);

    // Chip temperature
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
