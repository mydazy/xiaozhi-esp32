#include "websocket_baidu_protocol.h"
#include "board.h"
#include "display/display.h"
#include "audio/music_player.h"
#include "system_info.h"
#include "application.h"
#include "settings.h"

#include <cstring>
#include <inttypes.h>
#include <unordered_map>
#include <cJSON.h>
#include <esp_log.h>
#include "assets/lang_config.h"

#define TAG "BD"

// ============================================================================
// 百度协议常量定义 (参考 BRTC SDK: baidu_chat_agents_engine.h)
// ============================================================================

// ASR 结果前缀
#define ASR_Q_FIN_PREFIX        "[Q]:"          // ASR最终结果
#define ASR_Q_MID_PREFIX        "[Q]:[M]:"      // ASR中间结果
#define ASR_Q_FIN_APPEND        "[Q]:[C]:"      // ASR追加结果(final)
#define ASR_Q_MID_APPEND        "[Q]:[M]:[C]:"  // ASR追加结果(mid)

// LLM 回复前缀
#define AGENT_ANSWER_PREFIX     "[A]:"          // LLM最终回复
#define AGENT_ANSWER_MID_PREFIX "[A]:[M]:"      // LLM中间回复
#define AGENT_ANSWER_HINT       "[A]:[H]:"      // LLM引导语

// 指令前缀
#define AGENT_BREAK_STR         "[B]:"          // 立即打断播报
#define AGENT_BREAK_BEGIN       "[B]:[BEGIN]:"  // 打断并延迟丢弃音频，如 [B]:[BEGIN]:3000 表示3秒内丢弃
#define AGENT_BREAK_END         "[B]:[END]"     // 提前结束打断，不等超时
#define INPUT_TEXT_TO_AI        "[T]:"          // 发送文本给AI
#define INPUT_TEXT_TO_TTS       "[TTS]:"        // 直接TTS播报
#define AGENT_EVENT_PREFIX      "[E]:"          // 事件消息
#define FUNCTION_CALL_PREFIX    "[F]:"          // Function Call

// TTS 事件
#define EVENT_TTS_BEGIN         "[E]:[TTS_BEGIN_SPEAKING]"
#define EVENT_TTS_END           "[E]:[TTS_END_SPEAKING]"
#define EVENT_VOICE_COMING      "[E]:[VOICE_COMING]"
#define EVENT_VOICE_DISAPPEAR   "[E]:[VOICE_DISAPPEAR]"
#define EVENT_MEDIA_READY       "[E]:[MEDIA]:[READY]:"
#define EVENT_AGENT_ID          "[E]:[AGENTID]:"

// 云音乐事件和控制
#define EVENT_MUSIC_BEGIN       "[E]:[REMOTE_PLAYER_BEGIN]"
#define EVENT_MUSIC_END         "[E]:[REMOTE_PLAYER_END]"
#define CMD_MUSIC_STOP          "[E]:[CMD]:[REMOTE_PLAYER]:[STOP]"
#define CMD_MUSIC_PAUSE         "[E]:[CMD]:[REMOTE_PLAYER]:[PAUSE]"
#define CMD_MUSIC_RESUME        "[E]:[CMD]:[REMOTE_PLAYER]:[RESUME]"

// ASR 模式控制
#define CMD_ASR_REALTIME        "[E]:[CMD]:[ASR_ENABLE_REALTIME]"
#define CMD_ASR_MANUAL          "[E]:[CMD]:[ASR_DISABLE_REALTIME]"
#define CMD_ASR_START           "[E]:[CMD]:[ASR_START_LONGTEXT_REC]"
#define CMD_ASR_STOP            "[E]:[CMD]:[ASR_STOP_LONGTEXT_REC]"

// System prompt 动态切换（教育卡 P0）
#define CMD_UPDATE_PROMPT       "[SET]:[UPDATE_SYSTEM_PROMPT]:"
#define EVENT_PROMPT_UPDATED    "[E]:[SYSTEM_PROMPT_UPDATED]:"

// 设备配置
#define CMD_DEVICE_INFO         "[SET]:[DEVICE_INFO]:"
#define CMD_AUTO_INT_ON         "[SET]:[AUTO_INT]:[TRUE]"
#define CMD_AUTO_INT_OFF        "[SET]:[AUTO_INT]:[FALSE]"

// License 事件
#define EVENT_LIC_PREFIX        "[E]:[LIC]:"
#define EVENT_LIC_MUST          "[E]:[LIC]:[MUST]:"
#define EVENT_LIC_ACTIVE        "[E]:[LIC]:[ACTIVE]:"
#define EVENT_LIC_PASS          "[E]:[LIC]:[RES]:[PASS]:"
#define EVENT_LIC_FAILED        "[E]:[LIC]:[RES]:[FAILED]:"

// 服务器状态信息前缀
#define SERVER_STATUS_PREFIX    "[S]:"          // 服务器状态/版本信息

// 自定义数据前缀（服务器推送自定义控制指令）
#define CUSTOM_DATA_PREFIX      "[C]:"          // 自定义数据

// 常用别名
#define PREFIX_BREAK            AGENT_BREAK_STR

// 情绪映射（中文/英文 -> 标准情绪键）
static const char* TranslateEmotion(const std::string& raw) {
    if (raw.empty()) return "neutral";
    static const std::unordered_map<std::string_view, const char*> kMap = {
        {"高兴","happy"},{"开心","happy"},{"快乐","happy"},{"兴奋","happy"},
        {"伤心","sad"},{"难过","sad"},{"悲伤","sad"},{"沮丧","sad"},
        {"生气","angry"},{"愤怒","angry"},{"惊讶","surprised"},{"吃惊","surprised"},
        {"思考","thinking"},{"害羞","embarrassed"},{"尴尬","embarrassed"},
        {"困","sleepy"},{"困倦","sleepy"},{"疲惫","sleepy"},
        {"无语","confused"},{"困惑","confused"},{"平静","neutral"},
        {"happy","happy"},{"sad","sad"},{"angry","angry"},{"surprised","surprised"},
        {"thinking","thinking"},{"embarrassed","embarrassed"},{"sleepy","sleepy"},
        {"confused","confused"},{"neutral","neutral"},{"cool","cool"},
    };
    auto it = kMap.find(raw);
    return it != kMap.end() ? it->second : "neutral";
}

WebsocketBaiduProtocol::WebsocketBaiduProtocol() {
    event_group_handle_ = xEventGroupCreate();

    // 集中读取 NVS 配置（避免运行时反复读 NVS）
    Settings baidu_settings("baidu", false);
    server_sample_rate_ = 24000;     // 下行 PCM 采样率 = codec output（避免 audio_service 二次重采样）
    server_frame_duration_ = 20;     // 百度固定 20ms 帧
    break_delay_ms_ = baidu_settings.GetInt("break_delay_ms", 500);
    idle_timeout_seconds_ = baidu_settings.GetInt("idle_timeout", 300);
    license_key_ = baidu_settings.GetString("license_key", "759877c9b68b4aa082cc05390be0cea9");

    Settings aec_settings("aecMode", false);
    has_local_aec_ = (aec_settings.GetInt("aec", 0) == 1);
    aec_cfg_ = has_local_aec_ ? &kBaiduAecOn : &kBaiduAecOff;
    ESP_LOGI(TAG, "===== %s =====", aec_cfg_->mode_name);
    ESP_LOGI(TAG, "  dfda=%d  cloud_auto_int=%d  tts_end_delay=%dms",
             aec_cfg_->full_duplex, aec_cfg_->cloud_auto_int, aec_cfg_->tts_end_delay_ms);
    ESP_LOGI(TAG, "  send_audio_while_tts=%d  voice_coming_handled=%d",
             aec_cfg_->send_audio_while_tts, aec_cfg_->handle_voice_coming);

    // 初始 ASR 模式：默认实时模式（兼容既有行为）
    // 实际模式会在 SendStartListening() 中根据 ListeningMode 动态切换
    is_realtime_mode_ = true;
    auto_interrupt_ = true;

    // 创建无对话超时定时器（每30秒检查一次）
    idle_timer_handle_ = xTimerCreate(
        "idle_timer",
        pdMS_TO_TICKS(30000),  // 30秒检查一次
        pdTRUE,                // 自动重载
        this,                  // 定时器ID（传递this指针）
        IdleTimerCallback      // 回调函数
    );

    // 创建 LIC_ACTIVE 延迟重试定时器（一次性，2s 后在主线程重试）
    // 目的：TTS_END 回调在网络线程中，直接发送 LIC_ACTIVE 会阻塞 UART 5s 导致断连
    lic_retry_timer_handle_ = xTimerCreate(
        "lic_retry",
        pdMS_TO_TICKS(5000),  // 5s 延迟，给 4G modem UART 充足的空闲时间
        pdFALSE,              // 一次性
        this,
        [](TimerHandle_t timer) {
            auto* self = static_cast<WebsocketBaiduProtocol*>(pvTimerGetTimerID(timer));
            if (self) {
                auto guard = self->prevent_destroy_guard_;
                Application::GetInstance().Schedule([self, guard]() {
                    if (!guard->load()) return;
                    self->TryFlushPendingLicActive();
                });
            }
        });

    // 创建 listening 超时定时器（一次性，30s 无 ASR 结果自动关闭会话）
    // 弱网下服务器可能因 LIC_FAILED 停止处理，设备永久卡在 listening 发音频
    listening_timer_handle_ = xTimerCreate(
        "bd_listen_to",
        pdMS_TO_TICKS(kListeningTimeoutMs),
        pdFALSE,  // 一次性
        this,
        [](TimerHandle_t timer) {
            auto* self = static_cast<WebsocketBaiduProtocol*>(pvTimerGetTimerID(timer));
            if (self) {
                auto guard = self->prevent_destroy_guard_;
                Application::GetInstance().Schedule([self, guard]() {
                    if (!guard->load()) return;
                    ESP_LOGW(TAG, "Listening timeout (%ds no ASR result), closing session",
                             kListeningTimeoutMs / 1000);
                    self->StopListeningTimer();
                    self->CloseAudioChannel();
                });
            }
        });

}

