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

// JoyInside 协议无对应"开始录音"事件：连接建立后客户端持续发 AUDIO 帧即可，
// 服务端依据自由对话模式（默认 needManualCall=false）VAD 自动判断对话边界
void WebsocketJoeaiProtocol::SendStartListening(ListeningMode mode) {
    (void)mode;
}

// 仅手动模式 (URL needManualCall=true) 下有意义；自由对话模式服务端自动检测停顿
void WebsocketJoeaiProtocol::SendStopListening() {
    SendText(BuildClientEvent("CLIENT_AUDIO_FINISH", "{}"));
}

// 打断：上行 CLIENT_INTERRUPT (旧实现误用 CLIENT_TTS_ABORT，服务端不识别→打断失效)
// 三件事：(1) 立即抛 tts.stop 让 application 切走 speaking
//         (2) 置 interrupt_pending_=true，期间所有下行 TTS 帧丢弃，防残留音频复活 speaking
//         (3) 上行 CLIENT_INTERRUPT 通知服务端停 TTS
void WebsocketJoeaiProtocol::SendAbortSpeaking(AbortReason reason) {
    (void)reason;
    interrupt_pending_ = true;
    EmitTtsStop();
    SendText(BuildClientEvent("CLIENT_INTERRUPT", "{}"));
}

bool WebsocketJoeaiProtocol::IsAudioChannelOpened() const {
    return websocket_ != nullptr && websocket_->IsConnected() && !error_occurred_ && !IsTimeout();
}

