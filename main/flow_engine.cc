#include "flow_engine.h"
#include "application.h"
#include "board.h"
#include "device_state_event.h"
#include "system_info.h"

#include <cJSON.h>
#include <esp_log.h>
#include <cstring>

#define TAG "FlowEngine"

// 脚本最大大小 8KB
static constexpr int kMaxScriptSize = 8192;
// HTTP 超时 15 秒
static constexpr int kHttpTimeoutMs = 15000;
// HTTP 重试次数
static constexpr int kHttpRetryCount = 3;
// 恢复脚本的默认延时(ms)，可由脚本 JSON 的 resume_delay_s 字段覆盖
static constexpr int kDefaultResumeDelayMs = 5000;

FlowEngine::FlowEngine(Application* app) : app_(app) {
    // 创建延时定时器
    esp_timer_create_args_t timer_args = {
        .callback = DelayTimerCallback,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "flow_delay",
        .skip_unhandled_events = true,
    };
    esp_timer_create(&timer_args, &delay_timer_);

    // 注册设备状态变化回调，用于检测 TTS 播放完成
    DeviceStateEventManager::GetInstance().RegisterStateChangeCallback(
        [this](DeviceState prev, DeviceState curr) {
            OnDeviceStateChanged(prev, curr);
        });
}

FlowEngine::~FlowEngine() {
    Stop();
    if (delay_timer_) {
        esp_timer_stop(delay_timer_);
        esp_timer_delete(delay_timer_);
        delay_timer_ = nullptr;
    }
}

// ============================================================================
// 公共 API
// ============================================================================

void FlowEngine::Start(const std::string& url) {
    if (url.empty()) {
        ESP_LOGW(TAG, "Start: empty URL");
        return;
    }

    // 先停止当前播放
    if (state_ != FlowState::kIdle) {
        Stop();
    }

    pending_url_ = url;

    app_->Schedule([this]() {
        app_->Alert("直播伴侣", "加载脚本中...", "", "");

        // 在后台任务中下载脚本（TLS 需要内部 RAM 栈）
        BaseType_t ok = xTaskCreatePinnedToCore(
            LoadScriptTask, "flow_load", 6144, this, 1, &load_task_handle_, 0);
        if (ok != pdPASS) {
            load_task_handle_ = nullptr;
            app_->Alert("直播伴侣", "启动失败: 内存不足", "", "");
            ESP_LOGE(TAG, "Failed to create load task");
        }
    });
}

void FlowEngine::RestartLast() {
    if (last_script_json_.empty()) return;
    StartWithScript(last_script_json_);
}

void FlowEngine::StartWithScript(const std::string& json_str) {
    if (json_str.empty()) {
        ESP_LOGW(TAG, "StartWithScript: empty script");
        return;
    }

    // 保存脚本内容，用于唤醒后自动恢复
    last_script_json_ = json_str;

    // 先停止当前播放
    if (state_ != FlowState::kIdle) {
        Stop();
    }

    // 直接解析，无需 HTTP 下载
    if (!ParseScript(json_str)) {
        return;
    }

    state_ = FlowState::kPlaying;
    loop_count_ = 1;
    app_->ForceListeningMode(kListeningModeManualStop);

    ESP_LOGI(TAG, "========== 直播伴侣启动 ==========");
    ESP_LOGI(TAG, "脚本: %s | 共 %d 项 | 循环: %s",
             script_name_.c_str(), total_items_.load(), loop_ ? "是" : "否");
    char buf[80];
    snprintf(buf, sizeof(buf), "开始播放: %s (%d项%s)",
             script_name_.c_str(), total_items_.load(), loop_ ? ",循环" : "");
    app_->Alert("直播伴侣", buf, "", "");

    PlayCurrentItem();
}

void FlowEngine::NotifyTtsFinished() {
    if (state_ != FlowState::kPlaying) return;

    if (recap_pending_.exchange(false)) {
        ESP_LOGI(TAG, "<<< 回顾完毕, 继续脚本第 %d 项", current_index_.load() + 1);
        app_->Schedule([this]() { PlayCurrentItem(); });
    } else {
        ScheduleNext();
    }
}