WebsocketBaiduProtocol::~WebsocketBaiduProtocol() {
    // 标记对象即将销毁，所有已入队的 Schedule lambda 检查此标志后跳过执行
    // 防止：timer 触发 → Schedule 入队 → 析构完成 → lambda 访问悬空 this
    prevent_destroy_guard_->store(false);

    // 停止并删除定时器
    StopIdleTimer();
    if (idle_timer_handle_) {
        xTimerDelete(idle_timer_handle_, pdMS_TO_TICKS(1000));
        idle_timer_handle_ = nullptr;
    }

    if (lic_retry_timer_handle_) {
        xTimerStop(lic_retry_timer_handle_, pdMS_TO_TICKS(100));
        xTimerDelete(lic_retry_timer_handle_, pdMS_TO_TICKS(1000));
        lic_retry_timer_handle_ = nullptr;
    }

    if (listening_timer_handle_) {
        xTimerStop(listening_timer_handle_, pdMS_TO_TICKS(100));
        xTimerDelete(listening_timer_handle_, pdMS_TO_TICKS(1000));
        listening_timer_handle_ = nullptr;
    }

    if (websocket_ && websocket_->IsConnected()) {
        websocket_->Close();
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    websocket_.reset();
    vEventGroupDelete(event_group_handle_);
}

bool WebsocketBaiduProtocol::Start() {
    PrefetchSessionTokenAsync();

    return true;
}

bool WebsocketBaiduProtocol::SendAudio(std::unique_ptr<AudioStreamPacket> packet) {
    if (!websocket_ || !websocket_->IsConnected() || !media_ready_) {
        return false;
    }

    // TTS 期间音频发送策略 (来自 AEC 模式联动 § aec_cfg_)：
    if (is_speaking_ && !aec_cfg_->send_audio_while_tts) {
        return false;
    }

    // 帧节流：项目固定 60 ms 帧（OPUS_FRAME_DURATION_MS），audio_service 上行已按 60 ms 节奏出帧，无需协议层 pacing
    // 历史 18 ms 阈值（"20 ms - 2 ms 容差"）在 60 ms 帧下永不触发 → 移除避免误读
    UpdateActivityTime();

    // 电路断路器：连续失败 >= 3 次跳过发送，避免 Ml307Tcp 5s/次超时堆积
    if (audio_send_failures_ >= 3) {
        audio_send_failures_++;
        if (audio_send_failures_ % 50 == 0) {
            ESP_LOGW(TAG, "Audio circuit open (failures=%d)", audio_send_failures_.load());
        }
        // 累计 10 次且持续 5s → 关闭连接
        if (audio_send_failures_ >= 10) {
            auto elapsed = std::chrono::steady_clock::now() - first_audio_failure_time_;
            if (elapsed >= std::chrono::seconds(5) && websocket_ && websocket_->IsConnected()) {
                ESP_LOGW(TAG, "Audio send failed %d times, closing", audio_send_failures_.load());
                websocket_->Close();
            }
        }
        return false;
    }

    // 时间感知快速重试：WiFi 瞬时失败（< 50ms）重试一次，4G 慢超时跳过
    auto send_start = std::chrono::steady_clock::now();
    bool ok = websocket_->Send(packet->payload.data(), packet->payload.size(), true);
    if (!ok) {
        auto send_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - send_start).count();
        if (send_ms < 50) {
            ok = websocket_->Send(packet->payload.data(), packet->payload.size(), true);
        }
    }

    if (ok) {
        audio_tx_frames_++;
        audio_tx_bytes_ += packet->payload.size();
        if (audio_send_failures_ > 0) {
            ESP_LOGI(TAG, "Audio recovered after %d failures", audio_send_failures_.load());
            audio_send_failures_ = 0;
        }
        return true;
    }

    if (audio_send_failures_++ == 0) {
        first_audio_failure_time_ = std::chrono::steady_clock::now();
    }
    return false;
}

bool WebsocketBaiduProtocol::SendText(const std::string& text) {
    if (!websocket_ || !websocket_->IsConnected()) {
        return false;
    }

    cmd_seq_++;
    ESP_LOGI(TAG, "[%" PRIu32 "] >> %s", cmd_seq_, text.c_str());

    // 时间感知快速重试：WiFi 瞬时失败（< 200ms）重试一次，4G 慢超时跳过
    auto send_start = std::chrono::steady_clock::now();
    bool ok = websocket_->Send(text);
    if (!ok) {
        auto send_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - send_start).count();
        if (send_ms < 200) {
            ok = websocket_->Send(text);
        }
    }

    if (ok) {
        if (control_send_failures_ > 0) {
            ESP_LOGI(TAG, "Control recovered after %d failures", control_send_failures_.load());
            control_send_failures_ = 0;
        }
        return true;
    }

    if (control_send_failures_++ == 0) {
        first_control_failure_time_ = std::chrono::steady_clock::now();
    }
    ESP_LOGW(TAG, "Control send failed (%d): %.40s", control_send_failures_.load(), text.c_str());
    return false;
}

bool WebsocketBaiduProtocol::IsAudioChannelOpened() const {
    return websocket_ && websocket_->IsConnected() &&
           !error_occurred_ && media_ready_;
}

void WebsocketBaiduProtocol::CloseAudioChannel(bool send_goodbye) {
    // 百度协议 WS 保活复用 · 不发 goodbye · 仅消费参数避免 unused warning
    (void)send_goodbye;
    bool was_ready = media_ready_.exchange(false, std::memory_order_acq_rel);
    ESP_LOGI(TAG, "[CLOSE] was_ready=%d ws=%d", was_ready,
             websocket_ ? websocket_->IsConnected() : -1);

    StopIdleTimer();
    StopListeningTimer();
    if (lic_retry_timer_handle_) xTimerStop(lic_retry_timer_handle_, 0);

    // 重置 DeviceInfo 标志，下次 OpenAudioChannel 复用连接时重新发送
    // （AEC 切换后参数变化，需要通知服务端）
    device_info_sent_ = false;

    // 无条件清空解码队列：即使 TTS_END 已将 is_speaking_ 置 false，队列中仍可能有残留帧
    Application::GetInstance().GetAudioService().ResetDecoder();

    // 通知服务器停止当前会话（WS 保活复用），失败即止
    if (was_ready && websocket_ && websocket_->IsConnected()) {
        bool ok = true;
        if (ok && !is_realtime_mode_) ok = SendText(CMD_ASR_STOP);
        if (ok) ok = SendText(PREFIX_BREAK);
        if (ok && is_playing_music_) SendText(CMD_MUSIC_STOP);
    }
    is_playing_music_ = false;
    is_speaking_ = false;

    ESP_LOGI(TAG, "[PAUSE] TX:%" PRIu32 " RX:%" PRIu32 " frames", audio_tx_frames_, audio_rx_frames_);
    audio_tx_frames_ = audio_tx_bytes_ = audio_rx_frames_ = audio_rx_bytes_ = 0;
    greeting_sent_ = false;

    // 清除会话 Token 缓存，下次唤醒时获取新的会话实例
    session_token_fetched_ = false;
    cached_instance_id_.clear();
    cached_token_.clear();

    // 始终通知应用层，确保状态机能回到 Idle
    if (on_audio_channel_closed_) on_audio_channel_closed_();
}

