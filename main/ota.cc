#include "ota.h"
#include "application.h"
#include "system_info.h"
#include "settings.h"
#include "assets/lang_config.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cJSON.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_partition.h>
#include <esp_ota_ops.h>
#include <esp_app_format.h>
#include <esp_efuse.h>
#include <esp_efuse_table.h>
#include <esp_heap_caps.h>
#include <esp_task_wdt.h>
#ifdef SOC_HMAC_SUPPORTED
#include <esp_hmac.h>
#endif

#include <cstring>
#include <vector>
#include <sstream>
#include <algorithm>

#define TAG "Ota"


Ota::Ota() {
#ifdef ESP_EFUSE_BLOCK_USR_DATA
    // Read Serial Number from efuse user_data
    uint8_t serial_number[33] = {0};
    if (esp_efuse_read_field_blob(ESP_EFUSE_USER_DATA, serial_number, 32 * 8) == ESP_OK) {
        if (serial_number[0] == 0) {
            has_serial_number_ = false;
        } else {
            serial_number_ = std::string(reinterpret_cast<char*>(serial_number), 32);
            has_serial_number_ = true;
        }
    }
#endif
}

Ota::~Ota() {
}

std::string Ota::GetCheckVersionUrl() {
    Settings settings("wifi", false);
    std::string url = settings.GetString("ota_url");
    if (url.empty()) {
        url = CONFIG_OTA_URL;
    }
    return url;
}

std::unique_ptr<Http> Ota::SetupHttp() {
    auto& board = Board::GetInstance();
    auto network = board.GetNetwork();
    auto http = network->CreateHttp(0);
    auto user_agent = SystemInfo::GetUserAgent();
    http->SetHeader("Activation-Version", has_serial_number_ ? "2" : "1");
    http->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
    http->SetHeader("Client-Id", board.GetUuid());
    if (has_serial_number_) {
        http->SetHeader("Serial-Number", serial_number_.c_str());
        ESP_LOGI(TAG, "Setup HTTP, User-Agent: %s, Serial-Number: %s", user_agent.c_str(), serial_number_.c_str());
    }
    http->SetHeader("User-Agent", user_agent);
    http->SetHeader("Accept-Language", Lang::CODE);
    http->SetHeader("Content-Type", "application/json");

    return http;
}

/* 
 * Specification: https://ccnphfhqs21z.feishu.cn/wiki/FjW6wZmisimNBBkov6OcmfvknVd
 */
