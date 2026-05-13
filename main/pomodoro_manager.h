#ifndef POMODORO_MANAGER_H
#define POMODORO_MANAGER_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <esp_timer.h>

// 番茄钟管理器（单例 · 后台服务型 · 不进 ActivityType）
//
// 三维心智模型落位（详见 docs/p30-architecture.html § 一.5.2）：
//   - DeviceState : 不引入新态（12 态不动）
//   - ActivityType: 不进入（番茄钟是后台计时服务，不占用音频管线）
//   - SceneType   : kPomodoro（独占 UI · 与 kClock/kPlayer 互斥）
//   - AudioSource : 完成时复用 AlarmRinger 路径（PomodoroBell 待 ringer 分流扩展）
//
// 入口：
//   - MCP 语音 : self.pomodoro.{start, pause, resume, stop, status}
//   - 触屏    : UiDisplay 的中央圆按钮 → TogglePaused()
//
// 计时：esp_timer 1Hz 周期 tick · 回调投到 Application::Schedule → 主线程 + DisplayLock 内更新 UI
class PomodoroManager {
public:
    static PomodoroManager& GetInstance();

    PomodoroManager(const PomodoroManager&) = delete;
    PomodoroManager& operator=(const PomodoroManager&) = delete;

    enum class State : uint8_t {
        kIdle    = 0,   // 未启动
        kRunning = 1,   // 倒计时中
        kPaused  = 2,   // 已暂停
    };

    // ---- 控制（MCP / 触屏共用 · 线程安全）----
    // duration_sec=0 时使用默认 25 分钟
    bool Start(uint32_t duration_sec = 0);
    bool Pause();
    bool Resume();
    bool Stop();
    bool TogglePaused();   // 触屏按钮统一入口（Running↔Paused · Idle 时启动默认 25min）

    // ---- 查询 ----
    State GetState() const { return state_.load(std::memory_order_acquire); }
    uint32_t GetRemainSec() const { return remain_sec_.load(std::memory_order_acquire); }
    uint32_t GetTotalSec() const { return total_sec_; }
    bool IsActive() const {
        State s = GetState();
        return s == State::kRunning || s == State::kPaused;
    }

    // ---- UI 同步回调（每 1s tick 或状态切换时触发 · 在 esp_timer 任务上下文）----
    using TickCb   = std::function<void(State state, uint32_t remain_sec, uint32_t total_sec)>;
    using FinishCb = std::function<void()>;
    void SetTickCallback(TickCb cb)     { tick_cb_   = std::move(cb); }
    void SetFinishCallback(FinishCb cb) { finish_cb_ = std::move(cb); }

    // ---- MCP 语音控制注册（幂等）----
    void RegisterMcpTools();

    // 默认 / 上限（分钟）
    static constexpr uint32_t kDefaultDurationMin = 25;
    static constexpr uint32_t kMinDurationMin     = 1;
    static constexpr uint32_t kMaxDurationMin     = 99;

private:
    PomodoroManager();
    ~PomodoroManager();

    static void TimerCallback(void* arg);   // esp_timer 1Hz 回调
    void OnTick();                          // 实例方法 · remain-- 到 0 触发 finish_cb_
    void EmitTick();                        // 触发 tick_cb_（持当前 state/remain）

    esp_timer_handle_t    timer_ = nullptr;
    std::atomic<State>    state_{State::kIdle};
    std::atomic<uint32_t> remain_sec_{0};
    uint32_t              total_sec_ = 0;

    TickCb   tick_cb_;
    FinishCb finish_cb_;
};

inline const char* PomodoroStateToString(PomodoroManager::State s) {
    switch (s) {
        case PomodoroManager::State::kIdle:    return "idle";
        case PomodoroManager::State::kRunning: return "running";
        case PomodoroManager::State::kPaused:  return "paused";
    }
    return "?";
}

#endif  // POMODORO_MANAGER_H
