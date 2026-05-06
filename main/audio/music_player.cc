// MusicPlayer — 项目侧 adapter，桥接 AudioCodec / Board / Application 与
// project-agnostic mydazy::Mp3Player（v2.0 极简 C 风格 event API）。
//
// v2.0 适配要点：
//   - Mp3Player::Callbacks struct → mp3_event_cb_t 函数指针 + ctx
//   - GetCurrentTitle 删除 → MusicPlayer 自行维护 title_ 字符串
//   - Pause/Resume 删除 → 内部判 IsPaused 后调用 PauseToggle

#include "music_player.h"

#include "application.h"
#include "audio_codec.h"
#include "board.h"
#include "display/ui_display.h"
#include "http.h"
#include "mp3_player.h"
#include "network_interface.h"

#include <esp_log.h>

#include <memory>
#include <utility>

#define TAG "MusicPlayer"

namespace {

class CodecAudioOutput : public mydazy::IAudioOutput {
public:
    explicit CodecAudioOutput(AudioCodec* codec) : codec_(codec) {}
    int  output_sample_rate() const override { return codec_->output_sample_rate(); }
    int  output_channels()    const override { return codec_->output_channels(); }
    bool output_enabled()     const override { return codec_->output_enabled(); }
    void EnableOutput(bool enable) override  { codec_->EnableOutput(enable); }
    void OutputData(std::vector<int16_t>& pcm) override { codec_->OutputData(pcm); }
private:
    AudioCodec* codec_;
};

class HttpClientWrapper : public mydazy::IHttpClient {
public:
    explicit HttpClientWrapper(std::unique_ptr<Http> http) : http_(std::move(http)) {}
    bool   Open(const std::string& method, const std::string& url) override {
        return http_ && http_->Open(method, url);
    }
    int    GetStatusCode()              override { return http_ ? http_->GetStatusCode() : 0; }
    size_t GetBodyLength()              override { return http_ ? http_->GetBodyLength() : 0; }
    int    Read(char* buf, size_t size) override { return http_ ? http_->Read(buf, size) : -1; }
    void   Close()                      override { if (http_) http_->Close(); }
    void   SetTimeout(int ms)           override { if (http_) http_->SetTimeout(ms); }
    void   SetHeader(const std::string& key, const std::string& value) override {
        if (http_) http_->SetHeader(key, value);
    }
private:
    std::unique_ptr<Http> http_;
};

class BoardHttpFactory : public mydazy::IHttpFactory {
public:
    std::unique_ptr<mydazy::IHttpClient> CreateHttp() override {
        auto network = Board::GetInstance().GetNetwork();
        if (!network) return nullptr;
        auto http = network->CreateHttp(0);
        if (!http) return nullptr;
        return std::make_unique<HttpClientWrapper>(std::move(http));
    }
};

// Owned by MusicPlayer singleton; constructed once in Initialize().
std::unique_ptr<CodecAudioOutput> g_audio;
std::unique_ptr<BoardHttpFactory> g_http;

// 内部维护当前曲名（v2.0 Mp3Player 不再暴露 GetCurrentTitle）
std::string g_current_title;

// 静态 C 风格事件回调（v2.0 Mp3Player API） — 在 worker task 上下文，必须 Schedule 回主任务
void OnMp3Event(mydazy::mp3_event_t ev, int extra, const char *msg, void *ctx) {
    (void)ctx;
    (void)extra;
    auto& app = Application::GetInstance();
    auto title = g_current_title;

    switch (ev) {
        case mydazy::MP3_EVENT_ERROR: {
            std::string m = msg ? msg : "";
            app.Schedule([m = std::move(m)]() {
                MusicPlayer::GetInstance().Stop();
                if (auto* ui = dynamic_cast<UiDisplay*>(Board::GetInstance().GetDisplay())) {
                    ui->SwitchOutPlayerMode();
                }
                Application::GetInstance().Alert("Playback failed", m.c_str(),
                                                  "triangle_exclamation", "");
            });
            break;
        }
        case mydazy::MP3_EVENT_FINISHED: {
            app.Schedule([]() {
                if (auto* ui = dynamic_cast<UiDisplay*>(Board::GetInstance().GetDisplay())) {
                    ui->SwitchOutPlayerMode();
                }
                Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
            });
            break;
        }
        case mydazy::MP3_EVENT_PAUSE_TIMEOUT: {
            app.Schedule([]() {
                MusicPlayer::GetInstance().Stop();
                if (auto* ui = dynamic_cast<UiDisplay*>(Board::GetInstance().GetDisplay())) {
                    ui->SwitchOutPlayerMode();
                }
                Application::GetInstance().Alert("暂停超时", "已自动停止播放", "", "");
            });
            break;
        }
        case mydazy::MP3_EVENT_STARTED:
            // 首帧解码完成；当前 UI 在 Play() 同步流程已就位，无需额外动作
            break;
    }
}

}  // namespace

MusicPlayer& MusicPlayer::GetInstance() {
    static MusicPlayer instance;
    return instance;
}

void MusicPlayer::Initialize(AudioCodec* codec) {
    if (initialized_) return;
    if (!codec) {
        ESP_LOGE(TAG, "Initialize: codec is null");
        return;
    }
    g_audio = std::make_unique<CodecAudioOutput>(codec);
    g_http  = std::make_unique<BoardHttpFactory>();

    // v2.0：单一 mp3_event_cb_t 替代原 Callbacks struct（4 个 std::function）
    mydazy::Mp3Player::GetInstance().Initialize(
        g_audio.get(), g_http.get(), &OnMp3Event, nullptr);
    initialized_ = true;
    ESP_LOGI(TAG, "MusicPlayer adapter initialized (Mp3Player v2.0)");
}

bool MusicPlayer::Play(const std::string& url, const std::string& title, std::string* err_msg) {
    if (!initialized_) {
        if (err_msg) *err_msg = "player not initialized";
        return false;
    }

    Application::GetInstance().CloseAudioChannel();
    Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);

    g_current_title = title;   // 项目侧维护，Mp3Player v2.0 不再暴露 GetCurrentTitle
    bool ok = mydazy::Mp3Player::GetInstance().Play(url.c_str(), title.c_str());
    if (!ok) {
        if (err_msg) *err_msg = "Mp3Player::Play failed (see log)";
        Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
    }
    return ok;
}

void MusicPlayer::Stop() {
    mydazy::Mp3Player::GetInstance().Stop();
    g_current_title.clear();
    Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
}

bool MusicPlayer::IsPlaying() const  { return mydazy::Mp3Player::GetInstance().IsPlaying(); }
std::string MusicPlayer::GetCurrentTitle() const { return g_current_title; }

/* v2.0：Mp3Player 把 Pause+Resume 合并为 PauseToggle；MusicPlayer 保持双 API 以
   维持现有调用方（Player UI 暂停按钮等）兼容。仅当当前态与目标态不一致时才切换。*/
void MusicPlayer::Pause() {
    auto& mp3 = mydazy::Mp3Player::GetInstance();
    if (!mp3.IsPaused()) mp3.PauseToggle();
}
void MusicPlayer::Resume() {
    auto& mp3 = mydazy::Mp3Player::GetInstance();
    if (mp3.IsPaused()) mp3.PauseToggle();
}
bool MusicPlayer::IsPaused() const   { return mydazy::Mp3Player::GetInstance().IsPaused(); }
int  MusicPlayer::GetPositionMs() const { return mydazy::Mp3Player::GetInstance().GetPositionMs(); }
int  MusicPlayer::GetTotalDurationMs() const { return mydazy::Mp3Player::GetInstance().GetTotalDurationMs(); }
