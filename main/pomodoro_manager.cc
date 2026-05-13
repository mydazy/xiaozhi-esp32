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
        // 计时结束 · 停 timer · 切 Idle · 触发 finish 回调
        esp_timer_stop(timer_);
        state_.store(State::kIdle, std::memory_order_release);
        ESP_LOGI(TAG, "Finished");
        EmitTick();   // 让 UI 显示 00:00 一帧 + 切回 Idle 状态
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

    mcp.AddTool("self.pomodoro.start",
        "启动番茄钟倒计时（默认 25 分钟，最长 99 分钟）。"
        "**触发话术（任一即调用，不要反问）**："
        "『番茄钟 / 打开番茄钟 / 开始番茄钟 / 来个番茄钟』→ 默认 25 分钟（duration_min 不传）；"
        "『倒计时 X 分钟 / 设置 X 分钟倒计时 / X 分钟番茄钟 / 专注 X 分钟 / 学习 X 分钟 / 帮我计时 X 分钟』"
        "→ 从话术抽出整数 X 传给 duration_min（X 范围 1-99 · 小数四舍五入 · 超 99 截到 99 · 含『半』如『半小时』视为 30 · 含『一刻钟』视为 15）。"
        "**调用前不要确认，直接调用让设备显示倒计时**。"
        "运行中再次调用 = 重启计时（覆盖当前进度）。"
        "参数：duration_min=0-99（0 或不传 = 默认 25）。"
        "返回 JSON {state, total_sec, remain_sec}。",
        PropertyList({
            Property("duration_min", kPropertyTypeInteger, 0, (int)kMaxDurationMin)
        }),
        [this](const PropertyList& props) -> ReturnValue {
            int min = props["duration_min"].value<int>();
            uint32_t sec = (min > 0) ? (uint32_t)min * 60 : 0;
            if (!Start(sec)) return std::string("{\"success\":false}");

            char buf[96];
            snprintf(buf, sizeof(buf),
                     "{\"success\":true,\"state\":\"running\",\"total_sec\":%u,\"remain_sec\":%u}",
                     total_sec_, remain_sec_.load());
            return std::string(buf);
        });

    mcp.AddTool("self.pomodoro.pause",
        "暂停番茄钟。用户说『暂停 / 先停一下 / 等等』时调用。",
        PropertyList(),
        [this](const PropertyList&) -> ReturnValue {
            return std::string(Pause() ? "{\"success\":true,\"state\":\"paused\"}"
                                       : "{\"success\":false,\"reason\":\"not_running\"}");
        });

    mcp.AddTool("self.pomodoro.resume",
        "恢复番茄钟。用户说『继续 / 接着来』时调用。",
        PropertyList(),
        [this](const PropertyList&) -> ReturnValue {
            return std::string(Resume() ? "{\"success\":true,\"state\":\"running\"}"
                                        : "{\"success\":false,\"reason\":\"not_paused\"}");
        });

    mcp.AddTool("self.pomodoro.stop",
        "取消番茄钟（不算完成）。用户说『停止 / 取消 / 不做了』时调用。",
        PropertyList(),
        [this](const PropertyList&) -> ReturnValue {
            return std::string(Stop() ? "{\"success\":true,\"state\":\"idle\"}"
                                      : "{\"success\":false,\"reason\":\"already_idle\"}");
        });

    mcp.AddTool("self.pomodoro.status",
        "查询番茄钟状态。返回 {state: idle/running/paused, remain_sec, total_sec}。"
        "用户问『还剩多久 / 番茄钟到了吗』时调用。",
        PropertyList(),
        [this](const PropertyList&) -> ReturnValue {
            char buf[128];
            snprintf(buf, sizeof(buf),
                     "{\"state\":\"%s\",\"remain_sec\":%u,\"total_sec\":%u}",
                     PomodoroStateToString(GetState()),
                     remain_sec_.load(), total_sec_);
            return std::string(buf);
        });

    ESP_LOGI(TAG, "MCP 番茄钟工具已注册");
}
