#include "alarm_ringer.h"

#include "application.h"
#include "board.h"
#include "audio/audio_codec.h"
#include "assets/lang_config.h"

#include <esp_log.h>
#include <cstdio>

#define TAG "AlarmRinger"

// 三次提醒 · 绝对音量（不再按 saved_volume 倍率缩放）
static constexpr int kRingTotal = 3;
static constexpr int kVolStep1 = 60;
static constexpr int kVolStep2 = 80;
static constexpr int kVolStep3 = 100;

// 响铃 tick 周期 · 5s 一次
static constexpr int64_t kTickIntervalUs = 5 * 1000 * 1000;

// 兜底自停 · 防 ring_timer 异常时无限响（正常路径 t=10s 自停，远早于此）
static constexpr int64_t kAutoStopUs = 30 * 1000 * 1000ULL;  // 30s

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

    // 第 1 次响铃 · 绝对音量 60 · 立即响（OnTick 会接续第 2/3 次）
    if (codec) {
        codec->SetOutputVolume(kVolStep1);
    }
    app.PlaySound(Lang::Sounds::OGG_VIBRATION);   // vibration.ogg
    ring_count_ = 1;

    // 首次唤醒词：模拟"被主人喊醒"让 AI 接管对话（含 listening 等待用户响应）
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

    // 已响完 3 次 → 自停（不再循环）
    if (ring_count_ >= kRingTotal) {
        Stop("done");
        return;
    }

    // 第 2 次（ring_count_=1 → 80） / 第 3 次（ring_count_=2 → 100）
    int vol = (ring_count_ == 1) ? kVolStep2 : kVolStep3;
    if (codec) codec->SetOutputVolume(vol);
    app.PlaySound(Lang::Sounds::OGG_VIBRATION);
    ring_count_++;

    // 每次响铃都让 AI 再喊一次（≤10 字唤醒词）· 用户没响应就持续提醒
    app.Schedule([text = BuildWakeWord(message_, elapsed_s)]() {
        Application::GetInstance().WakeWordInvoke(text);
    });
    ESP_LOGI(TAG, "Ring #%d @ %ds · vol=%d", ring_count_, elapsed_s, vol);
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
