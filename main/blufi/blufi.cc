#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <functional>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#if CONFIG_BT_CONTROLLER_ENABLED || !CONFIG_BT_NIMBLE_ENABLED
#include "esp_bt.h"
#endif
#include "esp_blufi_api.h"
#include "esp_blufi.h"

#if CONFIG_GATTC_ENABLE
#include "esp_gap_ble_api.h"
#include "esp_gattc_api.h"
#endif

#ifdef CONFIG_BT_BLUEDROID_ENABLED
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#endif

#ifdef CONFIG_BT_NIMBLE_ENABLED
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "console/console.h"
#endif

#include "blufi.h"
#include "blufi_init.h"
#include "esp_err.h"
#include "system_info.h"  // 用于获取MAC地址



#define EXAMPLE_WIFI_CONNECTION_MAXIMUM_RETRY   2
#define EXAMPLE_INVALID_REASON                  255
#define EXAMPLE_INVALID_RSSI                    -128
#define WIFI_LIST_NUM   10
#define CONNECTED_BIT BIT0

bool Blufi::gl_sta_connected_ = false;
bool Blufi::gl_sta_is_connecting_ = false;
bool Blufi::gl_sta_got_ip_ = false;
uint8_t Blufi::gl_sta_bssid_[6] = {0};
uint8_t Blufi::gl_sta_ssid_[32] = {0};
int Blufi::gl_sta_ssid_len_ = 0;
bool Blufi::ble_is_connected_ = false;
esp_blufi_extra_info_t Blufi::gl_sta_conn_info;
EventGroupHandle_t Blufi::wifi_event_group_;
wifi_config_t Blufi::sta_config;
wifi_config_t Blufi::ap_config;
std::string Blufi::binding_code_ = "";
std::string Blufi::ssid_prefix_ = "MyDazy"; 


Blufi& Blufi::GetInstance() {
    static Blufi instance;
    return instance;
}

Blufi::Blufi() {
    esp_err_t ret;

    InitialiseWifi();

#if CONFIG_BT_CONTROLLER_ENABLED || !CONFIG_BT_NIMBLE_ENABLED
    ret = esp_blufi_controller_init();
    if (ret) {
        BLUFI_ERROR("%s BLUFI controller init failed: %s\n", __func__, esp_err_to_name(ret));
        return;
    }


    static esp_blufi_callbacks_t example_callbacks = {
        .event_cb = BlufiCallback,
        .negotiate_data_handler = blufi_dh_negotiate_data_handler,
        .encrypt_func = blufi_aes_encrypt,
        .decrypt_func = blufi_aes_decrypt,
        .checksum_func = blufi_crc_checksum,
    };
    ret = esp_blufi_host_and_cb_init(&example_callbacks);
    if (ret) {
        BLUFI_ERROR("%s initialise failed: %s", __func__, esp_err_to_name(ret));
        return;
    }

    #ifdef CONFIG_BT_NIMBLE_ENABLED
    // 使用统一方法获取MAC地址后4位
    std::string mac_last4 = SystemInfo::GetMacAddressLast4();
    char name[32] = {0};
    // 使用动态前缀生成蓝牙名称，支持中文
    snprintf(name, sizeof(name), "%s_%s", ssid_prefix_.c_str(), mac_last4.c_str());
    ret = ble_svc_gap_device_name_set(name);
    if (ret == ESP_OK) {
        BLUFI_INFO("Device name set to: %s", name);
    } else {
        BLUFI_ERROR("Failed to set device name: %s", esp_err_to_name(ret));
    }
    #endif

    BLUFI_INFO("BLUFI VERSION %04x", esp_blufi_get_version());
#endif
}

Blufi::~Blufi() {

}

void Blufi::SetSsidPrefix(const std::string& prefix) {
    ssid_prefix_ = prefix;

    #ifdef CONFIG_BT_NIMBLE_ENABLED
    // 更新蓝牙设备名称 - 使用统一方法
    std::string mac_last4 = SystemInfo::GetMacAddressLast4();
    char name[32] = {0};
    snprintf(name, sizeof(name), "%s_%s", ssid_prefix_.c_str(), mac_last4.c_str());

    esp_err_t ret = ble_svc_gap_device_name_set(name);
    if (ret == ESP_OK) {
        BLUFI_INFO("Device name updated to: %s", name);
    } else {
        BLUFI_ERROR("Failed to update device name: %s", esp_err_to_name(ret));
    }
    #endif
}

