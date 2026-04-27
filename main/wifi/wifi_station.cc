#include "wifi_station.h"
#include <cstring>
#include <algorithm>
#include <map>

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <esp_heap_caps.h>
#include <nvs.h>
#include "nvs_flash.h"
#include <esp_netif.h>
#include <esp_system.h>
#include <lwip/ip_addr.h>
#include "ssid_manager.h"

#define TAG "WifiStation"
#define WIFI_EVENT_CONNECTED BIT0
#define WIFI_EVENT_DISCONNECTED BIT1
#define WIFI_EVENT_GOT_IP BIT2
// 重连退避延迟（微秒）: 1s, 2s, 4s, 8s, 15s, 30s, 60s（封顶）
// 快速阶段 3 次（7s），慢速阶段无限重试（60s 间隔）
static const uint64_t kReconnectBackoffUs[] = {
    1000000, 2000000, 4000000, 8000000, 15000000, 30000000, 60000000
};
static const int kReconnectBackoffCount = sizeof(kReconnectBackoffUs) / sizeof(kReconnectBackoffUs[0]);

WifiStation& WifiStation::GetInstance() {
    static WifiStation instance;
    return instance;
}

WifiStation::WifiStation() {
    // Create the event group
    event_group_ = xEventGroupCreate();

    // 读取配置（【修复】正确处理 NVS 打开失败）
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("wifi", NVS_READONLY, &nvs);
    if (err == ESP_OK) {
        // NVS 打开成功，读取配置
        err = nvs_get_i8(nvs, "max_tx_power", &max_tx_power_);
        if (err != ESP_OK) {
            max_tx_power_ = 0;
        }
        err = nvs_get_u8(nvs, "remember_bssid", &remember_bssid_);
        if (err != ESP_OK) {
            remember_bssid_ = 0;
        }
        nvs_close(nvs);
    } else {
        // NVS 打开失败，使用默认值
        ESP_LOGW(TAG, "NVS open failed: %s, using defaults", esp_err_to_name(err));
        max_tx_power_ = 0;
        remember_bssid_ = 0;
    }

    // 初始化扫描配置
    scan_config_ = WifiScanConfig();
}

WifiStation::~WifiStation() {
    vEventGroupDelete(event_group_);
}

// ========== 扫描管理实现 ==========
void WifiStation::TriggerScan() {
    if (is_scanning_) {
        ESP_LOGI(TAG, "Scan already in progress, skip");
        return;
    }

    if (!initialized_) {
        ESP_LOGE(TAG, "WiFi not initialized, cannot scan");
        return;
    }

    is_scanning_ = true;
    ESP_LOGI(TAG, "Starting WiFi scan...");

    // 主动扫描：每信道80-120ms（默认120ms被动），总耗时约1.3s
    wifi_scan_config_t scan_cfg = {};
    scan_cfg.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    scan_cfg.scan_time.active.min = 80;
    scan_cfg.scan_time.active.max = 120;

    esp_err_t ret = esp_wifi_scan_start(&scan_cfg, false);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start scan: %s", esp_err_to_name(ret));
        is_scanning_ = false;
    } else {
        ESP_LOGI(TAG, "WiFi scan triggered (active mode: 80-120ms/ch)");
    }
}

std::vector<wifi_ap_record_t> WifiStation::GetScanCache() {
    std::lock_guard<std::mutex> lock(scan_mutex_);
    return scan_cache_;
}

std::vector<wifi_ap_record_t> WifiStation::GetDeduplicatedCache() {
    std::lock_guard<std::mutex> lock(scan_mutex_);
    std::vector<wifi_ap_record_t> result = scan_cache_;
    SortByRssi(result);
    DeduplicateBySsid(result);
    return result;
}

