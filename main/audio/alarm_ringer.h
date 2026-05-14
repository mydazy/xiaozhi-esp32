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

    // 响铃模式 · 两套策略走同一 ringer 单例
    enum class Kind : uint8_t {
        kAlarm    = 0,
        kReminder = 1,
    };

    // 到点触发 · 启动响铃 + AI 语音播报
    // 幂等：重复 Start 当前 message/kind 覆盖 · 不会启动多个响铃
    void Start(const std::string& message, Kind kind = Kind::kAlarm);

    // 任一关停事件调用 · reason 仅用于 LOG
    // 幂等：未在响铃中调用 Stop 是 no-op
    void Stop(const char* reason);

    // 按键 / 触摸 / 摇晃 handler 用此 API 抢占（响铃中先 Stop 不进对话）
    bool IsRinging() const { return ringing_.load(std::memory_order_acquire); }

    // 摇晃停闹（防走路误关 · 5s 窗内累计 min_count 次摇晃才真 Stop）
    // 返回值：是否在响铃中（true = 事件被吞 · 上层应 return）
    bool ShakeStop(int min_count);

private:
    AlarmRinger() = default;
    ~AlarmRinger() = default;

    // esp_timer 回调 · 5 秒周期调一次（渐入音量 + 再响铃声）
    static void OnTickStatic(void* arg);
    void OnTick();

    // esp_timer 回调 · 5 分钟一次（自动停防扰民）
    static void OnTimeoutStatic(void* arg);

    // 投递 WakeWordInvoke 到主线程（去重 Start / OnTick 三处 Schedule lambda）
    void DispatchWakeWord(int elapsed_s);

    std::atomic<bool> ringing_{false};
    esp_timer_handle_t ring_timer_ = nullptr;     // 周期 5s · 推进
    esp_timer_handle_t timeout_timer_ = nullptr;  // 一次性兜底自停
    int64_t start_us_ = 0;                        // Start 时戳（kAlarm 算 elapsed 渐入档位 · kReminder 仅 LOG）
    int saved_volume_ = -1;                       // 原音量 · Stop 时恢复
    int ring_count_ = 0;                          // kReminder：已响铃次数（1/2/3 · 第 3 次后自停）
    int shake_count_ = 0;                         // ShakeStop 累计窗口内摇晃次数
    int64_t shake_first_us_ = 0;                  // ShakeStop 当前窗口起点
    int last_ai_prompt_sec_ = 0;                  // kAlarm：上次 AI 念叨时点（控 20s 重念周期）
    Kind kind_ = Kind::kAlarm;                    // 当前响铃模式
    std::string message_;                         // AI 播报用 · 仅 Start 内更新
};

#endif // ALARM_RINGER_H
