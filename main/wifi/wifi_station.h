#ifndef _WIFI_STATION_H_
#define _WIFI_STATION_H_

#include <string>
#include <vector>
#include <functional>
#include <mutex>
#include <atomic>

#include <esp_event.h>
#include <esp_timer.h>
#include <esp_netif.h>
#include <esp_wifi_types_generic.h>

#define MAX_CONNECT_QUEUE 10
#define MAX_SCAN_CACHE 50

// 匹配凭证后的 AP 记录
struct WifiApRecord {
    std::string ssid;
    std::string password;
    int8_t rssi;
    int channel;
    wifi_auth_mode_t authmode;
    uint8_t bssid[6];
};

// 扫描配置
struct WifiScanConfig {
    int cache_valid_ms = 60000;        // 缓存有效期 (毫秒) - 60秒，蓝牙配网期间足够使用
    int max_ap_count = 20;             // 最大缓存 AP 数量
    bool auto_deduplicate = true;      // 自动去重同名 SSID
};

// WiFi 工作模式
enum class WifiMode {
    STA,      // 纯 Station 模式（联网）
    APSTA     // AP + Station 混合模式（配网）
};

// 连接结果
struct WifiConnectResult {
    bool success = false;
    std::string ssid;
    std::string ip_address;
    int8_t rssi = 0;
    uint8_t fail_reason = 0;  // wifi_err_reason_t
};

class WifiStation {
public:
    static WifiStation& GetInstance();

    // ========== 扫描管理 (新增) ==========

    // 触发扫描（非阻塞，结果通过回调或 GetScanCache 获取）
    void TriggerScan();

    // 获取扫描缓存（原始数据，已按 RSSI 排序）
    std::vector<wifi_ap_record_t> GetScanCache();

    // 获取去重后的扫描缓存（同名 SSID 只保留信号最强）
    std::vector<wifi_ap_record_t> GetDeduplicatedCache();

    // 获取匹配凭证的 AP 列表（核心方法：缓存 + 排序 + 去重 + 凭证匹配）
    // force_refresh: true=强制重新扫描，false=使用缓存（如果有效）
    std::vector<WifiApRecord> GetMatchedAccessPoints(bool force_refresh = false);

    // 检查缓存是否有效
    bool IsCacheValid() const;

    // 获取上次扫描时间 (毫秒时间戳)
    int64_t GetLastScanTime() const { return last_scan_time_ms_; }

    // 获取缓存的 AP 数量
    size_t GetCacheCount() const;

    // 清除扫描缓存
    void ClearScanCache();

    // 设置扫描配置
    void SetScanConfig(const WifiScanConfig& config);

    // 获取当前扫描配置
    WifiScanConfig GetScanConfig() const { return scan_config_; }

    // 扫描完成回调（供 Blufi/ConfigAp 等外部模块使用）
    void OnScanComplete(std::function<void(const std::vector<wifi_ap_record_t>&)> callback);

    void AddAuth(const std::string &&ssid, const std::string &&password);
    void Start();
    void Stop();
    bool IsConnected();
    bool WaitForConnected(int timeout_ms = 10000);
    int8_t GetRssi();
    std::string GetSsid() const { return ssid_; }
    std::string GetIpAddress() const { return ip_address_; }
    uint8_t GetChannel();
    void SetPowerSaveMode(bool enabled);

    void OnConnect(std::function<void(const std::string& ssid)> on_connect);
    void OnConnected(std::function<void(const std::string& ssid)> on_connected);
    void OnScanBegin(std::function<void()> on_scan_begin);

    // ========== 配网支持 (新增) ==========

    // 启动 WiFi（支持不同模式）
    // mode: STA=纯联网模式, APSTA=配网模式（同时开启热点）
    // ap_ssid: APSTA 模式下的热点名称
    void Start(WifiMode mode, const std::string& ap_ssid = "");

    // 运行时切换模式（无需 Stop/Start）
    void SetMode(WifiMode mode, const std::string& ap_ssid = "");

    // 获取当前模式
    WifiMode GetMode() const { return current_mode_; }

    // 配网专用：单次连接尝试（不自动重连，用于验证凭证）
    // 返回连接结果，包含成功/失败状态和详细信息
    WifiConnectResult TryConnect(const std::string& ssid, const std::string& password, int timeout_ms = 10000);

