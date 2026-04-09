#include "websocket_joyai_protocol.h"
#include "board.h"
#include "system_info.h"
#include "application.h"
#include "settings.h"

#include <cstring>
#include <cJSON.h>
#include <esp_log.h>
#include <web_socket.h>
#include "assets/lang_config.h"
#include <mbedtls/base64.h>
#include <cstdlib>
#include <inttypes.h>

#define TAG "JDWS"

WebsocketJoeaiProtocol::WebsocketJoeaiProtocol() {
    event_group_handle_ = xEventGroupCreate();
}

WebsocketJoeaiProtocol::~WebsocketJoeaiProtocol() {
    vEventGroupDelete(event_group_handle_);
}

bool WebsocketJoeaiProtocol::Start() {
    return true;
}

bool WebsocketJoeaiProtocol::SendAudio(std::unique_ptr<AudioStreamPacket> packet) {
    if (!IsAudioChannelOpened() || !websocket_) {
        return false;
    }
    // 上行二进制音频帧（OPUS）
    bool ok = websocket_->Send((const char*)packet->payload.data(), packet->payload.size(), true);
    if (!ok) {
        ESP_LOGE(TAG, "TX opus send failed (len=%u)", (unsigned)packet->payload.size());
    }
    return ok;
}

bool WebsocketJoeaiProtocol::SendText(const std::string& text) {
    if (!IsAudioChannelOpened() || !websocket_) {
        return false;
    }

    if (!websocket_->Send(text)) {
        ESP_LOGE(TAG, "Failed to send text: %s", text.c_str());
        SetError(Lang::Strings::SERVER_ERROR);
        return false;
    }

    return true;
}

void WebsocketJoeaiProtocol::SendStartListening(ListeningMode mode) {
    // 将上层控制映射为 Joyai 的会话事件：开始录音/模式声明
    // 参照文档，发送 CLIENT_VOICE_CHAT_UPDATE 可附带模式；保持最小实现，仅声明开始
    (void)mode;
    std::string event = BuildClientEvent("CLIENT_VOICE_CHAT_START", "{}");
    SendText(event);
}

void WebsocketJoeaiProtocol::SendStopListening() {
    std::string event = BuildClientEvent("CLIENT_VOICE_CHAT_STOP", "{}");
    SendText(event);
}

void WebsocketJoeaiProtocol::SendAbortSpeaking(AbortReason reason) {
    (void)reason;
    std::string event = BuildClientEvent("CLIENT_TTS_ABORT", "{}");
    SendText(event);
}

bool WebsocketJoeaiProtocol::IsAudioChannelOpened() const {
    return websocket_ != nullptr && websocket_->IsConnected() && !error_occurred_ && !IsTimeout();
}

void WebsocketJoeaiProtocol::CloseAudioChannel(bool send_goodbye) {
    websocket_.reset();
}