std::vector<WifiApRecord> WifiStation::GetMatchedAccessPoints(bool force_refresh) {
    // 如果需要强制刷新或缓存无效，触发扫描
    if (force_refresh || !IsCacheValid()) {
        TriggerScan();
        // 等待扫描完成（最多3秒）
        for (int i = 0; i < 30 && is_scanning_; i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }

    std::vector<WifiApRecord> result;
    std::vector<wifi_ap_record_t> cache;

    {
        std::lock_guard<std::mutex> lock(scan_mutex_);
        cache = scan_cache_;
    }

    // 排序和去重
    SortByRssi(cache);
    if (scan_config_.auto_deduplicate) {
        DeduplicateBySsid(cache);
    }

    // 与凭证匹配
    auto& ssid_manager = SsidManager::GetInstance();
    auto ssid_list = ssid_manager.GetSsidList();

    for (const auto& ap : cache) {
        auto it = std::find_if(ssid_list.begin(), ssid_list.end(), [&ap](const SsidItem& item) {
            return strcmp((char*)ap.ssid, item.ssid.c_str()) == 0;
        });

        if (it != ssid_list.end()) {
            WifiApRecord record;
            record.ssid = it->ssid;
            record.password = it->password;
            record.rssi = ap.rssi;
            record.channel = ap.primary;
            record.authmode = ap.authmode;
            memcpy(record.bssid, ap.bssid, 6);
            result.push_back(record);

            ESP_LOGI(TAG, "Matched AP: %s, RSSI: %d, Channel: %d",
                record.ssid.c_str(), record.rssi, record.channel);
        }
    }

    return result;
}

bool WifiStation::IsCacheValid() const {
    if (scan_cache_.empty()) {
        return false;
    }

    int64_t now = esp_timer_get_time() / 1000;  // 转换为毫秒
    return (now - last_scan_time_ms_) < scan_config_.cache_valid_ms;
}

size_t WifiStation::GetCacheCount() const {
    std::lock_guard<std::mutex> lock(scan_mutex_);
    return scan_cache_.size();
}

void WifiStation::ClearScanCache() {
    std::lock_guard<std::mutex> lock(scan_mutex_);
    scan_cache_.clear();
    last_scan_time_ms_ = 0;
    ESP_LOGI(TAG, "Scan cache cleared");
}

void WifiStation::UpdateScanCache(wifi_ap_record_t* records, uint16_t count) {
    std::lock_guard<std::mutex> lock(scan_mutex_);

    scan_cache_.clear();
    scan_cache_.reserve(count);

    for (uint16_t i = 0; i < count && i < scan_config_.max_ap_count && i < MAX_SCAN_CACHE; i++) {
        scan_cache_.push_back(records[i]);
    }

    // 排序
    SortByRssi(scan_cache_);

    last_scan_time_ms_ = esp_timer_get_time() / 1000;
    is_scanning_ = false;

    ESP_LOGI(TAG, "Scan cache updated: %d APs, timestamp: %lld ms",
        (int)scan_cache_.size(), last_scan_time_ms_);
}

// ========== 定时扫描控制实现 ==========

void WifiStation::SetScanConfig(const WifiScanConfig& config) {
    scan_config_ = config;
    ESP_LOGI(TAG, "Scan config updated: cache_valid=%dms, max_ap=%d, dedupe=%d",
        config.cache_valid_ms, config.max_ap_count, config.auto_deduplicate);
}

// ========== 扫描回调实现 ==========
void WifiStation::OnScanComplete(std::function<void(const std::vector<wifi_ap_record_t>&)> callback) {
    on_scan_complete_ = callback;
}

// ========== 静态工具方法实现 ==========
void WifiStation::SortByRssi(std::vector<wifi_ap_record_t>& records) {
    std::sort(records.begin(), records.end(), [](const wifi_ap_record_t& a, const wifi_ap_record_t& b) {
        return a.rssi > b.rssi;  // 降序排列，信号强的在前
    });
}

void WifiStation::DeduplicateBySsid(std::vector<wifi_ap_record_t>& records) {
    if (records.empty()) {
        return;
    }

    SortByRssi(records);

    // 去重：保留每个 SSID 信号最强的记录
    std::map<std::string, bool> seen;
    auto write = records.begin();
    for (const auto& record : records) {
        std::string ssid((char*)record.ssid);
        if (!ssid.empty() && seen.find(ssid) == seen.end()) {
            seen[ssid] = true;
            *write++ = record;
        }
    }
    records.erase(write, records.end());
}

// ========== 连接管理实现 (保留原有逻辑，增强) ==========
void WifiStation::AddAuth(const std::string &&ssid, const std::string &&password) {
    auto& ssid_manager = SsidManager::GetInstance();
    ssid_manager.AddSsid(ssid, password);
}

void WifiStation::Stop() {
    if (!initialized_) {
        ESP_LOGD(TAG, "WiFi not initialized, skip stop");
        return;
    }

    ESP_LOGI(TAG, "Stopping WiFi...");

    if (timer_handle_ != nullptr) {
        esp_timer_stop(timer_handle_);
        esp_timer_delete(timer_handle_);
        timer_handle_ = nullptr;
    }
    if (reconnect_timer_ != nullptr) {
        esp_timer_stop(reconnect_timer_);
        esp_timer_delete(reconnect_timer_);
        reconnect_timer_ = nullptr;
    }

    esp_wifi_scan_stop();
    esp_wifi_disconnect();

    // 取消注册事件处理程序
    if (instance_any_id_ != nullptr) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id_);
        instance_any_id_ = nullptr;
    }
    if (instance_got_ip_ != nullptr) {
        esp_event_handler_instance_unregister(IP_EVENT, ESP_EVENT_ANY_ID, instance_got_ip_);
        instance_got_ip_ = nullptr;
    }

    // Reset the WiFi stack
    esp_wifi_stop();
    esp_wifi_deinit();

    // 销毁 AP 网络接口
    if (ap_netif_ != nullptr) {
        esp_netif_destroy(ap_netif_);
        ap_netif_ = nullptr;
    }

    // 销毁 STA 网络接口
    if (station_netif_ != nullptr) {
        esp_netif_destroy(station_netif_);
        station_netif_ = nullptr;
    }

    // 清理回调函数，避免内存泄漏
    on_scan_begin_ = nullptr;
    on_connect_ = nullptr;
    on_connected_ = nullptr;
    on_scan_complete_ = nullptr;
    on_disconnected_ = nullptr;

    // 重置连接队列和状态
    connect_queue_.clear();
    reconnect_count_ = 0;
    ssid_.clear();
    password_.clear();
    ip_address_.clear();
    ap_ssid_.clear();

    // 清除扫描缓存
    ClearScanCache();

    // 重置配网状态
    try_connect_mode_ = false;
    scan_only_mode_ = false;
    last_disconnect_reason_ = 0;
    current_mode_ = WifiMode::STA;
    initialized_ = false;

    // Clear event group bits
    xEventGroupClearBits(event_group_, WIFI_EVENT_CONNECTED | WIFI_EVENT_DISCONNECTED | WIFI_EVENT_GOT_IP);

    ESP_LOGI(TAG, "WiFi stopped");
}

