#include "wifi_ap.h"
#include <cstdio>
#include <memory>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_err.h>
#include <esp_event.h>
#include <esp_wifi.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_netif.h>
#include <lwip/ip_addr.h>
#include <cJSON.h>
#include <esp_smartconfig.h>
#include "ssid_manager.h"
#include "application.h"
#include "board.h"
#include "wifi_station.h"

#include <atomic>

#define TAG "WifiAp"

// 防止 config_ok/reboot 回调被重复触发（用户快速多次点击提交）
static std::atomic<bool> config_callback_fired_{false};

extern const char index_html_start[] asm("_binary_wifi_configuration_html_start");
extern const char done_html_start[] asm("_binary_wifi_configuration_done_html_start");

WifiAp& WifiAp::GetInstance() {
    static WifiAp instance;
    return instance;
}

WifiAp::WifiAp() {
    language_ = "zh-CN";
}

WifiAp::~WifiAp() {
    // 不再持有周期 scan_timer_（按需扫描方案）
}

std::string WifiAp::GetSsid() {
    return ssid_prefix_;
}

std::string WifiAp::GetWebServerUrl() {
    return "http://192.168.4.1";
}

// ============ 生命周期 ============

void WifiAp::Start(const std::string& ssid_prefix, const std::string& language) {
    if (started_) {
        ESP_LOGW(TAG, "Already started");
        return;
    }

    // 重置防重入标记（支持多次配网）
    config_callback_fired_.store(false);

    // 检查 WifiStation 是否已初始化
    auto& wifi = WifiStation::GetInstance();
    if (!wifi.IsInitialized()) {
        ESP_LOGE(TAG, "WifiStation not initialized, please start it first");
        return;
    }

    ssid_prefix_ = ssid_prefix;
    language_ = language;

    ESP_LOGI(TAG, "Starting AP config mode: %s", ssid_prefix_.c_str());

    // 切换到 APSTA 模式
    wifi.SetMode(WifiMode::APSTA, ssid_prefix_);

    // 注册扫描完成回调
    wifi.OnScanComplete([](const std::vector<wifi_ap_record_t>& records) {
        WifiAp::OnScanDone();
    });

    // 配置 DNS 劫持
    ConfigureApInterface();

    // 启动 HTTP 服务
    StartWebServer();

    started_ = true;

    // 不再启动 10s 周期扫描器：与用户提交的 TryConnect 抢 STA 会触发
    // ESP_ERR_WIFI_STATE。改为：① WIFI_EVENT_STA_START 自动首扫
    // ② HTTP /scan 接口按需触发（5s 节流）
    ESP_LOGI(TAG, "AP config mode started (on-demand scan)");
}

void WifiAp::Stop() {
    if (!started_) {
        ESP_LOGD(TAG, "Not started, skip stop");
        return;
    }

    ESP_LOGI(TAG, "Stopping AP config mode...");
    started_ = false;

    // 清理回调
    auto& wifi = WifiStation::GetInstance();
    wifi.OnScanComplete(nullptr);

    // 停止 SmartConfig
    if (sc_event_instance_) {
        esp_smartconfig_stop();
        esp_event_handler_instance_unregister(SC_EVENT, ESP_EVENT_ANY_ID, sc_event_instance_);
        sc_event_instance_ = nullptr;
    }

    // 断开 AP 客户端（在停止 HTTP 前执行，避免 httpd_stop 阻塞）
    esp_wifi_deauth_sta(0);
    vTaskDelay(pdMS_TO_TICKS(100));

    // 停止 DNS 和 HTTP 服务
    dns_server_.Stop();
    if (server_) {
        httpd_stop(server_);
        server_ = nullptr;
    }

    credential_validator_ = nullptr;
    on_config_success_ = nullptr;

    ESP_LOGI(TAG, "AP config mode stopped");
}

// ============ AP 接口配置 ============

