#include "remote_cmd.h"
#include "application.h"
#include "flow_engine.h"
#include "edu_scene_pool.h"
#include "audio/music_player.h"
#include "audio/codecs/box_audio_codec.h"
#include "board.h"
#include "display.h"
#include "display/ui_display.h"
#include "device_state.h"
#include "ota.h"
#include "settings.h"
#include "system_info.h"
#include "assets/lang_config.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_heap_caps.h>
#include <cstring>
#include <tuple>

#define TAG "RemoteCmd"

RemoteCmd::RemoteCmd(Application* app) : app_(app) {
    // 启动时从 NVS 加载 stt_url
    Settings s("remote_cmd", false);
    stt_url_ = s.GetString("stt_url");
    if (!stt_url_.empty()) {
        ESP_LOGI(TAG, "STT URL loaded: %s", stt_url_.c_str());
    }
}

bool RemoteCmd::Handle(const cJSON* payload) {
    if (!cJSON_IsObject(payload)) return false;

    auto message = cJSON_GetObjectItem(payload, "message");
    cJSON* msg = nullptr;
    bool need_delete = false;

    if (cJSON_IsString(message)) {
        msg = cJSON_Parse(message->valuestring);
        need_delete = true;
    } else if (cJSON_IsObject(message)) {
        msg = message;
    } else if (cJSON_IsString(cJSON_GetObjectItem(payload, "type"))) {
        // 顶层命令格式：{"type":"music_play","url":"...","title":"..."} 等
        // 服务器直接下发命令，不走 {"type":"custom","payload":{"message":...}} 双层包装
        msg = const_cast<cJSON*>(payload);
    }

    if (!msg) return false;

    auto type_item = cJSON_GetObjectItem(msg, "type");
    const char* type = cJSON_IsString(type_item) ? type_item->valuestring : "";
    bool handled = true;

    if (strcmp(type, "reboot") == 0) OnReboot();
    else if (strcmp(type, "ota") == 0) OnOta();
    else if (strcmp(type, "sleep") == 0) OnSleep(msg);
    else if (strcmp(type, "reconnect") == 0) OnReconnect();
    else if (strcmp(type, "wakeup") == 0) OnWakeup(msg);
    else if (strcmp(type, "tts") == 0) OnTts(msg);
    else if (strcmp(type, "ttai") == 0) OnTtai(msg);
    else if (strcmp(type, "volume") == 0) OnVolume(msg);
    else if (strcmp(type, "gain") == 0) OnGain(msg);
    else if (strcmp(type, "mic_calibrate") == 0) OnMicCalibrate();
    else if (strcmp(type, "download") == 0) OnDownload(msg);
    else if (strcmp(type, "flow") == 0) OnFlow(msg);
    else if (strcmp(type, "stt_url") == 0) OnSttUrl(msg);
    else if (strcmp(type, "music_play") == 0) OnMusicPlay(msg);
    else if (strcmp(type, "music_stop") == 0) OnMusicStop();
    else if (strcmp(type, "music_pause") == 0) OnMusicPause();
    else if (strcmp(type, "music_resume") == 0) OnMusicResume();
    else if (strcmp(type, "edu_pool") == 0) OnEduPool(msg);
    else if (strcmp(type, "update_prompt") == 0) OnUpdatePrompt(msg);
    else {
        ESP_LOGW(TAG, "未知命令: %s", type);
        handled = false;
    }

    if (need_delete) cJSON_Delete(msg);
    return handled;
}

void RemoteCmd::OnReboot() {
    ESP_LOGI(TAG, "reboot");
    app_->Schedule([this]() {
        app_->Alert("重启设备", "正在重启", "", Lang::Sounds::OGG_VIBRATION);
        vTaskDelay(pdMS_TO_TICKS(2000));
        app_->Reboot();
    });
}

void RemoteCmd::OnOta() {
    ESP_LOGI(TAG, "OTA");
    app_->Schedule([this]() {
        // 退出当前对话状态
        if (app_->GetDeviceState() == kDeviceStateSpeaking) {
            app_->AbortSpeaking(kAbortReasonNone);
        }
        if (app_->GetDeviceState() == kDeviceStateListening) {
            app_->SetDeviceState(kDeviceStateIdle);
        }
        app_->CloseAudioChannel();

        // 执行 OTA 检查（包括激活状态）
        app_->Alert("检查更新", "检查激活状态", "", Lang::Sounds::OGG_VIBRATION);
        vTaskDelay(pdMS_TO_TICKS(1500));
        Ota ota;
        ota.CheckVersion();
    });
}

