#include "remote_cmd.h"

#include "application.h"
#include "audio/music_player.h"
#include "board.h"
#include "device_state.h"
#include "display/display.h"
#include "ota.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstring>
#include <string>
#include <utility>

#define TAG "RemoteCmd"

bool RemoteCmd::Handle(const cJSON* payload) {
    if (!cJSON_IsObject(payload)) return false;

    auto message = cJSON_GetObjectItem(payload, "message");
    cJSON* msg = nullptr;
    bool need_delete = false;
    if (cJSON_IsString(message)) {
        msg = cJSON_Parse(message->valuestring);
        need_delete = true;
    } else if (cJSON_IsObject(message)) {
        msg = const_cast<cJSON*>(message);
    }
    if (!msg) return false;

    auto type_item = cJSON_GetObjectItem(msg, "type");
    const char* type = cJSON_IsString(type_item) ? type_item->valuestring : "";
    bool handled = true;

    if (strcmp(type, "music_play") == 0) {
        OnMusicPlay(msg);
    } else if (strcmp(type, "music_stop") == 0) {
        OnMusicStop();
    } else if (strcmp(type, "wakeup") == 0) {
        OnWakeup(msg);
    } else if (strcmp(type, "send_text") == 0) {
        OnSendText(msg);
    } else if (strcmp(type, "reboot") == 0) {
        OnReboot();
    } else if (strcmp(type, "ota") == 0) {
        OnOta();
    } else {
        handled = false;
    }

    if (need_delete) cJSON_Delete(msg);
    return handled;
}

void RemoteCmd::OnMusicPlay(const cJSON* msg) {
    auto url_item = cJSON_GetObjectItem(msg, "url");
    auto title_item = cJSON_GetObjectItem(msg, "title");
    if (!cJSON_IsString(url_item) || url_item->valuestring == nullptr ||
        url_item->valuestring[0] == '\0') {
        ESP_LOGW(TAG, "music_play: missing url");
        return;
    }
    std::string url = url_item->valuestring;
    std::string title = (cJSON_IsString(title_item) && title_item->valuestring)
                            ? title_item->valuestring : "";
    ESP_LOGI(TAG, "music_play: %s (%s)", title.c_str(), url.c_str());

    app_->Schedule([url = std::move(url), title = std::move(title)]() {
        auto& app = Application::GetInstance();

        // Speaking 状态下先打断 TTS，给 MP3 解码让出 codec mutex。
        // 不强切 listening：CloseAudioChannel 在 Application 是私有的，强行
        // 公开会污染 application.h 的接口；而 listening 期间播 MP3 仅有轻
        // 微 Core1 抢占（可观察到偶发短暂卡顿），不会崩溃 —— 远程下发场景
        // 几乎都是 idle，实测影响可忽略。
        if (app.GetDeviceState() == kDeviceStateSpeaking) {
            app.AbortSpeaking(kAbortReasonNone);
        }
        app.GetAudioService().ResetDecoder();

        std::string err;
        if (!MusicPlayer::GetInstance().Play(url, title, &err)) {
            ESP_LOGW(TAG, "music_play: Play failed: %s", err.c_str());
            app.Alert("Playback failed", err.c_str(), "", "");
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
        if (was_playing) {
            Board::GetInstance().GetDisplay()->ShowNotification("已停止播放", 1500);
        }
    });
}

void RemoteCmd::OnWakeup(const cJSON* msg) {
    auto text_item = cJSON_GetObjectItem(msg, "text");
    if (!cJSON_IsString(text_item) || text_item->valuestring == nullptr ||
        text_item->valuestring[0] == '\0') {
        ESP_LOGW(TAG, "wakeup: missing text");
        return;
    }
    std::string text = text_item->valuestring;
    ESP_LOGI(TAG, "wakeup: %s", text.c_str());
    app_->WakeWordInvoke(text);
}

void RemoteCmd::OnSendText(const cJSON* msg) {
    auto text_item = cJSON_GetObjectItem(msg, "text");
    if (!cJSON_IsString(text_item) || text_item->valuestring == nullptr ||
        text_item->valuestring[0] == '\0') {
        ESP_LOGW(TAG, "send_text: missing text");
        return;
    }
    std::string text = text_item->valuestring;
    ESP_LOGI(TAG, "send_text: %s", text.c_str());
    // Application::SendText schedules the protocol write internally — no
    // need to wrap in app_->Schedule again.
    app_->SendText(text);
}

void RemoteCmd::OnReboot() {
    ESP_LOGI(TAG, "reboot");
    app_->Schedule([this]() {
        app_->Alert("重启设备", "正在重启...", "", "");
        vTaskDelay(pdMS_TO_TICKS(1500));  // let the alert paint before reset
        app_->Reboot();
    });
}

void RemoteCmd::OnOta() {
    ESP_LOGI(TAG, "ota");
    app_->Schedule([this]() {
        // Get out of any conversational state — OTA needs the radio dedicated
        // to firmware download, not to streaming audio.
        if (app_->GetDeviceState() == kDeviceStateSpeaking) {
            app_->AbortSpeaking(kAbortReasonNone);
        }

        app_->Alert("检查更新", "正在查询版本...", "", "");

        Ota ota;
        if (ota.CheckVersion() != ESP_OK) {
            ESP_LOGW(TAG, "ota: CheckVersion failed");
            app_->Alert("检查更新失败", "无法连接服务器", "", "");
            return;
        }
        if (!ota.HasNewVersion()) {
            ESP_LOGI(TAG, "ota: already on latest (%s)", ota.GetCurrentVersion().c_str());
            app_->Alert("已是最新版本", ota.GetCurrentVersion().c_str(), "", "");
            return;
        }

        ESP_LOGI(TAG, "ota: upgrading %s -> %s",
                 ota.GetCurrentVersion().c_str(), ota.GetFirmwareVersion().c_str());
        // UpgradeFirmware handles progress UI, partition write, and reboot on success.
        app_->UpgradeFirmware(ota.GetFirmwareUrl(), ota.GetFirmwareVersion());
    });
}
