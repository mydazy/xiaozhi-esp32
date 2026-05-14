#include "pomodoro_manager.h"
#include "mcp_server.h"

#include <esp_log.h>
#include <cstring>

#define TAG "Pomodoro"

PomodoroManager& PomodoroManager::GetInstance() {
    static PomodoroManager instance;
    return instance;
}

PomodoroManager::PomodoroManager() {
    esp_timer_create_args_t args = {
        .callback = &PomodoroManager::TimerCallback,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "pomodoro_tick",
        .skip_unhandled_events = true,
    };
    esp_err_t err = esp_timer_create(&args, &timer_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_timer_create failed: %s", esp_err_to_name(err));
        timer_ = nullptr;
    }
}

PomodoroManager::~PomodoroManager() {
    if (timer_) {
        esp_timer_stop(timer_);
        esp_timer_delete(timer_);
        timer_ = nullptr;
    }
}

// ============================================================
// 控制 API
// ============================================================

bool PomodoroManager::Start(uint32_t duration_sec) {
    if (!timer_) return false;

    if (duration_sec == 0) duration_sec = kDefaultDurationMin * 60;
    if (duration_sec > kMaxDurationMin * 60) duration_sec = kMaxDurationMin * 60;
    if (duration_sec < kMinDurationMin * 60) duration_sec = kMinDurationMin * 60;

    // 幂等：再次 Start 视为重启 → 先停定时器再重置
    esp_timer_stop(timer_);

    total_sec_ = duration_sec;
    remain_sec_.store(duration_sec, std::memory_order_release);
    state_.store(State::kRunning, std::memory_order_release);

    esp_err_t err = esp_timer_start_periodic(timer_, 1000ULL * 1000);  // 1 Hz
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_timer_start_periodic failed: %s", esp_err_to_name(err));
        state_.store(State::kIdle, std::memory_order_release);
        return false;
    }

    ESP_LOGI(TAG, "Start: %u sec (%u min)", duration_sec, duration_sec / 60);
    EmitTick();
    return true;
}

bool PomodoroManager::Pause() {
    if (!timer_) return false;
    State s = state_.load(std::memory_order_acquire);
    if (s != State::kRunning) return false;

    esp_timer_stop(timer_);
    state_.store(State::kPaused, std::memory_order_release);
    ESP_LOGI(TAG, "Pause @ remain=%u", remain_sec_.load());
    EmitTick();
    return true;
}

bool PomodoroManager::Resume() {
    if (!timer_) return false;
    State s = state_.load(std::memory_order_acquire);
    if (s != State::kPaused) return false;

    esp_err_t err = esp_timer_start_periodic(timer_, 1000ULL * 1000);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Resume start_periodic failed: %s", esp_err_to_name(err));
        return false;
    }
    state_.store(State::kRunning, std::memory_order_release);
    ESP_LOGI(TAG, "Resume @ remain=%u", remain_sec_.load());
    EmitTick();
    return true;
}

bool PomodoroManager::Stop() {
    if (!timer_) return false;
    State s = state_.load(std::memory_order_acquire);
    if (s == State::kIdle) return false;

    esp_timer_stop(timer_);
    state_.store(State::kIdle, std::memory_order_release);
    remain_sec_.store(0, std::memory_order_release);
    total_sec_ = 0;
    ESP_LOGI(TAG, "Stop");
    EmitTick();
    return true;
}

bool PomodoroManager::TogglePaused() {
    State s = state_.load(std::memory_order_acquire);
    switch (s) {
        case State::kIdle:    return Start(0);          // Idle 时启动默认 25 min
        case State::kRunning: return Pause();
        case State::kPaused:  return Resume();
    }
    return false;
}

// ============================================================
// 计时 tick
// ============================================================

void PomodoroManager::TimerCallback(void* arg) {
    static_cast<PomodoroManager*>(arg)->OnTick();
}