esp_err_t Ota::CheckVersion() {
    auto& board = Board::GetInstance();
    auto app_desc = esp_app_get_description();

    // Check if there is a new firmware version available
    current_version_ = app_desc->version;
    ESP_LOGI(TAG, "Current version: %s", current_version_.c_str());

    std::string url = GetCheckVersionUrl();
    if (url.length() < 10) {
        ESP_LOGE(TAG, "Check version URL is not properly set");
        return ESP_ERR_INVALID_ARG;
    }

    auto http = SetupHttp();

    std::string data = board.GetSystemInfoJson();
    std::string method = data.length() > 0 ? "POST" : "GET";
    ESP_LOGI(TAG, "%s %s body=%s", method.c_str(), url.c_str(), data.c_str());
    http->SetContent(std::move(data));

    int64_t t0 = esp_timer_get_time();
    if (!http->Open(method, url)) {
        int last_error = http->GetLastError();
        ESP_LOGE(TAG, "HTTP failed: 0x%x (%ld ms)", last_error, (long)((esp_timer_get_time() - t0) / 1000));
        return last_error;
    }

    auto status_code = http->GetStatusCode();
    if (status_code != 200) {
        ESP_LOGE(TAG, "Failed to check version, status code: %d", status_code);
        return status_code;
    }

    data = http->ReadAll();
    http->Close();

    ESP_LOGI(TAG, "Response: %s", data.c_str());

    // Response: { "firmware": { "version": "1.0.0", "url": "http://" } }
    // Parse the JSON response and check if the version is newer
    // If it is, set has_new_version_ to true and store the new version and URL
    
    cJSON *root = cJSON_Parse(data.c_str());
    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to parse JSON response");
        return ESP_ERR_INVALID_RESPONSE;
    }

    has_activation_code_ = false;
    has_activation_challenge_ = false;
    cJSON *activation = cJSON_GetObjectItem(root, "activation");
    if (cJSON_IsObject(activation)) {
        cJSON* message = cJSON_GetObjectItem(activation, "message");
        if (cJSON_IsString(message)) {
            activation_message_ = message->valuestring;
        }
        cJSON* code = cJSON_GetObjectItem(activation, "code");
        if (cJSON_IsString(code)) {
            activation_code_ = code->valuestring;
            has_activation_code_ = true;
        }
        cJSON* challenge = cJSON_GetObjectItem(activation, "challenge");
        if (cJSON_IsString(challenge)) {
            activation_challenge_ = challenge->valuestring;
            has_activation_challenge_ = true;
        }
        cJSON* timeout_ms = cJSON_GetObjectItem(activation, "timeout_ms");
        if (cJSON_IsNumber(timeout_ms)) {
            activation_timeout_ms_ = timeout_ms->valueint;
        }
    }

    has_mqtt_config_ = false;
    cJSON *mqtt = cJSON_GetObjectItem(root, "mqtt");
    if (cJSON_IsObject(mqtt)) {
        Settings settings("mqtt", true);
        cJSON *item = NULL;
        cJSON_ArrayForEach(item, mqtt) {
            if (cJSON_IsString(item)) {
                if (settings.GetString(item->string) != item->valuestring) {
                    settings.SetString(item->string, item->valuestring);
                }
            } else if (cJSON_IsNumber(item)) {
                if (settings.GetInt(item->string) != item->valueint) {
                    settings.SetInt(item->string, item->valueint);
                }
            }
        }
        has_mqtt_config_ = true;
    } else {
        ESP_LOGI(TAG, "No mqtt section found !");
    }

    has_websocket_config_ = false;
    cJSON *websocket = cJSON_GetObjectItem(root, "websocket");
    if (cJSON_IsObject(websocket)) {
        Settings settings("websocket", true);
        cJSON *item = NULL;
        cJSON_ArrayForEach(item, websocket) {
            if (cJSON_IsString(item)) {
                if (settings.GetString(item->string) != item->valuestring) {
                    settings.SetString(item->string, item->valuestring);
                }
            } else if (cJSON_IsNumber(item)) {
                if (settings.GetInt(item->string) != item->valueint) {
                    settings.SetInt(item->string, item->valueint);
                }
            }
        }
        has_websocket_config_ = true;
    } else {
        ESP_LOGI(TAG, "No websocket section found!");
    }

    has_server_time_ = false;
    cJSON *server_time = cJSON_GetObjectItem(root, "server_time");
    if (cJSON_IsObject(server_time)) {
        cJSON *timestamp = cJSON_GetObjectItem(server_time, "timestamp");
        cJSON *timezone_offset = cJSON_GetObjectItem(server_time, "timezone_offset");
        
        if (cJSON_IsNumber(timestamp)) {
            // 设置系统时间
            struct timeval tv;
            double ts = timestamp->valuedouble;
            
            // 如果有时区偏移，计算本地时间
            if (cJSON_IsNumber(timezone_offset)) {
                ts += (timezone_offset->valueint * 60 * 1000); // 转换分钟为毫秒
            }
            
            tv.tv_sec = (time_t)(ts / 1000);  // 转换毫秒为秒
            tv.tv_usec = (suseconds_t)((long long)ts % 1000) * 1000;  // 剩余的毫秒转换为微秒
            settimeofday(&tv, NULL);
            has_server_time_ = true;
        }
    } else {
        ESP_LOGW(TAG, "No server_time section found!");
    }

    has_new_version_ = false;
    cJSON *firmware = cJSON_GetObjectItem(root, "firmware");
    if (cJSON_IsObject(firmware)) {
        cJSON *version = cJSON_GetObjectItem(firmware, "version");
        if (cJSON_IsString(version)) {
            firmware_version_ = version->valuestring;
        }
        cJSON *url = cJSON_GetObjectItem(firmware, "url");
        if (cJSON_IsString(url)) {
            firmware_url_ = url->valuestring;
        }

        if (cJSON_IsString(version) && cJSON_IsString(url)) {
            // Check if the version is newer, for example, 0.1.0 is newer than 0.0.1
            has_new_version_ = IsNewVersionAvailable(current_version_, firmware_version_);
            if (has_new_version_) {
                ESP_LOGI(TAG, "New version available: %s", firmware_version_.c_str());
            } else {
                ESP_LOGI(TAG, "Current is the latest version");
            }
            // If the force flag is set to 1, the given version is forced to be installed
            cJSON *force = cJSON_GetObjectItem(firmware, "force");
            if (cJSON_IsNumber(force) && force->valueint == 1) {
                has_new_version_ = true;
            }
        }
    } else {
        ESP_LOGW(TAG, "No firmware section found!");
    }

    cJSON_Delete(root);
    return ESP_OK;
}