void WifiStation::OnScanBegin(std::function<void()> on_scan_begin) {
    on_scan_begin_ = on_scan_begin;
}

void WifiStation::OnConnect(std::function<void(const std::string& ssid)> on_connect) {
    on_connect_ = on_connect;
}

void WifiStation::OnConnected(std::function<void(const std::string& ssid)> on_connected) {
    on_connected_ = on_connected;
}

void WifiStation::Start() {
    // 委托给带参数的 Start 方法（STA 模式）
    Start(WifiMode::STA);
}

bool WifiStation::WaitForConnected(int timeout_ms) {
    auto bits = xEventGroupWaitBits(event_group_, WIFI_EVENT_CONNECTED, pdFALSE, pdFALSE, timeout_ms / portTICK_PERIOD_MS);
    return (bits & WIFI_EVENT_CONNECTED) != 0;
}

void WifiStation::HandleScanResult() {
    uint16_t ap_num = 0;
    esp_wifi_scan_get_ap_num(&ap_num);

    ESP_LOGI(TAG, "Scan done: %d APs found (scan_only=%d)", ap_num, scan_only_mode_);

    if (ap_num == 0) {
        ESP_LOGI(TAG, "No AP found");
        is_scanning_ = false;
        // 移除自动重试，让上层（SmartConnect）控制重试逻辑
        return;
    }

    // 【优化】优先使用 PSRAM 分配扫描结果（每个 AP 记录约 80 字节）
    wifi_ap_record_t *ap_records = (wifi_ap_record_t *)heap_caps_malloc(
        ap_num * sizeof(wifi_ap_record_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ap_records == nullptr) {
        // PSRAM 分配失败，尝试 Internal RAM
        ap_records = (wifi_ap_record_t *)malloc(ap_num * sizeof(wifi_ap_record_t));
    }
    if (ap_records == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate memory for scan results");
        is_scanning_ = false;
        return;
    }

    esp_wifi_scan_get_ap_records(&ap_num, ap_records);

    // 更新扫描缓存
    UpdateScanCache(ap_records, ap_num);

    // 触发扫描完成回调（注意：回调在锁外执行，避免死锁）
    // 回调中可能调用 GetDeduplicatedCache() 等需要锁的函数
    if (on_scan_complete_) {
        std::vector<wifi_ap_record_t> cache_copy;
        {
            std::lock_guard<std::mutex> lock(scan_mutex_);
            cache_copy = scan_cache_;
        }
        on_scan_complete_(cache_copy);
    }

    // 仅扫描模式：只缓存结果，不自动连接
    if (scan_only_mode_) {
        // 打印扫描到的热点（仅前5个，避免日志过多）
        int print_count = std::min((int)ap_num, 5);
        for (int i = 0; i < print_count; i++) {
            ESP_LOGI(TAG, "  [%d] %s (RSSI: %d, CH: %d)",
                i + 1, (char*)ap_records[i].ssid, ap_records[i].rssi, ap_records[i].primary);
        }
        if (ap_num > 5) {
            ESP_LOGI(TAG, "  ... and %d more APs", ap_num - 5);
        }
        free(ap_records);
        is_scanning_ = false;  // 重要：重置扫描状态，否则后续扫描都会被跳过
        return;
    }

    // 排序（已在 UpdateScanCache 中完成）
    // 构建连接队列
    auto& ssid_manager = SsidManager::GetInstance();
    auto ssid_list = ssid_manager.GetSsidList();

    connect_queue_.clear();

    for (int i = 0; i < ap_num; i++) {
        auto& ap_record = ap_records[i];
        auto it = std::find_if(ssid_list.begin(), ssid_list.end(), [&ap_record](const SsidItem& item) {
            return strcmp((char *)ap_record.ssid, item.ssid.c_str()) == 0;
        });
        if (it != ssid_list.end()) {
            ESP_LOGI(TAG, "Found AP: %s, BSSID: %02x:%02x:%02x:%02x:%02x:%02x, RSSI: %d, Channel: %d, Authmode: %d",
                (char *)ap_record.ssid,
                ap_record.bssid[0], ap_record.bssid[1], ap_record.bssid[2],
                ap_record.bssid[3], ap_record.bssid[4], ap_record.bssid[5],
                ap_record.rssi, ap_record.primary, ap_record.authmode);
            WifiApRecord record;
            record.ssid = it->ssid;
            record.password = it->password;
            record.rssi = ap_record.rssi;
            record.channel = ap_record.primary;
            record.authmode = ap_record.authmode;
            memcpy(record.bssid, ap_record.bssid, 6);

            // 添加队列大小检查，防止无界增长
            if (connect_queue_.size() >= MAX_CONNECT_QUEUE) {
                ESP_LOGW(TAG, "Connect queue full (%u), skipping AP: %s", MAX_CONNECT_QUEUE, (char *)ap_record.ssid);
            } else {
                connect_queue_.push_back(record);
            }
        }
    }
    free(ap_records);

    if (connect_queue_.empty()) {
        ESP_LOGI(TAG, "No matched AP found");
        is_scanning_ = false;  // 重置扫描状态
        // 移除自动重试，让上层（SmartConnect）控制重试逻辑
        return;
    }

    // 按信号强度排序，优先连接最强的 AP（同名多 AP 场景）
    std::sort(connect_queue_.begin(), connect_queue_.end(),
              [](const WifiApRecord& a, const WifiApRecord& b) {
                  return a.rssi > b.rssi;
              });

    StartConnect();
}

void WifiStation::StartConnect() {
    auto ap_record = connect_queue_.front();
    connect_queue_.erase(connect_queue_.begin());
    ssid_ = ap_record.ssid;
    password_ = ap_record.password;

    if (on_connect_) {
        on_connect_(ssid_);
    }

    wifi_config_t wifi_config;
    bzero(&wifi_config, sizeof(wifi_config));
    strncpy((char *)wifi_config.sta.ssid, ap_record.ssid.c_str(), sizeof(wifi_config.sta.ssid) - 1);
    wifi_config.sta.ssid[sizeof(wifi_config.sta.ssid) - 1] = '\0';
    strncpy((char *)wifi_config.sta.password, ap_record.password.c_str(), sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.password[sizeof(wifi_config.sta.password) - 1] = '\0';
    // 同名多 AP 场景：锁定 BSSID 确保连接信号最强的那个
    wifi_config.sta.channel = ap_record.channel;
    memcpy(wifi_config.sta.bssid, ap_record.bssid, 6);
    wifi_config.sta.bssid_set = true;
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config failed: %s", esp_err_to_name(err));
        is_scanning_ = false;
        return;
    }

    reconnect_count_ = 0;
    err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(err));
    }
}