void RemoteCmd::OnReconnect() {
    ESP_LOGI(TAG, "reconnect");
    app_->Schedule([this]() {
        if (app_->GetDeviceState() == kDeviceStateSpeaking) {
            app_->AbortSpeaking(kAbortReasonNone);
        }
        app_->CloseAudioChannel();
        app_->ScheduleDelayedWake("继续", 1000000);  // 延迟1秒继续对话
    });
}

void RemoteCmd::OnWakeup(const cJSON* msg) {
    auto text = cJSON_GetObjectItem(msg, "text");
    if (cJSON_IsString(text)) {
        ESP_LOGI(TAG, "wakeup: %s", text->valuestring);
        app_->WakeWordInvoke(text->valuestring);
    }
}

void RemoteCmd::OnTts(const cJSON* msg) {
    auto text = cJSON_GetObjectItem(msg, "text");
    if (!cJSON_IsString(text) || text->valuestring == nullptr || text->valuestring[0] == '\0') {
        ESP_LOGW(TAG, "tts 命令缺少 text");
        return;
    }

    std::string tts_text = text->valuestring;
    ESP_LOGI(TAG, "tts: %s", tts_text.c_str());
    app_->Schedule([this, tts_text = std::move(tts_text)]() {
        // 暂停直播伴侣（如果正在运行）
        if (auto* lc = app_->GetFlowEngine(); lc && lc->IsRunning()) {
            lc->Suspend();
        }
        app_->SendTextToTts(tts_text);
    });
}

void RemoteCmd::OnTtai(const cJSON* msg) {
    auto text = cJSON_GetObjectItem(msg, "text");
    if (!cJSON_IsString(text) || text->valuestring == nullptr || text->valuestring[0] == '\0') {
        ESP_LOGW(TAG, "ttai 命令缺少 text");
        return;
    }

    std::string ai_text = text->valuestring;
    ESP_LOGI(TAG, "ttai: %s", ai_text.c_str());
    app_->Schedule([this, ai_text = std::move(ai_text)]() {
        // 暂停直播伴侣（如果正在运行）
        if (auto* lc = app_->GetFlowEngine(); lc && lc->IsRunning()) {
            lc->Suspend();
        }
        app_->SendTextToAI(ai_text);
    });
}

void RemoteCmd::OnVolume(const cJSON* msg) {
    auto value = cJSON_GetObjectItem(msg, "value");
    if (!cJSON_IsNumber(value)) return;

    int volume = value->valueint;
    ESP_LOGI(TAG, "volume: %d", volume);

    app_->Schedule([this, volume]() {
        auto codec = Board::GetInstance().GetAudioCodec();
        if (codec) {
            codec->SetOutputVolume(volume);
            app_->Alert("远程控制", ("音量: " + std::to_string(volume)).c_str(), "", "");
        }
    });
}

void RemoteCmd::OnGain(const cJSON* msg) {
    // 协议示例：
    //   {"type":"gain","input":24}            ← 仅调 MIC
    //   {"type":"gain","aec":6}               ← 仅调 AEC 后软件增益
    //   {"type":"gain","input":24,"aec":6}    ← 同时调 input + aec
    auto input = cJSON_GetObjectItem(msg, "input");
    auto aec   = cJSON_GetObjectItem(msg, "aec");
    bool has_input = cJSON_IsNumber(input);
    bool has_aec   = cJSON_IsNumber(aec);
    float in_val  = has_input ? (float)input->valuedouble : 0.0f;
    float aec_val = has_aec   ? (float)aec->valuedouble   : 0.0f;
    ESP_LOGI(TAG, "gain: input=%s%.1f aec=%s%.1f",
             has_input ? "" : "(skip)", in_val,
             has_aec   ? "" : "(skip)", aec_val);
    app_->Schedule([this, in_val, aec_val, has_input, has_aec]() {
        auto codec = Board::GetInstance().GetAudioCodec();
        if (!codec) return;
        if (has_input) codec->SetInputGain(in_val);
        if (has_aec)   codec->SetAecGain(aec_val);
    });
}