void FlowEngine::Stop() {
    FlowState prev = state_.exchange(FlowState::kIdle);
    if (prev == FlowState::kIdle) return;

    esp_timer_stop(delay_timer_);

    int played_items = current_index_.load();
    int loops = loop_count_.load();

    {
        std::lock_guard<std::mutex> lock(script_mutex_);
        items_.clear();
    }

    current_index_ = 0;
    total_items_ = 0;
    loop_count_ = 0;
    recap_pending_ = false;

    // 恢复默认监听模式
    app_->ForceListeningMode(kListeningModeAutoStop);

    // 直播停止后确保设备回到 Idle 状态
    auto dev_state = app_->GetDeviceState();
    if (dev_state == kDeviceStateSpeaking || dev_state == kDeviceStateListening) {
        app_->SetDeviceState(kDeviceStateIdle);
    }

    ESP_LOGI(TAG, "========== 直播伴侣停止 ==========");
    ESP_LOGI(TAG, "已播放 %d 轮, 当前第 %d 项", loops, played_items);
}

void FlowEngine::Suspend() {
    // 从 Playing 或 Delay 状态暂停（只标记状态，不调用 AbortSpeaking 避免递归）
    FlowState expected = FlowState::kPlaying;
    if (state_.compare_exchange_strong(expected, FlowState::kSuspended)) {
        ESP_LOGI(TAG, ">>> 插播暂停 [第%d轮 %d/%d项] 脚本已挂起",
                 loop_count_.load(), current_index_.load() + 1, total_items_.load());
        return;
    }

    expected = FlowState::kDelay;
    if (state_.compare_exchange_strong(expected, FlowState::kSuspended)) {
        esp_timer_stop(delay_timer_);
        ESP_LOGI(TAG, ">>> 插播暂停 [第%d轮 %d/%d项] 等待间隔中断",
                 loop_count_.load(), current_index_.load() + 1, total_items_.load());
    }
}

void FlowEngine::Resume() {
    if (state_ != FlowState::kSuspended) return;

    app_->Schedule([this]() {
        if (state_ != FlowState::kSuspended) return;

        state_ = FlowState::kPlaying;
        app_->ForceListeningMode(kListeningModeManualStop);

        int idx = current_index_.load();
        std::string recap_text;
        {
            std::lock_guard<std::mutex> lock(script_mutex_);
            if (idx >= 0 && idx < (int)items_.size()) {
                recap_text = items_[idx].text;
            }
        }

        if (!recap_text.empty()) {
            // 先用 TTAI 回顾被打断的脚本内容，播完后再继续
            recap_pending_ = true;
            std::string prompt = "刚才直播间讲到「" + recap_text +
                "」，请用一句话简短回顾，然后说'好，我们继续'";
            ESP_LOGI(TAG, "<<< 插播结束, TTAI回顾后继续 [第%d轮 %d/%d项]",
                     loop_count_.load(), idx + 1, total_items_.load());
            app_->SendTextToAI(prompt);
        } else {
            ESP_LOGI(TAG, "<<< 插播结束, 恢复脚本 [第%d轮 %d/%d项]",
                     loop_count_.load(), idx + 1, total_items_.load());
            PlayCurrentItem();
        }
    });
}

void FlowEngine::Restart() {
    FlowState prev = state_.load();
    if (prev == FlowState::kIdle) return;

    esp_timer_stop(delay_timer_);
    current_index_ = 0;
    loop_count_ = 1;
    recap_pending_ = false;
    state_ = FlowState::kPlaying;
    app_->ForceListeningMode(kListeningModeManualStop);

    ESP_LOGI(TAG, "========== 直播伴侣从头开始 ==========");
    ESP_LOGI(TAG, "脚本: %s | 共 %d 项", script_name_.c_str(), total_items_.load());
    app_->Alert("直播伴侣", "从头开始播放", "", "");

    PlayCurrentItem();
}

// ============================================================================
// 脚本加载
// ============================================================================