int8_t WifiStation::GetRssi() {
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) != ESP_OK) {
        return -127;  // 未连接时返回最低信号值
    }
    return ap_info.rssi;
}

uint8_t WifiStation::GetChannel() {
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) != ESP_OK) {
        return 0;
    }
    return ap_info.primary;
}

bool WifiStation::IsConnected() {
    return xEventGroupGetBits(event_group_) & WIFI_EVENT_CONNECTED;
}

void WifiStation::SetPowerSaveMode(bool enabled) {
    // MIN_MODEM 比 MAX_MODEM 更稳定：MAX_MODEM 在 beacon 间隔关 RF，
    esp_err_t err = esp_wifi_set_ps(enabled ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "WiFi set power save failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "WiFi power save: %s", enabled ? "MIN_MODEM" : "NONE");
    }
}

// Static event handler functions
void WifiStation::WifiEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    auto* this_ = static_cast<WifiStation*>(arg);

    if (event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "WIFI_EVENT_STA_START: try_connect=%d, scan_only=%d",
            this_->try_connect_mode_, this_->scan_only_mode_);

        // 启动后自动触发首次扫描（快速模式）
        ESP_LOGI(TAG, "Auto triggering first scan...");
        this_->is_scanning_ = true;
        wifi_scan_config_t scan_cfg = {};
        scan_cfg.scan_type = WIFI_SCAN_TYPE_ACTIVE;
        scan_cfg.scan_time.active.min = 80;
        scan_cfg.scan_time.active.max = 120;
        esp_err_t ret = esp_wifi_scan_start(&scan_cfg, false);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Auto scan failed: %s", esp_err_to_name(ret));
            this_->is_scanning_ = false;
        }
        if (this_->on_scan_begin_) {
            this_->on_scan_begin_();
        }
    } else if (event_id == WIFI_EVENT_SCAN_DONE) {
        this_->HandleScanResult();
    } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
        auto* event = static_cast<wifi_event_sta_disconnected_t*>(event_data);
        this_->last_disconnect_reason_ = event->reason;

        xEventGroupClearBits(this_->event_group_, WIFI_EVENT_CONNECTED | WIFI_EVENT_GOT_IP);
        xEventGroupSetBits(this_->event_group_, WIFI_EVENT_DISCONNECTED);

        // 配网连接模式：不自动重连，让 TryConnect 处理结果
        if (this_->try_connect_mode_) {
            ESP_LOGW(TAG, "TryConnect disconnected, reason: %d", event->reason);
            if (this_->on_disconnected_) {
                this_->on_disconnected_(event->reason);
            }
            return;
        }

        // 正常联网模式：指数退避持续重连（1s→2s→...→60s 封顶，无限重试）
        {
            // 创建重连定时器（首次）
            if (this_->reconnect_timer_ == nullptr) {
                esp_timer_create_args_t args = {
                    .callback = [](void* arg) {
                        esp_wifi_connect();
                    },
                    .arg = nullptr,
                    .dispatch_method = ESP_TIMER_TASK,
                    .name = "wifi_reconn",
                    .skip_unhandled_events = true,
                };
                esp_timer_create(&args, &this_->reconnect_timer_);
            }
            int idx = this_->reconnect_count_ < kReconnectBackoffCount
                      ? this_->reconnect_count_ : kReconnectBackoffCount - 1;
            uint64_t delay = kReconnectBackoffUs[idx];
            this_->reconnect_count_++;
            ESP_LOGI(TAG, "Reconnecting %s in %llu ms (attempt %d)",
                     this_->ssid_.c_str(), delay / 1000, this_->reconnect_count_);
            esp_timer_start_once(this_->reconnect_timer_, delay);

            // 触发断开回调（让 UI 可以显示断连状态）
            if (this_->on_disconnected_) {
                this_->on_disconnected_(event->reason);
            }
        }
    } else if (event_id == WIFI_EVENT_STA_CONNECTED) {
        ESP_LOGI(TAG, "WiFi connected to AP");
    } else if (event_id == WIFI_EVENT_STA_BEACON_TIMEOUT) {
        // 弱信号下路由器 beacon 丢失（不会触发 STA_DISCONNECTED）
        // 主动 disconnect 让现有指数退避状态机接管，避免设备假在线
        ESP_LOGW(TAG, "BEACON_TIMEOUT, forcing disconnect to trigger reconnect");
        if (!this_->try_connect_mode_) {
            esp_wifi_disconnect();
        }
    }
}