void WebsocketBaiduProtocol::DisconnectWebSocket() {
    ESP_LOGI(TAG, "[DISCONNECT]");
    media_ready_ = false;
    licensed_ = false;
    device_info_sent_ = false;
    lic_active_pending_ = false;
    if (lic_retry_timer_handle_) xTimerStop(lic_retry_timer_handle_, 0);

    if (websocket_) websocket_->Close();
    websocket_.reset();
    audio_tx_frames_ = audio_tx_bytes_ = audio_rx_frames_ = audio_rx_bytes_ = 0;

    session_token_fetched_ = false;
    cached_instance_id_.clear();
    cached_token_.clear();
    PrefetchSessionTokenAsync();
}

// 获取百度会话 Token（连接方式一需要每次获取新的会话ID和Token）
// 服务器接口：GET /ota/baiduVoiceChat
// 返回格式：{"ai_agent_instance_id": 222, "instance_type": "VoiceChat", "context": {"cid": 1, "token": "xxx"}}
bool WebsocketBaiduProtocol::FetchSessionToken(std::string& instance_id, std::string& token) {
    Settings settings("wifi", false);
    std::string ota_url = settings.GetString("ota_url");
    if (ota_url.empty()) {
        ota_url = CONFIG_OTA_URL;
    }

    // 构建接口 URL：移除末尾斜杠后添加 baiduVoiceChat
    while (!ota_url.empty() && ota_url.back() == '/') {
        ota_url.pop_back();
    }
    std::string api_url = ota_url + "/baiduVoiceChat";

    ESP_LOGI(TAG, "[0] Fetch session token: %s", api_url.c_str());

    auto& board = Board::GetInstance();
    auto network = board.GetNetwork();
    auto http = network->CreateHttp(0);

    // 设置请求头（复用 OTA 的认证信息）
    http->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
    http->SetHeader("Client-Id", board.GetUuid());
    http->SetHeader("User-Agent", SystemInfo::GetUserAgent());
    http->SetHeader("Content-Type", "application/json");
    
    // 发送 GET 请求 (注意参数顺序: method, url)
    if (!http->Open("GET", api_url)) {
        ESP_LOGE(TAG, "Failed to open HTTP connection");
        return false;
    }

    std::string response;
    int status_code = http->GetStatusCode();
    if (status_code != 200) {
        ESP_LOGE(TAG, "HTTP request failed with status code: %d", status_code);
        http->Close();
        return false;
    }

    // 读取响应
    char buffer[512];
    int bytes_read;
    while ((bytes_read = http->Read(buffer, sizeof(buffer) - 1)) > 0) {
        buffer[bytes_read] = '\0';
        response += buffer;
    }
    http->Close();

    ESP_LOGI(TAG, "Session response: %s", response.c_str());

    cJSON* root = cJSON_Parse(response.c_str());
    if (!root) return false;

    // 支持 {"data":{...}} 或直接 {...} 格式
    cJSON* data = cJSON_GetObjectItem(root, "data");
    if (!cJSON_IsObject(data)) data = root;

    // 获取 instance_id（大数字用 valuedouble 避免溢出）
    cJSON* id_item = cJSON_GetObjectItem(data, "ai_agent_instance_id");
    if (cJSON_IsNumber(id_item)) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.0f", id_item->valuedouble);
        instance_id = buf;
    } else if (cJSON_IsString(id_item)) {
        instance_id = id_item->valuestring;
    } else {
        cJSON_Delete(root);
        return false;
    }

    // 获取 token
    cJSON* ctx = cJSON_GetObjectItem(data, "context");
    cJSON* tok = ctx ? cJSON_GetObjectItem(ctx, "token") : nullptr;
    if (!cJSON_IsString(tok)) {
        cJSON_Delete(root);
        return false;
    }
    token = tok->valuestring;

    cJSON_Delete(root);
    ESP_LOGI(TAG, "Session token fetched: id=%s, token=%s...",
             instance_id.c_str(), token.substr(0, 10).c_str());
    return true;
}

void WebsocketBaiduProtocol::PrefetchSessionTokenAsync() {
    auto guard = prevent_destroy_guard_;
    Application::GetInstance().Schedule([this, guard]() {
        if (!guard->load() || session_token_fetched_) return;
        Settings settings("websocket", false);
        std::string url = settings.GetString("url");
        if (url.find("&id=") == std::string::npos || url.find("&t=") == std::string::npos) return;

        if (FetchSessionToken(cached_instance_id_, cached_token_)) {
            session_token_fetched_ = true;
            ESP_LOGI(TAG, "Token prefetched: id=%s", cached_instance_id_.c_str());

            if (!this->IsAudioChannelOpened() &&
                !autoconn_task_alive_.exchange(true, std::memory_order_acq_rel)) {
                xTaskCreatePinnedToCore([](void* arg) {
                    auto* p = static_cast<WebsocketBaiduProtocol*>(arg);
                    if (p->prevent_destroy_guard_->load()) {
                        ESP_LOGI(TAG, "Auto-connect WS for server push");
                        p->OpenAudioChannel();
                    }
                    p->autoconn_task_alive_.store(false, std::memory_order_release);
                    vTaskDelete(nullptr);
                }, "bd_autoconn", 8192, this, 1, nullptr, 0 /* Core 0 · 网络协议栈 */);
            }
        }
    });
}

// 替换 URL 中的参数值
static std::string ReplaceUrlParam(const std::string& url, const char* param, const std::string& value) {
    std::string key = std::string(param) + "=";
    size_t pos = url.find(key);
    if (pos == std::string::npos) {
        // 参数不存在，添加到末尾
        return url + "&" + param + "=" + value;
    }

    pos += key.length();
    size_t end = url.find('&', pos);
    if (end == std::string::npos) {
        // 参数在末尾
        return url.substr(0, pos) + value;
    } else {
        // 参数在中间
        return url.substr(0, pos) + value + url.substr(end);
    }
}