// ============================================================
// 通用 POST OTA_URL/<path>
// ============================================================

esp_err_t Ota::PostToOta(const std::string& path, cJSON* payload) {
    // 设备未就绪（启动中/激活中）时跳过，避免网络未连接时阻塞
    auto state = Application::GetInstance().GetDeviceState();
    if (state < kDeviceStateIdle) {
        cJSON_Delete(payload);
        return ESP_ERR_INVALID_STATE;
    }

    auto& board = Board::GetInstance();
    auto network = board.GetNetwork();
    if (!network) {
        cJSON_Delete(payload);
        return ESP_ERR_INVALID_STATE;
    }

    Settings settings("wifi", false);
    std::string url = settings.GetString("ota_url");
    if (url.empty()) url = CONFIG_OTA_URL;
    if (url.length() < 10) return ESP_ERR_INVALID_ARG;
    // 去掉末尾 / 避免双斜杠
    while (!url.empty() && url.back() == '/') url.pop_back();
    url += path;

    auto http = network->CreateHttp(0);
    if (!http) {
        cJSON_Delete(payload);
        return ESP_ERR_NO_MEM;
    }

    http->SetTimeout(10000);  // 10 秒超时
    http->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
    http->SetHeader("Client-Id", board.GetUuid());
    http->SetHeader("Content-Type", "application/json");

    char* json = cJSON_PrintUnformatted(payload);
    http->SetContent(std::string(json));
    cJSON_free(json);
    cJSON_Delete(payload);

    ESP_LOGI(TAG, "POST %s", url.c_str());
    if (!http->Open("POST", url)) {
        ESP_LOGW(TAG, "POST %s failed: %d", path.c_str(), http->GetLastError());
        return ESP_FAIL;
    }

    int code = http->GetStatusCode();
    std::string body = http->ReadAll();
    http->Close();
    ESP_LOGI(TAG, "%s response: %d %s", path.c_str(), code, body.c_str());

    return (code >= 200 && code < 300) ? ESP_OK : ESP_FAIL;
}

// ============================================================
// /switch — NFC 或 iBeacon 触发切换
// ============================================================

esp_err_t Ota::RequestSwitch(const std::string& type, cJSON* data) {
    cJSON_AddStringToObject(data, "type", type.c_str());
    return PostToOta("/switch", data);
}

// ============================================================
// /status — 上报设备状态
// ============================================================

esp_err_t Ota::ReportStatus(cJSON* payload) {
    auto state = Application::GetInstance().GetDeviceState();
    if (state != kDeviceStateIdle) {
        ESP_LOGD(TAG, "skip /status POST, state=%d (仅 idle 上报)", (int)state);
        cJSON_Delete(payload);
        return ESP_ERR_INVALID_STATE;
    }
    return PostToOta("/status", payload);
}

void Ota::MarkCurrentVersionValid() {
    auto partition = esp_ota_get_running_partition();
    if (strcmp(partition->label, "factory") == 0) {
        ESP_LOGI(TAG, "Running from factory partition, skipping");
        return;
    }

    ESP_LOGI(TAG, "Running partition: %s", partition->label);
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(partition, &state) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get state of partition");
        return;
    }

    if (state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGI(TAG, "Marking firmware as valid");
        esp_ota_mark_app_valid_cancel_rollback();
    }
}

