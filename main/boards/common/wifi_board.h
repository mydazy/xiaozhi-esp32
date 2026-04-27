#ifndef WIFI_BOARD_H
#define WIFI_BOARD_H

#include "board.h"

// 配网模式
enum class ConfigMode {
    BLUFI,  // 蓝牙配网
    AP      // 热点配网
};

class WifiBoard : public Board {
protected:
    bool wifi_config_mode_ = false;
    ConfigMode current_config_mode_ = ConfigMode::BLUFI;
    bool config_initialized_ = false;
    int switch_count_ = 0;  // 切换计数器
    NetworkEventCallback network_event_callback_;  // application 注册的事件管道
    bool SmartConnect();
    bool StartConfigMode(ConfigMode mode, bool is_switch = false);
    bool CheckResources();
    virtual std::string GetBoardJson() override;

public:
    WifiBoard();
    virtual void EnterWifiConfigMode();  // 长按按键 / WiFi 连接超时触发
    virtual std::string GetBoardType() override;
    virtual void StartNetwork() override;
    virtual void SetNetworkEventCallback(NetworkEventCallback callback) override {
        network_event_callback_ = std::move(callback);
    }
    virtual NetworkInterface* GetNetwork() override;
    virtual const char* GetNetworkStateIcon() override;
    virtual void SetPowerSaveLevel(PowerSaveLevel level) override;
    virtual void ResetWifiConfiguration();
    virtual void SwitchConfigMode();
    virtual void StopConfigMode();
    void OnConfigSuccess();
    virtual AudioCodec* GetAudioCodec() override { return nullptr; }
    virtual std::string GetDeviceStatusJson() override;
};

#endif // WIFI_BOARD_H