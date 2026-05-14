#include "alarm_ringer.h"

#include "application.h"
#include "board.h"
#include "audio/audio_codec.h"
#include "assets/lang_config.h"

#include <esp_log.h>
#include <cstdio>

#define TAG "AlarmRinger"

// 共用 · tick 周期 5s 一次（两模式都用）
static constexpr int64_t kTickIntervalUs = 5 * 1000 * 1000;

// ===== kAlarm 闹铃模式 · 必醒（60s 循环 · 每周期 3 次提醒 · 5 次循环 = 5min）=====
namespace alarm_mode {
constexpr int kStepsPerCycle   = 12;        // 60s / 5s = 12 步
constexpr int kAiStep          = 2;         // 第 2 步唤醒 AI（前 2 步响 vibration）
constexpr int kVolStage0Pct    = 80;        // step 0 起步
constexpr int kVolStage1Pct    = 90;        // step 1 加强
constexpr int kVolStage2Pct    = 100;       // step 2+ 必醒
constexpr int64_t kAutoStopUs  = 5 * 60 * 1000 * 1000ULL;  // 5min（≈5 个周期）
}  // namespace alarm_mode

// ===== kReminder 提醒模式 · 温和（绝对音量 + 3 次自停 + 每次都念）=====
namespace reminder_mode {
constexpr int kRingTotal       = 3;
constexpr int kVolStep1        = 60;        // 第 1 次（t=0）
constexpr int kVolStep2        = 80;        // 第 2 次（t=5s）
constexpr int kVolStep3        = 100;       // 第 3 次（t=10s）
constexpr int64_t kAutoStopUs  = 30 * 1000 * 1000ULL;       // 30s 兜底（正常 t=15s 自停）
}  // namespace reminder_mode

AlarmRinger& AlarmRinger::GetInstance() {
    static AlarmRinger instance;
    return instance;
}

// 设备端不做任何拼接/判断 · 裸用即可
void AlarmRinger::DispatchWakeWord() {
    Application::GetInstance().Schedule(
        [text = message_]() {
            Application::GetInstance().WakeWordInvoke(text);
        });
}

void AlarmRinger::Start(const std::string& message, Kind kind) {
    bool was_ringing = ringing_.exchange(true, std::memory_order_acq_rel);
    message_ = message;

    if (was_ringing) {
        ESP_LOGI(TAG, "Already ringing · update message=%s (kind locked to first call)", message.c_str());
        return;
    }

    kind_ = kind;
    auto& app = Application::GetInstance();
    auto& board = Board::GetInstance();
    auto* codec = board.GetAudioCodec();

    start_us_ = esp_timer_get_time();
    saved_volume_ = codec ? codec->output_volume() : -1;
    ring_count_ = (kind == Kind::kReminder) ? 1 : 0;   // kReminder 视为"将立即响第 1 次"
    cycle_step_ = 1;                                   // kAlarm：Start 已响 step 0 · OnTick 从 step 1 开始

    ESP_LOGI(TAG, "Start · kind=%s · message=%s · saved_volume=%d",
             kind == Kind::kReminder ? "Reminder" : "Alarm",
             message.c_str(), saved_volume_);

    // 抑制自动休眠 · 响铃期间禁止 PowerSaveTimer 进深睡
    board.EnableAutoSleep(false);

    // 第 1 次响铃 · 音量起点：kReminder 绝对 60 / kAlarm saved×80%
    const int first_vol = (kind == Kind::kReminder)
        ? reminder_mode::kVolStep1
        : alarm_mode::kVolStage0Pct;
    if (codec && first_vol >= 0) codec->SetOutputVolume(first_vol);
    app.PlaySound(Lang::Sounds::OGG_VIBRATION);
    if (kind == Kind::kReminder) DispatchWakeWord();

    // 创建周期 tick timer（5s）· 共用
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

    // 创建一次性 timeout timer · 按模式分流
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
    esp_timer_start_once(timeout_timer_,
        (kind == Kind::kReminder) ? reminder_mode::kAutoStopUs : alarm_mode::kAutoStopUs);
}

void AlarmRinger::OnTickStatic(void* arg) {
    static_cast<AlarmRinger*>(arg)->OnTick();
}

void AlarmRinger::OnTick() {
    if (!ringing_.load(std::memory_order_acquire)) return;

    auto& app = Application::GetInstance();
    auto* codec = Board::GetInstance().GetAudioCodec();
    int elapsed_s = static_cast<int>((esp_timer_get_time() - start_us_) / 1000000);

    if (kind_ == Kind::kReminder) {
        // ===== kReminder 提醒模式 · 3 次自停 =====
        if (ring_count_ >= reminder_mode::kRingTotal) {
            Stop("done");
            return;
        }
        int vol = (ring_count_ == 1) ? reminder_mode::kVolStep2 : reminder_mode::kVolStep3;
        if (codec) codec->SetOutputVolume(vol);
        app.PlaySound(Lang::Sounds::OGG_VIBRATION);
        ring_count_++;
        DispatchWakeWord();
        ESP_LOGI(TAG, "Reminder #%d @ %ds · vol=%d", ring_count_, elapsed_s, vol);
        return;
    }

    // ===== kAlarm 60s 周期 · step 0/1 vibration（80/90%） · step 2 AI 语音（100%） · step 3-11 静默 =====
    const int s = cycle_step_;
    const int pct = (s == 0) ? alarm_mode::kVolStage0Pct
                  : (s == 1) ? alarm_mode::kVolStage1Pct
                  :            alarm_mode::kVolStage2Pct;
    if (codec && saved_volume_ > 0 && s <= alarm_mode::kAiStep) {
        codec->SetOutputVolume(pct);
    }
    if (s < alarm_mode::kAiStep) {
        app.PlaySound(Lang::Sounds::OGG_VIBRATION);
        ESP_LOGI(TAG, "Alarm vibration step=%d vol=%d%% elapsed=%ds", s, pct, elapsed_s);
    } else if (s == alarm_mode::kAiStep) {
        DispatchWakeWord();
        ESP_LOGI(TAG, "Alarm AI step=%d elapsed=%ds", s, elapsed_s);
    }
    cycle_step_ = (s + 1) % alarm_mode::kStepsPerCycle;
}

void AlarmRinger::OnTimeoutStatic(void* arg) {
    auto* self = static_cast<AlarmRinger*>(arg);
    // ESP_TIMER_TASK 不能阻塞 · Stop 内含恢复音量等操作 ·  调用方简单 · 安全
    self->Stop("timeout");
}

bool AlarmRinger::ShakeStop(int min_count) {
    if (!IsRinging()) return false;
    int64_t now = esp_timer_get_time();
    if (now - shake_first_us_ > 5000000LL) {     // 5s 滑窗 · 超出则重置计数
        shake_count_ = 0;
        shake_first_us_ = now;
    }
    if (++shake_count_ >= min_count) {
        Stop("shake×N");
        shake_count_ = 0;
    } else {
        ESP_LOGI(TAG, "shake stop pending (%d/%d)", shake_count_, min_count);
    }
    return true;
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

    // 清 ShakeStop 累计状态（下次响铃从 0 开始）
    shake_count_ = 0;
    shake_first_us_ = 0;

    // 恢复原音量
    if (saved_volume_ >= 0) {
        auto* codec = Board::GetInstance().GetAudioCodec();
        if (codec) codec->SetOutputVolume(saved_volume_);
        saved_volume_ = -1;
    }

    // 恢复自动休眠
    Board::GetInstance().EnableAutoSleep(true);
}