void WifiStation::IpEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    auto* this_ = static_cast<WifiStation*>(arg);

    if (event_id == IP_EVENT_STA_GOT_IP) {
        auto* event = static_cast<ip_event_got_ip_t*>(event_data);
        char ip_address[16];
        esp_ip4addr_ntoa(&event->ip_info.ip, ip_address, sizeof(ip_address));
        this_->ip_address_ = ip_address;
        ESP_LOGI(TAG, "Got IP: %s", this_->ip_address_.c_str());

        xEventGroupSetBits(this_->event_group_, WIFI_EVENT_CONNECTED | WIFI_EVENT_GOT_IP);
        if (this_->on_connected_) {
            this_->on_connected_(this_->ssid_);
        }
        this_->connect_queue_.clear();
        this_->reconnect_count_ = 0;
        // 连接成功，清理退避定时器
        if (this_->reconnect_timer_) {
            esp_timer_stop(this_->reconnect_timer_);
            esp_timer_delete(this_->reconnect_timer_);
            this_->reconnect_timer_ = nullptr;
        }
    } else if (event_id == IP_EVENT_STA_LOST_IP) {
        // DHCP lease 过期或路由器主动撤回 IP，主动断开走 STA_DISCONNECTED 退避路径
        ESP_LOGW(TAG, "LOST_IP, forcing disconnect to trigger reconnect");
        xEventGroupClearBits(this_->event_group_, WIFI_EVENT_GOT_IP);
        if (!this_->try_connect_mode_) {
            esp_wifi_disconnect();
        }
    }
}