bool WebsocketJoeaiProtocol::OpenAudioChannel() {
    error_occurred_ = false;
    Settings settings("websocket", false);
    url_ = settings.GetString("url");
    token_ = settings.GetString("token");
    // 解析 URL 查询串，提取 botId/sessionId/requestId 三个字段
    {
        auto parse_query = [this](const std::string& u){
            size_t q = u.find('?');
            if (q == std::string::npos) return;
            std::string qs = u.substr(q + 1);
            size_t pos = 0;
            while (pos < qs.size()) {
                size_t amp = qs.find('&', pos);
                std::string kv = (amp == std::string::npos) ? qs.substr(pos) : qs.substr(pos, amp - pos);
                size_t eq = kv.find('=');
                if (eq != std::string::npos) {
                    std::string key = kv.substr(0, eq);
                    std::string val = kv.substr(eq + 1);
                    if (key == "botId") bot_id_ = val;
                    else if (key == "sessionId") session_id_ = val;
                    else if (key == "requestId") request_id_ = val;
                }
                if (amp == std::string::npos) break;
                pos = amp + 1;
            }
        };
        parse_query(url_);
    }

    ESP_LOGI(TAG, "URL parsed: botId=%s sessionId=%s requestId=%s",
             bot_id_.c_str(), session_id_.c_str(), request_id_.c_str());

    if (url_.empty()) {
        ESP_LOGE(TAG, "websocket settings missing: url");
        return false;
    }

    auto network = Board::GetInstance().GetNetwork();
    websocket_ = network->CreateWebSocket(1);
    if (websocket_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create websocket");
        return false;
    }

    if (!token_.empty()) {
        if (token_.find(" ") == std::string::npos) {
            token_ = "Bearer " + token_;
        }
        websocket_->SetHeader("Authorization", token_.c_str());
    }
    websocket_->SetHeader("Protocol-Version", "1");
    websocket_->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
    websocket_->SetHeader("Client-Id", Board::GetInstance().GetUuid().c_str());

    websocket_->OnData([this](const char* data, size_t len, bool binary) {
        if (binary) {
            HandleBinaryMessage(data, len);
        } else {
            HandleTextMessage(data, len);
        }
        last_incoming_time_ = std::chrono::steady_clock::now();
    });

    websocket_->OnDisconnected([this]() {
        ESP_LOGI(TAG, "Websocket disconnected");
        connected_ = false;
        if (on_audio_channel_closed_ != nullptr) {
            on_audio_channel_closed_();
        }
    });

    // 规避底层 URI 解析将查询参数中的 ':' 误识别为端口分隔导致 stoi 抛异常
    // 仅转义查询部分 ':' 为 "%3A"，不影响 scheme/host/path
    std::string sanitized_url(url_);
    size_t qpos = sanitized_url.find('?');
    if (qpos != std::string::npos) {
        for (size_t i = qpos + 1; i < sanitized_url.size(); ++i) {
            if (sanitized_url[i] == ':') {
                sanitized_url.replace(i, 1, "%3A");
                i += 2; // 跳过刚插入的两个字符
            }
        }
    }

    ESP_LOGI(TAG, "Connecting to joyai server: %s", sanitized_url.c_str());
    if (!websocket_->Connect(sanitized_url.c_str())) {
        ESP_LOGE(TAG, "Failed to connect to joyai server");
        SetError(Lang::Strings::SERVER_NOT_CONNECTED);
        return false;
    }

    connected_ = true;

    // 发送会话参数，保障与 SDK 行为一致（若失败则视为不可用）
    if (!ConfigureChatParameters()) {
        ESP_LOGE(TAG, "ConfigureChatParameters failed");
        SetError(Lang::Strings::SERVER_ERROR);
        return false;
    }

    // 等待服务端事件确认就绪（例如 SERVER_VOICE_CHAT_UPDATED/STARTED）
    EventBits_t bits = xEventGroupWaitBits(event_group_handle_, WEBSOCKET_JOEAI_SERVER_READY_EVENT,
        pdTRUE, pdFALSE, pdMS_TO_TICKS(10000));
    if (!(bits & WEBSOCKET_JOEAI_SERVER_READY_EVENT)) {
        ESP_LOGE(TAG, "Server ready wait timeout");
        SetError(Lang::Strings::SERVER_TIMEOUT);
        return false;
    }

    if (on_audio_channel_opened_ != nullptr) {
        on_audio_channel_opened_();
    }
    return true;
}

bool WebsocketJoeaiProtocol::ConfigureChatParameters() {
    if (!websocket_) return false;

    std::string mid = Board::GetInstance().GetUuid();
    std::string uid = SystemInfo::GetMacAddress();

    // mid 对应 botId，uid 对应 clientId（Device-Id 在 header），requestId 对应设备的 deviceId
    // 如果 URL 中携带了明确字段，则优先使用
    if (!bot_id_.empty()) mid = bot_id_;
    // uid 继续沿用 Board::UUID 作为 clientId
    auto msg = BuildUpdateChatConfigMessage(mid, uid, true, "opus", 16000, "opus", 24000, 60);
    ESP_LOGI(TAG, "Send chat config: mid=%s uid=%s in=opus/16000 out=opus/24000 frame=60 len=%u",
             mid.c_str(), uid.c_str(), (unsigned)msg.size());
    return SendText(msg);
}

