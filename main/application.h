#ifndef _APPLICATION_H_
#define _APPLICATION_H_

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
#include <esp_timer.h>

#include <string>
#include <mutex>
#include <deque>
#include <memory>
#include <atomic>

#include "protocol.h"
#include "ota.h"
#include "audio_service.h"
#include "device_state.h"
#include "device_state_machine.h"
#include "activity_type.h"
#include "audio_source.h"

class FlowEngine;
class RemoteCmd;

// Main event bits
#define MAIN_EVENT_SCHEDULE             (1 << 0)
#define MAIN_EVENT_SEND_AUDIO           (1 << 1)
#define MAIN_EVENT_WAKE_WORD_DETECTED   (1 << 2)
#define MAIN_EVENT_VAD_CHANGE           (1 << 3)
#define MAIN_EVENT_ERROR                (1 << 4)
#define MAIN_EVENT_ACTIVATION_DONE      (1 << 5)
#define MAIN_EVENT_CLOCK_TICK           (1 << 6)
#define MAIN_EVENT_NETWORK_CONNECTED    (1 << 7)
#define MAIN_EVENT_NETWORK_DISCONNECTED (1 << 8)
#define MAIN_EVENT_TOGGLE_CHAT          (1 << 9)
#define MAIN_EVENT_START_LISTENING      (1 << 10)
#define MAIN_EVENT_STOP_LISTENING       (1 << 11)
#define MAIN_EVENT_STATE_CHANGED        (1 << 12)


enum AecMode {
    kAecOff,
    kAecOnDeviceSide,
    kAecOnServerSide,
};

class Application {
public:
    static Application& GetInstance() {
        static Application instance;
        return instance;
    }
    // Delete copy constructor and assignment operator
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    /**
     * Initialize the application
     * This sets up display, audio, network callbacks, etc.
     * Network connection starts asynchronously.
     */
    void Initialize();

    /**
     * Run the main event loop
     * This function runs in the main task and never returns.
     * It handles all events including network, state changes, and user interactions.
     */
    void Run();

    DeviceState GetDeviceState() const { return state_machine_.GetState(); }
    bool IsVoiceDetected() const { return audio_service_.IsVoiceDetected(); }
    
    /**
     * Request state transition
     * Returns true if transition was successful
     */
    bool SetDeviceState(DeviceState state);

    /**
     * Schedule a callback to be executed in the main task
     */
    void Schedule(std::function<void()>&& callback);

    /**
     * Alert with status, message, emotion and optional sound
     */
    void Alert(const char* status, const char* message, const char* emotion = "", const std::string_view& sound = "");
    void DismissAlert();

    void AbortSpeaking(AbortReason reason);

    /**
     * Toggle chat state (event-based, thread-safe)
     * Sends MAIN_EVENT_TOGGLE_CHAT to be handled in Run()
     */
    void ToggleChatState();

    /**
     * Start listening (event-based, thread-safe)
     * Sends MAIN_EVENT_START_LISTENING to be handled in Run()
     */
    void StartListening();

    /**
     * Stop listening (event-based, thread-safe)
     * Sends MAIN_EVENT_STOP_LISTENING to be handled in Run()
     */
    void StopListening();

    void Reboot();
    void WakeWordInvoke(const std::string& wake_word);
    bool UpgradeFirmware(const std::string& url, const std::string& version = "");

    // 不重启切换平台: 关连接 → 重做 OTA → 按新配置重建 protocol (仅 RemoteCmd "reload" 触发)
    bool SwitchProtocol();
    bool CanEnterSleepMode();
    void SendMcpMessage(const std::string& payload);
    void CloseAudioChannel();
    bool SendTextToTts(const std::string& text);   // false = 协议未实现 / channel 未开
    void SendTextToAI(const std::string& text);
    bool SendProtocolText(const std::string& text);
    // 教育卡 P0：动态切换 system prompt（model_type=2 视觉 / 0 聊天 / 空 prompt 恢复默认）
    bool UpdateSystemPrompt(int model_type, const std::string& prompt);
    void ForceListeningMode(ListeningMode mode) { listening_mode_ = mode; }
    void ScheduleDelayedWake(const std::string& wake_text, uint64_t delay_us);
    void SetAecMode(AecMode mode);
    AecMode GetAecMode() const { return aec_mode_; }