void FlowEngine::LoadScriptTask(void* arg) {
    auto* lc = static_cast<FlowEngine*>(arg);

    bool success = lc->LoadScript(lc->pending_url_);

    if (success) {
        Application::GetInstance().Schedule([lc]() {
            lc->state_ = FlowState::kPlaying;
            // 设置 ManualStop 模式，确保 TTS 完成后回到 Idle
            Application::GetInstance().ForceListeningMode(kListeningModeManualStop);
            lc->PlayCurrentItem();

            char buf[64];
            snprintf(buf, sizeof(buf), "开始播放: %d 项%s",
                     lc->total_items_.load(), lc->loop_ ? " (循环)" : "");
            Application::GetInstance().Alert("直播伴侣", buf, "", "");
            ESP_LOGI(TAG, "Script started: %d items, loop=%d",
                     lc->total_items_.load(), lc->loop_);
        });
    }

    lc->load_task_handle_ = nullptr;
    vTaskDelete(NULL);
}

bool FlowEngine::LoadScript(const std::string& url) {
    auto& board = Board::GetInstance();
    auto network = board.GetNetwork();
    if (!network) {
        ESP_LOGE(TAG, "Network not available");
        return false;
    }

    for (int attempt = 0; attempt < kHttpRetryCount; ++attempt) {
        auto http = network->CreateHttp(0);
        http->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
        http->SetTimeout(kHttpTimeoutMs);

        if (!http->Open("GET", url)) {
            ESP_LOGW(TAG, "HTTP open failed, attempt %d/%d", attempt + 1, kHttpRetryCount);
            http->Close();
            if (attempt < kHttpRetryCount - 1) {
                vTaskDelay(pdMS_TO_TICKS(2000 * (1 << attempt)));
            }
            continue;
        }

        int status = http->GetStatusCode();
        if (status != 200) {
            ESP_LOGW(TAG, "HTTP status %d, attempt %d/%d", status, attempt + 1, kHttpRetryCount);
            http->Close();
            if (attempt < kHttpRetryCount - 1) {
                vTaskDelay(pdMS_TO_TICKS(2000 * (1 << attempt)));
            }
            continue;
        }

        std::string body = http->ReadAll();
        http->Close();

        if (body.empty() || body.size() > kMaxScriptSize) {
            ESP_LOGE(TAG, "Script size invalid: %d bytes", (int)body.size());
            Application::GetInstance().Schedule([]() {
                Application::GetInstance().Alert("直播伴侣", "脚本大小异常", "", "");
            });
            return false;
        }

        return ParseScript(body);
    }

    // 所有重试失败
    Application::GetInstance().Schedule([]() {
        Application::GetInstance().Alert("直播伴侣", "脚本加载失败", "", "");
    });
    return false;
}