void WebsocketJoeaiProtocol::CloseAudioChannel(bool send_goodbye) {
    (void)send_goodbye;
    tts_started_ = false;
    interrupt_pending_ = false;
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

// 首次音频帧到达时（无论二进制还是 base64 文本）才向上抛 tts.start，
// 防止 LLM 思考期/已打断后残留事件错误激活 speaking 状态
void WebsocketJoeaiProtocol::EmitTtsStartIfNeeded() {
    if (tts_started_) return;
    tts_started_ = true;
    cJSON* fake = cJSON_CreateObject();
    cJSON_AddStringToObject(fake, "type", "tts");
    cJSON_AddStringToObject(fake, "state", "start");
    if (on_incoming_json_ != nullptr) on_incoming_json_(fake);
    cJSON_Delete(fake);
}

void WebsocketJoeaiProtocol::EmitTtsStop() {
    if (!tts_started_) return;
    tts_started_ = false;
    cJSON* fake = cJSON_CreateObject();
    cJSON_AddStringToObject(fake, "type", "tts");
    cJSON_AddStringToObject(fake, "state", "stop");
    if (on_incoming_json_ != nullptr) on_incoming_json_(fake);
    cJSON_Delete(fake);
}

void WebsocketJoeaiProtocol::HandleEventMessage(const cJSON* content) {
    auto eventType = cJSON_GetObjectItem(content, "eventType");
    auto eventData = cJSON_GetObjectItem(content, "eventData");
    if (!cJSON_IsString(eventType)) return;
    const char* et = eventType->valuestring;
    ESP_LOGI(TAG, "event: %s", et);

    // 任何 CALL_AGENT_START_EVENT (新一轮对话开始) / CFG_BOT_EVENT (新连接) 都意味着
    // 之前的打断窗口已结束，恢复正常下行处理
    if (strcmp(et, "CALL_AGENT_START_EVENT") == 0 ||
        strcmp(et, "CFG_BOT_EVENT") == 0) {
        interrupt_pending_ = false;
    }

    // 服务端就绪：CFG_BOT_EVENT (连接默认配置) + SERVER_VOICE_CHAT_UPDATED (配置更新成功响应)
    if (strcmp(et, "CFG_BOT_EVENT") == 0 ||
        strcmp(et, "SERVER_VOICE_CHAT_UPDATED") == 0) {
        // CFG_BOT_EVENT 的 eventData.tts: { sr, aue, bit, channels }
        // SERVER_VOICE_CHAT_UPDATED 的 eventData.audio.output: { sampleRate, frameSizeMs }
        if (cJSON_IsObject(eventData)) {
            int sr = 0, fm = 0;
            auto audio = cJSON_GetObjectItem(eventData, "audio");
            if (cJSON_IsObject(audio)) {
                auto output = cJSON_GetObjectItem(audio, "output");
                if (cJSON_IsObject(output)) {
                    auto srj = cJSON_GetObjectItem(output, "sampleRate");
                    auto fmj = cJSON_GetObjectItem(output, "frameSizeMs");
                    if (cJSON_IsNumber(srj)) sr = srj->valueint;
                    else if (cJSON_IsString(srj)) sr = atoi(srj->valuestring);
                    if (cJSON_IsNumber(fmj)) fm = fmj->valueint;
                    else if (cJSON_IsString(fmj)) fm = atoi(fmj->valuestring);
                }
            }
            auto tts = cJSON_GetObjectItem(eventData, "tts");
            if (cJSON_IsObject(tts) && sr == 0) {
                auto srj = cJSON_GetObjectItem(tts, "sr");
                if (cJSON_IsNumber(srj)) sr = srj->valueint;
                else if (cJSON_IsString(srj)) sr = atoi(srj->valuestring);
            }
            if (sr > 0) server_sample_rate_ = sr;
            if (fm > 0) server_frame_duration_ = fm;
        }
        ESP_LOGI(TAG, "Server ready: sr=%d, frame=%dms", server_sample_rate_, server_frame_duration_);
        xEventGroupSetBits(event_group_handle_, WEBSOCKET_JOEAI_SERVER_READY_EVENT);
        return;
    }

    // 字幕事件：text 字段在 eventData.text
    if (strcmp(et, "TTS_SENTENCE_START") == 0) {
        const char* txt = nullptr;
        if (cJSON_IsObject(eventData)) {
            auto t = cJSON_GetObjectItem(eventData, "text");
            if (cJSON_IsString(t)) txt = t->valuestring;
        }
        cJSON* fake = cJSON_CreateObject();
        cJSON_AddStringToObject(fake, "type", "tts");
        cJSON_AddStringToObject(fake, "state", "sentence_start");
        if (txt != nullptr) cJSON_AddStringToObject(fake, "text", txt);
        if (on_incoming_json_ != nullptr) on_incoming_json_(fake);
        cJSON_Delete(fake);
        return;
    }

    // 停止/结束/打断/空内容 → 统一抛 tts.stop 让上层切走 speaking
    // 服务端确认打断或本轮自然结束 → 清打断窗口，下一轮恢复正常下行
    if (strcmp(et, "TTS_COMPLETE") == 0 ||
        strcmp(et, "COMPLETE") == 0 ||
        strcmp(et, "CALL_AGENT_INTERRUPTED") == 0 ||
        strcmp(et, "EMPTY_CONTENT") == 0) {
        interrupt_pending_ = false;
        EmitTtsStop();
        return;
    }
}

void WebsocketJoeaiProtocol::HandleTextMessage(const char* data, size_t len) {
    std::string s(data, len);
    auto root = cJSON_Parse(s.c_str());
    if (!root) {
        ESP_LOGE(TAG, "json parse failed: %s", s.c_str());
        return;
    }

    // 兼容上层直接构造的通用 JSON（带顶层 type 字段）
    auto type = cJSON_GetObjectItem(root, "type");
    if (cJSON_IsString(type)) {
        if (on_incoming_json_ != nullptr) on_incoming_json_(root);
        cJSON_Delete(root);
        return;
    }

    // JoyInside 下行 contentType 清单：EVENT / ASR / AGENT / ACTIVITY / TTS / PONG
    auto contentType = cJSON_GetObjectItem(root, "contentType");
    auto content = cJSON_GetObjectItem(root, "content");
    if (!cJSON_IsString(contentType)) {
        ESP_LOGE(TAG, "Missing contentType, data: %s", s.c_str());
        cJSON_Delete(root);
        return;
    }
    const char* ct = contentType->valuestring;

    if (strcmp(ct, "EVENT") == 0 && cJSON_IsObject(content)) {
        HandleEventMessage(content);
    } else if (strcmp(ct, "ASR") == 0 && cJSON_IsObject(content)) {
        // 用户语音识别 → 新一轮对话开始，清打断窗口
        interrupt_pending_ = false;
        // textType: START (中间) / IS_FINAL (最终)，仅最终结果 emit stt
        // 对齐 baidu / 小智协议——让 application.cc 在用户说完一句时仅响一次 OGG_POPUP（"已收到"提示音）
        auto textTypeJ = cJSON_GetObjectItem(content, "textType");
        bool is_final = (cJSON_IsString(textTypeJ) &&
                         strcmp(textTypeJ->valuestring, "IS_FINAL") == 0);
        auto t = cJSON_GetObjectItem(content, "text");
        if (is_final && cJSON_IsString(t) && t->valuestring[0] != '\0') {
            cJSON* fake = cJSON_CreateObject();
            cJSON_AddStringToObject(fake, "type", "stt");
            cJSON_AddStringToObject(fake, "text", t->valuestring);
            if (on_incoming_json_ != nullptr) on_incoming_json_(fake);
            cJSON_Delete(fake);
        }
    } else if (strcmp(ct, "TTS") == 0 && cJSON_IsObject(content)) {
        // base64 文本承载的下行音频（audio.binary=false 时；当前配置走二进制）
        // 打断窗口期内：丢弃残留音频，不复活 speaking 状态
        if (interrupt_pending_) {
            cJSON_Delete(root);
            return;
        }
        if (on_incoming_audio_ != nullptr) {
            auto dataj = cJSON_GetObjectItem(content, "audioBase64");
            if (cJSON_IsString(dataj)) {
                const char* b64 = dataj->valuestring;
                size_t in_len = strlen(b64);
                std::vector<uint8_t> payload((in_len * 3) / 4 + 4);
                size_t out_len = 0;
                int rc = mbedtls_base64_decode(payload.data(), payload.size(), &out_len,
                                               (const unsigned char*)b64, in_len);
                if (rc == 0 && out_len > 0) {
                    payload.resize(out_len);
                    EmitTtsStartIfNeeded();
                    on_incoming_audio_(std::make_unique<AudioStreamPacket>(AudioStreamPacket{
                        .sample_rate = server_sample_rate_,
                        .frame_duration = server_frame_duration_,
                        .timestamp = 0,
                        .payload = std::move(payload)
                    }));
                } else {
                    ESP_LOGE(TAG, "base64 decode failed: %d", rc);
                }
            }
        }
    } else if (strcmp(ct, "PONG") == 0) {
        // 心跳响应，静默
    } else if (strcmp(ct, "AGENT") == 0 || strcmp(ct, "ACTIVITY") == 0) {
        // 增量文本回复 / 主动文本回复：依赖 TTS_SENTENCE_START 显示字幕，此处不处理
    } else {
        ESP_LOGD(TAG, "ignored contentType=%s", ct);
    }
    cJSON_Delete(root);
}

void WebsocketJoeaiProtocol::HandleBinaryMessage(const char* data, size_t len) {
    if (on_incoming_audio_ == nullptr) return;
    // 打断窗口期内：丢弃残留下行音频，避免复活 speaking 状态
    if (interrupt_pending_) return;
    EmitTtsStartIfNeeded();
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