void WifiAp::ConfigureApInterface() {
    // 获取 AP 网络接口
    auto& wifi = WifiStation::GetInstance();
    auto* ap_netif = wifi.GetApNetif();

    if (ap_netif) {
        // 启动 DNS 劫持
        esp_netif_ip_info_t ip_info;
        esp_netif_get_ip_info(ap_netif, &ip_info);
        dns_server_.Start(ip_info.gw);
        ESP_LOGI(TAG, "DNS server started");
    } else {
        ESP_LOGW(TAG, "AP netif not available");
    }
}

// ============ 统一接口 ============

void WifiAp::SetCredentialValidator(
    std::function<bool(const std::string& ssid,
                       const std::string& password,
                       std::string& error_message)> validator) {
    credential_validator_ = validator;
}

void WifiAp::OnConfigSuccess(std::function<void()> callback) {
    on_config_success_ = callback;
}

// ============ 扫描回调 ============

void WifiAp::OnScanDone() {
    auto& self = WifiAp::GetInstance();
    if (!self.started_) return;

    // 扫描结果由 WifiStation 管理，这里只打印日志
    // 不再排周期 timer：HTTP /scan 接口按需触发新扫描
    auto count = WifiStation::GetInstance().GetCacheCount();
    ESP_LOGI(TAG, "Scan done, %d APs (cached for /scan endpoint)", (int)count);
}

// ============ AP 事件回调（日志用）============

void WifiAp::OnApStaConnected(wifi_event_ap_staconnected_t* event) {
    if (event) {
        ESP_LOGI(TAG, "Station " MACSTR " joined, AID=%d", MAC2STR(event->mac), event->aid);
    }
}

void WifiAp::OnApStaDisconnected(wifi_event_ap_stadisconnected_t* event) {
    if (event) {
        ESP_LOGI(TAG, "Station " MACSTR " left, AID=%d", MAC2STR(event->mac), event->aid);
    }
}

// ============ HTTP 服务 ============