bool Ota::Upgrade(const std::string& firmware_url, std::function<void(int progress, size_t speed)> callback) {
    ESP_LOGI(TAG, "Upgrading firmware from %s", firmware_url.c_str());
    esp_ota_handle_t update_handle = 0;
    auto update_partition = esp_ota_get_next_update_partition(NULL);
    if (update_partition == NULL) {
        ESP_LOGE(TAG, "Failed to get update partition");
        return false;
    }

    ESP_LOGI(TAG, "Writing to partition %s at offset 0x%lx", update_partition->label, update_partition->address);
    bool image_header_checked = false;
    std::string image_header;

    auto network = Board::GetInstance().GetNetwork();
    auto http = network->CreateHttp(0);
    // 4G + 921600 UART 下大固件下载典型 ~44s；默认 30s 在弱网时会让单片读取假超时
    // → 触发不必要的指数退避。提到 90s 给单连接 read 留足容错窗口。
    http->SetTimeout(90000);
    if (!http->Open("GET", firmware_url)) {
        ESP_LOGE(TAG, "Failed to open HTTP connection");
        return false;
    }

    if (http->GetStatusCode() != 200) {
        ESP_LOGE(TAG, "Failed to get firmware, status code: %d", http->GetStatusCode());
        return false;
    }

    size_t content_length = http->GetBodyLength();
    if (content_length == 0) {
        ESP_LOGE(TAG, "Failed to get content length");
        return false;
    }

    constexpr size_t PAGE_SIZE = 4096;
    char* buffer = (char*)heap_caps_malloc(PAGE_SIZE, MALLOC_CAP_INTERNAL);
    if (buffer == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate buffer");
        return false;
    }

    // 暂停看门狗：4G 弱网下大文件下载可能数分钟，防 task WDT 触发
    // RAII 退出（含 break/return）自动恢复
#ifdef CONFIG_ESP_TASK_WDT_EN
    TaskHandle_t current_task = xTaskGetCurrentTaskHandle();
    esp_task_wdt_delete(current_task);
    ESP_LOGI(TAG, "Task WDT paused for OTA download");
    struct WdtGuard { TaskHandle_t t; ~WdtGuard() { esp_task_wdt_add(t); } } _wdt_guard{current_task};
#endif

    size_t buffer_offset = 0;             // Current data size in buffer
    size_t total_read = 0, recent_read = 0;
    auto last_calc_time = esp_timer_get_time();
    int retry_count = 0;
    constexpr int kMaxRetries = 5;        // 指数退避 3s/6s/12s/24s/48s 共 ~93s

    while (true) {
        int ret = http ? http->Read(buffer + buffer_offset, PAGE_SIZE - buffer_offset) : -1;

        // ──────── 网络中断检测：read 失败 / 服务端早断（content-length 未下完）─────────
        if (ret < 0 || (ret == 0 && total_read < content_length)) {
            if (http) { http->Close(); http.reset(); }
            if (retry_count >= kMaxRetries) {
                ESP_LOGE(TAG, "OTA download failed after %d retries", kMaxRetries);
                if (image_header_checked) esp_ota_abort(update_handle);
                heap_caps_free(buffer);
                return false;
            }
            retry_count++;
            ESP_LOGW(TAG, "Download interrupted at %u/%u bytes, retry %d/%d",
                     (unsigned)total_read, (unsigned)content_length, retry_count, kMaxRetries);

            // ⚠ flush buffer 内已读未写的字节到 partition：保证 partition 写入字节 == total_read
            // 否则下次 Range: bytes=total_read- 续传时 partition 会缺 buffer_offset 字节
            if (image_header_checked && buffer_offset > 0) {
                if (esp_ota_write(update_handle, buffer, buffer_offset) != ESP_OK) {
                    esp_ota_abort(update_handle);
                    heap_caps_free(buffer);
                    return false;
                }
                buffer_offset = 0;
            }

            // 指数退避：3s/6s/12s/24s/48s（封顶 48s 给 modem 足够时间释放 HTTP 资源）
            int delay_ms = 3000 * (1 << (retry_count - 1));
            if (delay_ms > 48000) delay_ms = 48000;
            ESP_LOGI(TAG, "Waiting %d ms before retry...", delay_ms);
            vTaskDelay(pdMS_TO_TICKS(delay_ms));

            // Range 重连（仅接受 206 · 主流 OSS/Nginx 都支持，不支持则继续重试）
            http = network->CreateHttp(0);
            http->SetTimeout(90000);
            http->SetHeader("Range", "bytes=" + std::to_string(total_read) + "-");
            if (!http->Open("GET", firmware_url) || http->GetStatusCode() != 206) {
                ESP_LOGE(TAG, "Resume failed (status=%d)", http ? http->GetStatusCode() : -1);
                if (http) http->Close();
                http.reset();
                continue;
            }
            ESP_LOGI(TAG, "Resumed from byte %u", (unsigned)total_read);
            recent_read = 0;
            last_calc_time = esp_timer_get_time();
            continue;
        }

        if (ret == 0) break;        // 正常下载完成
        retry_count = 0;            // 成功读取 → 重置重试计数

        // Calculate speed and progress every second
        recent_read += ret;
        total_read += ret;
        buffer_offset += ret;
        if (esp_timer_get_time() - last_calc_time >= 1000000) {
            size_t progress = total_read * 100 / content_length;
            ESP_LOGI(TAG, "Progress: %u%% (%u/%u), Speed: %uB/s", progress, total_read, content_length, recent_read);
            if (callback) {
                callback(progress, recent_read);
            }
            last_calc_time = esp_timer_get_time();
            recent_read = 0;
        }

        if (!image_header_checked) {
            image_header.append(buffer, buffer_offset);
            if (image_header.size() >= sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t)) {
                esp_app_desc_t new_app_info;
                memcpy(&new_app_info, image_header.data() + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t), sizeof(esp_app_desc_t));

                if (esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &update_handle)) {
                    esp_ota_abort(update_handle);
                    ESP_LOGE(TAG, "Failed to begin OTA");
                    heap_caps_free(buffer);
                    return false;
                }

                image_header_checked = true;
                std::string().swap(image_header);
            }
        }

        // Write to flash when buffer is full (4KB)
        if (buffer_offset == PAGE_SIZE) {
            auto err = esp_ota_write(update_handle, buffer, buffer_offset);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Failed to write OTA data: %s", esp_err_to_name(err));
                esp_ota_abort(update_handle);
                heap_caps_free(buffer);
                return false;
            }
            buffer_offset = 0;
        }
    }

    // 收尾：flush buffer 残余字节到 partition
    if (image_header_checked && buffer_offset > 0) {
        auto err = esp_ota_write(update_handle, buffer, buffer_offset);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to flush final OTA data: %s", esp_err_to_name(err));
            esp_ota_abort(update_handle);
            heap_caps_free(buffer);
            return false;
        }
    }
    if (http) http->Close();
    heap_caps_free(buffer);

    esp_err_t err = esp_ota_end(update_handle);
    if (err != ESP_OK) {
        if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
            ESP_LOGE(TAG, "Image validation failed, image is corrupted");
        } else {
            ESP_LOGE(TAG, "Failed to end OTA: %s", esp_err_to_name(err));
        }
        return false;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set boot partition: %s", esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG, "Firmware upgrade successful");
    return true;
}

