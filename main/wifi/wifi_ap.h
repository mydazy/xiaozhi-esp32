#ifndef _WIFI_AP_H_
#define _WIFI_AP_H_

#include <string>
#include <functional>

#include <esp_http_server.h>
#include <esp_event.h>
#include <esp_timer.h>
#include <esp_netif.h>
#include <esp_wifi_types_generic.h>

#include "dns_server.h"

/**
 * WifiAp - WiFi AP 纯数据通道
 *
 * 职责：HTTP 服务、DNS 劫持（仅数据传输）
 * 不处理业务逻辑：凭证验证、保存由业务层（wifi_board.cc）处理
 *
 * 统一接口（与 Blufi 一致）：
 * - Start(device_name, language) 启动通道
 * - Stop() 停止通道
 * - SetCredentialValidator(validator) 设置凭证验证器
 * - OnConfigSuccess(callback) 设置成功回调
 *
 * 配网流程：
 * 1. 业务层设置 SetCredentialValidator() 和 OnConfigSuccess()
 * 2. 业务层启动 WifiStation（APSTA 模式）
 * 3. 业务层调用 Start()
 * 4. 用户输入凭证 → 通道调用验证器 → 返回结果给用户
 * 5. 验证成功 → 用户点击重启 → 调用成功回调
 */
class WifiAp {
public:
    static WifiAp& GetInstance();

    // 启动 WiFi AP（需要 WifiStation 已启动）
    void Start(const std::string& ssid_prefix, const std::string& language = "zh-CN");

    // 停止 WiFi AP
    void Stop();

    std::string GetSsid();
    std::string GetWebServerUrl();

    // ===== 统一接口（与 Blufi 一致）=====

    // 设置凭证验证器（业务层提供验证逻辑，通道调用）
    // 返回 true=验证成功，false=验证失败（error_message 会显示给用户）
    void SetCredentialValidator(
        std::function<bool(const std::string& ssid,
                           const std::string& password,
                           std::string& error_message)> validator);

    // 设置配网成功回调（验证成功且用户点击重启后调用）
    void OnConfigSuccess(std::function<void()> callback);

    bool IsStarted() const { return started_; }

    // SmartConfig 支持
    void StartSmartConfig();

    // Delete copy constructor and assignment operator
    WifiAp(const WifiAp&) = delete;
    WifiAp& operator=(const WifiAp&) = delete;

private:
    // Private constructor
    WifiAp();
    ~WifiAp();

    DnsServer dns_server_;
    httpd_handle_t server_ = NULL;
    std::string ssid_prefix_;
    std::string language_;
    bool started_ = false;

    // 凭证验证器（业务层提供）
    std::function<bool(const std::string&, const std::string&, std::string&)> credential_validator_;

    // 配网成功回调
    std::function<void()> on_config_success_;

    // 配置 AP 接口（不初始化 WiFi 驱动）
    void ConfigureApInterface();
    void StartWebServer();

    // WiFi 事件回调（仅处理 AP 相关事件）
    static void OnApStaConnected(wifi_event_ap_staconnected_t* event);
    static void OnApStaDisconnected(wifi_event_ap_stadisconnected_t* event);
    static void OnScanDone();

    // SmartConfig 事件处理
    static void SmartConfigEventHandler(void* arg, esp_event_base_t event_base,
                                        int32_t event_id, void* event_data);
    esp_event_handler_instance_t sc_event_instance_ = nullptr;
};

#endif // _WIFI_AP_H_