void WifiAp::StartWebServer() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 24;
    config.max_open_sockets = 3;  // LWIP_MAX_SOCKETS=6，httpd 需预留 3 给系统
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.recv_wait_timeout = 15;
    config.send_wait_timeout = 15;
    esp_err_t err = httpd_start(&server_, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start web server: %s", esp_err_to_name(err));
        return;
    }

    // 首页
    httpd_uri_t index_html = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = [](httpd_req_t *req) -> esp_err_t {
            httpd_resp_set_hdr(req, "Connection", "close");
            httpd_resp_send(req, index_html_start, strlen(index_html_start));
            return ESP_OK;
        },
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server_, &index_html);

    // 已保存列表
    httpd_uri_t saved_list = {
        .uri = "/saved/list",
        .method = HTTP_GET,
        .handler = [](httpd_req_t *req) -> esp_err_t {
            auto ssid_list = SsidManager::GetInstance().GetSsidList();
            std::string json_str = "[";
            for (const auto& ssid : ssid_list) {
                json_str += "\"" + ssid.ssid + "\",";
            }
            if (json_str.length() > 1) json_str.pop_back();
            json_str += "]";
            httpd_resp_set_type(req, "application/json");
            httpd_resp_set_hdr(req, "Connection", "close");
            httpd_resp_send(req, json_str.c_str(), HTTPD_RESP_USE_STRLEN);
            return ESP_OK;
        },
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server_, &saved_list);

    // 设置默认
    httpd_uri_t saved_set_default = {
        .uri = "/saved/set_default",
        .method = HTTP_GET,
        .handler = [](httpd_req_t *req) -> esp_err_t {
            std::string uri = req->uri;
            auto pos = uri.find("?index=");
            if (pos != std::string::npos) {
                int index = -1;
                sscanf(&req->uri[pos+7], "%d", &index);
                SsidManager::GetInstance().SetDefaultSsid(index);
            }
            httpd_resp_set_type(req, "application/json");
            httpd_resp_set_hdr(req, "Connection", "close");
            httpd_resp_send(req, "{}", HTTPD_RESP_USE_STRLEN);
            return ESP_OK;
        },
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server_, &saved_set_default);

    // 删除
    httpd_uri_t saved_delete = {
        .uri = "/saved/delete",
        .method = HTTP_GET,
        .handler = [](httpd_req_t *req) -> esp_err_t {
            std::string uri = req->uri;
            auto pos = uri.find("?index=");
            if (pos != std::string::npos) {
                int index = -1;
                sscanf(&req->uri[pos+7], "%d", &index);
                SsidManager::GetInstance().RemoveSsid(index);
            }
            httpd_resp_set_type(req, "application/json");
            httpd_resp_set_hdr(req, "Connection", "close");
            httpd_resp_send(req, "{}", HTTPD_RESP_USE_STRLEN);
            return ESP_OK;
        },
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server_, &saved_delete);

    // 扫描结果（按需触发：手机端拉取时若缓存陈旧 >5s 且 STA 空闲，触发一次新扫描）
    httpd_uri_t scan = {
        .uri = "/scan",
        .method = HTTP_GET,
        .handler = [](httpd_req_t *req) -> esp_err_t {
            auto& wifi = WifiStation::GetInstance();

            // 5s 节流：缓存过期 + STA 空闲（不在 connecting/正在扫描）才触发新扫描
            // 不阻塞响应，立即返回当前缓存（手机端可下拉刷新拿新结果）
            int64_t now_ms = esp_timer_get_time() / 1000;
            int64_t age_ms = now_ms - wifi.GetLastScanTime();
            if (age_ms > 5000 && !wifi.IsScanning() && !wifi.IsConnected()) {
                // STA 未连接 + 未扫描中 → 安全触发；wifi.TriggerScan 内部会再判 is_scanning_
                wifi.TriggerScan();
            }

            // 直接从 WifiStation 获取去重后的扫描缓存（可能是上次扫描结果，由前端轮询拿新）
            auto ap_records = wifi.GetDeduplicatedCache();

            bool support_5g = false;
#ifdef CONFIG_SOC_WIFI_SUPPORT_5G
            support_5g = true;
#endif

            httpd_resp_set_type(req, "application/json");
            httpd_resp_set_hdr(req, "Connection", "close");
            httpd_resp_sendstr_chunk(req, "{\"support_5g\":");
            httpd_resp_sendstr_chunk(req, support_5g ? "true" : "false");
            httpd_resp_sendstr_chunk(req, ",\"aps\":[");

            for (size_t i = 0; i < ap_records.size(); i++) {
                char buf[128];
                snprintf(buf, sizeof(buf), "{\"ssid\":\"%s\",\"rssi\":%d,\"authmode\":%d}",
                    (char *)ap_records[i].ssid,
                    ap_records[i].rssi,
                    ap_records[i].authmode);
                httpd_resp_sendstr_chunk(req, buf);
                if (i < ap_records.size() - 1) {
                    httpd_resp_sendstr_chunk(req, ",");
                }
            }

            httpd_resp_sendstr_chunk(req, "]}");
            httpd_resp_sendstr_chunk(req, NULL);
            return ESP_OK;
        },
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server_, &scan);

    // 提交表单（调用业务层验证器）
    httpd_uri_t form_submit = {
        .uri = "/submit",
        .method = HTTP_POST,
        .handler = [](httpd_req_t *req) -> esp_err_t {
            size_t buf_len = req->content_len;
            if (buf_len > 1024) {
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Payload too large");
                return ESP_FAIL;
            }

            char *buf = (char *)malloc(buf_len + 1);
            if (!buf) {
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
                return ESP_FAIL;
            }

            int ret = httpd_req_recv(req, buf, buf_len);
            if (ret <= 0) {
                free(buf);
                httpd_resp_send_408(req);
                return ESP_FAIL;
            }
            buf[ret] = '\0';

            cJSON *json = cJSON_Parse(buf);
            free(buf);
            if (!json) {
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
                return ESP_FAIL;
            }

            cJSON *ssid_item = cJSON_GetObjectItemCaseSensitive(json, "ssid");
            cJSON *password_item = cJSON_GetObjectItemCaseSensitive(json, "password");

            if (!cJSON_IsString(ssid_item) || !ssid_item->valuestring || strlen(ssid_item->valuestring) >= 33) {
                cJSON_Delete(json);
                httpd_resp_send(req, "{\"success\":false,\"error\":\"Invalid SSID\"}", HTTPD_RESP_USE_STRLEN);
                return ESP_OK;
            }

            std::string ssid_str = ssid_item->valuestring;
            std::string password_str;
            if (cJSON_IsString(password_item) && password_item->valuestring && strlen(password_item->valuestring) < 65) {
                password_str = password_item->valuestring;
            }
            cJSON_Delete(json);

            // 调用业务层验证器（WifiStation.TryConnectAndSave）
            auto& ap = WifiAp::GetInstance();
            std::string error_message;
            bool success = false;

            if (ap.credential_validator_) {
                success = ap.credential_validator_(ssid_str, password_str, error_message);
            } else {
                error_message = "No validator configured";
            }

            httpd_resp_set_type(req, "application/json");
            httpd_resp_set_hdr(req, "Connection", "close");

            if (success) {
                httpd_resp_send(req, "{\"success\":true}", HTTPD_RESP_USE_STRLEN);
                // 配网成功，延迟触发回调（防重入：避免用户多次点击）
                if (ap.on_config_success_ && !config_callback_fired_.exchange(true)) {
                    xTaskCreatePinnedToCoreWithCaps([](void *ctx) {
                        vTaskDelay(pdMS_TO_TICKS(2000));  // 等待页面跳转
                        auto& ap = WifiAp::GetInstance();
                        if (ap.on_config_success_) {
                            ap.on_config_success_();
                        }
                        vTaskDelete(NULL);
                    }, "config_ok", 4096, NULL, 3, NULL, 1, MALLOC_CAP_SPIRAM);
                }
            } else {
                char response[256];
                snprintf(response, sizeof(response), "{\"success\":false,\"error\":\"%s\"}", error_message.c_str());
                httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
            }
            return ESP_OK;
        },
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server_, &form_submit);

    // 完成页面
    httpd_uri_t done_html = {
        .uri = "/done.html",
        .method = HTTP_GET,
        .handler = [](httpd_req_t *req) -> esp_err_t {
            httpd_resp_set_hdr(req, "Connection", "close");
            httpd_resp_send(req, done_html_start, strlen(done_html_start));
            return ESP_OK;
        },
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server_, &done_html);

    // 状态接口 (获取当前连接信息)
    httpd_uri_t status = {
        .uri = "/status",
        .method = HTTP_GET,
        .handler = [](httpd_req_t *req) -> esp_err_t {
            auto& wifi = WifiStation::GetInstance();

            char response[256];
            if (wifi.IsConnected()) {
                snprintf(response, sizeof(response),
                    "{\"ssid\":\"%s\",\"ip\":\"%s\",\"rssi\":%d}",
                    wifi.GetSsid().c_str(),
                    wifi.GetIpAddress().c_str(),
                    wifi.GetRssi());
            } else {
                snprintf(response, sizeof(response),
                    "{\"ssid\":\"\",\"ip\":\"\",\"rssi\":0}");
            }

            httpd_resp_set_type(req, "application/json");
            httpd_resp_set_hdr(req, "Connection", "close");
            httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
            return ESP_OK;
        },
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server_, &status);

    // 重启/连接（调用成功回调）
    httpd_uri_t reboot = {
        .uri = "/reboot",
        .method = HTTP_POST,
        .handler = [](httpd_req_t *req) -> esp_err_t {
            httpd_resp_set_type(req, "application/json");
            httpd_resp_set_hdr(req, "Cache-Control", "no-store");
            httpd_resp_set_hdr(req, "Connection", "close");
            httpd_resp_send(req, "{\"success\":true}", HTTPD_RESP_USE_STRLEN);

            auto& ap = WifiAp::GetInstance();

            // 防重入：避免用户多次点击 reboot 按钮
            if (!config_callback_fired_.exchange(true)) {
                if (ap.on_config_success_) {
                    // v1.9.68-rc4 2026-04-21 P0 修复：tskNO_AFFINITY → Core 1（避 Core 0 NVS flash 死锁）
                    xTaskCreatePinnedToCoreWithCaps([](void *ctx) {
                        vTaskDelay(pdMS_TO_TICKS(1000));
                        auto& ap = WifiAp::GetInstance();
                        if (ap.on_config_success_) {
                            ap.on_config_success_();
                        }
                        vTaskDelete(NULL);
                    }, "config_ok", 4096, NULL, 3, NULL, 1, MALLOC_CAP_SPIRAM);
                } else {
                    // v1.9.68-rc4 2026-04-21 P0 修复：tskNO_AFFINITY → Core 1（避 Core 0 NVS flash 死锁）
                    xTaskCreatePinnedToCoreWithCaps([](void *ctx) {
                        vTaskDelay(pdMS_TO_TICKS(1000));
                        WifiAp::GetInstance().Stop();
                        vTaskDelay(pdMS_TO_TICKS(100));
                        Application::GetInstance().Reboot();
                        vTaskDelete(NULL);
                    }, "reboot", 4096, NULL, 3, NULL, 1, MALLOC_CAP_SPIRAM);
                }
            }

            return ESP_OK;
        },
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server_, &reboot);

    // Captive Portal 重定向
    auto captive_handler = [](httpd_req_t *req) -> esp_err_t {
        auto *self = static_cast<WifiAp *>(req->user_ctx);
        std::string url = self->GetWebServerUrl() + "/?lang=" + self->language_ +
                          "&_=" + std::to_string(esp_timer_get_time());
        httpd_resp_set_type(req, "text/html");
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", url.c_str());
        httpd_resp_set_hdr(req, "Connection", "close");
        httpd_resp_send(req, NULL, 0);
        return ESP_OK;
    };

    const char* portal_urls[] = {
        "/hotspot-detect.html", "/generate_204*", "/mobile/status.php",
        "/check_network_status.txt", "/ncsi.txt", "/fwlink/",
        "/connectivity-check.html", "/success.txt", "/portal.html",
        "/library/test/success.html"
    };

    for (const auto& url : portal_urls) {
        httpd_uri_t redirect = {
            .uri = url,
            .method = HTTP_GET,
            .handler = captive_handler,
            .user_ctx = this
        };
        httpd_register_uri_handler(server_, &redirect);
    }

    ESP_LOGI(TAG, "Web server started");
}