bool WebsocketBaiduProtocol::OpenAudioChannel() {
    bool expected = false;
    if (!opening_.compare_exchange_strong(expected, true)) {
        ESP_LOGW(TAG, "OpenAudioChannel re-entry blocked (another task is opening)");
        return false;
    }
    struct OpeningGuard {
        std::atomic<bool>& f;
        ~OpeningGuard() { f.store(false, std::memory_order_release); }
    } opening_guard{opening_};

    // 全量重置会话状态
    cmd_seq_ = 0;
    audio_tx_frames_ = audio_tx_bytes_ = audio_rx_frames_ = audio_rx_bytes_ = 0;
    greeting_sent_ = false;
    lic_active_pending_ = false;
    error_occurred_ = false;
    audio_send_failures_ = 0;
    control_send_failures_ = 0;
    is_speaking_ = false;
    is_playing_music_ = false;
    last_final_asr_text_.clear();
    last_final_asr_time_ = {};
    idle_goodbye_sent_ = false;
    is_realtime_mode_ = false;  // 强制 SendStartListening 重新发送模式命令
    auto_interrupt_ = false;

    // WS 已连接：直接复用，跳过 DNS/TCP/TLS/WS 握手
    if (websocket_ && websocket_->IsConnected()) {
        ESP_LOGI(TAG, "===== BAIDU REUSE WS =====");
        if (!device_info_sent_) {
            SendInitialDeviceInfo();
            device_info_sent_ = true;
        }
        media_ready_ = true;
        StartIdleTimer();
        if (on_connected_) on_connected_();
        if (on_audio_channel_opened_) on_audio_channel_opened_();
        return true;
    }

    ESP_LOGI(TAG, "===== BAIDU INIT =====");

    Settings settings("websocket", false);
    url_ = settings.GetString("url");
//    if (1) {
//        url_ = "wss://rtc-aiotgw.exp.bcelive.com/v1/realtime?a=apprmpwazcyzemj&ak=REDACTED_BAIDU_AK&sk=REDACTED_BAIDU_SK&ac=opus";
//    }

    // 解析 URL 参数
    auto parseParam = [](const std::string& url, const char* param) -> std::string {
        std::string key = std::string(param) + "=";
        size_t pos = url.find(key);
        if (pos == std::string::npos) return "";
        pos += key.length();
        size_t end = url.find('&', pos);
        return url.substr(pos, end == std::string::npos ? url.length() - pos : end - pos);
    };

    device_id_ = SystemInfo::GetMacAddress();
    std::string uid = parseParam(url_, "uid");
    std::string room = parseParam(url_, "roomname");
    user_id_ = !uid.empty() ? uid : (!room.empty() ? room : device_id_);

    // 连接方式一(id+t) vs 方式二(ak+sk)
    bool use_session_token = (url_.find("&id=") != std::string::npos &&
                              url_.find("&t=") != std::string::npos);

    Settings baidu_cfg("baidu", false);
    url_ = ReplaceUrlParam(url_, "ac", baidu_cfg.GetString("audiocodec", "opus"));

    if (use_session_token) {
        // 连接方式一：只在首次连接或断开后重连时获取新的会话 Token
        if (!session_token_fetched_) {
            if (FetchSessionToken(cached_instance_id_, cached_token_)) {
                session_token_fetched_ = true;
            } else {
                ESP_LOGW(TAG, "Token fetch failed, using original URL");
            }
        }
        if (session_token_fetched_ && !cached_instance_id_.empty()) {
            url_ = ReplaceUrlParam(url_, "id", cached_instance_id_);
            url_ = ReplaceUrlParam(url_, "t", cached_token_);
        }
    }

    media_ready_ = false;
    licensed_ = false;
    xEventGroupClearBits(event_group_handle_,
        BAIDU_PROTOCOL_CONNECTED_EVENT |
        BAIDU_PROTOCOL_LICENSED_EVENT |
        BAIDU_PROTOCOL_MEDIA_READY_EVENT);

    auto network = Board::GetInstance().GetNetwork();
    if (websocket_) {
        websocket_->Close();
        vTaskDelay(pdMS_TO_TICKS(100));   // 等 esp-tls receive task 真正退出
        websocket_.reset();
    }
    websocket_ = network->CreateWebSocket(1);
    if (!websocket_) {
        ESP_LOGE(TAG, "WS create failed");
        return false;
    }

    websocket_->OnData([this](const char* data, size_t len, bool binary) {
        if (binary) {
            HandleBinaryMessage(data, len);
        } else {
            HandleTextMessage(data, len);
        }
        UpdateLastIncomingTime();
    });

    websocket_->OnDisconnected([this]() {
        ESP_LOGW(TAG, "WS disconnected");
        bool was_ready = media_ready_.exchange(false, std::memory_order_acq_rel);
        device_info_sent_ = false;

        // 补发 TTS stop 通知应用层
        if (is_speaking_.exchange(false)) {
            cJSON* json = CreateJsonMessage("tts");
            cJSON_AddStringToObject(json, "state", "stop");
            if (on_incoming_json_) on_incoming_json_(json);
            cJSON_Delete(json);
        }
        is_playing_music_ = false;

        // 清除 Token 缓存（方式二 AK/SK 直连不需要 Prefetch）
        if (session_token_fetched_) {
            session_token_fetched_ = false;
            cached_instance_id_.clear();
            cached_token_.clear();
        }
        PrefetchSessionTokenAsync();

        audio_send_failures_ = 0;
        control_send_failures_ = 0;

        if (was_ready && on_audio_channel_closed_) on_audio_channel_closed_();
    });

    ESP_LOGI(TAG, "PSM warmup: PERFORMANCE → wait 500ms before TLS");
    Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    vTaskDelay(pdMS_TO_TICKS(500));

    // 重试策略: 最多 3 次, 每次失败间隔递增 (500ms / 1500ms)
    constexpr int kMaxConnectAttempts = 3;
    constexpr int kRetryDelaysMs[] = { 500, 1500 };  // 第 2 次后等 500ms, 第 3 次后等 1500ms
    bool connected = false;
    for (int attempt = 0; attempt < kMaxConnectAttempts; ++attempt) {
        if (attempt > 0) {
            int delay_ms = kRetryDelaysMs[attempt - 1];
            ESP_LOGW(TAG, "Connect attempt %d failed, retrying after %dms ...", attempt, delay_ms);
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
        }
        ESP_LOGI(TAG, "[1] Connect (attempt %d/%d): %s", attempt + 1, kMaxConnectAttempts, url_.c_str());
        connected = websocket_->Connect(url_.c_str());
        if (connected) break;
    }
    if (!connected) {
        SetError(Lang::Strings::SERVER_NOT_CONNECTED);
        return false;
    }

    xEventGroupSetBits(event_group_handle_, BAIDU_PROTOCOL_CONNECTED_EVENT);

    // 等待 License 或 MediaReady（10s，百度通常 1s 内响应）
    EventBits_t bits = xEventGroupWaitBits(event_group_handle_,
        BAIDU_PROTOCOL_MEDIA_READY_EVENT | BAIDU_PROTOCOL_LICENSED_EVENT,
        pdTRUE, pdFALSE, pdMS_TO_TICKS(10000));
    if (!(bits & (BAIDU_PROTOCOL_MEDIA_READY_EVENT | BAIDU_PROTOCOL_LICENSED_EVENT))) {
        if (!websocket_ || !websocket_->IsConnected()) {
            SetError(Lang::Strings::SERVER_TIMEOUT);
            return false;
        }
        ESP_LOGW(TAG, "Wait timeout, continue (AK/SK mode)");
    }

    if (!device_info_sent_) {
        SendInitialDeviceInfo();
        device_info_sent_ = true;
    }
    vTaskDelay(pdMS_TO_TICKS(100));
    media_ready_ = true;

    ESP_LOGI(TAG, "===== INIT OK =====");
    StartIdleTimer();

    if (on_connected_) on_connected_();
    if (on_audio_channel_opened_) on_audio_channel_opened_();

    return true;
}

void WebsocketBaiduProtocol::HandleTextMessage(const char* data, size_t len) {
    // 优化：避免不必要的 string 构造，直接操作 data 指针
    if (len < 3) return;

    // 快速前缀判断（避免多次 find 调用）
    // 前缀格式：[X]: 其中 X 是单字符标识
    if (data[0] == '[') {
        char prefix_char = data[1];

        // [E]: 事件（包括 License、TTS、Media 等）
        if (prefix_char == 'E' && len > 4 && data[2] == ']' && data[3] == ':') {
            std::string msg(data, len);
            if (msg.find("[LIC]:") != std::string::npos) {
                HandleLicenseEvent(msg);
            } else {
                HandleEvent(msg);
            }
            return;
        }

        // [Q]: ASR 结果
        if (prefix_char == 'Q' && len > 4 && data[2] == ']' && data[3] == ':') {
            std::string msg(data, len);
            // [Q]:[M]:[C]: ASR追加中间结果 (最长匹配优先)
            if (len > 12 && msg.find(ASR_Q_MID_APPEND) == 0) {
                HandleAsrResult(msg.substr(strlen(ASR_Q_MID_APPEND)), false);
            }
            // [Q]:[M]: 中间结果
            else if (len > 8 && msg.find(ASR_Q_MID_PREFIX) == 0) {
                HandleAsrResult(msg.substr(strlen(ASR_Q_MID_PREFIX)), false);
            }
            // [Q]:[C]: ASR追加最终结果
            else if (len > 8 && msg.find(ASR_Q_FIN_APPEND) == 0) {
                HandleAsrResult(msg.substr(strlen(ASR_Q_FIN_APPEND)), true);
            }
            // [Q]: 最终结果
            else {
                HandleAsrResult(msg.substr(strlen(ASR_Q_FIN_PREFIX)), true);
            }
            return;
        }

        // [A]: LLM 结果
        if (prefix_char == 'A' && len > 4 && data[2] == ']' && data[3] == ':') {
            std::string msg(data, len);
            // [A]:[H]: 引导语（作为 LLM 文本传递给应用层）
            if (len > 8 && msg.find(AGENT_ANSWER_HINT) == 0) {
                ESP_LOGI(TAG, "<< [A]:[H] %s", msg.c_str() + strlen(AGENT_ANSWER_HINT));
                HandleLLMResult(msg.substr(strlen(AGENT_ANSWER_HINT)), true);
            }
            // [A]:[M]: 中间结果
            else if (len > 8 && msg.find(AGENT_ANSWER_MID_PREFIX) == 0) {
                HandleLLMResult(msg.substr(strlen(AGENT_ANSWER_MID_PREFIX)), false);
            }
            // [A]: 最终结果
            else {
                HandleLLMResult(msg.substr(strlen(AGENT_ANSWER_PREFIX)), true);
            }
            return;
        }

        // [S]: 服务器状态/版本信息（仅记录日志，不做业务处理）
        if (prefix_char == 'S' && len > 4 && data[2] == ']' && data[3] == ':') {
            ESP_LOGI(TAG, "<< [S] %.*s", (int)(len - strlen(SERVER_STATUS_PREFIX)),
                     data + strlen(SERVER_STATUS_PREFIX));
            return;
        }

        // [F]: Function Call
        if (prefix_char == 'F' && len > 4 && data[2] == ']' && data[3] == ':') {
            HandleFunctionCall(std::string(data + strlen(FUNCTION_CALL_PREFIX), len - strlen(FUNCTION_CALL_PREFIX)));
            return;
        }

        // [C]: 自定义数据（服务器推送控制指令）
        // 格式：[C]:{"type":"xxx", ...} 或 [C]:任意JSON
        // 转换为 application.cc 兼容的 custom 消息格式
        if (prefix_char == 'C' && len > 4 && data[2] == ']' && data[3] == ':') {
            HandleCustomData(std::string(data + strlen(CUSTOM_DATA_PREFIX), len - strlen(CUSTOM_DATA_PREFIX)));
            return;
        }
    }

    // 兼容裸 JSON 消息（服务器可能直接发送 {"type":"xxx"} 格式）
    // 如：{"type":"ota"}, {"type":"reboot"} 等
    if (data[0] == '{' && len > 2) {
        cJSON* json = cJSON_Parse(data);
        if (json) {
            cJSON* type = cJSON_GetObjectItem(json, "type");
            if (cJSON_IsString(type)) {
                ESP_LOGI(TAG, "<< [JSON] type=%s", type->valuestring);
                // 作为自定义数据处理，复用 HandleCustomData 逻辑
                cJSON_Delete(json);
                HandleCustomData(std::string(data, len));
                return;
            }
            cJSON_Delete(json);
        }
    }

    // 未识别的消息
    ESP_LOGW(TAG, "<< [UNKNOWN] %.*s", (int)len, data);
}

