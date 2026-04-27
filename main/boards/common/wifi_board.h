#ifndef WIFI_BOARD_H
#define WIFI_BOARD_H

#include "board.h"
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <esp_timer.h>
#include <atomic>
#include <functional>

class WifiBoard : public Board {
public:
    // 配网模式（运行时可切换，默认值由 Settings("wifi").blufi 决定）
    enum class ConfigMode {
        AP,      // WiFi 热点配网（老流程）
        BLUFI,   // BLE 蓝牙配网（小程序优先）
    };

protected:
    esp_timer_handle_t connect_timer_ = nullptr;
    bool in_config_mode_ = false;
    ConfigMode current_config_mode_ = ConfigMode::AP;
    // 切换互斥：双击事件 schedule 到主任务执行 SwitchConfigMode 期间忽略再次双击
    std::atomic<bool> switching_config_mode_{false};
    NetworkEventCallback network_event_callback_ = nullptr;

    virtual std::string GetBoardJson() override;

    /**
     * Handle network event (called from WiFi manager callbacks)
     * @param event The network event type
     * @param data Additional data (e.g., SSID for Connecting/Connected events)
     */
    void OnNetworkEvent(NetworkEvent event, const std::string& data = "");

    /**
     * Start WiFi connection attempt
     */
    void TryWifiConnect();

    /**
     * Enter WiFi configuration mode (reads default mode from Settings)
     */
    void StartWifiConfigMode();

    /**
     * Start a specific config mode. Handles flash-op × LVGL protection internally.
     * @return true on success, false on failure (caller decides fallback)
     */
    bool StartConfigMode(ConfigMode mode);

    /**
     * Stop whichever config mode is currently active. Idempotent.
     */
    void StopConfigMode();

    /**
     * Build the double-click callback that schedules SwitchConfigMode via
     * Application::Schedule (decouples LVGL event thread from BLE/flash op).
     */
    std::function<void()> MakeSwitchCallback();

    /**
     * WiFi connection timeout callback
     */
    static void OnWifiConnectTimeout(void* arg);

public:
    WifiBoard();
    virtual ~WifiBoard();
    
    virtual std::string GetBoardType() override;
    
    /**
     * Start network connection asynchronously
     * This function returns immediately. Network events are notified through the callback set by SetNetworkEventCallback().
     */
    virtual void StartNetwork() override;
    
    virtual NetworkInterface* GetNetwork() override;
    virtual void SetNetworkEventCallback(NetworkEventCallback callback) override;
    virtual const char* GetNetworkStateIcon() override;
    virtual void SetPowerSaveLevel(PowerSaveLevel level) override;
    virtual AudioCodec* GetAudioCodec() override { return nullptr; }
    virtual std::string GetDeviceStatusJson() override;
    
    /**
     * Enter WiFi configuration mode (thread-safe, can be called from any task)
     */
    void EnterWifiConfigMode();

    /**
     * Check if in WiFi config mode
     */
    bool IsInWifiConfigMode() const;

    /**
     * Get current running config mode (only meaningful while in_config_mode_).
     */
    ConfigMode GetConfigMode() const { return current_config_mode_; }

    /**
     * Switch between BLUFI and AP at runtime. No-op if not in config mode.
     * Persists choice to Settings("wifi").blufi. Auto-falls back on failure.
     */
    void SwitchConfigMode();
};

#endif // WIFI_BOARD_H
