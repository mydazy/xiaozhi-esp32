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

// ===== kAlarm 闹铃模式 · 必醒（saved_volume 渐入 + 5min 循环 + 20s 重念）=====
namespace alarm_mode {
constexpr int kStage1Sec       = 10;        // 0-10s : 60% 起步
constexpr int kStage2Sec       = 30;        // 10-30s: 80% 加强
constexpr int kVolStage0Pct    = 60;        // 30s+  : 100% 必醒
constexpr int kVolStage1Pct    = 80;
constexpr int kVolStage2Pct    = 100;
constexpr int kAiRepromptSec   = 20;        // AI 重念周期
constexpr int64_t kAutoStopUs  = 5 * 60 * 1000 * 1000ULL;  // 5min 兜底
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

// 拼接唤醒词 · 红线：≤10 汉字（message 由 self.alarm.add 限 ≤8 字保证）
static std::string BuildWakeWord(const std::string& message, int elapsed_s, AlarmRinger::Kind kind) {
    if (elapsed_s > 0) return "再次提醒";
    const bool is_alarm = (kind == AlarmRinger::Kind::kAlarm);
    if (message.empty()) return is_alarm ? "闹钟响了" : "提醒到了";
    return is_alarm ? ("闹钟" + message) : message;
}

// 投递唤醒词到主线程（Schedule + WakeWordInvoke 的统一封装）
void AlarmRinger::DispatchWakeWord(int elapsed_s) {
    Application::GetInstance().Schedule(
        [text = BuildWakeWord(message_, elapsed_s, kind_)]() {
            Application::GetInstance().WakeWordInvoke(text);
        });
}

void AlarmRinger::Start(const std::string& message, Kind kind) {
    // 幂等：如果已经在响铃中，仅更新 message 不重启 timer（防止重复 Start 累计）
    // 注意：模式不切换（首次 Kind 锁定 · 重入 Start 即使传不同 kind 也忽略）
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
    last_ai_prompt_sec_ = 0;

    ESP_LOGI(TAG, "Start · kind=%s · message=%s · saved_volume=%d",
             kind == Kind::kReminder ? "Reminder" : "Alarm",
             message.c_str(), saved_volume_);

    // 抑制自动休眠 · 响铃期间禁止 PowerSaveTimer 进深睡
    board.EnableAutoSleep(false);

    // 第 1 次响铃 · 音量起点：kReminder 绝对 60 / kAlarm saved×60%
    const int first_vol = (kind == Kind::kReminder)
        ? reminder_mode::kVolStep1
        : (saved_volume_ > 0 ? saved_volume_ * alarm_mode::kVolStage0Pct / 100 : -1);
    if (codec && first_vol >= 0) codec->SetOutputVolume(first_vol);
    app.PlaySound(Lang::Sounds::OGG_VIBRATION);
    DispatchWakeWord(0);

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
        // ring_count_=1 → 第 2 次（80） · =2 → 第 3 次（100）
        int vol = (ring_count_ == 1) ? reminder_mode::kVolStep2 : reminder_mode::kVolStep3;
        if (codec) codec->SetOutputVolume(vol);
        app.PlaySound(Lang::Sounds::OGG_VIBRATION);
        ring_count_++;
        DispatchWakeWord(elapsed_s);
        ESP_LOGI(TAG, "Reminder #%d @ %ds · vol=%d", ring_count_, elapsed_s, vol);
        return;
    }

    // ===== kAlarm 闹铃模式 · 5min 循环 + 渐入（60→80→100%）+ 20s 重念 =====
    if (codec && saved_volume_ > 0) {
        const int pct = (elapsed_s < alarm_mode::kStage1Sec) ? alarm_mode::kVolStage0Pct
                      : (elapsed_s < alarm_mode::kStage2Sec) ? alarm_mode::kVolStage1Pct
                      :                                         alarm_mode::kVolStage2Pct;
        codec->SetOutputVolume(saved_volume_ * pct / 100);
    }
    app.PlaySound(Lang::Sounds::OGG_VIBRATION);

    // 周期重唤醒：每 20s（elapsed=0 已在 Start 念过）
    if (elapsed_s > 0 && (elapsed_s - last_ai_prompt_sec_) >= alarm_mode::kAiRepromptSec) {
        last_ai_prompt_sec_ = elapsed_s;
        DispatchWakeWord(elapsed_s);
        ESP_LOGI(TAG, "Alarm re-wake @ %ds", elapsed_s);
    }
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