// ========== 配网支持实现 ==========

void WifiStation::Start(WifiMode mode, const std::string& ap_ssid) {
    if (initialized_) {
        ESP_LOGW(TAG, "WiFi already initialized, switching mode");
        SetMode(mode, ap_ssid);
        return;
    }

    if (!InitWifiDriver(mode, ap_ssid)) {
        ESP_LOGE(TAG, "Failed to initialize WiFi driver");
        initialized_ = false;
        return;
    }
    initialized_ = true;
    current_mode_ = mode;
    ap_ssid_ = ap_ssid;

    ESP_LOGI(TAG, "WiFi started in %s mode", mode == WifiMode::APSTA ? "APSTA" : "STA");
}

void WifiStation::SetMode(WifiMode mode, const std::string& ap_ssid) {
    if (!initialized_) {
        ESP_LOGW(TAG, "WiFi not initialized, call Start() first");
        Start(mode, ap_ssid);
        return;
    }

    if (mode == current_mode_ && (mode == WifiMode::STA || ap_ssid == ap_ssid_)) {
        ESP_LOGD(TAG, "Already in requested mode");
        return;
    }

    ESP_LOGI(TAG, "Switching WiFi mode: %s -> %s",
        current_mode_ == WifiMode::APSTA ? "APSTA" : "STA",
        mode == WifiMode::APSTA ? "APSTA" : "STA");

    // 停止扫描
    esp_wifi_scan_stop();

    if (mode == WifiMode::APSTA) {
        // 切换到 APSTA 模式
#if CONFIG_ESP_WIFI_SOFTAP_SUPPORT
        ConfigureApInterface(ap_ssid);
        esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to set APSTA mode: %s", esp_err_to_name(err));
            return;
        }

        // 配置 AP SSID（关键：设置热点名称）
        if (!ap_ssid.empty()) {
            wifi_config_t ap_config = {};
            strncpy((char*)ap_config.ap.ssid, ap_ssid.c_str(), sizeof(ap_config.ap.ssid) - 1);
            ap_config.ap.ssid_len = ap_ssid.length();
            ap_config.ap.max_connection = 4;
            ap_config.ap.authmode = WIFI_AUTH_OPEN;
            err = esp_wifi_set_config(WIFI_IF_AP, &ap_config);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to set AP config: %s", esp_err_to_name(err));
            } else {
                ESP_LOGI(TAG, "AP SSID configured: %s", ap_ssid.c_str());
            }
        }
#else
        ESP_LOGW(TAG, "SoftAP not supported in this build, falling back to STA mode");
        esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to set STA mode: %s", esp_err_to_name(err));
            return;
        }