    // 配网专用：验证连接并保存凭证（成功后自动保存到 SsidManager）
    // 返回连接结果，成功时凭证已保存
    WifiConnectResult TryConnectAndSave(const std::string& ssid, const std::string& password, int timeout_ms = 10000);

    // 配网专用：断开连接回调
    void OnDisconnected(std::function<void(uint8_t reason)> callback);

    // 是否已初始化
    bool IsInitialized() const { return initialized_; }

    // 是否正在扫描
    bool IsScanning() const { return is_scanning_; }

    // 设置仅扫描模式（配网用，启用后只缓存扫描结果，不自动连接）
    void SetScanOnlyMode(bool enabled) { scan_only_mode_ = enabled; }

    // 获取 AP 网络接口（APSTA 模式下有效）
    esp_netif_t* GetApNetif() const { return ap_netif_; }

    // 获取 STA 网络接口
    esp_netif_t* GetStaNetif() const { return station_netif_; }

    // 处理扫描结果：排序 + 去重（供配网模块在过渡期使用）
    // 注意：配网模块应优先使用 GetDeduplicatedCache() 获取已处理的缓存
    static void ProcessScanResults(std::vector<wifi_ap_record_t>& records);

private:
    WifiStation();
    ~WifiStation();
    WifiStation(const WifiStation&) = delete;
    WifiStation& operator=(const WifiStation&) = delete;

    // 连接相关
    EventGroupHandle_t event_group_;
    esp_timer_handle_t timer_handle_ = nullptr;
    esp_event_handler_instance_t instance_any_id_ = nullptr;
    esp_event_handler_instance_t instance_got_ip_ = nullptr;
    esp_netif_t* station_netif_ = nullptr;
    std::string ssid_;
    std::string password_;
    std::string ip_address_;
    int8_t max_tx_power_;
    uint8_t remember_bssid_;
    int reconnect_count_ = 0;
    esp_timer_handle_t reconnect_timer_ = nullptr;  // 重连退避定时器
    // P0：connect_queue_ 跨核访问保护 · WiFi event task(Core 0) 写读 + main task Stop() 写
    mutable std::mutex connect_queue_mutex_;
    std::vector<WifiApRecord> connect_queue_;

    // 连接回调
    std::function<void(const std::string& ssid)> on_connect_;
    std::function<void(const std::string& ssid)> on_connected_;
    std::function<void()> on_scan_begin_;

    // 扫描缓存
    mutable std::mutex scan_mutex_;
    std::vector<wifi_ap_record_t> scan_cache_;
    int64_t last_scan_time_ms_ = 0;
    WifiScanConfig scan_config_;
    std::atomic<bool> is_scanning_{false};

    // 扫描回调
    std::function<void(const std::vector<wifi_ap_record_t>&)> on_scan_complete_;

    // ========== 配网支持成员变量 ==========
    bool initialized_ = false;              // WiFi 是否已初始化
    WifiMode current_mode_ = WifiMode::STA; // 当前工作模式
    esp_netif_t* ap_netif_ = nullptr;       // AP 网络接口（APSTA 模式）
    std::string ap_ssid_;                   // AP 热点名称

    // 配网连接状态（TryConnect 使用）
    bool try_connect_mode_ = false;         // 是否处于 TryConnect 模式
    bool scan_only_mode_ = false;           // 仅扫描模式（配网用，不自动连接）
    uint8_t last_disconnect_reason_ = 0;    // 最后断开原因

    // 断开回调
    std::function<void(uint8_t reason)> on_disconnected_;

    // 内部方法
    void HandleScanResult();
    void StartConnect();
    void UpdateScanCache(wifi_ap_record_t* records, uint16_t count);
    bool InitWifiDriver(WifiMode mode, const std::string& ap_ssid);  // 初始化 WiFi 驱动
#if CONFIG_ESP_WIFI_SOFTAP_SUPPORT
    void ConfigureApInterface(const std::string& ap_ssid);           // 配置 AP 接口
#endif

    // 扫描结果处理（内部使用）
    static void SortByRssi(std::vector<wifi_ap_record_t>& records);
    static void DeduplicateBySsid(std::vector<wifi_ap_record_t>& records);

    static void WifiEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);
    static void IpEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);
};

#endif // _WIFI_STATION_H_