void Blufi::SsidSave(std::function<void(const std::string& ssid, const std::string& password)> callback)
{
    ssid_save_ = callback;
}

void Blufi::Save(const std::string& ssid, const std::string& password) {
    if (ssid_save_ != nullptr) {
        ssid_save_(ssid, password);
    }
}

void Blufi::WifiSettingOk(std::function<void()> callback)
{
    wifi_setting_ok_ = callback;
}

void Blufi::SettingOk()
{
    if(wifi_setting_ok_ != nullptr){
        wifi_setting_ok_();
    }
}

void Blufi::WifiSettingFail(std::function<void(const std::string& message)> callback)
{
    wifi_setting_fail_ = callback;
}

void Blufi::SettingFail(const std::string& message)
{
    if(wifi_setting_fail_ != nullptr){
        wifi_setting_fail_(message);
    }
}

void Blufi::SendData(const char *data, int len){
    BLUFI_INFO("send custom data");
    esp_blufi_send_custom_data((uint8_t *)data, len);
}

void Blufi::BlufiCallback(esp_blufi_cb_event_t event, esp_blufi_cb_param_t *param)
{
    /* actually, should post to blufi_task handle the procedure,
     * now, as a example, we do it more simply */
    auto& self = Blufi::GetInstance();
    switch (event) {
    case ESP_BLUFI_EVENT_INIT_FINISH:
        BLUFI_INFO("BLUFI init finish");

        esp_blufi_adv_start();
        break;
    case ESP_BLUFI_EVENT_DEINIT_FINISH:
        BLUFI_INFO("BLUFI deinit finish");
        break;
    case ESP_BLUFI_EVENT_BLE_CONNECT:
        BLUFI_INFO("BLUFI ble connect");
        ble_is_connected_ = true;
        esp_blufi_adv_stop();
        blufi_security_init();

        // 蓝牙连接成功后，立即发送MAC地址给小程序
//        {
//            std::string mac_address = SystemInfo::GetMacAddress();
//            BLUFI_INFO("发送MAC地址给小程序: %s", mac_address.c_str());
//            esp_blufi_send_custom_data((uint8_t*)mac_address.c_str(), mac_address.length());
//        }
        break;
    case ESP_BLUFI_EVENT_BLE_DISCONNECT:
        BLUFI_INFO("BLUFI ble disconnect");
        ble_is_connected_ = false;
        blufi_security_deinit();
        esp_blufi_adv_start();
        break;
    case ESP_BLUFI_EVENT_SET_WIFI_OPMODE:
        BLUFI_INFO("BLUFI Set WIFI opmode %d", param->wifi_mode.op_mode);
        ESP_ERROR_CHECK( esp_wifi_set_mode(param->wifi_mode.op_mode) );
        break;
    case ESP_BLUFI_EVENT_REQ_CONNECT_TO_AP:
    {
        BLUFI_INFO("BLUFI requset wifi connect to AP");
        /* there is no wifi callback when the device has already connected to this wifi
        so disconnect wifi before connection.
        */         
        
        esp_wifi_disconnect();
        self.WifiConnect();
        break;
    }
    case ESP_BLUFI_EVENT_REQ_DISCONNECT_FROM_AP:
        BLUFI_INFO("BLUFI requset wifi disconnect from AP");
         esp_wifi_disconnect();
        break;
    case ESP_BLUFI_EVENT_REPORT_ERROR:
        BLUFI_ERROR("BLUFI report error, error code %d", param->report_error.state);
        esp_blufi_send_error_info(param->report_error.state);
        break;
    case ESP_BLUFI_EVENT_GET_WIFI_STATUS: {
        wifi_mode_t mode;
        esp_blufi_extra_info_t info;

        esp_wifi_get_mode(&mode);

        if (gl_sta_connected_) {
            memset(&info, 0, sizeof(esp_blufi_extra_info_t));
            memcpy(info.sta_bssid, gl_sta_bssid_, 6);
            info.sta_bssid_set = true;
            info.sta_ssid = gl_sta_ssid_;
            info.sta_ssid_len = gl_sta_ssid_len_;
            esp_blufi_send_wifi_conn_report(mode, gl_sta_got_ip_ ? ESP_BLUFI_STA_CONN_SUCCESS : ESP_BLUFI_STA_NO_IP, self.SoftapGetCurrentConnectionNumber(), &info);
        } else if (gl_sta_is_connecting_) {
            esp_blufi_send_wifi_conn_report(mode, ESP_BLUFI_STA_CONNECTING, self.SoftapGetCurrentConnectionNumber(), &gl_sta_conn_info);
        } else {
            esp_blufi_send_wifi_conn_report(mode, ESP_BLUFI_STA_CONN_FAIL, self.SoftapGetCurrentConnectionNumber(), &gl_sta_conn_info);
        }
        BLUFI_INFO("BLUFI get wifi status from AP");

        break;
    }
    case ESP_BLUFI_EVENT_RECV_SLAVE_DISCONNECT_BLE:
        BLUFI_INFO("blufi close a gatt connection");
        esp_blufi_disconnect();
        break;
    case ESP_BLUFI_EVENT_DEAUTHENTICATE_STA:
        /* TODO */
        break;
	case ESP_BLUFI_EVENT_RECV_STA_BSSID:
        memcpy(sta_config.sta.bssid, param->sta_bssid.bssid, 6);
        sta_config.sta.bssid_set = 1;
        esp_wifi_set_config(WIFI_IF_STA, &sta_config);
        BLUFI_INFO("Recv STA BSSID %s", param->sta_bssid.bssid);
        break;
	case ESP_BLUFI_EVENT_RECV_STA_SSID:
        strncpy((char *)sta_config.sta.ssid, (char *)param->sta_ssid.ssid, param->sta_ssid.ssid_len);
        sta_config.sta.ssid[param->sta_ssid.ssid_len] = '\0';
        esp_wifi_set_config(WIFI_IF_STA, &sta_config);
        BLUFI_INFO("Recv STA SSID %s (len: %d)", sta_config.sta.ssid, param->sta_ssid.ssid_len);
        break;
	case ESP_BLUFI_EVENT_RECV_STA_PASSWD:
        strncpy((char *)sta_config.sta.password, (char *)param->sta_passwd.passwd, param->sta_passwd.passwd_len);
        sta_config.sta.password[param->sta_passwd.passwd_len] = '\0';
        esp_wifi_set_config(WIFI_IF_STA, &sta_config);
        BLUFI_INFO("Recv STA PASSWORD (configured)");  // 不打印密码长度，保护隐私
        break;
	case ESP_BLUFI_EVENT_RECV_SOFTAP_SSID:
         strncpy((char *)ap_config.ap.ssid, (char *)param->softap_ssid.ssid, param->softap_ssid.ssid_len);
         ap_config.ap.ssid[param->softap_ssid.ssid_len] = '\0';
         ap_config.ap.ssid_len = param->softap_ssid.ssid_len;
         esp_wifi_set_config(WIFI_IF_AP, &ap_config);
        BLUFI_INFO("Recv SOFTAP SSID %s, ssid len %d", param->softap_ssid.ssid, param->softap_ssid.ssid_len);
        break;
	case ESP_BLUFI_EVENT_RECV_SOFTAP_PASSWD:
         strncpy((char *)ap_config.ap.password, (char *)param->softap_passwd.passwd, param->softap_passwd.passwd_len);
         ap_config.ap.password[param->softap_passwd.passwd_len] = '\0';
         esp_wifi_set_config(WIFI_IF_AP, &ap_config);
        BLUFI_INFO("Recv SOFTAP PASSWORD %s len = %d", param->softap_passwd.passwd, param->softap_passwd.passwd_len);
        break;
	case ESP_BLUFI_EVENT_RECV_SOFTAP_MAX_CONN_NUM:
        if (param->softap_max_conn_num.max_conn_num > 4) {
            return;
        }
         ap_config.ap.max_connection = param->softap_max_conn_num.max_conn_num;
         esp_wifi_set_config(WIFI_IF_AP, &ap_config);
        BLUFI_INFO("Recv SOFTAP MAX CONN NUM %d", param->softap_max_conn_num.max_conn_num);
        break;
	case ESP_BLUFI_EVENT_RECV_SOFTAP_AUTH_MODE:
        if (param->softap_auth_mode.auth_mode >= WIFI_AUTH_MAX) {
            return;
        }
         ap_config.ap.authmode = param->softap_auth_mode.auth_mode;
         esp_wifi_set_config(WIFI_IF_AP, &ap_config);
        BLUFI_INFO("Recv SOFTAP AUTH MODE %d", param->softap_auth_mode.auth_mode);
        break;
	case ESP_BLUFI_EVENT_RECV_SOFTAP_CHANNEL:
        if (param->softap_channel.channel > 13) {
            return;
        }
         ap_config.ap.channel = param->softap_channel.channel;
         esp_wifi_set_config(WIFI_IF_AP, &ap_config);
        BLUFI_INFO("Recv SOFTAP CHANNEL %d", param->softap_channel.channel);
        break;
    case ESP_BLUFI_EVENT_GET_WIFI_LIST:{
        BLUFI_INFO("BLUFI get wifi list");
        wifi_scan_config_t scanConf = {
            .ssid = NULL,
            .bssid = NULL,
            .channel = 0,
            .show_hidden = false
        };
        esp_err_t ret = esp_wifi_scan_start(&scanConf, true);
        if (ret != ESP_OK) {
            esp_blufi_send_error_info(ESP_BLUFI_WIFI_SCAN_FAIL);
        }
        break;
    }
    case ESP_BLUFI_EVENT_RECV_CUSTOM_DATA:
        // 直接保存BindingCode到静态变量
        binding_code_ = std::string(reinterpret_cast<const char*>(param->custom_data.data), param->custom_data.data_len);
        BLUFI_INFO("BindingCode saved: %s", binding_code_.c_str());
        break;
	case ESP_BLUFI_EVENT_RECV_USERNAME:
        /* Not handle currently */
        break;
	case ESP_BLUFI_EVENT_RECV_CA_CERT:
        /* Not handle currently */
        break;
	case ESP_BLUFI_EVENT_RECV_CLIENT_CERT:
        /* Not handle currently */
        break;
	case ESP_BLUFI_EVENT_RECV_SERVER_CERT:
        /* Not handle currently */
        break;
	case ESP_BLUFI_EVENT_RECV_CLIENT_PRIV_KEY:
        /* Not handle currently */
        break;
	case ESP_BLUFI_EVENT_RECV_SERVER_PRIV_KEY:
        /* Not handle currently */
        break;
    default:
        break;
    }
}