// 远程触发 MIC 校准（调试 / 售后重校 / 强制覆盖）
// 协议: {"type":"custom","payload":{"type":"mic_calibrate"}}
// 直接调 CalibrateMicOnce，不停 audio_service（用户负责确保设备空闲）
void RemoteCmd::OnMicCalibrate() {
    ESP_LOGI(TAG, "mic_calibrate: 触发");
    app_->Schedule([]() {
        auto* box = dynamic_cast<BoxAudioCodec*>(Board::GetInstance().GetAudioCodec());
        if (box) box->CalibrateMicOnce();
    });
}

void RemoteCmd::OnDownload(const cJSON* msg) {
    auto files = cJSON_GetObjectItem(msg, "files");
    auto emoji = cJSON_GetObjectItem(msg, "emoji");
    if (!cJSON_IsArray(files)) return;

    ESP_LOGI(TAG, "download");
    cJSON* files_copy = cJSON_Duplicate(files, true);
    std::string emotion = cJSON_IsString(emoji) ? emoji->valuestring : "";

    app_->Schedule([this, files_copy, emotion]() {
        app_->Alert("同步文件", "下载中...", "", "");
        ESP_LOGW(TAG, "File sync not yet implemented in V2");
        if (!emotion.empty()) {
            Board::GetInstance().GetDisplay()->SetEmotion(emotion.c_str());
        }
        cJSON_Delete(files_copy);
    });
}

void RemoteCmd::OnFlow(const cJSON* msg) {
    auto action_item = cJSON_GetObjectItem(msg, "action");
    const char* action = cJSON_IsString(action_item) ? action_item->valuestring : "";
    auto* lc = app_->GetFlowEngine();
    if (!lc) {
        ESP_LOGW(TAG, "FlowEngine not initialized");
        return;
    }

    if (strcmp(action, "start") == 0) {
        // 方式1: 通过 URL 拉取脚本
        auto url_item = cJSON_GetObjectItem(msg, "url");
        if (cJSON_IsString(url_item) && url_item->valuestring[0] != '\0') {
            std::string url = url_item->valuestring;
            ESP_LOGI(TAG, "flow start: %s", url.c_str());
            app_->Schedule([lc, url = std::move(url)]() {
                lc->Start(url);
            });
            return;
        }
        // 方式2: 脚本直接通过 WS 推送（script 字段为 JSON 对象）
        auto script_item = cJSON_GetObjectItem(msg, "script");
        if (cJSON_IsObject(script_item)) {
            char* script_str = cJSON_PrintUnformatted(script_item);
            if (script_str) {
                std::string script_json = script_str;
                cJSON_free(script_str);
                ESP_LOGI(TAG, "flow start: inline script (%d bytes)",
                         (int)script_json.size());
                app_->Schedule([lc, script_json = std::move(script_json)]() {
                    lc->StartWithScript(script_json);
                });
                return;
            }
        }
        ESP_LOGW(TAG, "flow start: missing url or script");
    } else if (strcmp(action, "stop") == 0) {
        ESP_LOGI(TAG, "flow stop");
        app_->Schedule([lc]() { lc->Stop(); });
    } else if (strcmp(action, "restart") == 0) {
        ESP_LOGI(TAG, "flow restart");
        app_->Schedule([lc]() { lc->Restart(); });
    } else if (strcmp(action, "status") == 0) {
        app_->Schedule([this, lc]() {
            const char* state_names[] = {"空闲", "播放中", "等待中", "暂停中"};
            int state_idx = static_cast<int>(lc->GetState());
            char buf[80];
            snprintf(buf, sizeof(buf), "状态: %s | 进度: %d/%d",
                     state_names[state_idx],
                     lc->GetCurrentIndex() + 1, lc->GetTotalItems());
            app_->Alert("直播伴侣", buf, "", "");
            ESP_LOGI(TAG, "flow: %s", buf);
        });
    } else {
        ESP_LOGW(TAG, "flow: unknown action: %s", action);
    }
}