bool FlowEngine::ParseScript(const std::string& json_str) {
    cJSON* root = cJSON_Parse(json_str.c_str());
    if (!root) {
        ESP_LOGE(TAG, "JSON parse failed");
        Application::GetInstance().Schedule([]() {
            Application::GetInstance().Alert("直播伴侣", "脚本格式错误", "", "");
        });
        return false;
    }

    // 全局配置
    auto* name_item = cJSON_GetObjectItem(root, "name");
    script_name_ = cJSON_IsString(name_item) ? name_item->valuestring : "未命名";

    auto* loop_item = cJSON_GetObjectItem(root, "loop");
    loop_ = cJSON_IsBool(loop_item) ? cJSON_IsTrue(loop_item) : true;

    auto* vol_item = cJSON_GetObjectItem(root, "default_volume");
    default_volume_ = cJSON_IsNumber(vol_item) ? vol_item->valueint : -1;

    auto* resume_item = cJSON_GetObjectItem(root, "resume_delay_s");
    resume_delay_ms_ = cJSON_IsNumber(resume_item) ? resume_item->valueint * 1000 : kDefaultResumeDelayMs;

    // 解析脚本项
    auto* items_array = cJSON_GetObjectItem(root, "items");
    if (!cJSON_IsArray(items_array) || cJSON_GetArraySize(items_array) == 0) {
        ESP_LOGE(TAG, "No items in script");
        cJSON_Delete(root);
        Application::GetInstance().Schedule([]() {
            Application::GetInstance().Alert("直播伴侣", "脚本为空", "", "");
        });
        return false;
    }

    std::lock_guard<std::mutex> lock(script_mutex_);
    items_.clear();

    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, items_array) {
        ScriptItem si;

        auto* type_item = cJSON_GetObjectItem(item, "type");
        if (cJSON_IsString(type_item)) {
            if (strcmp(type_item->valuestring, "ttai") == 0) si.type = ScriptItem::Type::kTtai;
            else if (strcmp(type_item->valuestring, "set") == 0) si.type = ScriptItem::Type::kSet;
        }

        auto* text_item = cJSON_GetObjectItem(item, "text");
        if (si.type == ScriptItem::Type::kSet && cJSON_IsObject(text_item)) {
            // set 类型: text 为 JSON 对象，直接序列化发送
            // 脚本写法: {"type":"set","text":{"tts_url":"DEFAULT{...}"}}
            char* str = cJSON_PrintUnformatted(text_item);
            si.text = str;
            cJSON_free(str);
        } else if (cJSON_IsString(text_item) && text_item->valuestring[0] != '\0') {
            si.text = text_item->valuestring;
        } else {
            continue;
        }

        auto* delay_item = cJSON_GetObjectItem(item, "delay_ms");
        si.delay_ms = cJSON_IsNumber(delay_item) ? delay_item->valueint : 0;

        auto* vol = cJSON_GetObjectItem(item, "volume");
        si.volume = cJSON_IsNumber(vol) ? vol->valueint : -1;

        items_.push_back(std::move(si));
    }

    total_items_ = items_.size();
    current_index_ = 0;

    cJSON_Delete(root);
    ESP_LOGI(TAG, "Parsed script '%s': %d items, loop=%d, vol=%d",
             script_name_.c_str(), (int)items_.size(), loop_, default_volume_);
    return !items_.empty();
}

// ============================================================================
// 播放控制
// ============================================================================

void FlowEngine::PlayCurrentItem() {
    if (state_ != FlowState::kPlaying) return;

    int idx = current_index_.load();

    std::lock_guard<std::mutex> lock(script_mutex_);

    if (idx >= (int)items_.size()) {
        if (loop_) {
            current_index_ = 0;
            idx = 0;
            int lc = loop_count_.fetch_add(1) + 1;
            ESP_LOGI(TAG, "---------- 第 %d 轮循环开始 ----------", lc);
        } else {
            state_ = FlowState::kIdle;
            app_->ForceListeningMode(kListeningModeAutoStop);
            app_->SetDeviceState(kDeviceStateIdle);
            ESP_LOGI(TAG, "========== 脚本播放完毕 (共 %d 轮) ==========",
                     loop_count_.load());
            app_->Alert("直播伴侣", "脚本播放完毕", "", "");
            return;
        }
    }

    const auto& item = items_[idx];
    const char* type_str = item.type == ScriptItem::Type::kTtai ? "TTAI"
                         : item.type == ScriptItem::Type::kSet  ? "SET" : "TTS";
    ESP_LOGI(TAG, "[第%d轮 %d/%d] %s: %.60s",
             loop_count_.load(), idx + 1, (int)items_.size(),
             type_str, item.text.c_str());

    // 设置音量
    int vol = (item.volume >= 0) ? item.volume : default_volume_;
    if (vol >= 0) {
        auto codec = Board::GetInstance().GetAudioCodec();
        if (codec) codec->SetOutputVolume(vol);
    }

    // 执行：set 发 DEVICE_INFO 命令并立即下一项，tts/ttai 等待播放完成
    if (item.type == ScriptItem::Type::kSet) {
        bool ok = app_->SendProtocolText("[SET]:[DEVICE_INFO]:" + item.text);
        ESP_LOGI(TAG, "SET 发送%s", ok ? "成功" : "失败");
        // SET 不等待服务器响应，直接推进到下一项
        current_index_ = idx + 1;
        PlayCurrentItem();
    } else if (item.type == ScriptItem::Type::kTts) {
        ESP_LOGI(TAG, "发送 TTS 文本 (%d bytes)", (int)item.text.size());
        app_->SendTextToTts(item.text);
    } else {
        ESP_LOGI(TAG, "发送 TTAI 文本 (%d bytes)", (int)item.text.size());
        app_->SendTextToAI(item.text);
    }
}