    // 说话结束提示音开关（持久化到 NVS · audio.stt_popup）
    void SetSttPopupEnabled(bool enabled);
    bool IsSttPopupEnabled() const { return stt_popup_enabled_; }

    // 开机自动对话（A1 · 取代 board WelcomeTask 内 vTaskDelay+ToggleChat）
    // board 在播欢迎音前调用 → state listener 在首次进 Idle 时自动 ToggleChat
    void RequestAutoChatOnIdle();

    void PlaySound(const std::string_view& sound);
    AudioService& GetAudioService() { return audio_service_; }

    // UI 主屏时钟与对话模式切换（由 remote_cmd 调用）
    FlowEngine* GetFlowEngine() { return flow_engine_.get(); }
    RemoteCmd* GetRemoteCmd() { return remote_cmd_.get(); }

    /**
     * Reset protocol resources (thread-safe)
     * Can be called from any task to release resources allocated after network connected
     * This includes closing audio channel, resetting protocol and ota objects
     */
    void ResetProtocol();

private:
    Application();
    ~Application();

    std::mutex mutex_;
    std::deque<std::function<void()>> main_tasks_;
    std::unique_ptr<Protocol> protocol_;
    EventGroupHandle_t event_group_ = nullptr;
    esp_timer_handle_t clock_timer_handle_ = nullptr;
    DeviceStateMachine state_machine_;
    ListeningMode listening_mode_ = kListeningModeAutoStop;
    AecMode aec_mode_ = kAecOff;
    std::string last_error_message_;
    AudioService audio_service_;
    std::unique_ptr<Ota> ota_;
    std::unique_ptr<RemoteCmd> remote_cmd_;
    std::unique_ptr<FlowEngine> flow_engine_;
    esp_timer_handle_t delayed_wake_timer_ = nullptr;
    std::string pending_wake_text_;

    bool has_server_time_ = false;
    bool aborted_ = false;
    bool assets_version_checked_ = false;
    bool play_popup_on_listening_ = false;  // Flag to play popup sound after state changes to listening

    bool stt_popup_enabled_ = true;
    std::atomic<bool> auto_chat_pending_{false};  // 开机自动对话 pending（A1 · 见 RequestAutoChatOnIdle）
    std::atomic<bool> skip_next_stt_popup_{false};
    int clock_ticks_ = 0;
    int audio_send_fail_count_ = 0;
    int reconnect_attempts_in_window_ = 0;
    int64_t reconnect_window_start_us_ = 0;
    static constexpr int kMaxReconnectInWindow = 3;
    static constexpr int64_t kReconnectWindowUs = 300LL * 1000 * 1000;
    std::atomic<bool> user_initiated_close_{false};
    TaskHandle_t activation_task_handle_ = nullptr;


    // Event handlers
    void HandleStateChangedEvent();
    void HandleToggleChatEvent();
    void HandleStartListeningEvent();
    void HandleStopListeningEvent();
    void HandleNetworkConnectedEvent();
    void HandleNetworkDisconnectedEvent();
    void HandleActivationDoneEvent();
    void HandleWakeWordDetectedEvent();
    void ContinueOpenAudioChannel(ListeningMode mode);
    void ContinueWakeWordInvoke(const std::string& wake_word);

    // Activation task (runs in background)
    void ActivationTask();

    // Helper methods
    void CheckAssetsVersion();
    void CheckNewVersion();
    void InitializeProtocol();
    void ShowActivationCode(const std::string& code, const std::string& message);
    void SetListeningMode(ListeningMode mode);
    ListeningMode GetDefaultListeningMode() const;
    
    // State change handler called by state machine
    void OnStateChanged(DeviceState old_state, DeviceState new_state);
};


class TaskPriorityReset {
public:
    TaskPriorityReset(BaseType_t priority) {
        original_priority_ = uxTaskPriorityGet(NULL);
        vTaskPrioritySet(NULL, priority);
    }
    ~TaskPriorityReset() {
        vTaskPrioritySet(NULL, original_priority_);
    }

private:
    BaseType_t original_priority_;
};

#endif // _APPLICATION_H_