void RemoteCmd::OnSleep(const cJSON* msg) {
    // 获取陀螺仪唤醒参数，默认启用
    auto gyro_item = cJSON_GetObjectItem(msg, "gyro");
    bool enable_gyro_wakeup = cJSON_IsBool(gyro_item) ? cJSON_IsTrue(gyro_item) : true;

    ESP_LOGI(TAG, "sleep: gyro=%d", enable_gyro_wakeup);

    app_->Schedule([this, enable_gyro_wakeup]() {
        // 如果正在播放，先停止
        if (app_->GetDeviceState() == kDeviceStateSpeaking) {
            app_->AbortSpeaking(kAbortReasonNone);
        }
        app_->Alert("解绑设备", "进入休眠", "", Lang::Sounds::OGG_UNBUNDLE);
        vTaskDelay(pdMS_TO_TICKS(3000));

        ESP_LOGI(TAG, "RemoteCmd 触发 EnterDeepSleep（gyro=%d）", enable_gyro_wakeup);
        Board::GetInstance().EnterDeepSleep(enable_gyro_wakeup);

        // 兜底：基类默认空实现（未 override 的板）才会走到这里，复位回安全态
        ESP_LOGW(TAG, "EnterDeepSleep 未实现，降级 esp_restart");
        esp_restart();
    });
}

void RemoteCmd::OnSttUrl(const cJSON* msg) {
    auto url_item = cJSON_GetObjectItem(msg, "url");
    if (!cJSON_IsString(url_item)) {
        ESP_LOGW(TAG, "stt_url: missing url field");
        return;
    }

    std::string url = url_item->valuestring;
    stt_url_ = url;

    // 持久化到 NVS
    Settings s("remote_cmd", true);
    s.SetString("stt_url", url);

    if (url.empty()) {
        ESP_LOGI(TAG, "STT URL cleared");
        app_->Schedule([this]() {
            app_->Alert("STT回调", "已清除", "", "");
        });
    } else {
        ESP_LOGI(TAG, "STT URL set: %s", url.c_str());
        app_->Schedule([this, url]() {
            app_->Alert("STT回调", url.c_str(), "", Lang::Sounds::OGG_VIBRATION);
        });
    }
}