#endif
        ap_ssid_ = ap_ssid;
    } else {
        // 切换到纯 STA 模式
        // 注意：不销毁 AP netif，只切换 WiFi 模式
        // esp_netif_destroy 在运行时调用可能导致崩溃（use after free）
        // AP netif 会在下次切换回 APSTA 时复用
        esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to set STA mode: %s", esp_err_to_name(err));
            return;
        }
        ap_ssid_.clear();
    }

    current_mode_ = mode;
    ESP_LOGI(TAG, "WiFi mode switched to %s", mode == WifiMode::APSTA ? "APSTA" : "STA");
}

bool WifiStation::InitWifiDriver(WifiMode mode, const std::string& ap_ssid) {
    // 初始化 TCP/IP 协议栈
    esp_err_t ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(ret));
        return false;
    }

    // 创建 STA 网络接口
    station_netif_ = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (station_netif_ == nullptr) {
        station_netif_ = esp_netif_create_default_wifi_sta();
        if (station_netif_ == nullptr) {
            ESP_LOGE(TAG, "Failed to create STA netif");
            return false;
        }
    }

    // 如果是 APSTA 模式，同时创建 AP 接口
#if CONFIG_ESP_WIFI_SOFTAP_SUPPORT
    if (mode == WifiMode::APSTA) {
        ConfigureApInterface(ap_ssid);
    }
#endif

    // 初始化 WiFi 驱动
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.nvs_enable = false;
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // 注册事件处理器
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &WifiStation::WifiEventHandler,
                                                        this,
                                                        &instance_any_id_));
    // ESP_EVENT_ANY_ID：handler 同时收到 STA_GOT_IP 和 STA_LOST_IP（DHCP lease 过期场景）
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &WifiStation::IpEventHandler,
                                                        this,
                                                        &instance_got_ip_));

    // 设置 WiFi 模式
#if CONFIG_ESP_WIFI_SOFTAP_SUPPORT
    wifi_mode_t wifi_mode = (mode == WifiMode::APSTA) ? WIFI_MODE_APSTA : WIFI_MODE_STA;
#else
    wifi_mode_t wifi_mode = WIFI_MODE_STA;
    if (mode == WifiMode::APSTA) {
        ESP_LOGW(TAG, "SoftAP not supported, using STA mode only");
    }
#endif
    ESP_ERROR_CHECK(esp_wifi_set_mode(wifi_mode));

#if CONFIG_ESP_WIFI_SOFTAP_SUPPORT
    // 如果是 APSTA 模式，配置 AP
    if (mode == WifiMode::APSTA && !ap_ssid.empty()) {
        wifi_config_t ap_config = {};
        strncpy((char*)ap_config.ap.ssid, ap_ssid.c_str(), sizeof(ap_config.ap.ssid) - 1);
        ap_config.ap.ssid_len = ap_ssid.length();
        ap_config.ap.max_connection = 4;
        ap_config.ap.authmode = WIFI_AUTH_OPEN;
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    }
#endif

    ESP_ERROR_CHECK(esp_wifi_start());

    if (max_tx_power_ != 0) {
        esp_err_t pwr_err = esp_wifi_set_max_tx_power(max_tx_power_);
        if (pwr_err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to set max TX power %d: %s", max_tx_power_, esp_err_to_name(pwr_err));
        }
    }

    // 创建扫描定时器（用于连接失败后重试）
    if (timer_handle_ == nullptr) {
        esp_timer_create_args_t timer_args = {
            .callback = [](void* arg) {
                auto* self = static_cast<WifiStation*>(arg);
                if (!self->try_connect_mode_ && !self->is_scanning_) {
                    // 非配网连接模式，用于连接失败后重试
                    self->is_scanning_ = true;
                    wifi_scan_config_t cfg = {};
                    cfg.scan_type = WIFI_SCAN_TYPE_ACTIVE;
                    cfg.scan_time.active.min = 80;
                    cfg.scan_time.active.max = 120;
                    esp_err_t ret = esp_wifi_scan_start(&cfg, false);
                    if (ret != ESP_OK) {
                        self->is_scanning_ = false;
                    }
                }
            },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "WiFiScanTimer",
            .skip_unhandled_events = true
        };
        ESP_ERROR_CHECK(esp_timer_create(&timer_args, &timer_handle_));
    }

    return true;
}