void WebsocketBaiduProtocol::HandleBinaryMessage(const char* data, size_t len) {
    // WS 保活期间（media_ready_=false）丢弃残留音频帧，避免待命杂音
    if (!media_ready_ && len > 0) {
        static int drop_count = 0;
        if (++drop_count % 50 == 1) {
            ESP_LOGW(TAG, "Audio frame dropped: media_ready=false (total=%d, len=%d)", drop_count, (int)len);
        }
        return;
    }
    // 百度协议文档无最小帧限制，OPUS RFC 6716 允许 1 字节 DTX 静音帧（仅 TOC byte）
    // 丢弃有效小帧会导致 TTS 播放断断续续（dfda=false 时服务器下发静音填充）
    if (on_incoming_audio_ && len > 0) {
        auto packet = std::make_unique<AudioStreamPacket>();
        packet->sample_rate = server_sample_rate_;
        packet->frame_duration = server_frame_duration_;
        packet->payload = std::vector<uint8_t>(
            reinterpret_cast<const uint8_t*>(data),
            reinterpret_cast<const uint8_t*>(data) + len);
        on_incoming_audio_(std::move(packet));

        audio_rx_frames_++;
        audio_rx_bytes_ += len;
    }
}

void WebsocketBaiduProtocol::HandleAsrResult(const std::string& text, bool final) {
    // 收到 ASR 结果说明服务器在正常处理，重置/停止 listening 超时
    if (final) {
        StopListeningTimer();
    } else {
        StartListeningTimer();  // 部分结果：重置定时器
    }

    // 收到用户输入，更新活动时间
    UpdateActivityTime();

    std::string asr_text = text;
    std::string emotion;

    // 解析扩展字段: 文本|||[emotion]:xxx
    size_t ext_pos = text.find("|||");
    if (ext_pos != std::string::npos) {
        asr_text = text.substr(0, ext_pos);
        std::string ext = text.substr(ext_pos + 3);
        size_t emo_pos = ext.find("[emotion]:");
        if (emo_pos != std::string::npos) {
            size_t end = ext.find('&', emo_pos);
            emotion = ext.substr(emo_pos + 10,
                end == std::string::npos ? ext.length() - emo_pos - 10 : end - emo_pos - 10);
        }
    }

    // ASR final 去重：百度 asr_vad_append 模式下 [Q]: 和 [Q]:[C]: 可能对同一文本重复推送 final
    if (final && !asr_text.empty()) {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - last_final_asr_time_).count();
        if (asr_text == last_final_asr_text_ && elapsed < 2000) {
            ESP_LOGD(TAG, "ASR dedup: skip repeated final \"%s\" (%" PRId64 "ms)",
                     asr_text.c_str(), (int64_t)elapsed);
            return;
        }
        last_final_asr_text_ = asr_text;
        last_final_asr_time_ = now;
    }

    ESP_LOGI(TAG, "ASR: \"%s\" (final=%d, emotion=%s)",
             asr_text.c_str(), final, emotion.empty() ? "none" : emotion.c_str());

    // 发送STT消息
    cJSON* json = CreateJsonMessage("stt", asr_text.c_str());
    if (on_incoming_json_) on_incoming_json_(json);
    cJSON_Delete(json);

    // 情绪触发表情
    if (!emotion.empty()) {
        const char* en = TranslateEmotion(emotion);
        if (en) {
            ESP_LOGI(TAG, "😊 Emotion detected: %s -> %s", emotion.c_str(), en);
            cJSON* emo_json = cJSON_CreateObject();
            cJSON_AddStringToObject(emo_json, "type", "llm");
            cJSON_AddStringToObject(emo_json, "emotion", en);
            if (on_incoming_json_) on_incoming_json_(emo_json);
            cJSON_Delete(emo_json);
        }
    }
}

// 过滤 UTF-8 文本中的 4 字节 emoji（U+10000+: 😊🎉💕 等）
// 设备使用 Font Awesome 私有区图标（U+E000-U+F8FF，3 字节），这些保留不过滤
static std::string StripEmoji(const std::string& input) {
    std::string result;
    result.reserve(input.size());
    for (size_t i = 0; i < input.size(); ) {
        unsigned char c = input[i];
        int len = 1;
        if ((c & 0x80) == 0)        len = 1;       // ASCII
        else if ((c & 0xE0) == 0xC0) len = 2;       // 2 字节
        else if ((c & 0xF0) == 0xE0) len = 3;       // 3 字节（含 CJK + Font Awesome PUA）
        else if ((c & 0xF8) == 0xF0) len = 4;       // 4 字节（emoji）
        else { i++; continue; }

        if (i + len > input.size()) break;

        // 仅跳过 4 字节字符（标准 emoji: U+10000+）
        if (len < 4) {
            result.append(input, i, len);
        }
        i += len;
    }
    return result;
}

void WebsocketBaiduProtocol::HandleLLMResult(const std::string& text, bool final) {
    // 收到 AI 回复说明会话已开始（含服务器自动招呼），标记已问候，防止 SendStartListening 重复发送
    greeting_sent_ = true;

    // 收到AI回复，更新活动时间
    UpdateActivityTime();

    // 过滤 emoji（设备字库不支持 4 字节 UTF-8 字符）
    std::string display_text = StripEmoji(text);
    ESP_LOGI(TAG, "<< [A] %s%s", final ? "" : "[M]:", display_text.c_str());
    cJSON* json = CreateJsonMessage("tts", display_text.c_str());
    cJSON_AddStringToObject(json, "state", "sentence_start");
    if (on_incoming_json_) on_incoming_json_(json);
    cJSON_Delete(json);
}

// 通知 TTS 状态 + 情绪变化（减少重复 cJSON 代码）
void WebsocketBaiduProtocol::NotifyTtsState(const char* state, const char* emotion) {
    cJSON* json = CreateJsonMessage("tts");
    cJSON_AddStringToObject(json, "state", state);
    if (on_incoming_json_) on_incoming_json_(json);
    cJSON_Delete(json);
    if (emotion) {
        cJSON* emo = cJSON_CreateObject();
        cJSON_AddStringToObject(emo, "type", "llm");
        cJSON_AddStringToObject(emo, "emotion", emotion);
        if (on_incoming_json_) on_incoming_json_(emo);
        cJSON_Delete(emo);
    }
}