bool Ota::StartUpgrade(std::function<void(int progress, size_t speed)> callback) {
    return Upgrade(firmware_url_, callback);
}

// 通用 HTTP GET → PSRAM 缓冲下载（用于动态 GIF/PNG 等小资源）
// 与 Upgrade 不同：不做 Range 续传（小文件重传成本低），不做 WdtGuard（下载快），
// 不做 esp_ota_*（写到 PSRAM 而非 flash partition）。
bool Ota::Download(const std::string& url, size_t max_size,
                   uint8_t** buffer, size_t* size) {
    *buffer = nullptr;
    *size = 0;

    auto network = Board::GetInstance().GetNetwork();
    constexpr int kMaxRetries = 2;

    uint8_t* buf = (uint8_t*)heap_caps_malloc(max_size, MALLOC_CAP_SPIRAM);
    if (!buf) {
        ESP_LOGE(TAG, "Download: PSRAM alloc %u failed", (unsigned)max_size);
        return false;
    }

    for (int attempt = 0; attempt <= kMaxRetries; attempt++) {
        if (attempt > 0) {
            ESP_LOGW(TAG, "Download: retry %d/%d", attempt, kMaxRetries);
            vTaskDelay(pdMS_TO_TICKS(500 * attempt));
        }

        auto http = network->CreateHttp(0);
        http->SetTimeout(15000);
        if (!http->Open("GET", url)) {
            ESP_LOGE(TAG, "Download: HTTP open failed");
            continue;
        }
        int status = http->GetStatusCode();
        if (status != 200) {
            ESP_LOGW(TAG, "Download: HTTP %d", status);
            http->Close();
            break;  // 非网络错误（404 等），不重试
        }
        size_t expected = http->GetBodyLength();

        // 流式读到 PSRAM
        size_t total_read = 0;
        while (total_read < max_size) {
            int n = http->Read((char*)(buf + total_read), max_size - total_read);
            if (n <= 0) break;
            total_read += n;
        }
        http->Close();

        // Content-Length 完整性校验（4G AT 通道可能丢数据块）
        if (expected > 0 && total_read < expected) {
            ESP_LOGW(TAG, "Download: incomplete %u/%u", (unsigned)total_read, (unsigned)expected);
            continue;
        }
        if (total_read == 0) {
            ESP_LOGW(TAG, "Download: empty body");
            continue;
        }

        // 缩小 PSRAM 到实际大小
        uint8_t* shrunk = (uint8_t*)heap_caps_realloc(buf, total_read, MALLOC_CAP_SPIRAM);
        if (shrunk) buf = shrunk;
        *buffer = buf;
        *size = total_read;
        ESP_LOGI(TAG, "Download: %u bytes from %s", (unsigned)total_read, url.c_str());
        return true;
    }

    heap_caps_free(buf);
    return false;
}