void FlowEngine::ScheduleNext() {
    int idx = current_index_.load();
    int delay_ms = 0;

    {
        std::lock_guard<std::mutex> lock(script_mutex_);
        if (idx >= 0 && idx < (int)items_.size()) {
            delay_ms = items_[idx].delay_ms;
        }
    }

    // 推进到下一项
    current_index_ = idx + 1;

    if (delay_ms > 0) {
        state_ = FlowState::kDelay;
        esp_timer_start_once(delay_timer_, (uint64_t)delay_ms * 1000);
        ESP_LOGI(TAG, "等待 %d ms 后播放下一项", delay_ms);
    } else {
        app_->Schedule([this]() {
            FlowState s = state_.load();
            if (s == FlowState::kPlaying || s == FlowState::kDelay) {
                state_ = FlowState::kPlaying;
                PlayCurrentItem();
            } else {
                ESP_LOGW(TAG, "ScheduleNext 跳过: state=%d", (int)s);
            }
        });
    }
}

void FlowEngine::ScheduleResumeAfterDelay(int delay_ms) {
    esp_timer_start_once(delay_timer_, (uint64_t)delay_ms * 1000);
}

// ============================================================================
// 回调
// ============================================================================

void FlowEngine::OnDeviceStateChanged(DeviceState prev, DeviceState curr) {
    if (!IsRunning()) return;

    // 暂停状态下有新的 TTS 开始播放 → 用户仍在互动中，取消恢复定时器
    if (curr == kDeviceStateSpeaking && state_ == FlowState::kSuspended) {
        esp_timer_stop(delay_timer_);
        ESP_LOGI(TAG, "--- 用户互动中, 取消恢复定时器");
        return;
    }

    // Suspended 状态下回到 Idle（用户交互结束），启动恢复定时器
    // 覆盖两种场景：Speaking→Idle（AI回复完）和 Listening→Idle（用户没说话超时）
    if (curr == kDeviceStateIdle && state_.load() == FlowState::kSuspended) {
        ESP_LOGI(TAG, "<<< 用户互动结束, %d ms 后恢复脚本", resume_delay_ms_);
        ScheduleResumeAfterDelay(resume_delay_ms_);
        return;
    }

    // 只关心 TTS 播放完成（Speaking → Idle 或 Speaking → Listening）
    if (prev != kDeviceStateSpeaking) return;
    if (curr != kDeviceStateIdle && curr != kDeviceStateListening) return;

    // 如果到了 Listening（AutoStop 模式），先推回 Idle
    if (curr == kDeviceStateListening) {
        app_->Schedule([this]() {
            if (IsRunning()) {
                app_->SetDeviceState(kDeviceStateIdle);
            }
        });
        return;  // 等 Listening→Idle 的回调再处理
    }

    // Speaking → Idle：TTS 播放完成
    FlowState current_state = state_.load();

    if (current_state == FlowState::kPlaying) {
        if (recap_pending_.exchange(false)) {
            // 回顾 TTAI 播完，继续播放当前脚本项
            ESP_LOGI(TAG, "<<< 回顾完毕, 继续脚本第 %d 项", current_index_.load() + 1);
            app_->Schedule([this]() { PlayCurrentItem(); });
        } else {
            // 当前脚本项播完，进入下一项
            ScheduleNext();
        }
    }
}

void FlowEngine::DelayTimerCallback(void* arg) {
    auto* lc = static_cast<FlowEngine*>(arg);

    Application::GetInstance().Schedule([lc]() {
        FlowState current = lc->state_.load();  // 在 main_event_loop 中实时读取
        if (current == FlowState::kDelay) {
            // 延时结束，播放下一项
            lc->state_ = FlowState::kPlaying;
            lc->PlayCurrentItem();
        } else if (current == FlowState::kSuspended) {
            // 恢复延时结束，恢复播放
            lc->Resume();
        }
    });
}