void RemoteCmd::PostSttText(const std::string& text) {
    if (stt_url_.empty() || text.empty()) return;

    // 防止并发 POST（上一个还没完成就跳过）
    bool expected = false;
    if (!stt_posting_.compare_exchange_strong(expected, true)) {
        ESP_LOGW(TAG, "STT POST busy, skip: %.30s", text.c_str());
        return;
    }

    std::string url = stt_url_;
    std::string stt_text = text;

    // 在 PSRAM 后台任务中 POST，不阻塞主线程
    auto task_func = [](void* param) {
        auto* args = static_cast<std::tuple<RemoteCmd*, std::string, std::string>*>(param);
        auto* self = std::get<0>(*args);
        auto& url = std::get<1>(*args);
        auto& text = std::get<2>(*args);

        auto& board = Board::GetInstance();
        std::string mac = SystemInfo::GetMacAddress();
        std::string client_id = board.GetUuid();
        std::string user_agent = SystemInfo::GetUserAgent();

        // 构造 JSON body
        cJSON* body = cJSON_CreateObject();
        cJSON_AddStringToObject(body, "text", text.c_str());
        cJSON_AddStringToObject(body, "device_id", mac.c_str());
        cJSON_AddStringToObject(body, "client_id", client_id.c_str());
        cJSON_AddNumberToObject(body, "timestamp", (double)(esp_timer_get_time() / 1000));
        char* json_str = cJSON_PrintUnformatted(body);
        cJSON_Delete(body);

        if (json_str) {
            auto network = board.GetNetwork();
            auto http = network->CreateHttp(0);
            http->SetHeader("Content-Type", "application/json");
            http->SetHeader("Device-Id", mac);
            http->SetHeader("Client-Id", client_id);
            http->SetHeader("User-Agent", user_agent);
            http->SetTimeout(5000);
            http->SetContent(std::string(json_str));
            cJSON_free(json_str);

            if (http->Open("POST", url)) {
                int status = http->GetStatusCode();
                if (status >= 200 && status < 300) {
                    ESP_LOGI(TAG, "STT POST ok (%d): %.30s", status, text.c_str());
                } else {
                    ESP_LOGW(TAG, "STT POST failed (%d): %.30s", status, text.c_str());
                }
                http->Close();
            } else {
                ESP_LOGW(TAG, "STT POST connect failed: %s", url.c_str());
            }
        }

        self->stt_posting_.store(false);
        delete args;
        vTaskDelete(nullptr);
    };

    auto* args = new std::tuple<RemoteCmd*, std::string, std::string>(this, std::move(url), std::move(stt_text));
    // 🔴 修红线（2026-04-28 P0）：从 PSRAM Core 0 改为 INT Core 1。
    // 根因：ESP32-S3 cache 与 PSRAM 共享 SPI · NVS/OTA flash op 触发 spi_flash_op_lock()
    //       会同时禁用两核 cache+PSRAM · PSRAM 栈任务被调度即崩（SP=0x60100000）。
    // 改动：① MALLOC_CAP_SPIRAM → MALLOC_CAP_INTERNAL ② Core 0 → Core 1（减 Core 0 负担）
    // 代价：4 KB 内部 RAM 临时占用（HTTP POST 自删后释放）。
    //
    // P0c 修：xTaskCreatePinnedToCoreWithCaps（动态 INT alloc）→ xTaskCreateStaticPinnedToCore
    // 4 KB 静态 BSS 栈 · 减堆碎片 · stt_posting_ atomic_flag 已防重入（buffer 复用安全）
    constexpr uint32_t kSttPostStackSize = 4096;
    static StackType_t s_stt_post_stack[kSttPostStackSize / sizeof(StackType_t)];
    static StaticTask_t s_stt_post_tcb;
    TaskHandle_t handle = xTaskCreateStaticPinnedToCore(
        task_func, "stt_post", kSttPostStackSize / sizeof(StackType_t), args,
        1, s_stt_post_stack, &s_stt_post_tcb, 1);
    if (handle == nullptr) {
        ESP_LOGE(TAG, "Failed to create stt_post task");
        stt_posting_.store(false);
        delete args;
    }
}

void RemoteCmd::OnMusicPlay(const cJSON* msg) {
    auto url_item = cJSON_GetObjectItem(msg, "url");
    auto title_item = cJSON_GetObjectItem(msg, "title");

    if (!cJSON_IsString(url_item) || url_item->valuestring == nullptr || url_item->valuestring[0] == '\0') {
        ESP_LOGW(TAG, "music_play 缺少 url 字段");
        app_->Alert("播放失败", "URL 为空", "", "");
        return;
    }

    std::string url = url_item->valuestring;
    std::string title = cJSON_IsString(title_item) && title_item->valuestring ? title_item->valuestring : "";
    ESP_LOGI(TAG, "music_play: %s (%s)", title.c_str(), url.c_str());

    app_->Schedule([url = std::move(url), title = std::move(title)]() {
        auto& app = Application::GetInstance();

        // 关键：切回 idle，关闭 AFE voice communication + stop listening
        // （否则 listening 模式下 AFE 持续采集 → Core 1 CPU 争抢 → AFE 饥饿刷屏 + modem 卡死）
        auto state = app.GetDeviceState();
        if (state == kDeviceStateSpeaking) {
            app.AbortSpeaking(kAbortReasonNone);
        }
        if (state == kDeviceStateListening || state == kDeviceStateSpeaking ||
            state == kDeviceStateConnecting) {
            app.CloseAudioChannel();  // 关协议通道 → 触发回 idle（会停 AFE voice processing）
        }
        app.GetAudioService().ResetDecoder();
        // 暂停直播伴侣
        if (auto* lc = app.GetFlowEngine(); lc && lc->IsRunning()) {
            lc->Suspend();
        }

        std::string err;
        if (!MusicPlayer::GetInstance().Play(url, title, &err)) {
            ESP_LOGW(TAG, "music_play 启动失败: %s", err.c_str());
            app.Alert("播放失败", err.c_str(), "", "");
        } else if (auto* ui = dynamic_cast<UiDisplay*>(Board::GetInstance().GetDisplay())) {
            // 切到极简播放器页（曲名 + Play/Pause + 进度条），自动 200ms 刷新进度
            ui->SwitchToPlayerMode(title.empty() ? "正在播放" : title.c_str());
            ui->OnPlayerPauseToggle([] {
                auto& mp = MusicPlayer::GetInstance();
                if (mp.IsPaused()) mp.Resume(); else mp.Pause();
            });
        } else if (!title.empty()) {
            Board::GetInstance().GetDisplay()->ShowNotification(title.c_str(), 3000);
        }
    });
}

