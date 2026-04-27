#pragma once

#include <functional>
#include <atomic>
#include <esp_wifi.h>
#include <esp_timer.h>

// CSI 接近检测距离档位
enum CsiZone {
    kCsiZoneNone = 0,    // 无人
    kCsiZoneFar,         // 远区 (>3m)
    kCsiZoneMedium,      // 中区 (1-3m)
    kCsiZoneNear,        // 近区 (<1m)
};

// CSI 接近事件
struct CsiEvent {
    CsiZone zone;
    CsiZone previous_zone;
    float variance;       // 活动指标（调试用）
    float amplitude;      // 平均幅度（调试用）
};

/**
 * WiFi CSI 人体接近检测
 *
 * 通过分析 WiFi CSI 子载波幅度方差变化检测人体靠近。
 * 默认关闭，通过远程命令 {"type":"csi","enabled":true} 开启。
 * 仅在 WiFi STA 连接后可用。
 */
class WifiCsi {
public:
    static WifiCsi& GetInstance();

    // 功能开关（NVS 持久化，默认关闭）
    void SetEnabled(bool enabled);
    bool IsEnabled() const { return enabled_.load(); }

    void Start();
    void Stop();
    bool IsRunning() const { return running_.load(); }
    CsiZone GetZone() const { return zone_.load(); }

    // 区域变化回调（esp_timer 上下文，不要阻塞）
    void OnZoneChange(std::function<void(const CsiEvent&)> callback);

    // 调整阈值
    void SetThresholds(float near_val, float medium_val, float far_val);

    void PrintStats();

private:
    WifiCsi();
    ~WifiCsi();
    WifiCsi(const WifiCsi&) = delete;
    WifiCsi& operator=(const WifiCsi&) = delete;

    static void CsiRxCallback(void* ctx, wifi_csi_info_t* data);
    static void AnalysisCallback(void* arg);
    static void PingCallback(void* arg);
    void Analyze();
    static float Variance(const float* data, int len);

    std::atomic<bool> enabled_{false};
    std::atomic<bool> running_{false};
    std::atomic<CsiZone> zone_{kCsiZoneNone};

    std::function<void(const CsiEvent&)> on_zone_change_;
    esp_timer_handle_t timer_ = nullptr;
    esp_timer_handle_t ping_timer_ = nullptr;

    // 滑动窗口
    static constexpr int kWindow = 10;
    float var_buf_[kWindow];
    int buf_idx_ = 0;
    int buf_count_ = 0;

    // CSI 回调累加器（每个分析周期内的所有帧取平均）
    std::atomic<float> sum_var_{0.0f};
    std::atomic<int> frames_{0};

    // 基线自适应（无人时的 score 基线）
    float baseline_ = 0.0f;
    float initial_baseline_ = 0.0f;  // 初始基线快照，用于限制漂移上限
    int baseline_samples_ = 0;
    static constexpr int kBaselineSamples = 5;

    // 进入阈值（从低区域进入高区域，需要更高的 ratio）
    float th_near_enter_ = 10.0f;
    float th_medium_enter_ = 5.0f;
    float th_far_enter_ = 2.5f;

    // 离开阈值（从高区域降到低区域，ratio 需要更低，形成迟滞）
    float th_near_leave_ = 6.0f;
    float th_medium_leave_ = 3.0f;
    float th_far_leave_ = 1.5f;

    // 去抖（进入需要连续确认 kConfirmEnter 次，离开需要 kConfirmLeave 次）
    int confirm_count_ = 0;
    CsiZone pending_zone_ = kCsiZoneNone;
    static constexpr int kConfirmEnter = 3;   // 4.5s 确认进入
    static constexpr int kConfirmLeave = 4;   // 6.0s 确认离开

    // 基线漂移上限：不超过初始基线的 N 倍
    static constexpr float kBaselineCeilRatio = 2.5f;
};
