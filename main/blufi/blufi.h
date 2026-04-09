#ifndef _BLUFI_H_
#define _BLUFI_H_
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_timer.h>
#include <string>
#include <vector>
#include <mutex>

#include "esp_blufi_api.h"

#include "esp_attr.h"

class Blufi
{
public:
    static Blufi& GetInstance();
    Blufi(/* args */);
    ~Blufi();

    void SetSsidPrefix(const std::string& prefix);
    void SsidSave(std::function<void(const std::string& ssid, const std::string& password)> callback);
    void Save(const std::string& ssid, const std::string& password);

    void WifiSettingOk(std::function<void()> callback);
    void SettingOk();
    void WifiSettingFail(std::function<void(const std::string& message)> callback);
    void SettingFail(const std::string& message);
    void SendData(const char *data, int len);

    static std::string GetBindingCode();

protected:
    /* data */
    std::function<void(const std::string& ssid, const std::string& password)> ssid_save_;
    std::function<void()> wifi_setting_ok_;
    std::function<void(const std::string& message)> wifi_setting_fail_;

    static bool gl_sta_connected_;
    static bool gl_sta_is_connecting_;
    static bool gl_sta_got_ip_;
    static uint8_t gl_sta_bssid_[6];
    static uint8_t gl_sta_ssid_[32];
    static int gl_sta_ssid_len_;
    static bool ble_is_connected_;
    static esp_blufi_extra_info_t gl_sta_conn_info;
    static EventGroupHandle_t wifi_event_group_;
    static wifi_config_t sta_config;
    static wifi_config_t ap_config;
    static std::string binding_code_;
    static std::string ssid_prefix_;
    uint8_t wifi_retry_ = 0;

    static void BlufiCallback(esp_blufi_cb_event_t event, esp_blufi_cb_param_t *param);
    void InitialiseWifi(void);
    void RecordWifiConnInfo(int rssi, uint8_t reason);
    static void WifiEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);
    void WifiConnect();
    bool WifiReconnect();
    int SoftapGetCurrentConnectionNumber();
    static void IpEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);

};

#endif  // _BLUFI_H_