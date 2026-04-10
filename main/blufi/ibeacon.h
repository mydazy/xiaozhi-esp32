#ifndef IBEACON_H
#define IBEACON_H

#include <string>
#include <functional>
#include <cstdint>

/**
 * iBeacon 数据结构
 */
struct IBeaconInfo {
    std::string uuid;      // UUID
    uint16_t major;        // Major 值
    uint16_t minor;        // Minor 值
    int8_t rssi;           // 信号强度 (dBm)
    int8_t tx_power;       // 发射功率

    std::string ToString() const;
    float CalculateDistance() const;
};

using IBeaconCallback = std::function<void(const IBeaconInfo& beacon)>;

/**
 * IBeacon - iBeacon 扫描器
 *
 * 省电模式: 1280ms 扫描间隔, ~3-5mA
 */
class IBeacon {
public:
    static IBeacon& GetInstance();

    bool Start();
    void StartDeferred(uint32_t timeout_ms = 30000);
    void Stop();
    void OnDetected(IBeaconCallback callback);

    bool IsScanning() const { return scanning_; }
    bool IsInitialized() const { return initialized_; }

private:
    IBeacon() = default;
    ~IBeacon() = default;
    IBeacon(const IBeacon&) = delete;
    IBeacon& operator=(const IBeacon&) = delete;

    static int ScanCallback(struct ble_gap_event* event, void* arg);
    static bool ParseIBeacon(const uint8_t* adv_data, size_t len, IBeaconInfo& beacon);
    static std::string UuidToString(const uint8_t* uuid);

    bool scanning_ = false;
    bool initialized_ = false;
    IBeaconCallback on_detected_;
};

#endif // IBEACON_H