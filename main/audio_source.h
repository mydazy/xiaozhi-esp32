#ifndef _AUDIO_SOURCE_H_
#define _AUDIO_SOURCE_H_

#include <cstdint>

/**
 * AudioSource — 音频源标识（Speaking 态下查询用）
 *
 * 详见 docs/p30-architecture.html § 一.5（三维心智模型）。
 *
 * Speaking 状态下究竟是谁在出声？用于 LED / UI / 打断逻辑分支。
 * 事件型 Activity（Alarm / PomodoroBell / Reminder）通过此 enum 区分，
 * 不占用 ActivityType 枚举值——它们是短促事件，不是长会话。
 *
 * 当前阶段 0 实现：Tts / Mp3 / None 三个值由 GetCurrentAudioSource() 自动推断。
 * AlarmBell / PomodoroBell / Reminder 等待对应模块（AlarmManager / Pomodoro）
 * 加入后由模块显式 SetAudioSource()，本 enum 先预留枚举值。
 */
enum class AudioSource : uint8_t {
    kNone         = 0,   // 静默（非 Speaking 态）
    kTts          = 1,   // AI TTS（Chat / Flow 输出）
    kMp3          = 2,   // MusicPlayer 输出
    kAlarmBell    = 3,   // 闹钟铃声（拟新增 · AlarmManager 模块）
    kPomodoroBell = 4,   // 番茄钟阶段铃（拟新增 · Pomodoro 模块）
    kReminder     = 5,   // 待办语音提醒（拟新增 · Reminder 模块）
};

inline const char* AudioSourceToString(AudioSource s) {
    switch (s) {
        case AudioSource::kNone:         return "None";
        case AudioSource::kTts:          return "Tts";
        case AudioSource::kMp3:          return "Mp3";
        case AudioSource::kAlarmBell:    return "AlarmBell";
        case AudioSource::kPomodoroBell: return "PomodoroBell";
        case AudioSource::kReminder:     return "Reminder";
    }
    return "?";
}

#endif  // _AUDIO_SOURCE_H_