void WebsocketBaiduProtocol::HandleEvent(const std::string& event) {
    if (event.find("[TTS_BEGIN_SPEAKING]") != std::string::npos) {
        is_speaking_ = true;
        StopListeningTimer();
        if (is_playing_music_) SendText(CMD_MUSIC_PAUSE);
        NotifyTtsState("start", "happy");
    }
    else if (event.find("[TTS_END_SPEAKING]") != std::string::npos) {
        is_speaking_ = false;
        // TTS 结束时立即清空解码队列，防止残留帧产生杂音
        Application::GetInstance().GetAudioService().ResetDecoder();
        ScheduleLicRetry();
        if (is_playing_music_) SendText(CMD_MUSIC_RESUME);
        NotifyTtsState("stop", "neutral");

        // 2026-05-13 v2.2.14: 告别 TTS 完整播放结束 → 立刻关通道, 而非等 idle_dc 12s 兜底
        //   优化路径: 实际 TTS 4-6s 播完即关, 用户体验更紧凑
        //   12s 兜底 timer 仍保留 (idle_goodbye_sent_ 仍为 true), 防 LLM 不响应永远不关
        if (idle_goodbye_sent_) {
            ESP_LOGI(TAG, "Goodbye TTS finished → close channel now (fast path)");
            auto guard = prevent_destroy_guard_;
            Application::GetInstance().Schedule([this, guard]() {
                if (!guard->load() || !idle_goodbye_sent_) return;
                Application::GetInstance().GetAudioService().ResetDecoder();
                media_ready_ = false;
                is_speaking_ = false;
                StopIdleTimer();
                if (on_audio_channel_closed_) on_audio_channel_closed_();
                ESP_LOGI(TAG, "Idle goodbye complete → audio channel closed");
            });
        }
    }
    else if (event.find("[REMOTE_PLAYER_BEGIN]") != std::string::npos) {
        is_playing_music_ = true;
        is_speaking_ = true;
        StopListeningTimer();  // 音乐开始播放，停止 ASR 超时定时器
        NotifyTtsState("start", nullptr);
    }
    else if (event.find("[REMOTE_PLAYER_END]") != std::string::npos) {
        is_playing_music_ = false;
        is_speaking_ = false;
        ScheduleLicRetry();
        NotifyTtsState("stop", nullptr);
    }
    else if (event.find("[VOICE_COMING]") != std::string::npos) {
        // 来自 AEC 模式联动 § aec_cfg_->handle_voice_coming
        // AEC ON: 响应云端 VAD 检测到的人声 → 打断 TTS (AEC ON 模式独有, 因为只有它发了 disable_voice_auto_int=false)
        // AEC OFF: 忽略 (云端 VAD 已通过 disable_voice_auto_int=true 关闭, 不会发此事件; 防御性 ignore)
        if (aec_cfg_->handle_voice_coming && is_speaking_) {
            ESP_LOGI(TAG, "<< VOICE_COMING → interrupt TTS (cloud VAD)");
            is_speaking_ = false;
            NotifyTtsState("stop", nullptr);
        } else {
            ESP_LOGD(TAG, "<< VOICE_COMING ignored (handle=%d speaking=%d)",
                     aec_cfg_->handle_voice_coming, is_speaking_.load());
        }
    }
    else if (event.find("[MEDIA]:[READY]:") != std::string::npos ||
             event.find("[AGENTID]:") != std::string::npos) {
        media_ready_ = true;
        xEventGroupSetBits(event_group_handle_, BAIDU_PROTOCOL_MEDIA_READY_EVENT);
    }
    // 云端 → 设备 远程音乐控制（用户语音"暂停/继续/停止" → 云端 → 设备本地 MusicPlayer 同步）
    // 顺序敏感：先匹配最长前缀再匹配短前缀，否则 STOP 会被 PAUSE/RESUME 的更短匹配吃掉
    else if (event.find("[CMD]:[REMOTE_PLAYER]:[PAUSE]") != std::string::npos) {
        HandleRemoteMusicCommand("pause");
    }
    else if (event.find("[CMD]:[REMOTE_PLAYER]:[RESUME]") != std::string::npos) {
        HandleRemoteMusicCommand("resume");
    }
    else if (event.find("[CMD]:[REMOTE_PLAYER]:[STOP]") != std::string::npos) {
        HandleRemoteMusicCommand("stop");
    }
    // 教育卡：system prompt 切档结果回执（带 model_type/result，简化为日志 + 轻提示）
    else if (event.find("[SYSTEM_PROMPT_UPDATED]") != std::string::npos) {
        ESP_LOGI(TAG, "<< SYSTEM_PROMPT_UPDATED");
        auto guard = prevent_destroy_guard_;
        Application::GetInstance().Schedule([guard]() {
            if (!guard->load()) return;
            if (auto* d = Board::GetInstance().GetDisplay()) {
                d->ShowNotification("已切换教学模式", 1500);
            }
        });
    }
    else {
        ESP_LOGD(TAG, "<< [E] %s", event.c_str());
    }
}

void WebsocketBaiduProtocol::HandleLicenseEvent(const std::string& event) {
    if (event.find("[LIC]:[MUST]:") != std::string::npos) {
        // 4G 弱网优化：LIC_ACTIVE 永远不立即发送，统一走 timer 延迟。
        // 原因：
        // 1. WS 连接刚建立时 modem UART 仍忙于处理 TLS/WS 帧，立即发送必超时
        // 2. 每次发送超时消耗 TCP 层全局失败名额，连续失败导致断连→重连→循环
        // 3. 延迟 3s 发送，给 UART 空闲时间，大幅降低失败率
        if (!lic_active_pending_ && !licensed_) {
            lic_active_pending_ = true;
            ESP_LOGI(TAG, "<< LIC_MUST (deferred 3s)");
            ScheduleLicRetry();
        } else {
            ESP_LOGD(TAG, "<< LIC_MUST (already pending or licensed)");
        }
    }
    else if (event.find("[LIC]:[RES]:[PASS]:") != std::string::npos) {
        ESP_LOGI(TAG, "<< LIC_PASS");
        licensed_ = true;
        xEventGroupSetBits(event_group_handle_, BAIDU_PROTOCOL_LICENSED_EVENT);
    }
    else if (event.find("[LIC]:[RES]:[FAILED]:") != std::string::npos) {
        ESP_LOGE(TAG, "<< LIC_FAILED");
        // 服务器已判定失败，停止一切重试（防止无效重试浪费资源/加速断连）
        lic_active_pending_ = false;
        if (lic_retry_timer_handle_) {
            xTimerStop(lic_retry_timer_handle_, 0);
        }
        SetError("设备授权失败");
    }
}

void WebsocketBaiduProtocol::HandleFunctionCall(const std::string& data) {
    ESP_LOGI(TAG, "<< [FC] %s", data.c_str());

    cJSON* fc = cJSON_Parse(data.c_str());
    if (!fc) return;

    cJSON* session_id = cJSON_GetObjectItem(fc, "session_id");
    cJSON* content = cJSON_GetObjectItem(fc, "content");

    if (!cJSON_IsString(content)) {
        cJSON_Delete(fc);
        return;
    }

    cJSON* payload = cJSON_Parse(content->valuestring);
    if (!payload) {
        cJSON_Delete(fc);
        return;
    }

    cJSON* func_name = cJSON_GetObjectItem(payload, "function_name");
    cJSON* param_list = cJSON_GetObjectItem(payload, "parameter_list");

    // 构造 JSONRPC 2.0 tools/call 消息
    cJSON* jsonrpc = cJSON_CreateObject();
    cJSON_AddStringToObject(jsonrpc, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(jsonrpc, "id", 0);
    cJSON_AddStringToObject(jsonrpc, "method", "tools/call");

    cJSON* params = cJSON_CreateObject();
    if (cJSON_IsString(func_name)) {
        // 百度 FC 工具名用 __ 替代 .，还原为 MCP 标准名
        std::string name(func_name->valuestring);
        size_t pos = 0;
        while ((pos = name.find("__", pos)) != std::string::npos) {
            name.replace(pos, 2, ".");
            pos += 1;
        }
        cJSON_AddStringToObject(params, "name", name.c_str());
    }

    // parameter_list 是数组，合并为单个 arguments 对象
    cJSON* arguments = cJSON_CreateObject();
    if (cJSON_IsArray(param_list)) {
        cJSON* item = nullptr;
        cJSON_ArrayForEach(item, param_list) {
            if (cJSON_IsObject(item)) {
                cJSON* child = item->child;
                while (child) {
                    cJSON_AddItemToObject(arguments, child->string, cJSON_Duplicate(child, true));
                    child = child->next;
                }
            }
        }
    }
    cJSON_AddItemToObject(params, "arguments", arguments);

    if (cJSON_IsString(session_id)) {
        cJSON_AddStringToObject(params, "_baidu_session_id", session_id->valuestring);
    }
    cJSON_AddItemToObject(jsonrpc, "params", params);

    // 包装为 {"type":"mcp","payload": jsonrpc}
    cJSON* json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "type", "mcp");
    cJSON_AddItemToObject(json, "payload", jsonrpc);

    if (on_incoming_json_) on_incoming_json_(json);
    cJSON_Delete(json);
    cJSON_Delete(payload);
    cJSON_Delete(fc);
}