void Blufi::RecordWifiConnInfo(int rssi, uint8_t reason)
{
    memset(&gl_sta_conn_info, 0, sizeof(esp_blufi_extra_info_t));
    if (gl_sta_is_connecting_) {
        gl_sta_conn_info.sta_max_conn_retry_set = true;
        gl_sta_conn_info.sta_max_conn_retry = EXAMPLE_WIFI_CONNECTION_MAXIMUM_RETRY;
    } else {
        gl_sta_conn_info.sta_conn_rssi_set = true;
        gl_sta_conn_info.sta_conn_rssi = rssi;
        gl_sta_conn_info.sta_conn_end_reason_set = true;
        gl_sta_conn_info.sta_conn_end_reason = reason;
    }
}

void Blufi::WifiConnect()
{
    wifi_retry_ = 0;
    gl_sta_is_connecting_ = (esp_wifi_connect() == ESP_OK);
    RecordWifiConnInfo(EXAMPLE_INVALID_RSSI, EXAMPLE_INVALID_REASON);
}

bool Blufi::WifiReconnect()
{
    bool ret;
    if (gl_sta_is_connecting_ && wifi_retry_++ < EXAMPLE_WIFI_CONNECTION_MAXIMUM_RETRY) {
        BLUFI_INFO("BLUFI WiFi starts reconnection");
        gl_sta_is_connecting_ = (esp_wifi_connect() == ESP_OK);
        RecordWifiConnInfo(EXAMPLE_INVALID_RSSI, EXAMPLE_INVALID_REASON);
        ret = true;
    } else {
        ret = false;
    }
    return ret;
}

