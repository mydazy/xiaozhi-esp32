#ifndef _WEBSOCKET_JOEAI_PROTOCOL_H_
#define _WEBSOCKET_JOEAI_PROTOCOL_H_

#include "protocol.h"

#include <cJSON.h>
#include <web_socket.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>

#include <atomic>
#include <memory>
#include <string>

#define WEBSOCKET_JOEAI_SERVER_READY_EVENT (1 << 0)

class WebsocketJoeaiProtocol : public Protocol {
public:
    WebsocketJoeaiProtocol();
    ~WebsocketJoeaiProtocol();

    bool Start() override;
    bool SendAudio(std::unique_ptr<AudioStreamPacket> packet) override;
    bool OpenAudioChannel() override;
    void CloseAudioChannel(bool send_goodbye = true) override;
    bool IsAudioChannelOpened() const override;

    void SendStartListening(ListeningMode mode) override;
    void SendStopListening() override;
    void SendAbortSpeaking(AbortReason reason) override;

private:
    EventGroupHandle_t event_group_handle_;
    std::unique_ptr<WebSocket> websocket_;

    // 基本配置
    int version_ = 1;
    std::string url_;
    std::string token_;
    std::string bot_id_;
    std::string session_id_;
    std::string request_id_;

    // 连接状态（错误标记使用基类的 error_occurred_）
    std::atomic<bool> connected_ { false };
    std::atomic<bool> tts_started_ { false };
    std::atomic<bool> interrupt_pending_ { false };

    // 内部辅助
    bool SendText(const std::string& text) override;
    void HandleTextMessage(const char* data, size_t len);
    void HandleBinaryMessage(const char* data, size_t len);
    void HandleErrorMessage(const char* data, size_t len);
    void HandleEventMessage(const cJSON* content);
    void EmitTtsStartIfNeeded();
    void EmitTtsStop();
    bool ConfigureChatParameters();
    std::string BuildClientEvent(const std::string& event_type, const std::string& event_data_json);

    // 构建与 joyai 协议一致的配置更新消息
    std::string BuildUpdateChatConfigMessage(const std::string& mid,
            const std::string& uid,
            bool binary,
            const std::string& input_codec,
            int input_sr,
            const std::string& output_codec,
            int output_sr,
            int frame_ms);

    
};

#endif