#if CONFIG_ESP_WIFI_SOFTAP_SUPPORT
void WifiStation::ConfigureApInterface(const std::string& ap_ssid) {
    // 创建 AP 网络接口
    ap_netif_ = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap_netif_ == nullptr) {
        ap_netif_ = esp_netif_create_default_wifi_ap();
    }

    // 配置 AP IP 地址
    esp_netif_ip_info_t ip_info;
    IP4_ADDR(&ip_info.ip, 192, 168, 4, 1);
    IP4_ADDR(&ip_info.gw, 192, 168, 4, 1);
    IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);

    esp_netif_dhcps_stop(ap_netif_);
    esp_netif_set_ip_info(ap_netif_, &ip_info);
    esp_netif_dhcps_start(ap_netif_);

    ESP_LOGI(TAG, "AP interface configured: %s @ 192.168.4.1", ap_ssid.c_str());
}
#endif

WifiConnectResult WifiStation::TryConnect(const std::string& ssid, const std::string& password, int timeout_ms) {
    WifiConnectResult result;
    result.ssid = ssid;

    if (!initialized_) {
        ESP_LOGE(TAG, "WiFi not initialized");
        result.fail_reason = 255;  // 未初始化
        return result;
    }

    if (ssid.empty()) {
        ESP_LOGE(TAG, "SSID cannot be empty");
        result.fail_reason = 254;  // 无效参数
        return result;
    }

    ESP_LOGI(TAG, "TryConnect: %s (timeout: %dms)", ssid.c_str(), timeout_ms);

    // 进入配网连接模式
    try_connect_mode_ = true;
    last_disconnect_reason_ = 0;

    // 停止扫描
    esp_wifi_scan_stop();

    // 清除之前的连接状态
    xEventGroupClearBits(event_group_, WIFI_EVENT_CONNECTED | WIFI_EVENT_DISCONNECTED | WIFI_EVENT_GOT_IP);

    // 配置 WiFi 连接
    wifi_config_t wifi_config = {};
    strncpy((char*)wifi_config.sta.ssid, ssid.c_str(), sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char*)wifi_config.sta.password, password.c_str(), sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wifi_config.sta.failure_retry_cnt = 1;  // 只尝试一次

    esp_err_t ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config failed: %s", esp_err_to_name(ret));
        try_connect_mode_ = false;
        result.fail_reason = 252;
        return result;
    }

    // 保存当前 SSID
    ssid_ = ssid;
    password_ = password;

    // 开始连接
    ret = esp_wifi_connect();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(ret));
        try_connect_mode_ = false;
        result.fail_reason = 253;  // 连接启动失败
        return result;
    }

    // 等待连接结果
    EventBits_t bits = xEventGroupWaitBits(
        event_group_,
        WIFI_EVENT_GOT_IP | WIFI_EVENT_DISCONNECTED,
        pdTRUE,  // 清除位
        pdFALSE, // 等待任意一个
        pdMS_TO_TICKS(timeout_ms)
    );

    try_connect_mode_ = false;

    if (bits & WIFI_EVENT_GOT_IP) {
        result.success = true;
        result.ip_address = ip_address_;

        // 获取 RSSI
        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            result.rssi = ap_info.rssi;
        }

        ESP_LOGI(TAG, "TryConnect success: %s, IP: %s, RSSI: %d",
            ssid.c_str(), result.ip_address.c_str(), result.rssi);

    } else {
        result.fail_reason = last_disconnect_reason_;
        ESP_LOGW(TAG, "TryConnect failed: %s, reason: %d", ssid.c_str(), result.fail_reason);
    }

    return result;
}

WifiConnectResult WifiStation::TryConnectAndSave(const std::string& ssid, const std::string& password, int timeout_ms) {
    // 先验证连接
    auto result = TryConnect(ssid, password, timeout_ms);

    // 成功则保存凭证
    if (result.success) {
        ESP_LOGI(TAG, "TryConnectAndSave: saving credentials for %s", ssid.c_str());
        SsidManager::GetInstance().AddSsid(ssid, password);
    }

    return result;
}

void WifiStation::OnDisconnected(std::function<void(uint8_t reason)> callback) {
    on_disconnected_ = callback;
}

void WifiStation::ProcessScanResults(std::vector<wifi_ap_record_t>& records) {
    SortByRssi(records);
    DeduplicateBySsid(records);
}
