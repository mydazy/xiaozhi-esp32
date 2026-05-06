#ifndef _OTA_H
#define _OTA_H

#include <functional>
#include <string>

#include <esp_err.h>
#include <cJSON.h>
#include "board.h"

class Ota {
public:
    Ota();
    ~Ota();

    esp_err_t CheckVersion();
    esp_err_t Activate();

    /// POST OTA_URL/switch — 切换智能体（NFC/iBeacon/自定义）
    /// @param type 触发类型（"nfc", "ibeacon" 等）
    /// @param data 附加数据（调用后自动释放）
    static esp_err_t RequestSwitch(const std::string& type, cJSON* data);

    /// POST OTA_URL/status — 上报设备状态（GPS、电量等）
    static esp_err_t ReportStatus(cJSON* payload);
    bool ReportStatus();
    bool HasActivationChallenge() { return has_activation_challenge_; }
    bool HasNewVersion() { return has_new_version_; }
    bool HasMqttConfig() { return has_mqtt_config_; }
    bool HasWebsocketConfig() { return has_websocket_config_; }
    bool HasActivationCode() { return has_activation_code_; }
    bool HasServerTime() { return has_server_time_; }
    bool StartUpgrade(std::function<void(int progress, size_t speed)> callback);
    static bool Upgrade(const std::string& firmware_url, std::function<void(int progress, size_t speed)> callback);
    void MarkCurrentVersionValid();

    // 通用 HTTP GET 下载到 PSRAM 缓冲（动态图片 / 小资源等）。
    //   max_size: 安全上限（防服务器返回畸形大数据撑爆 PSRAM）
    //   返回 true 时 *buffer 由调用方 heap_caps_free 释放，*size = 实际字节数
    //   失败 *buffer = nullptr。简单 2 次重试 + 递增退避，无 Range 续传（小文件不值得）
    static bool Download(const std::string& url, size_t max_size,
                         uint8_t** buffer, size_t* size);

    const std::string& GetFirmwareVersion() const { return firmware_version_; }
    const std::string& GetCurrentVersion() const { return current_version_; }
    const std::string& GetFirmwareUrl() const { return firmware_url_; }
    const std::string& GetActivationMessage() const { return activation_message_; }
    const std::string& GetActivationCode() const { return activation_code_; }
    std::string GetCheckVersionUrl();

private:
    std::string activation_message_;
    std::string activation_code_;
    bool has_new_version_ = false;
    bool has_mqtt_config_ = false;
    bool has_websocket_config_ = false;
    bool has_server_time_ = false;
    bool has_activation_code_ = false;
    bool has_serial_number_ = false;
    bool has_activation_challenge_ = false;
    std::string current_version_;
    std::string firmware_version_;
    std::string firmware_url_;
    std::string activation_challenge_;
    std::string serial_number_;
    int activation_timeout_ms_ = 30000;

    std::function<void(int progress, size_t speed)> upgrade_callback_;
    std::vector<int> ParseVersion(const std::string& version);
    bool IsNewVersionAvailable(const std::string& currentVersion, const std::string& newVersion);
    std::string GetActivationPayload();
    std::unique_ptr<Http> SetupHttp();

    /// 通用: POST OTA_URL/<path>，payload 由调用方构建，调用后自动释放
    static esp_err_t PostToOta(const std::string& path, cJSON* payload);
};

#endif // _OTA_H
