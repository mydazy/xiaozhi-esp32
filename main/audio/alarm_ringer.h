#ifndef ALARM_RINGER_H
#define ALARM_RINGER_H

#include <atomic>
#include <string>
#include <esp_timer.h>

// AlarmRinger · 双模响铃（闹铃 / 提醒）+ 多种关停 · 单例
class AlarmRinger {
public:
    static AlarmRinger& GetInstance();

    AlarmRinger(const AlarmRinger&) = delete;
    AlarmRinger& operator=(const AlarmRinger&) = delete;

    enum class Kind : uint8_t {
        kAlarm    = 0,
        kReminder = 1,
    };

    void Start(const std::string& message, Kind kind = Kind::kAlarm);
    void Stop(const char* reason);
    bool IsRinging() const { return ringing_.load(std::memory_order_acquire); }
    bool ShakeStop(int min_count);

private:
    AlarmRinger() = default;
    ~AlarmRinger() = default;

    // esp_timer 回调 · 5 秒周期调一次（渐入音量 + 再响铃声）
    static void OnTickStatic(void* arg);
    void OnTick();

    // esp_timer 回调 · 5 分钟一次（自动停防扰民）
    static void OnTimeoutStatic(void* arg);

    void DispatchWakeWord();

    std::atomic<bool> ringing_{false};
    esp_timer_handle_t ring_timer_ = nullptr;     // 周期 5s · 推进
    esp_timer_handle_t timeout_timer_ = nullptr;  // 一次性兜底自停
    int64_t start_us_ = 0;                        // Start 时戳（kAlarm 算 elapsed 渐入档位 · kReminder 仅 LOG）
    int saved_volume_ = -1;                       // 原音量 · Stop 时恢复
    int ring_count_ = 0;                          // kReminder：已响铃次数（1/2/3 · 第 3 次后自停）
    int shake_count_ = 0;                         // ShakeStop 累计窗口内摇晃次数
    int64_t shake_first_us_ = 0;                  // ShakeStop 当前窗口起点
    int cycle_step_ = 0;                          // kAlarm：周期内 tick 步数（0..17 · 5s/步 · 90s/周期）
    Kind kind_ = Kind::kAlarm;                    // 当前响铃模式
    std::string message_;                         // AI 播报用 · 仅 Start 内更新
};

#endif // ALARM_RINGER_H