int Blufi::SoftapGetCurrentConnectionNumber()
{
    esp_err_t ret;
    wifi_sta_list_t gl_sta_list;
    ret = esp_wifi_ap_get_sta_list(&gl_sta_list);
    if (ret == ESP_OK)
    {
        return gl_sta_list.num;
    }

    return 0;
}

void Blufi::WifiEventHandler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    wifi_event_sta_connected_t *event;
    wifi_event_sta_disconnected_t *disconnected_event;
    wifi_mode_t mode;

    auto& self = Blufi::GetInstance();

    switch (event_id) {
    case WIFI_EVENT_STA_START:
        self.WifiConnect();
        break;
    case WIFI_EVENT_STA_CONNECTED:
        gl_sta_connected_ = true;
        gl_sta_is_connecting_ = false;
        event = (wifi_event_sta_connected_t*) event_data;
        memcpy(gl_sta_bssid_, event->bssid, 6);
        memcpy(gl_sta_ssid_, event->ssid, event->ssid_len);
        gl_sta_ssid_len_ = event->ssid_len;
        break;
    case WIFI_EVENT_STA_DISCONNECTED:
        /* Only handle reconnection during connecting */
        if (gl_sta_connected_ == false && self.WifiReconnect() == false) {
            gl_sta_is_connecting_ = false;
            disconnected_event = (wifi_event_sta_disconnected_t*) event_data;
            self.RecordWifiConnInfo(disconnected_event->rssi, disconnected_event->reason);
        }
        /* This is a workaround as ESP32 WiFi libs don't currently
           auto-reassociate. */
        gl_sta_connected_ = false;
        gl_sta_got_ip_ = false;
        memset(gl_sta_ssid_, 0, 32);
        memset(gl_sta_bssid_, 0, 6);
        gl_sta_ssid_len_ = 0;
        xEventGroupClearBits(wifi_event_group_, CONNECTED_BIT);
        break;
    case WIFI_EVENT_AP_START:
        BLUFI_INFO("BLUFI AP start");
        esp_wifi_get_mode(&mode);

        /* TODO: get config or information of softap, then set to report extra_info */
        if (ble_is_connected_ == true) {
            if (gl_sta_connected_) {
                esp_blufi_extra_info_t info;
                memset(&info, 0, sizeof(esp_blufi_extra_info_t));
                memcpy(info.sta_bssid, gl_sta_bssid_, 6);
                info.sta_bssid_set = true;
                info.sta_ssid = gl_sta_ssid_;
                info.sta_ssid_len = gl_sta_ssid_len_;
                esp_blufi_send_wifi_conn_report(mode, gl_sta_got_ip_ ? ESP_BLUFI_STA_CONN_SUCCESS : ESP_BLUFI_STA_NO_IP, self.SoftapGetCurrentConnectionNumber(), &info);
            } else if (gl_sta_is_connecting_) {
                esp_blufi_send_wifi_conn_report(mode, ESP_BLUFI_STA_CONNECTING, self.SoftapGetCurrentConnectionNumber(), &gl_sta_conn_info);
            } else {
                esp_blufi_send_wifi_conn_report(mode, ESP_BLUFI_STA_CONN_FAIL, self.SoftapGetCurrentConnectionNumber(), &gl_sta_conn_info);
            }
        } else {
            BLUFI_INFO("BLUFI BLE is not connected yet");
        }
        break;
    case WIFI_EVENT_SCAN_DONE: {
        uint16_t apCount = 0;
        esp_wifi_scan_get_ap_num(&apCount);
        if (apCount == 0) {
            BLUFI_INFO("Nothing AP found");
            break;
        }

        // 限制WiFi列表数量，避免BLE传输数据过大导致序列号错误
        const uint16_t MAX_AP_SEND = 10;
        if (apCount > MAX_AP_SEND) {
            BLUFI_INFO("Too many APs (%d), limit to %d", apCount, MAX_AP_SEND);
            apCount = MAX_AP_SEND;
        }

        wifi_ap_record_t *ap_list = (wifi_ap_record_t *)malloc(sizeof(wifi_ap_record_t) * apCount);
        if (!ap_list) {
            BLUFI_ERROR("malloc error, ap_list is NULL");
            esp_wifi_clear_ap_list();
            break;
        }
        ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&apCount, ap_list));
        esp_blufi_ap_record_t * blufi_ap_list = (esp_blufi_ap_record_t *)malloc(apCount * sizeof(esp_blufi_ap_record_t));
        if (!blufi_ap_list) {
            if (ap_list) {
                free(ap_list);
            }
            BLUFI_ERROR("malloc error, blufi_ap_list is NULL");
            break;
        }
        for (int i = 0; i < apCount; ++i)
        {
            blufi_ap_list[i].rssi = ap_list[i].rssi;
            memcpy(blufi_ap_list[i].ssid, ap_list[i].ssid, sizeof(ap_list[i].ssid));
        }

        if (ble_is_connected_ == true) {
            // 等待WiFi扫描状态完全稳定后再发送，避免BLE序列号混乱
            vTaskDelay(pdMS_TO_TICKS(200));
            BLUFI_INFO("send wifi list, count: %d", apCount);
            esp_blufi_send_wifi_list(apCount, blufi_ap_list);
        } else {
            BLUFI_INFO("BLUFI BLE is not connected yet");
        }

        esp_wifi_scan_stop();
        free(ap_list);
        free(blufi_ap_list);
        break;
    }
    case WIFI_EVENT_AP_STACONNECTED: {
        wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
        BLUFI_INFO("station " MACSTR " join, AID=%d", MAC2STR(event->mac), event->aid);
        break;
    }
    case WIFI_EVENT_AP_STADISCONNECTED: {
        wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) event_data;
        BLUFI_INFO("station " MACSTR " leave, AID=%d, reason=%d", MAC2STR(event->mac), event->aid, event->reason);
        break;
    }

    default:
        break;
    }
    return;
}

