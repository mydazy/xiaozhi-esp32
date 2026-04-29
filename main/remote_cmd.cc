#include "remote_cmd.h"
#include "application.h"
#include "flow_engine.h"
#include "audio/music_player.h"
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
    else if (strcmp(type, "download") == 0) OnDownload(msg);
    else if (strcmp(type, "vad_config") == 0) OnVadConfig(msg);
    else if (strcmp(type, "flow") == 0) OnFlow(msg);
    else if (strcmp(type, "stt_url") == 0) OnSttUrl(msg);
    else if (strcmp(type, "music_play") == 0) OnMusicPlay(msg);
    else if (strcmp(type, "music_stop") == 0) OnMusicStop();
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
    auto input = cJSON_GetObjectItem(msg, "input");
    bool has_input = cJSON_IsNumber(input);
    float in_val = has_input ? (float)input->valuedouble : 0;
    app_->Schedule([this, in_val, has_input]() {
        auto codec = Board::GetInstance().GetAudioCodec();
        if (!codec) return;
        std::string info;
        char buf[32];
        if (has_input) {
            codec->SetInputGain(in_val);
            snprintf(buf, sizeof(buf), "MIC: %.1fdB", in_val);
            info += buf;
        }
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
        // TODO: ProcessCustomContent 需要从189移植 Ota 扩展
        ESP_LOGW(TAG, "File sync not yet implemented in V2");
        if (!emotion.empty()) {
            Board::GetInstance().GetDisplay()->SetEmotion(emotion.c_str());
        }
        cJSON_Delete(files_copy);
    });
}

void RemoteCmd::OnVadConfig(const cJSON* msg) {
    auto speech = cJSON_GetObjectItem(msg, "min_speech");
    auto noise = cJSON_GetObjectItem(msg, "min_noise");
    auto delay = cJSON_GetObjectItem(msg, "delay");
    auto agc = cJSON_GetObjectItem(msg, "agc");

    bool has_speech = cJSON_IsNumber(speech);
    bool has_noise = cJSON_IsNumber(noise);
    bool has_delay = cJSON_IsNumber(delay);
    bool has_agc = cJSON_IsNumber(agc);

    int v_speech = has_speech ? speech->valueint : 0;
    int v_noise = has_noise ? noise->valueint : 0;
    int v_delay = has_delay ? delay->valueint : 0;
    int v_agc = has_agc ? agc->valueint : 0;

    ESP_LOGI(TAG, "vad: speech=%d noise=%d delay=%d agc=%d", v_speech, v_noise, v_delay, v_agc);

    app_->Schedule([this, has_speech, has_noise, has_delay, has_agc,
                    v_speech, v_noise, v_delay, v_agc]() {
        Settings s("audio_afe", true);
        std::string info;
        char buf[48];
        bool changed = false;

        if (has_speech) { s.SetInt("vad_min_speech_ms", v_speech); snprintf(buf, sizeof(buf), "语音: %dms\n", v_speech); info += buf; changed = true; }
        if (has_noise) { s.SetInt("vad_min_noise_ms", v_noise); snprintf(buf, sizeof(buf), "静音: %dms\n", v_noise); info += buf; changed = true; }
        if (has_delay) { s.SetInt("vad_delay_ms", v_delay); snprintf(buf, sizeof(buf), "延迟: %dms\n", v_delay); info += buf; changed = true; }
        if (has_agc) { s.SetInt("agc_init", v_agc); snprintf(buf, sizeof(buf), "AGC: %s\n", v_agc ? "开" : "关"); info += buf; changed = true; }

        if (changed) {
            info += "(重启生效)";
        } else {
            Settings r("audio_afe", false);
            snprintf(buf, sizeof(buf), "语音:%d 静音:%d 延迟:%d AGC:%s",
                     (int)r.GetInt("vad_min_speech_ms", 128), (int)r.GetInt("vad_min_noise_ms", 500),
                     (int)r.GetInt("vad_delay_ms", 300), r.GetInt("agc_init", 0) ? "开" : "关");
            info = buf;
        }
        app_->Alert("VAD配置", info.c_str(), "", changed ? Lang::Sounds::OGG_VIBRATION : "");
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
        app_->Alert("解绑设备", "进入休眠", "", Lang::Sounds::OGG_DISCONNECT);
        vTaskDelay(pdMS_TO_TICKS(3000));
        // TODO: V2 使用 Board 特定的深睡方法
        ESP_LOGW(TAG, "Deep sleep not yet implemented in V2 RemoteCmd");
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
    BaseType_t ret = xTaskCreatePinnedToCoreWithCaps(
        task_func, "stt_post", 4096, args,
        1, nullptr, 0, MALLOC_CAP_SPIRAM);
    if (ret != pdPASS) {
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