void RemoteCmd::OnMusicStop() {
    ESP_LOGI(TAG, "music_stop");
    app_->Schedule([]() {
        bool was_playing = MusicPlayer::GetInstance().IsPlaying();
        MusicPlayer::GetInstance().Stop();
        if (auto* ui = dynamic_cast<UiDisplay*>(Board::GetInstance().GetDisplay())) {
            ui->SwitchOutPlayerMode();   // 退出播放页 → 回时钟
        }
        if (was_playing) {
            Board::GetInstance().GetDisplay()->ShowNotification("已停止播放", 1500);
        }
    });
}

// 远程暂停 / 恢复（与 BRTC SDK [E]:[CMD]:[REMOTE_PLAYER]:[PAUSE/RESUME] 概念一致，
// 但作用对象是设备本地 MusicPlayer 流，而非云端 RTC 远端音乐流）
void RemoteCmd::OnMusicPause() {
    ESP_LOGI(TAG, "music_pause");
    app_->Schedule([]() {
        auto& mp = MusicPlayer::GetInstance();
        if (mp.IsPlaying() && !mp.IsPaused()) mp.Pause();
    });
}

void RemoteCmd::OnMusicResume() {
    ESP_LOGI(TAG, "music_resume");
    app_->Schedule([]() {
        auto& mp = MusicPlayer::GetInstance();
        if (mp.IsPaused()) mp.Resume();
    });
}

// 远程下推 system prompt（教育卡冷启切档场景）
//   {"type":"update_prompt","model_type":2,"prompt":"<识字老师 prompt>"}
//   model_type 默认 2（视觉/教学槽），prompt 为空 = 恢复默认
void RemoteCmd::OnUpdatePrompt(const cJSON* msg) {
    auto* mt_item = cJSON_GetObjectItem(msg, "model_type");
    auto* prompt_item = cJSON_GetObjectItem(msg, "prompt");
    int model_type = cJSON_IsNumber(mt_item) ? mt_item->valueint : 2;
    std::string prompt = (cJSON_IsString(prompt_item) && prompt_item->valuestring)
                            ? prompt_item->valuestring : "";
    ESP_LOGI(TAG, "update_prompt: type=%d len=%u", model_type, (unsigned)prompt.size());
    app_->Schedule([this, model_type, prompt = std::move(prompt)]() {
        bool ok = app_->UpdateSystemPrompt(model_type, prompt);
        if (!ok) {
            ESP_LOGW(TAG, "UpdateSystemPrompt failed (channel/protocol)");
        }
    });
}

// 远程更新摇一摇启蒙场景池（10 个完整替换 · NVS 持久化）
// 协议（极简单字符串 '|' 分隔，不用 JSON 数组）：
//   {"type":"edu_pool","names":"故事盒子|百科精灵|猜一猜|接龙啦|唱歌啦|念古诗|说英语|比比快|转转脑|对暗号"}
// 必须正好 10 段，每段 ≤24 字节（≈ 8 汉字）
void RemoteCmd::OnEduPool(const cJSON* msg) {
    auto* names = cJSON_GetObjectItem(msg, "names");
    if (!cJSON_IsString(names) || !names->valuestring[0]) {
        ESP_LOGW(TAG, "edu_pool: missing 'names' string");
        return;
    }
    ESP_LOGI(TAG, "edu_pool: \"%s\"", names->valuestring);
    app_->Schedule([s = std::string(names->valuestring)]() {
        bool ok = EduScenePool::GetInstance().UpdateFromString(s.c_str());
        Board::GetInstance().GetDisplay()->ShowNotification(
            ok ? "启蒙场景已更新" : "启蒙场景更新失败", 2000);
    });
}