void WebsocketJoeaiProtocol::HandleTextMessage(const char* data, size_t len) {
    // 与 websocket_protocol 对齐：仅在 JSON 合法且包含字符串 type 时转发到上层
    std::string s(data, len);
    auto root = cJSON_Parse(s.c_str());
    if (!root) {
        ESP_LOGE(TAG, "json parse failed: %s", s.c_str());
        return;
    }
    auto type = cJSON_GetObjectItem(root, "type");
    if (cJSON_IsString(type)) {
        if (on_incoming_json_ != nullptr) {
            on_incoming_json_(root);
        }
    } else {
        // Joyai 文本事件为 contentType/EVENT 或 AUDIO 结构
        auto contentType = cJSON_GetObjectItem(root, "contentType");
        auto content = cJSON_GetObjectItem(root, "content");
        if (cJSON_IsString(contentType)) {
            // 处理事件：用于获取服务端音频参数并置位 ready
            if (strcmp(contentType->valuestring, "EVENT") == 0 && cJSON_IsObject(content)) {
                auto eventType = cJSON_GetObjectItem(content, "eventType");
                auto eventData = cJSON_GetObjectItem(content, "eventData");
                if (cJSON_IsString(eventType)) {
                    const char* et = eventType->valuestring;
                    ESP_LOGI(TAG, "event: %s", et);
                    bool is_ready_event = (
                        strcmp(et, "SERVER_VOICE_CHAT_UPDATED") == 0 ||
                        strcmp(et, "SERVER_VOICE_CHAT_STARTED") == 0 ||
                        strcmp(et, "SERVER_VOICE_CHAT_READY") == 0 ||
                        strcmp(et, "SERVER_READY") == 0
                    );
                    if (is_ready_event) {
                        int sample_rate = server_sample_rate_;
                        int frame_ms = server_frame_duration_;
                        if (cJSON_IsObject(eventData)) {
                            auto audio = cJSON_GetObjectItem(eventData, "audio");
                            if (cJSON_IsObject(audio)) {
                                auto output = cJSON_GetObjectItem(audio, "output");
                                if (cJSON_IsObject(output)) {
                                    auto sr = cJSON_GetObjectItem(output, "sampleRate");
                                    auto fm = cJSON_GetObjectItem(output, "frameSizeMs");
                                    if (cJSON_IsNumber(sr)) sample_rate = sr->valueint; else if (cJSON_IsString(sr)) sample_rate = atoi(sr->valuestring);
                                    if (cJSON_IsNumber(fm)) frame_ms = fm->valueint; else if (cJSON_IsString(fm)) frame_ms = atoi(fm->valuestring);
                                }
                            }
                        }
                        if (sample_rate > 0) server_sample_rate_ = sample_rate;
                        if (frame_ms > 0) server_frame_duration_ = frame_ms;
                        ESP_LOGI(TAG, "Server ready: sr=%d, frame=%dms", server_sample_rate_, server_frame_duration_);
                        xEventGroupSetBits(event_group_handle_, WEBSOCKET_JOEAI_SERVER_READY_EVENT);
                        // 继续处理其他事件，不 return
                    }

                    // 将 Joyai 事件映射为系统通用 JSON（type: tts/stt）上抛
                    auto emit_tts = [this](const char* state, const char* text){
                        cJSON* fake = cJSON_CreateObject();
                        cJSON_AddStringToObject(fake, "type", "tts");
                        cJSON_AddStringToObject(fake, "state", state);
                        if (text != nullptr) {
                            cJSON_AddStringToObject(fake, "text", text);
                        }
                        if (on_incoming_json_ != nullptr) on_incoming_json_(fake);
                        cJSON_Delete(fake);
                    };

                    // 常见事件映射
                    if (strcmp(et, "CALL_AGENT_START_EVENT") == 0) {
                        emit_tts("start", nullptr);
                    } else if (strcmp(et, "TTS_SENTENCE_START") == 0) {
                        const char* txt = nullptr;
                        if (cJSON_IsObject(eventData)) {
                            auto t = cJSON_GetObjectItem(eventData, "text");
                            if (cJSON_IsString(t)) txt = t->valuestring;
                        }
                        emit_tts("sentence_start", txt);
                    } else if (strcmp(et, "TTS_COMPLETE") == 0 || strcmp(et, "COMPLETE") == 0
                               || strcmp(et, "CALL_AGENT_INTERRUPTED") == 0 || strcmp(et, "INTERRUPT") == 0) {
                        emit_tts("stop", nullptr);
                    } else if (strcmp(et, "EMPTY_CONTENT") == 0) {
                        // 无内容，按 stop 处理，避免卡在 speaking/listening 边界
                        emit_tts("stop", nullptr);
                    }
                }
            } else if (strcmp(contentType->valuestring, "AUDIO") == 0) {
                // 可选：文本承载的 base64 音频帧
                if (on_incoming_audio_ != nullptr && cJSON_IsObject(content)) {
                    int sr = server_sample_rate_;
                    int fm = server_frame_duration_;
                    auto srj = cJSON_GetObjectItem(content, "sampleRate");
                    auto fmj = cJSON_GetObjectItem(content, "frameSizeMs");
                    if (cJSON_IsNumber(srj)) sr = srj->valueint; else if (cJSON_IsString(srj)) sr = atoi(srj->valuestring);
                    if (cJSON_IsNumber(fmj)) fm = fmj->valueint; else if (cJSON_IsString(fmj)) fm = atoi(fmj->valuestring);
                    auto dataj = cJSON_GetObjectItem(content, "data");
                    if (cJSON_IsString(dataj)) {
                        const char* b64 = dataj->valuestring;
                        std::vector<uint8_t> payload;
                        size_t in_len = strlen(b64);
                        // 预估最大输出长度并解码
                        payload.resize((in_len * 3) / 4 + 4);
                        size_t out_len = 0;
                        int rc = mbedtls_base64_decode(payload.data(), payload.size(), &out_len, (const unsigned char*)b64, in_len);
                        if (rc == 0 && out_len > 0) {
                            payload.resize(out_len);
                            // 下发音频前，若上层未切 speaking，主动发出 tts start，避免音频被丢弃
                            cJSON* fake = cJSON_CreateObject();
                            cJSON_AddStringToObject(fake, "type", "tts");
                            cJSON_AddStringToObject(fake, "state", "start");
                            if (on_incoming_json_ != nullptr) on_incoming_json_(fake);
                            cJSON_Delete(fake);
                            on_incoming_audio_(std::make_unique<AudioStreamPacket>(AudioStreamPacket{
                                .sample_rate = sr,
                                .frame_duration = fm,
                                .timestamp = 0,
                                .payload = std::move(payload)
                            }));
                        } else {
                            ESP_LOGE(TAG, "base64 decode failed: %d", rc);
                        }
                    }
                }
            } else if (strcmp(contentType->valuestring, "TEXT") == 0) {
                // 文本识别结果，映射到系统 stt
                if (cJSON_IsObject(content)) {
                    const char* txt = nullptr;
                    auto t = cJSON_GetObjectItem(content, "text");
                    if (cJSON_IsString(t)) txt = t->valuestring;
                    if (txt != nullptr) {
                        cJSON* fake = cJSON_CreateObject();
                        cJSON_AddStringToObject(fake, "type", "stt");
                        cJSON_AddStringToObject(fake, "text", txt);
                        if (on_incoming_json_ != nullptr) on_incoming_json_(fake);
                        cJSON_Delete(fake);
                    }
                }
            }
        } else {
            ESP_LOGE(TAG, "Missing message type, data: %s", s.c_str());
        }
    }
    cJSON_Delete(root);
}

