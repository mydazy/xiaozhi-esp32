/**
 * @file ml307_gnss.h
 * @brief ML307R GNSS 定位（通过 AtUart 共享 UART，不独占串口）
 *
 * 支持: 开关 GNSS、配置定位类型(GPS/BDS/GLONASS)、卫星数/坐标回调
 * 用法: 在 4G 网络连接成功后通过 modem->GetAtUart() 获取 uart 共享通道
 */

#pragma once

#include <at_uart.h>
#include <functional>
#include <memory>
#include <string>
#include <atomic>

/// GNSS 定位系统类型（可组合）
enum GnssSystem : uint8_t {
    kGnssGps      = 0x01,
    kGnssBds      = 0x02,
    kGnssGlonass  = 0x04,
    kGnssAll      = 0x07,
};

/// GNSS 定位数据
struct GnssFix {
    bool valid = false;
    double latitude = 0.0;
    double longitude = 0.0;
    int satellites = 0;
    double hdop = 0.0;
    char utc_time[16] = {};
};

using GnssFixCallback = std::function<void(const GnssFix& fix)>;
using GnssSatCallback = std::function<void(int satellites_in_view)>;

class Ml307Gnss {
public:
    Ml307Gnss(std::shared_ptr<AtUart> uart);
    ~Ml307Gnss();

    Ml307Gnss(const Ml307Gnss&) = delete;
    Ml307Gnss& operator=(const Ml307Gnss&) = delete;

    /// 开启 GNSS
    /// @param systems 定位系统组合，默认 GPS+BDS
    bool Start(uint8_t systems = kGnssGps | kGnssBds);

    /// 关闭 GNSS
    bool Stop();

    bool IsRunning() const { return running_; }
    const GnssFix& GetLastFix() const { return last_fix_; }

    void SetFixCallback(GnssFixCallback cb) { fix_callback_ = std::move(cb); }
    void SetSatCallback(GnssSatCallback cb) { sat_callback_ = std::move(cb); }

private:
    std::shared_ptr<AtUart> uart_;
    std::atomic<bool> running_{false};
    GnssFix last_fix_;
    GnssFixCallback fix_callback_;
    GnssSatCallback sat_callback_;
    std::list<UrcCallback>::iterator urc_handle_;
    bool urc_registered_ = false;

    void ParseNmea(const std::string& line);
    void ParseGga(const char* sentence);
    void ParseRmc(const char* sentence);
    void ParseGsv(const char* sentence);
    static bool NmeaToDecimal(const char* raw, char hemisphere, double& out);
    static int SplitFields(char* buf, char* fields[], int max_fields);
};