void PomodoroManager::OnTick() {
    uint32_t remain = remain_sec_.load(std::memory_order_acquire);
    if (remain == 0) {
        // 兜底（不应触发：上一轮 tick 已停了 timer）
        esp_timer_stop(timer_);
        return;
    }

    --remain;
    remain_sec_.store(remain, std::memory_order_release);

    if (remain == 0) {
        esp_timer_stop(timer_);
        state_.store(State::kIdle, std::memory_order_release);
        ESP_LOGI(TAG, "Finished");
        if (finish_cb_) finish_cb_();
        return;
    }

    EmitTick();
}

void PomodoroManager::EmitTick() {
    if (tick_cb_) tick_cb_(state_.load(std::memory_order_acquire),
                           remain_sec_.load(std::memory_order_acquire),
                           total_sec_);
}

// ============================================================
// MCP 语音控制
// ============================================================

void PomodoroManager::RegisterMcpTools() {
    static bool registered = false;
    if (registered) return;
    registered = true;

    auto& mcp = McpServer::GetInstance();

    //   触发词：番茄钟/倒计时/专注 X 分钟/学习 X 分钟 → 立即调（不反问）
    //   时长抽取：『半』=30 · 『一刻钟』=15 · X 范围 1-99 · 超 99 截 99
    //   duration_min 不传 = 默认 25 · 运行中再调 = 重启
    mcp.AddTool("self.pomodoro.start",
        "番茄钟/倒计时/专注/学习 X 分钟 → 立即调用不反问。"
        "X 抽数字（半=30/一刻钟=15/超 99 截 99）。"
        "不传 duration_min = 默认 25。运行中再调 = 重启。"
        "返回 {state, total_sec, remain_sec}。",
        PropertyList({
            Property("duration_min", kPropertyTypeInteger, 0, (int)kMaxDurationMin)
        }),
        [this](const PropertyList& props) -> ReturnValue {
            int min = props["duration_min"].value<int>();
            uint32_t sec = (min > 0) ? (uint32_t)min * 60 : 0;
            if (!Start(sec)) return std::string("{\"success\":false}");

            char buf[96];
            snprintf(buf, sizeof(buf),
                     "{\"success\":true,\"state\":\"running\",\"total_sec\":%lu,\"remain_sec\":%lu}",
                     (unsigned long)total_sec_, (unsigned long)remain_sec_.load());
            return std::string(buf);
        });

    mcp.AddTool("self.pomodoro.pause",
        "暂停番茄钟。用户说『暂停 / 停一下』时调。",
        PropertyList(),
        [this](const PropertyList&) -> ReturnValue {
            return std::string(Pause() ? "{\"success\":true,\"state\":\"paused\"}"
                                       : "{\"success\":false,\"reason\":\"not_running\"}");
        });


    mcp.AddTool("self.pomodoro.stop",
        "取消番茄钟。用户说『停止 / 取消 / 不做了』时调。",
        PropertyList(),
        [this](const PropertyList&) -> ReturnValue {
            return std::string(Stop() ? "{\"success\":true,\"state\":\"idle\"}"
                                      : "{\"success\":false,\"reason\":\"already_idle\"}");
        });

    mcp.AddTool("self.pomodoro.status",
        "查番茄钟状态。返回 {state: idle/running/paused, remain_sec, total_sec}。"
        "**仅在用户主动询问**『还剩多久 / 番茄钟到了吗 / 设了多久』等需要数据的问题时调用。"
        "（收到唤醒词『番茄钟到了鼓励一下』时已含完整语义 · 直接温柔夸奖+提醒休息眼睛即可 · 不必调本工具）",
        PropertyList(),
        [this](const PropertyList&) -> ReturnValue {
            char buf[128];
            snprintf(buf, sizeof(buf),
                     "{\"state\":\"%s\",\"remain_sec\":%lu,\"total_sec\":%lu}",
                     PomodoroStateToString(GetState()),
                     (unsigned long)remain_sec_.load(), (unsigned long)total_sec_);
            return std::string(buf);
        });

    ESP_LOGI(TAG, "MCP 番茄钟工具已注册");
}
