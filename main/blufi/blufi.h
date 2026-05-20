#ifndef _BLUFI_H_
#define _BLUFI_H_

#include <string>
#include <functional>
#include <atomic>
#include "esp_blufi_api.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/**
 * Blufi - 蓝牙配网纯数据通道
 *
 * 职责：BLE 广播/连接、数据传输
 * 凭证验证/保存由业务层通过 validator 处理
 */
class Blufi {
public:
    static Blufi& GetInstance();

    bool InitializeController();
    bool Start(const std::string& device_name);
    void Stop();

    bool IsInitialized() const { return initialized_; }
    bool IsControllerInitialized() const { return controller_initialized_; }
    bool IsAdvertising() const { return advertising_; }

    // 永久释放 BT 控制器静态 .bss/.data 区回 heap（30-50KB 内部 RAM）
    // ⚠️ 调用后本次启动若再次触发 InitializeController，内部会自动走
    //    Application::Reboot（不是裸 esp_restart —— 会先停音频服务、关功放 LDO、关背光，
    //    避免 I2S DMA 写已断电 codec 导致的 I2C 阻塞 TWDT / 破音 / 黑屏闪烁）
    // 调用前提：BT controller 已 deinit 或从未 init（如已有 WiFi 凭证直接 SmartConnect 成功）
    // 幂等：内部用类静态 flag，重复调用只释放一次
    static void ReleaseStaticMem();

    // 是否已调过 ReleaseStaticMem（InitializeController 需据此判断）
    static bool IsStaticMemReleased() { return static_mem_released_; }

    // 设置凭证验证器
    void SetCredentialValidator(
        std::function<bool(const std::string& ssid,
                           const std::string& password,
                           std::string& error_message)> validator);

    // 设置配网成功回调
    void OnConfigSuccess(std::function<void()> callback);

    void SendData(const char* data, int len);
    const std::string& GetBindingCode() const { return binding_code_; }

private:
    Blufi() = default;
    ~Blufi() = default;
    Blufi(const Blufi&) = delete;
    Blufi& operator=(const Blufi&) = delete;

    static void BlufiCallback(esp_blufi_cb_event_t event, esp_blufi_cb_param_t* param);
    static void OnScanDone();
    bool StartAdvertising();

    std::function<void()> on_success_;
    std::function<bool(const std::string&, const std::string&, std::string&)> credential_validator_;

    // 状态
    std::atomic<bool> controller_initialized_{false};
    std::atomic<bool> initialized_{false};
    std::atomic<bool> advertising_{false};
    std::atomic<bool> ble_connected_{false};
    std::atomic<bool> stopping_{false};
    std::atomic<bool> scanning_{false};
    std::atomic<int> scan_retry_count_{0};
    std::atomic<esp_timer_handle_t> retry_timer_{nullptr};

    SemaphoreHandle_t init_done_sem_ = nullptr;

    std::atomic<uint16_t> conn_handle_{0};

    // 数据
    std::string device_name_;
    std::string ssid_;
    std::string password_;
    std::string binding_code_;

    static bool static_mem_released_;
};

#endif