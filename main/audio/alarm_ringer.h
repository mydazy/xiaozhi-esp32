#ifndef ALARM_RINGER_H
#define ALARM_RINGER_H

#include <atomic>
#include <string>
#include <esp_timer.h>

// AlarmRinger · 闹钟持续响铃 + 渐入音量 + 多种关停
//
// 设计意图（详见 docs/p30-alarm-flows.html § 10.3）：
//   闹钟 ≠ 一声通知 · 必须循环响 + 渐入 + 强制关停（摇晃/按键/触摸/语音/超时）
//
// 状态：Idle / Ringing · 单例 · 闹钟到点触发 Start · 5 种关停均调 Stop
class AlarmRinger {
public:
    static AlarmRinger& GetInstance();

    AlarmRinger(const AlarmRinger&) = delete;
    AlarmRinger& operator=(const AlarmRinger&) = delete;

    // 闹钟到点调用 · 启动循环响铃 + 渐入音量 + AI 语音播报
    // 幂等：重复 Start 当前 message 覆盖 · 不会启动多个响铃
    void Start(const std::string& message);

    // 任一关停事件调用 · reason 仅用于 LOG
    // 幂等：未在响铃中调用 Stop 是 no-op
    void Stop(const char* reason);

    // 按键 / 触摸 / 摇晃 handler 用此 API 抢占（响铃中先 Stop 不进对话）
    bool IsRinging() const { return ringing_.load(std::memory_order_acquire); }

private:
    AlarmRinger() = default;
    ~AlarmRinger() = default;

    // esp_timer 回调 · 5 秒周期调一次（渐入音量 + 再响铃声）
    static void OnTickStatic(void* arg);
    void OnTick();

    // esp_timer 回调 · 5 分钟一次（自动停防扰民）
    static void OnTimeoutStatic(void* arg);

    std::atomic<bool> ringing_{false};
    esp_timer_handle_t ring_timer_ = nullptr;     // 周期 5s · 三次响铃推进
    esp_timer_handle_t timeout_timer_ = nullptr;  // 一次性兜底自停（防 ring_timer 异常）
    int64_t start_us_ = 0;                        // Start 时戳（仅 LOG elapsed 用）
    int saved_volume_ = -1;                       // 原音量 · Stop 时恢复
    int ring_count_ = 0;                          // 已响铃次数（1/2/3 · 第 3 次后自停）
    std::string message_;                         // AI 播报用 · 仅 Start 内更新
};

#endif // ALARM_RINGER_H