void Blufi::IpEventHandler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    wifi_mode_t mode;

    auto& self = Blufi::GetInstance();

    switch (event_id) {
    case IP_EVENT_STA_GOT_IP: {
        esp_blufi_extra_info_t info;

        xEventGroupSetBits(wifi_event_group_, CONNECTED_BIT);
        esp_wifi_get_mode(&mode);

        memset(&info, 0, sizeof(esp_blufi_extra_info_t));
        memcpy(info.sta_bssid, gl_sta_bssid_, 6);
        info.sta_bssid_set = true;
        info.sta_ssid = gl_sta_ssid_;
        info.sta_ssid_len = gl_sta_ssid_len_;
        gl_sta_got_ip_ = true;
        if (ble_is_connected_ == true) {
            esp_blufi_send_wifi_conn_report(mode, ESP_BLUFI_STA_CONN_SUCCESS, self.SoftapGetCurrentConnectionNumber(), &info);
            BLUFI_INFO("WiFi mode: %u", mode);
            self.Save(std::string(reinterpret_cast<const char*>(sta_config.sta.ssid)),
                        std::string(reinterpret_cast<const char*>(sta_config.sta.password)));

            BLUFI_INFO("WiFi连接成功，配网完成，调用SettingOk");
            self.SettingOk();
        } else {
            BLUFI_INFO("BLUFI BLE is not connected yet");
        }
        break;
    }
    default:
        break;
    }
    return;
}

void Blufi::InitialiseWifi(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    wifi_event_group_ = xEventGroupCreate();
    // ESP_ERROR_CHECK(esp_event_loop_create_default());    // 主函数里调用过了，这里不能重复调用
    esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta_netif == NULL) {
        sta_netif = esp_netif_create_default_wifi_sta();
    }
    assert(sta_netif);
    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
    assert(ap_netif);
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &WifiEventHandler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &IpEventHandler, NULL));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK( esp_wifi_init(&cfg) );
    ESP_ERROR_CHECK( esp_wifi_set_mode(WIFI_MODE_STA) );
    RecordWifiConnInfo(EXAMPLE_INVALID_RSSI, EXAMPLE_INVALID_REASON);
    ESP_ERROR_CHECK( esp_wifi_start() );
}

std::string Blufi::GetBindingCode() {
    return binding_code_;
}


