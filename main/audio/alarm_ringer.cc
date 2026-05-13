#include "alarm_ringer.h"

#include "application.h"
#include "board.h"
#include "audio/audio_codec.h"
#include "assets/lang_config.h"

#include <esp_log.h>
#include <cstdio>

#define TAG "AlarmRinger"

// 渐入档位（saved_volume 倍率）
//   0-10s : 60%   起步清晰可闻 · 不吓人
//   10-30s: 80%   加强
//   30s+  : 100%  最大 · 必醒
static constexpr int kStage1Sec = 10;
static constexpr int kStage2Sec = 30;
static constexpr int kVolStage0Pct = 60;
static constexpr int kVolStage1Pct = 80;
static constexpr int kVolStage2Pct = 100;

// 响铃 tick 周期 · 每次再放 1 次 OGG_VIBRATION + 更新音量档位
static constexpr int64_t kTickIntervalUs = 5 * 1000 * 1000;  // 5s

// AI 重复念叨周期 · 每隔 N 秒让 AI 重新提醒一次（覆盖"用户耳朵记住提醒事项"）
// 第 0 秒念一次 · 之后每 20 秒念一次 · 5 分钟内最多念 ~15 次
static constexpr int kAiRepromptSec = 20;

// 自动停超时 · 防扰民兜底
static constexpr int64_t kAutoStopUs = 5 * 60 * 1000 * 1000ULL;  // 5min

AlarmRinger& AlarmRinger::GetInstance() {
    static AlarmRinger instance;
    return instance;
}

// 闹钟"唤醒词" · 红线：≤10 汉字（防止长字符串污染 AI 上下文）
static std::string BuildWakeWord(const std::string& /*message*/, int elapsed_s) {
    return (elapsed_s == 0) ? "闹钟响了" : "闹钟还在响";
}

void AlarmRinger::Start(const std::string& message) {
    // 幂等：如果已经在响铃中，仅更新 message 不重启 timer（防止重复 Start 累计）
    bool was_ringing = ringing_.exchange(true, std::memory_order_acq_rel);
    message_ = message;

    if (was_ringing) {
        ESP_LOGI(TAG, "Already ringing · update message=%s", message.c_str());
        return;
    }

    auto& app = Application::GetInstance();
    auto& board = Board::GetInstance();
    auto* codec = board.GetAudioCodec();

    start_us_ = esp_timer_get_time();
    saved_volume_ = codec ? codec->output_volume() : -1;

    ESP_LOGI(TAG, "Start · message=%s · saved_volume=%d", message.c_str(), saved_volume_);

    // 抑制自动休眠 · 响铃期间禁止 PowerSaveTimer 进深睡（用户没响应不能再睡过头）
    board.EnableAutoSleep(false);

    // 设初始音量（渐入起点 70%）· 立即放第一声铃
    if (codec && saved_volume_ > 0) {
        codec->SetOutputVolume(saved_volume_ * kVolStage0Pct / 100);
    }
    app.PlaySound(Lang::Sounds::OGG_VIBRATION);   // vibration.ogg · 闹铃 / 番茄钟到点共用

    // 首次唤醒词：模拟"被主人喊醒"让 AI 接管对话（含 listening 等待用户响应）
    // OnTick 每 20s 会再 wake 一次直到关停
    last_ai_prompt_sec_ = 0;
    app.Schedule([text = BuildWakeWord(message, 0)]() {
        Application::GetInstance().WakeWordInvoke(text);
    });

    // 创建周期 tick timer（5s · 渐入 + 再响铃声）
    if (!ring_timer_) {
        esp_timer_create_args_t args = {
            .callback = &AlarmRinger::OnTickStatic,
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "alarm_ring",
            .skip_unhandled_events = true,
        };
        esp_timer_create(&args, &ring_timer_);
    }
    esp_timer_start_periodic(ring_timer_, kTickIntervalUs);

    // 创建一次性 timeout timer（5min · 兜底自停）
    if (!timeout_timer_) {
        esp_timer_create_args_t args = {
            .callback = &AlarmRinger::OnTimeoutStatic,
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "alarm_timeout",
            .skip_unhandled_events = true,
        };
        esp_timer_create(&args, &timeout_timer_);
    }
    esp_timer_start_once(timeout_timer_, kAutoStopUs);
}

void AlarmRinger::OnTickStatic(void* arg) {
    static_cast<AlarmRinger*>(arg)->OnTick();
}

void AlarmRinger::OnTick() {
    if (!ringing_.load(std::memory_order_acquire)) return;

    auto& app = Application::GetInstance();
    auto* codec = Board::GetInstance().GetAudioCodec();
    int elapsed_s = static_cast<int>((esp_timer_get_time() - start_us_) / 1000000);

    // 渐入音量
    if (codec && saved_volume_ > 0) {
        int vol_pct;
        if      (elapsed_s < kStage1Sec) vol_pct = kVolStage0Pct;  // 60%
        else if (elapsed_s < kStage2Sec) vol_pct = kVolStage1Pct;  // 80%
        else                              vol_pct = kVolStage2Pct;  // 100%
        codec->SetOutputVolume(saved_volume_ * vol_pct / 100);
    }

    // 每 5s 响一次铃 · ESP_TIMER_TASK 不阻塞（PlaySound 内部异步 dispatch）
    app.PlaySound(Lang::Sounds::OGG_VIBRATION);   // vibration.ogg · 闹铃 / 番茄钟到点共用

    // 周期重唤醒：每 kAiRepromptSec(20s) 再调一次 WakeWordInvoke
    // 用户没响应就反复"喊" AI 来提醒（AI Speaking 中会先 Abort 再起）
    // 跳过 elapsed_s=0（Start 已 wake 第一次）
    if (elapsed_s > 0 && (elapsed_s - last_ai_prompt_sec_) >= kAiRepromptSec) {
        last_ai_prompt_sec_ = elapsed_s;
        app.Schedule([text = BuildWakeWord(message_, elapsed_s)]() {
            Application::GetInstance().WakeWordInvoke(text);
        });
        ESP_LOGI(TAG, "Re-wake @ %ds", elapsed_s);
    }

    ESP_LOGD(TAG, "Tick · elapsed=%ds", elapsed_s);
}

void AlarmRinger::OnTimeoutStatic(void* arg) {
    auto* self = static_cast<AlarmRinger*>(arg);
    // ESP_TIMER_TASK 不能阻塞 · Stop 内含恢复音量等操作 ·  调用方简单 · 安全
    self->Stop("timeout");
}

void AlarmRinger::Stop(const char* reason) {
    bool was_ringing = ringing_.exchange(false, std::memory_order_acq_rel);
    if (!was_ringing) return;

    int elapsed_ms = static_cast<int>((esp_timer_get_time() - start_us_) / 1000);
    ESP_LOGI(TAG, "Stop by '%s' after %d ms", reason ? reason : "?", elapsed_ms);

    if (ring_timer_) {
        esp_timer_stop(ring_timer_);
    }
    if (timeout_timer_) {
        esp_timer_stop(timeout_timer_);
    }

    // 恢复原音量
    if (saved_volume_ >= 0) {
        auto* codec = Board::GetInstance().GetAudioCodec();
        if (codec) codec->SetOutputVolume(saved_volume_);
        saved_volume_ = -1;
    }

    // 恢复自动休眠
    Board::GetInstance().EnableAutoSleep(true);
}