void WebsocketJoeaiProtocol::HandleBinaryMessage(const char* data, size_t len) {
    if (on_incoming_audio_ == nullptr) {
        return;
    }
    // 可按需添加更详细日志
    on_incoming_audio_(std::make_unique<AudioStreamPacket>(AudioStreamPacket{
        .sample_rate = server_sample_rate_,
        .frame_duration = server_frame_duration_,
        .timestamp = 0,
        .payload = std::vector<uint8_t>((const uint8_t*)data, (const uint8_t*)data + len)
    }));
}

void WebsocketJoeaiProtocol::HandleErrorMessage(const char* data, size_t len) {
    (void)data; (void)len;
    error_occurred_ = true;
    if (on_audio_channel_closed_ != nullptr) {
        on_audio_channel_closed_();
    }
}

std::string WebsocketJoeaiProtocol::BuildUpdateChatConfigMessage(const std::string& mid,
        const std::string& uid,
        bool binary,
        const std::string& input_codec,
        int input_sr,
        const std::string& output_codec,
        int output_sr,
        int frame_ms) {
    std::string audio = std::string("\"audio\":{") + "\"binary\":" + (binary ? "true" : "false");
    audio += ",\"input\":{\"codec\":\"" + input_codec + "\",\"sampleRate\":" + std::to_string(input_sr) + "}";
    audio += ",\"output\":{\"codec\":\"" + output_codec + "\",\"sampleRate\":" + std::to_string(output_sr) + ",\"frameSizeMs\":" + std::to_string(frame_ms) + "}";
    audio += "}";
    std::string msg = "{\"mid\":\"" + mid + "\",\"contentType\":\"EVENT\",\"uid\":\"" + uid +
        "\",\"content\":{\"eventType\":\"CLIENT_VOICE_CHAT_UPDATE\",\"eventData\":{" + audio + "}}}";
    return msg;
}

std::string WebsocketJoeaiProtocol::BuildClientEvent(const std::string& event_type, const std::string& event_data_json) {
    // 统一的事件封装，兼容 demo/文档结构
    // mid=botId，uid=clientId（Board UUID），requestId=设备 deviceId（MAC）
    std::string mid = bot_id_;
    std::string uid = Board::GetInstance().GetUuid();
    std::string requestId = SystemInfo::GetMacAddress();
    std::string msg = std::string("{") +
        "\"requestId\":\"" + requestId + "\"," +
        "\"mid\":\"" + mid + "\"," +
        "\"contentType\":\"EVENT\"," +
        "\"uid\":\"" + uid + "\"," +
        "\"content\":{\"eventType\":\"" + event_type + "\",\"eventData\":" + event_data_json + "}}";
    return msg;
}