// ============ SmartConfig ============

void WifiAp::StartSmartConfig() {
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        SC_EVENT, ESP_EVENT_ANY_ID, &SmartConfigEventHandler, this, &sc_event_instance_));

    smartconfig_start_config_t cfg = SMARTCONFIG_START_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_smartconfig_start(&cfg));
    ESP_LOGI(TAG, "SmartConfig started");
}

void WifiAp::SmartConfigEventHandler(void *arg, esp_event_base_t event_base,
                                                  int32_t event_id, void *event_data) {

    switch (event_id) {
    case SC_EVENT_SCAN_DONE:
        ESP_LOGI(TAG, "SmartConfig scan done");
        break;
    case SC_EVENT_FOUND_CHANNEL:
        ESP_LOGI(TAG, "Found SmartConfig channel");
        break;
    case SC_EVENT_GOT_SSID_PSWD: {
        auto *evt = (smartconfig_event_got_ssid_pswd_t *)event_data;
        char ssid[33] = {0}, password[65] = {0};
        memcpy(ssid, evt->ssid, sizeof(evt->ssid));
        memcpy(password, evt->password, sizeof(evt->password));
        ESP_LOGI(TAG, "SmartConfig: %s", ssid);
        // SmartConfig 不经过验证，直接保存凭证
        SsidManager::GetInstance().AddSsid(ssid, password);

        xTaskCreatePinnedToCore([](void *ctx) {
            vTaskDelay(pdMS_TO_TICKS(3000));
            esp_restart();
        }, "restart", 4096, NULL, 5, NULL, 1);
        break;
    }
    case SC_EVENT_SEND_ACK_DONE:
        ESP_LOGI(TAG, "SmartConfig ACK sent");
        esp_smartconfig_stop();
        break;
    }
}