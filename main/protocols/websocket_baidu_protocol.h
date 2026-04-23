#ifndef _WEBSOCKET_BAIDU_PROTOCOL_H_
#define _WEBSOCKET_BAIDU_PROTOCOL_H_

#include "protocol.h"

#include <web_socket.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <string>
#include <memory>
#include <atomic>
#include <chrono>

// 百度协议事件位
#define BAIDU_PROTOCOL_CONNECTED_EVENT    (1 << 0)
#define BAIDU_PROTOCOL_LICENSED_EVENT     (1 << 1)
#define BAIDU_PROTOCOL_MEDIA_READY_EVENT  (1 << 2)

/**
 * 百度 RTC 大模型互动 WebSocket 协议实现
 *
 * 协议文档: docs/baidu.md
 *
 * 连接模型（与标准 websocket_protocol 对齐）:
 * - CloseAudioChannel 保活 WS，下次 OpenAudioChannel 快速复用
 * - 空闲时仅预取 Token（HTTP），不建 WS 连接
 * - 断连后不自动重连，等待用户唤醒时重建
 *
 * 音频格式:
 * - 发送: OPUS 16kHz 20ms 帧 (规范建议最佳帧时长)
 * - 接收: OPUS 24kHz 60ms 帧
 */
class WebsocketBaiduProtocol : public Protocol {
public:
    WebsocketBaiduProtocol();
    ~WebsocketBaiduProtocol();

    // Protocol 接口实现
    bool Start() override;
    bool SendAudio(std::unique_ptr<AudioStreamPacket> packet) override;

    // 百度协议使用 20ms 帧时长 (规范建议最佳20ms，可提高 ASR 响应速度)
    int client_frame_duration() const override { return 20; }
    bool OpenAudioChannel() override;
    void CloseAudioChannel(bool send_goodbye = true) override;
    bool IsAudioChannelOpened() const override;

    // 百度协议适配方法
    void SendWakeWordDetected(const std::string& wake_word) override;
    void SendStartListening(ListeningMode mode) override;
    void SendStopListening() override;
    void SendAbortSpeaking(AbortReason reason) override;
    void SendMcpMessage(const std::string& payload) override;
    bool SendTextToTts(const std::string& text) override;
    bool SendTextToAI(const std::string& text) override;

    // 百度特有方法
    void SendInterrupt();
    void SetAutoInterrupt(bool enable);
    void PauseAsr();
    void ResumeAsr();
    void SendFunctionCallResult(const std::string& session_id,
                                const std::string& result,
                                const std::string& message);

    // 连接管理
    void DisconnectWebSocket();  // 断开WebSocket（休眠/断网时调用）

private:
    EventGroupHandle_t event_group_handle_;
    std::unique_ptr<WebSocket> websocket_;

    // 配置
    std::string url_;
    std::string device_id_;
    std::string user_id_;
    std::string license_key_;

    // 状态（跨线程访问的标志使用 atomic）
    std::atomic<bool> licensed_{false};
    std::atomic<bool> media_ready_{false};
    std::atomic<bool> is_speaking_{false};
    std::atomic<bool> is_playing_music_{false};
    bool is_realtime_mode_ = true;
    bool auto_interrupt_ = false;
    bool greeting_sent_ = false;
    bool lic_active_pending_ = false;
    bool has_local_aec_ = false;
    ListeningMode current_mode_ = kListeningModeRealtime;

    // ========== 发送失败追踪（音频/控制分离，避免音频丢帧污染控制面）==========
    std::atomic<int> audio_send_failures_{0};
    std::chrono::steady_clock::time_point first_audio_failure_time_;
    std::atomic<int> control_send_failures_{0};
    std::chrono::steady_clock::time_point first_control_failure_time_;

    // 统计
    uint32_t cmd_seq_ = 0;
    uint32_t audio_tx_frames_ = 0;
    uint32_t audio_tx_bytes_ = 0;
    uint32_t audio_rx_frames_ = 0;
    uint32_t audio_rx_bytes_ = 0;

    // ASR 去重
    std::string last_final_asr_text_;
    std::chrono::steady_clock::time_point last_final_asr_time_;

    // 音频发送节流（20ms pacing）
    std::chrono::steady_clock::time_point last_audio_send_time_;

    // NVS 可配置参数（构造时读取）
    int break_delay_ms_ = 500;
    int idle_timeout_seconds_ = 300;

    // 无对话超时机制
    std::chrono::steady_clock::time_point last_activity_time_;
    TimerHandle_t idle_timer_handle_ = nullptr;
    bool idle_goodbye_sent_ = false;

    // Listening 超时
    TimerHandle_t listening_timer_handle_ = nullptr;
    static constexpr int kListeningTimeoutMs = 30000;

    // License 激活防抖
    std::chrono::steady_clock::time_point last_lic_active_time_;
    TimerHandle_t lic_retry_timer_handle_ = nullptr;

    // 会话 Token 缓存（连接方式一）
    bool session_token_fetched_ = false;
    std::string cached_instance_id_;
    std::string cached_token_;

    // 连接状态（网络线程 OnDisconnected 回调也会修改，需 atomic）
    std::atomic<bool> device_info_sent_{false};

    // 析构保护：防止 timer/Schedule lambda 访问悬空 this
    std::shared_ptr<std::atomic<bool>> prevent_destroy_guard_ = std::make_shared<std::atomic<bool>>(true);

    // 消息处理
    void HandleTextMessage(const char* data, size_t len);
    void HandleBinaryMessage(const char* data, size_t len);
    void HandleAsrResult(const std::string& text, bool final);
    void HandleLLMResult(const std::string& text, bool final);
    void HandleEvent(const std::string& event);
    void HandleFunctionCall(const std::string& data);
    void HandleLicenseEvent(const std::string& event);
    void HandleCustomData(const std::string& data);

    // 内部方法
    bool SendText(const std::string& text) override;
    bool SendLicenseActivation();
    void TryFlushPendingLicActive();
    void ScheduleLicRetry();
    void SendInitialDeviceInfo();
    cJSON* CreateJsonMessage(const char* type, const char* text = nullptr);
    void NotifyTtsState(const char* state, const char* emotion);

    // 无对话超时管理
    void UpdateActivityTime();
    void StartIdleTimer();
    void StopIdleTimer();
    void CheckIdleTimeout();
    static void IdleTimerCallback(TimerHandle_t timer);

    // Listening 超时管理
    void StartListeningTimer();
    void StopListeningTimer();

    // 连接方式一：动态获取会话 Token
    bool FetchSessionToken(std::string& instance_id, std::string& token);
    void PrefetchSessionTokenAsync();
};

#endif // _WEBSOCKET_BAIDU_PROTOCOL_H_
