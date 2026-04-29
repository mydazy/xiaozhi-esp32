#ifndef _ACTIVITY_TYPE_H_
#define _ACTIVITY_TYPE_H_

#include <cstdint>

/**
 * ActivityType — 业务活动维度（B 维度）
 *
 * 详见 docs/p30-architecture.html § 一.5（三维心智模型）。
 *
 * 收拢散落的 MusicPlayer::IsPlaying() / flow_engine_->IsRunning() 等判断，
 * 替代它们在 Application / DeepSleep / WakeWord 等多处的散落 if-else。
 *
 * 入选标准：仅"会话型"业务活动（长持续 + 占用音频管线）进入此 enum。
 *   - 事件型业务（Alarm / PomodoroBell / Reminder）走 AudioSource enum，不进 ActivityType。
 *   - 后台服务型（Pomodoro 计时）使用 esp_timer，不进 ActivityType。
 *
 * 量产期纪律：本 enum 不抽基类，仅作类型标识。
 * 真有第 4 个会话型 Activity 时再考虑抽 LongSession 基类（见 § 一.5.6 阶段 2）。
 */
enum class ActivityType : uint8_t {
    kNone  = 0,   // 真 Idle，无业务活动
    kChat  = 1,   // 默认对话（含教育卡 / GIF / 表情等子能力）
    kFlow  = 2,   // 流程脚本（FlowEngine，文件 flow_engine.cc）—— 教学 / 互动 / 问答
    kMusic = 3,   // MP3 流式播放（独占音频，CloseAudioChannel 实现）
};

/**
 * 转字符串，便于日志输出与远程命令上报。
 * 保持极短，避免日志膨胀。
 */
inline const char* ActivityTypeToString(ActivityType t) {
    switch (t) {
        case ActivityType::kNone:  return "None";
        case ActivityType::kChat:  return "Chat";
        case ActivityType::kFlow:  return "Flow";
        case ActivityType::kMusic: return "Music";
    }
    return "?";
}

#endif  // _ACTIVITY_TYPE_H_