std::vector<int> Ota::ParseVersion(const std::string& version) {
    std::vector<int> versionNumbers;
    std::stringstream ss(version);
    std::string segment;
    
    while (std::getline(ss, segment, '.')) {
        versionNumbers.push_back(std::stoi(segment));
    }
    
    return versionNumbers;
}

bool Ota::IsNewVersionAvailable(const std::string& currentVersion, const std::string& newVersion) {
    std::vector<int> current = ParseVersion(currentVersion);
    std::vector<int> newer = ParseVersion(newVersion);
    
    for (size_t i = 0; i < std::min(current.size(), newer.size()); ++i) {
        if (newer[i] > current[i]) {
            return true;
        } else if (newer[i] < current[i]) {
            return false;
        }
    }
    
    return newer.size() > current.size();
}

std::string Ota::GetActivationPayload() {
    if (!has_serial_number_) {
        return "{}";
    }

    std::string hmac_hex;
#ifdef SOC_HMAC_SUPPORTED
    uint8_t hmac_result[32]; // SHA-256 输出为32字节
    
    // 使用Key0计算HMAC
    esp_err_t ret = esp_hmac_calculate(HMAC_KEY0, (uint8_t*)activation_challenge_.data(), activation_challenge_.size(), hmac_result);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "HMAC calculation failed: %s", esp_err_to_name(ret));
        return "{}";
    }

    for (size_t i = 0; i < sizeof(hmac_result); i++) {
        char buffer[3];
        sprintf(buffer, "%02x", hmac_result[i]);
        hmac_hex += buffer;
    }
#endif

    cJSON *payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "algorithm", "hmac-sha256");
    cJSON_AddStringToObject(payload, "serial_number", serial_number_.c_str());
    cJSON_AddStringToObject(payload, "challenge", activation_challenge_.c_str());
    cJSON_AddStringToObject(payload, "hmac", hmac_hex.c_str());
    auto json_str = cJSON_PrintUnformatted(payload);
    std::string json(json_str);
    cJSON_free(json_str);
    cJSON_Delete(payload);

    ESP_LOGI(TAG, "Activation payload: %s", json.c_str());
    return json;
}