void WebsocketBaiduProtocol::HandleCustomData(const std::string& data) {
    // 服务器推送自定义数据：[C]:{"type":"xxx", ...} 或 [C]:任意JSON
    // {"type": "custom", "payload": {"message": {...}}}
    ESP_LOGI(TAG, "<< [CUSTOM] %s", data.c_str());

    cJSON* custom_data = cJSON_Parse(data.c_str());
    if (!custom_data) {
        ESP_LOGW(TAG, "Invalid custom data JSON: %s", data.c_str());
        return;
    }

    // 构造 application.cc 兼容的 custom 消息格式
    cJSON* json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "type", "custom");

    // payload.message = custom_data（原始数据作为 message）
    cJSON* payload = cJSON_CreateObject();
    cJSON_AddItemToObject(payload, "message", custom_data);  // custom_data 所有权转移
    cJSON_AddItemToObject(json, "payload", payload);

    if (on_incoming_json_) {
        on_incoming_json_(json);
    }
    cJSON_Delete(json);
}

bool WebsocketBaiduProtocol::SendLicenseActivation() {
    if (license_key_.empty()) {
        media_ready_ = true;
        xEventGroupSetBits(event_group_handle_, BAIDU_PROTOCOL_MEDIA_READY_EVENT);
        return true;
    }

    cJSON* json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "devId", device_id_.c_str());
    cJSON_AddStringToObject(json, "uId", user_id_.c_str());
    cJSON_AddStringToObject(json, "licKey", license_key_.c_str());

    char* str = cJSON_PrintUnformatted(json);
    bool ok = SendText(std::string(EVENT_LIC_ACTIVE) + str);
    cJSON_free(str);
    cJSON_Delete(json);

    if (!ok) {
        ESP_LOGE(TAG, "LIC_ACTIVE send failed, will retry when UART idle");
    }
    return ok;
}

void WebsocketBaiduProtocol::TryFlushPendingLicActive() {
    if (!lic_active_pending_ || licensed_) {
        return;
    }
    ESP_LOGI(TAG, "Retrying deferred LIC_ACTIVE");
    if (SendLicenseActivation()) {
        last_lic_active_time_ = std::chrono::steady_clock::now();
        lic_active_pending_ = false;
    } else {
        // 仍然失败，继续延迟重试（不立即重试，避免消耗 TCP 失败名额）
        ESP_LOGW(TAG, "LIC_ACTIVE still failing, retry in 5s");
        ScheduleLicRetry();
    }
}

void WebsocketBaiduProtocol::ScheduleLicRetry() {
    if (!lic_active_pending_ || licensed_) {
        return;
    }
    // 延迟 2s 后在主线程重试（避免在网络回调中同步阻塞 UART）
    if (lic_retry_timer_handle_) {
        xTimerStart(lic_retry_timer_handle_, 0);
    }
}

void WebsocketBaiduProtocol::SendInitialDeviceInfo() {
    // AEC 模式由构造时确定 (aec_cfg_), 此处不再读 NVS — 避免与 SendAudio/[VOICE_COMING] 等消费点状态不一致
    cJSON* json = cJSON_CreateObject();

    // ===== 设备标识 (官方 DeviceInfo 规范, 见 doc/RTC/s/sm9qgxvfq) =====
    cJSON_AddStringToObject(json, "model", BOARD_TYPE);
    cJSON_AddStringToObject(json, "os", "esp-idf");
    cJSON_AddStringToObject(json, "soc", "esp32s3");
    if (!user_id_.empty()) {
        cJSON_AddStringToObject(json, "user_id", user_id_.c_str());
    }

    // ===== AEC 联动配置 (统一从 aec_cfg_ 派生, 避免散落 if/else) =====
    cJSON_AddBoolToObject(json, "dfda", aec_cfg_->full_duplex);
    cJSON_AddBoolToObject(json, "disable_voice_auto_int", !aec_cfg_->cloud_auto_int);
    cJSON_AddNumberToObject(json, "tts_end_delay_ms", aec_cfg_->tts_end_delay_ms);

    // ===== TTS 弱网加速 (与 AEC 模式无关, 二者都开) =====
    cJSON_AddBoolToObject(json, "tts_enable_fast_send", true);
    cJSON_AddNumberToObject(json, "tts_fast_send_second", kBaiduTtsFastSendSeconds);
    cJSON_AddNumberToObject(json, "tts_fast_send_ratio", kBaiduTtsFastSendRatio);

    // ===== 防御性显式关闭云端 3A =====
    cJSON* cloud_3a = cJSON_CreateObject();
    for (const char* algo : { "AEC", "ANS", "AGC" }) {
        cJSON* obj = cJSON_CreateObject();
        cJSON_AddBoolToObject(obj, "enable", false);
        cJSON_AddItemToObject(cloud_3a, algo, obj);
    }
    cJSON_AddItemToObject(json, "cloud_3A_url", cloud_3a);

    char* str = cJSON_PrintUnformatted(json);
    ESP_LOGI(TAG, ">> DeviceInfo (%s): %s", aec_cfg_->mode_name, str);
    SendText(std::string(CMD_DEVICE_INFO) + str);
    cJSON_free(str);
    cJSON_Delete(json);
}

cJSON* WebsocketBaiduProtocol::CreateJsonMessage(const char* type, const char* text) {
    cJSON* json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "type", type);
    if (text && text[0]) {
        cJSON_AddStringToObject(json, "text", text);
    }
    return json;
}

bool WebsocketBaiduProtocol::SendTextToAI(const std::string& text) {
    return SendText(std::string(INPUT_TEXT_TO_AI) + text);
}

bool WebsocketBaiduProtocol::SendTextToTts(const std::string& text) {
    return SendText(std::string(INPUT_TEXT_TO_TTS) + text);
}

bool WebsocketBaiduProtocol::UpdateSystemPrompt(int model_type, const std::string& prompt) {
    if (!websocket_ || !websocket_->IsConnected()) {
        ESP_LOGW(TAG, "UpdateSystemPrompt: ws not connected");
        return false;
    }

    cJSON* json = cJSON_CreateObject();
    char model_type_buf[8];
    snprintf(model_type_buf, sizeof(model_type_buf), "%d", model_type);
    cJSON_AddStringToObject(json, "model_type", model_type_buf);
    cJSON_AddStringToObject(json, "prompt", prompt.c_str());

    char* str = cJSON_PrintUnformatted(json);
    bool ok = SendText(std::string(CMD_UPDATE_PROMPT) + (str ? str : ""));
    if (str) cJSON_free(str);
    cJSON_Delete(json);

    ESP_LOGI(TAG, ">> UPDATE_PROMPT type=%d len=%u %s",
             model_type, (unsigned)prompt.size(), ok ? "ok" : "fail");
    return ok;
}

// 本地 → 云端 远程音乐控制（CMD_MUSIC_STOP/PAUSE/RESUME）
//   action: "stop" / "pause" / "resume"
bool WebsocketBaiduProtocol::SendRemoteMusicControl(const std::string& action) {
    if (!websocket_ || !websocket_->IsConnected()) return false;
    if (action == "stop")   return SendText(CMD_MUSIC_STOP);
    if (action == "pause")  return SendText(CMD_MUSIC_PAUSE);
    if (action == "resume") return SendText(CMD_MUSIC_RESUME);
    ESP_LOGW(TAG, "SendRemoteMusicControl: unknown action='%s'", action.c_str());
    return false;
}

