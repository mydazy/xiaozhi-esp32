// MusicPlayer — adapter that wires the project's AudioCodec / Board / Application
// into the project-agnostic mydazy::Mp3Player component.

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
    // Range 断点续传需要：转发到底层 Http::SetHeader
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
        // W5 修（2026-04-29）· 错误时除 Alert 外还要：
        // ① 主动 Stop（清理 download/decode/output 三任务，避免残留 abort 后野指针）
        // ② 退出 Player UI 模式（参考 cb.on_pause_timeout 的写法）
        std::string s = status ? status : "";
        std::string m = message ? message : "";
        Application::GetInstance().Schedule([s = std::move(s), m = std::move(m)]() {
            auto& app = Application::GetInstance();
            MusicPlayer::GetInstance().Stop();
            if (auto* ui = dynamic_cast<UiDisplay*>(Board::GetInstance().GetDisplay())) {
                ui->SwitchOutPlayerMode();
            }
            app.Alert(s.c_str(), m.c_str(), "triangle_exclamation", "");
        });
    };
    // Graceful end-of-stream → 自动退出 Player UI 模式（必须 hop 回主任务，组件回调来自 worker task）
    cb.on_finished = []() {
        Application::GetInstance().Schedule([]() {
            if (auto* ui = dynamic_cast<UiDisplay*>(Board::GetInstance().GetDisplay())) {
                ui->SwitchOutPlayerMode();
            }
            // 正常播完没走 Stop 路径，单独恢复 WiFi 省电（Play 时切到 PERFORMANCE）
            Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        });
    };
    // Pause 超过 ~50s（OSS TCP keepalive 前）→ 主动 Stop + 通知用户
    // 不让 Resume 后 server 关连接被误判为"播完"
    cb.on_pause_timeout = []() {
        Application::GetInstance().Schedule([]() {
            auto& app = Application::GetInstance();
            MusicPlayer::GetInstance().Stop();
            if (auto* ui = dynamic_cast<UiDisplay*>(Board::GetInstance().GetDisplay())) {
                ui->SwitchOutPlayerMode();
            }
            app.Alert("暂停超时", "已自动停止播放", "", "");
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

    // 🔴 2026-04-30 v3：mp3 Play 前主动关闭 protocol（MQTT / WebSocket）
    // 根因：4G ML307 进入 HTTP binary mode 后，所有 AT 命令回应字节会污染下行流。
    //       即便 SendCommand 已加白名单允许 MQTT/MIPSEND 通过，业务层的 publish
    //       仍可能触发 modem 回应（OK/ERROR）混入 mp3 流；更糟的是失败 publish
    //       会触发用户可见 Alert（"发送失败，请检查网络"）。
    // 策略：mp3 启动前关 protocol → IsAudioChannelOpened()=false → 业务层
    //       所有 SendStartListening/publish 自动 silent fail，无 Alert，无污染。
    //       下次唤醒时 HandleWakeWordDetectedEvent 自动 OpenAudioChannel 重连。
    Application::GetInstance().CloseAudioChannel();

    // 播放期间切 PERFORMANCE（关 WiFi 省电 MIN_MODEM）
    // 根因：MIN_MODEM 在 beacon 间隔关 RF，OSS 长下载 socket 易抖动 → SSL -76 频发
    // Stop / on_error / on_finished / on_pause_timeout 路径都会经过 MusicPlayer::Stop 恢复
    Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    bool ok = mydazy::Mp3Player::GetInstance().Play(url, title, err_msg);
    if (!ok) {
        // Play 自身失败（参数错或资源分配失败）也要恢复，否则 WiFi 永远不省电
        Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
    }
    return ok;
}

void MusicPlayer::Stop() {
    mydazy::Mp3Player::GetInstance().Stop();
    // 恢复 WiFi 省电（Application 后续状态切换会再调一次也没关系——幂等）
    Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
}

bool MusicPlayer::IsPlaying() const {
    return mydazy::Mp3Player::GetInstance().IsPlaying();
}

std::string MusicPlayer::GetCurrentTitle() const {
    return mydazy::Mp3Player::GetInstance().GetCurrentTitle();
}

void MusicPlayer::Pause() { mydazy::Mp3Player::GetInstance().Pause(); }
void MusicPlayer::Resume() { mydazy::Mp3Player::GetInstance().Resume(); }
bool MusicPlayer::IsPaused() const { return mydazy::Mp3Player::GetInstance().IsPaused(); }
int  MusicPlayer::GetPositionMs() const { return mydazy::Mp3Player::GetInstance().GetPositionMs(); }
int  MusicPlayer::GetTotalDurationMs() const { return mydazy::Mp3Player::GetInstance().GetTotalDurationMs(); }