esp_err_t Ota::Activate() {
    if (!has_activation_challenge_) {
        ESP_LOGW(TAG, "No activation challenge found");
        return ESP_FAIL;
    }

    std::string url = GetCheckVersionUrl();
    if (url.back() != '/') {
        url += "/activate";
    } else {
        url += "activate";
    }

    auto http = SetupHttp();

    std::string data = GetActivationPayload();
    http->SetContent(std::move(data));

    if (!http->Open("POST", url)) {
        ESP_LOGE(TAG, "Failed to open HTTP connection");
        return ESP_FAIL;
    }
    
    auto status_code = http->GetStatusCode();
    if (status_code == 202) {
        return ESP_ERR_TIMEOUT;
    }
    if (status_code != 200) {
        ESP_LOGE(TAG, "Failed to activate, code: %d, body: %s", status_code, http->ReadAll().c_str());
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Activation successful");
    return ESP_OK;
}

// P30: 状态上报到 MyDazy 服务器
bool Ota::ReportStatus() {
    auto state = Application::GetInstance().GetDeviceState();
    if (state != kDeviceStateIdle) {
        ESP_LOGD(TAG, "skip /status POST, state=%d (仅 idle 上报)", (int)state);
        return false;
    }

    std::string url = GetCheckVersionUrl();
    if (url.length() < 10) {
        ESP_LOGE(TAG, "Status URL base not set");
        return false;
    }

    if (url.find("mydazy") == std::string::npos) {
        url = "https://www.mydazy.cn/v1/ota/status";
    } else {
        url += (url.back() == '/') ? "status" : "/status";
    }

    auto http = SetupHttp();
    auto& board = Board::GetInstance();
    std::string status_json = board.GetDeviceStatusJson();
    std::string board_json = board.GetBoardJson();
    if (status_json.empty()) status_json = "{}";
    if (board_json.empty()) board_json = "{}";
    std::string payload = std::string("{\"status\":") + status_json + ",\"board\":" + board_json + "}";
    ESP_LOGI(TAG, "status url: %s", url.c_str());
    http->SetContent(std::move(payload));
    if (!http->Open("POST", url)) {
        ESP_LOGE(TAG, "Failed to open HTTP for status");
        return false;
    }
    int code = http->GetStatusCode();
    std::string resp;
    if (code == 200) {
        resp = http->ReadAll();
    }
    http->Close();

    if (code != 200 && code != 202 && code != 204) {
        ESP_LOGW(TAG, "Status post code: %d", code);
        return false;
    }

    if (!resp.empty()) {
        auto *resp_ptr = new std::string(std::move(resp));
        // 重入保护：CheckVersion 由 Activation 串行调用，物理不可能两次同时跑（依赖时序）
        constexpr uint32_t kStatusAssetsStackSize = 4096;
        static StackType_t s_status_assets_stack[kStatusAssetsStackSize / sizeof(StackType_t)];
        static StaticTask_t s_status_assets_tcb;
        TaskHandle_t task_created = xTaskCreateStaticPinnedToCore([](void* p) {
            std::unique_ptr<std::string> holder(static_cast<std::string*>(p));
            cJSON* root = cJSON_Parse(holder->c_str());
            if (!root) { vTaskDelete(NULL); return; }

            // 同步服务器时间
            cJSON* server_time = cJSON_GetObjectItem(root, "server_time");
            if (cJSON_IsObject(server_time)) {
                cJSON* timestamp = cJSON_GetObjectItem(server_time, "timestamp");
                cJSON* timezone_offset = cJSON_GetObjectItem(server_time, "timezone_offset");
                if (cJSON_IsNumber(timestamp)) {
                    struct timeval tv;
                    double ts = timestamp->valuedouble;
                    if (cJSON_IsNumber(timezone_offset)) ts += (timezone_offset->valueint * 60 * 1000);
                    tv.tv_sec = static_cast<time_t>(ts / 1000);
                    tv.tv_usec = static_cast<suseconds_t>(static_cast<long long>(ts) % 1000) * 1000;
                    settimeofday(&tv, NULL);
                }
            }

            // 处理远程设置控制
            cJSON* settings_obj = cJSON_GetObjectItem(root, "settings");
            if (cJSON_IsObject(settings_obj)) {
                cJSON* status_obj = cJSON_GetObjectItem(settings_obj, "status");
                if (cJSON_IsObject(status_obj)) {
                    Settings status_settings("status", true);
                    const char* keys[] = {"deepSleep", "report", "autoStart", "pickupWake"};
                    for (const char* key : keys) {
                        cJSON* val = cJSON_GetObjectItem(status_obj, key);
                        if (cJSON_IsNumber(val)) {
                            status_settings.SetInt(key, val->valueint);
                            ESP_LOGI(TAG, "Remote setting status.%s = %d", key, val->valueint);
                        }
                    }
                }
            }

            cJSON_Delete(root);
            vTaskDelete(NULL);
        }, "status_assets", kStatusAssetsStackSize / sizeof(StackType_t),
           resp_ptr, 4, s_status_assets_stack, &s_status_assets_tcb, 0);

        if (task_created == nullptr) {
            delete resp_ptr;
        }
    }

    return true;
}