void WebsocketBaiduProtocol::HandleRemoteMusicCommand(const char* action) {
    if (!action) return;
    ESP_LOGI(TAG, "<< REMOTE_PLAYER:%s", action);

    std::string a = action;
    auto guard = prevent_destroy_guard_;
    Application::GetInstance().Schedule([a = std::move(a), guard]() {
        if (!guard->load()) return;
        auto& mp = MusicPlayer::GetInstance();
        if (a == "stop") {
            if (mp.IsPlaying()) mp.Stop();
            // 解码队列同步清空，防止云端流残留
            Application::GetInstance().GetAudioService().ResetDecoder();
        } else if (a == "pause") {
            if (mp.IsPlaying() && !mp.IsPaused()) mp.Pause();
        } else if (a == "resume") {
            if (mp.IsPaused()) mp.Resume();
        }
    });

    // 同步协议侧"音乐播放中"标志（SendStartListening 等分支用）
    if (std::strcmp(action, "stop") == 0) {
        is_playing_music_ = false;
        is_speaking_ = false;
    } else if (std::strcmp(action, "pause") == 0) {
        // pause 期间云端不再下发音频，但会话仍存在，保持 is_playing_music_ 为 true 以触发 RESUME 时的逻辑
    }
}

void WebsocketBaiduProtocol::SendInterrupt() {
    SendText(PREFIX_BREAK);
}

void WebsocketBaiduProtocol::SetAutoInterrupt(bool enable) {
    SendText(enable ? CMD_AUTO_INT_ON : CMD_AUTO_INT_OFF);
}

void WebsocketBaiduProtocol::PauseAsr() {
    ESP_LOGI(TAG, "Pause ASR");
    SendText(CMD_ASR_MANUAL);
}

void WebsocketBaiduProtocol::ResumeAsr() {
    ESP_LOGI(TAG, "Resume ASR");
    SendText(CMD_ASR_REALTIME);
}

void WebsocketBaiduProtocol::SendFunctionCallResult(
    const std::string& session_id,
    const std::string& result,
    const std::string& message) {
    cJSON* json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "session_id", session_id.c_str());
    cJSON_AddStringToObject(json, "result", result.c_str());
    cJSON_AddStringToObject(json, "message", message.c_str());

    char* str = cJSON_PrintUnformatted(json);
    SendText(std::string(FUNCTION_CALL_PREFIX) + str);
    cJSON_free(str);
    cJSON_Delete(json);
}

void WebsocketBaiduProtocol::SendWakeWordDetected(const std::string& wake_word) {
    // 直播/音乐播放中唤醒：跳过问候语，避免打断直播内容
    if (!is_playing_music_) {
        SendTextToAI(wake_word.empty() ? "你好" : wake_word);
    }
    greeting_sent_ = true;
    UpdateActivityTime();
}

void WebsocketBaiduProtocol::SendStartListening(ListeningMode mode) {
    current_mode_ = mode;

    if (mode == kListeningModeManualStop) {
        if (is_realtime_mode_) {
            is_realtime_mode_ = false;
            auto_interrupt_ = false;
            SendText(CMD_ASR_MANUAL);
            SendText(CMD_AUTO_INT_OFF);
        }
        SendText(CMD_ASR_START);
    } else {
        bool want_auto_int = (mode == kListeningModeRealtime);
        if (!is_realtime_mode_) {
            is_realtime_mode_ = true;
            SendText(CMD_ASR_REALTIME);
        }
        if (auto_interrupt_ != want_auto_int) {
            auto_interrupt_ = want_auto_int;
            SendText(want_auto_int ? CMD_AUTO_INT_ON : CMD_AUTO_INT_OFF);
        }
    }

    if (!greeting_sent_ && !is_playing_music_) {
        SendTextToAI("你好");
        greeting_sent_ = true;
    }
    StartListeningTimer();
    UpdateActivityTime();
}

void WebsocketBaiduProtocol::SendStopListening() {
    if (!is_realtime_mode_) SendText(CMD_ASR_STOP);
}

void WebsocketBaiduProtocol::SendAbortSpeaking(AbortReason reason) {
    StopListeningTimer();

    // 仅在 TTS 播放中才发送 stop（空闲唤醒时不发，避免误关通道）
    if (is_speaking_.exchange(false)) {
        Application::GetInstance().GetAudioService().ResetDecoder();
        NotifyTtsState("stop", "neutral");
    }

    // 唤醒词打断: 丢弃 N ms 音频避免回声; 手动打断: 立即打断
    if (reason == kAbortReasonWakeWordDetected) {
        SendText(std::string(AGENT_BREAK_BEGIN) + std::to_string(break_delay_ms_));
    } else {
        SendText(PREFIX_BREAK);
    }

    if (!is_realtime_mode_) SendText(CMD_ASR_STOP);
    // 音乐/直播：暂停而非停止，TTS 结束后自动恢复（见 HandleEvent TTS_END）
    if (is_playing_music_) SendText(CMD_MUSIC_PAUSE);

    StartListeningTimer();

    TryFlushPendingLicActive();
    UpdateActivityTime();
}

void WebsocketBaiduProtocol::SendMcpMessage(const std::string& payload) {
    ESP_LOGI(TAG, "MCP: %s", payload.c_str());
    SendText(std::string(FUNCTION_CALL_PREFIX) + payload);
}

void WebsocketBaiduProtocol::StartListeningTimer() {
    if (listening_timer_handle_) {
        // 无论是否已在运行，重置定时器（收到部分 ASR 结果时重置）
        xTimerReset(listening_timer_handle_, 0);
    }
}

void WebsocketBaiduProtocol::StopListeningTimer() {
    if (listening_timer_handle_) {
        xTimerStop(listening_timer_handle_, 0);
    }
}

void WebsocketBaiduProtocol::UpdateActivityTime() {
    last_activity_time_ = std::chrono::steady_clock::now();
    idle_goodbye_sent_ = false;
}

void WebsocketBaiduProtocol::StartIdleTimer() {
    if (idle_timer_handle_ && !xTimerIsTimerActive(idle_timer_handle_)) {
        UpdateActivityTime();
        xTimerStart(idle_timer_handle_, 0);
    }
}

void WebsocketBaiduProtocol::StopIdleTimer() {
    if (idle_timer_handle_ && xTimerIsTimerActive(idle_timer_handle_)) {
        xTimerStop(idle_timer_handle_, 0);
    }
    idle_goodbye_sent_ = false;
}

void WebsocketBaiduProtocol::CheckIdleTimeout() {
    if (!IsAudioChannelOpened()) return;

    auto cur_state = Application::GetInstance().GetDeviceState();
    if (cur_state != kDeviceStateIdle || is_speaking_) {
        UpdateActivityTime();
        return;
    }

    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - last_activity_time_).count();

    if (elapsed < idle_timeout_seconds_ || idle_goodbye_sent_) return;

    idle_goodbye_sent_ = true;
    auto guard = prevent_destroy_guard_;
    Application::GetInstance().Schedule([this, guard]() {
        if (!guard->load() || !idle_goodbye_sent_) return;
        SendTextToAI("我要先去忙了，你跟我说再见吧");

        auto t = xTimerCreate("idle_dc", pdMS_TO_TICKS(12000), pdFALSE, this,
            [](TimerHandle_t timer) {
                auto* self = static_cast<WebsocketBaiduProtocol*>(pvTimerGetTimerID(timer));
                if (self) {
                    auto g = self->prevent_destroy_guard_;
                    Application::GetInstance().Schedule([self, g]() {
                        if (!g->load()) return;
                        // 用户在 12s 内唤醒过 → idle_goodbye_sent_ 已被 OpenAudioChannel 清, 跳过关闭
                        if (!self->idle_goodbye_sent_) {
                            ESP_LOGI(TAG, "Idle dc cancelled (user resumed during goodbye)");
                            return;
                        }
                        // 清空残留音频，防止进入待命后扬声器噗噗响
                        Application::GetInstance().GetAudioService().ResetDecoder();
                        // 只关闭音频通道，不断开 WS（保留推送能力）
                        self->media_ready_ = false;
                        self->is_speaking_ = false;
                        self->StopIdleTimer();
                        if (self->on_audio_channel_closed_) self->on_audio_channel_closed_();
                        ESP_LOGI(TAG, "Idle timeout: audio channel closed, WS kept alive");
                    });
                }
                xTimerDelete(timer, 0);
            });
        if (t) xTimerStart(t, 0);
    });
}

void WebsocketBaiduProtocol::IdleTimerCallback(TimerHandle_t timer) {
    // 从定时器ID获取对象指针
    auto* protocol = static_cast<WebsocketBaiduProtocol*>(pvTimerGetTimerID(timer));
    if (protocol) {
        protocol->CheckIdleTimeout();
    }
}
