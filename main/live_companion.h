#ifndef LIVE_COMPANION_H
#define LIVE_COMPANION_H

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_timer.h>

#include <string>
#include <vector>
#include <atomic>
#include <mutex>

#include "device_state.h"

class Application;

// 直播伴侣状态
enum class LiveState {
    kIdle,       // 空闲
    kPlaying,    // 播放中（等待 TTS 完成）
    kDelay,      // 两项之间的延时等待
    kSuspended,  // 被用户交互暂停
};

// 脚本项
struct ScriptItem {
    enum class Type { kTts, kTtai, kSet };
    Type type = Type::kTts;
    std::string text;
    int delay_ms = 0;   // 播放完后等待时间(ms)
    int volume = -1;     // -1 表示使用全局默认
};

/**
 * AI 直播伴侣
 *
 * 通过 HTTP 拉取 JSON 脚本，按顺序循环播放 TTS/TTAI 内容。
 * 支持高优先级用户交互打断，完成后自动恢复脚本。
 *
 * 远程命令格式:
 * ┌───────────────────┬───────────────────────────────────────────────────────┐
 * │ 启动              │ {"type":"live_companion","action":"start","url":"..."} │
 * │ 停止              │ {"type":"live_companion","action":"stop"}              │
 * │ 状态              │ {"type":"live_companion","action":"status"}            │
 * └───────────────────┴───────────────────────────────────────────────────────┘
 */
class LiveCompanion {
public:
    explicit LiveCompanion(Application* app);
    ~LiveCompanion();

    LiveCompanion(const LiveCompanion&) = delete;
    LiveCompanion& operator=(const LiveCompanion&) = delete;

    /// 通过 URL 加载远程脚本并开始播放（异步 HTTP 下载）
    void Start(const std::string& url);

    /// 直接加载 JSON 脚本内容并开始播放（WS 推送场景）
    void StartWithScript(const std::string& json_str);

    /// 停止播放并清空脚本
    void Stop();

    /// 暂停脚本（用户交互打断时调用）
    void Suspend();

    /// 恢复脚本播放
    void Resume();

    /// 从头重新开始当前脚本
    void Restart();

    LiveState GetState() const { return state_.load(); }
    int GetCurrentIndex() const { return current_index_.load(); }
    int GetTotalItems() const { return total_items_.load(); }
    int GetLoopCount() const { return loop_count_.load(); }
    bool IsRunning() const { return state_.load() != LiveState::kIdle; }

    /// TTS 播放完成通知（由 Application tts.stop 拦截后调用，替代状态转换）
    void NotifyTtsFinished();

    /// 是否有可恢复的脚本（停止后仍可重启）
    bool HasLastScript() const { return !last_script_json_.empty(); }

    /// 重新启动上一次的脚本
    void RestartLast();

private:
    Application* app_;

    // 状态
    std::atomic<LiveState> state_{LiveState::kIdle};
    std::atomic<int> current_index_{0};
    std::atomic<int> total_items_{0};
    std::atomic<int> loop_count_{0};
    std::atomic<bool> recap_pending_{false};

    // 脚本数据
    std::mutex script_mutex_;
    std::vector<ScriptItem> items_;
    bool loop_ = true;
    int default_volume_ = -1;
    int resume_delay_ms_ = 5000;  // 打断后恢复延时，默认 5 秒
    std::string script_name_;
    std::string pending_url_;
    std::string last_script_json_;  // 保存最后的脚本内容，用于唤醒后自动恢复

    // 延时定时器
    esp_timer_handle_t delay_timer_ = nullptr;

    // 加载任务句柄
    TaskHandle_t load_task_handle_ = nullptr;

    // 内部方法
    void OnDeviceStateChanged(DeviceState prev, DeviceState curr);
    void PlayCurrentItem();
    void ScheduleNext();
    void ScheduleResumeAfterDelay(int delay_ms);

    bool LoadScript(const std::string& url);
    bool ParseScript(const std::string& json_str);

    static void LoadScriptTask(void* arg);
    static void DelayTimerCallback(void* arg);
};

#endif // LIVE_COMPANION_H
