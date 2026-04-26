// MusicPlayer — adapter that wires the project's AudioCodec / Board / Application
// into the project-agnostic mydazy::Mp3Player component.

#include "music_player.h"

#include "application.h"
#include "audio_codec.h"
#include "board.h"
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

    mydazy::Mp3Player::Callbacks cb;
    // Async error → hop back to the main task before touching UI.
    cb.on_error = [](const char* status, const char* message) {
        std::string s = status ? status : "";
        std::string m = message ? message : "";
        Application::GetInstance().Schedule([s = std::move(s), m = std::move(m)]() {
            Application::GetInstance().Alert(s.c_str(), m.c_str(), "", "");
        });
    };
    mydazy::Mp3Player::GetInstance().Initialize(g_audio.get(), g_http.get(), cb);
    initialized_ = true;
    ESP_LOGI(TAG, "MusicPlayer adapter initialized");
}

bool MusicPlayer::Play(const std::string& url, const std::string& title, std::string* err_msg) {
    if (!initialized_) {
        if (err_msg) *err_msg = "player not initialized";
        return false;
    }
    return mydazy::Mp3Player::GetInstance().Play(url, title, err_msg);
}

void MusicPlayer::Stop() {
    mydazy::Mp3Player::GetInstance().Stop();
}

bool MusicPlayer::IsPlaying() const {
    return mydazy::Mp3Player::GetInstance().IsPlaying();
}

std::string MusicPlayer::GetCurrentTitle() const {
    return mydazy::Mp3Player::GetInstance().GetCurrentTitle();
}
