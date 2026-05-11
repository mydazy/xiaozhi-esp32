#ifndef _SCENE_TYPE_H_
#define _SCENE_TYPE_H_

#include <cstdint>

/**
 * SceneType — UI 场景维度（C 维度）
 *
 * 当前已实现（✅）：Clock / Chat / Player / ConfigQr / ControlCenter
 * 拟新增（后续 PR）：Pomodoro / Todo / RoleSwitcher
 *
 */
enum class SceneType : uint8_t {
    kClock         = 0,  // 时钟主屏（Idle 默认）· 88px 时间 + 30px 日期
    kChat          = 1,  // 对话表情（Listening / Speaking）· emoji_box
    kPlayer        = 2,  // MP3 播放器（替代 is_player_mode_）
    kConfigQr      = 3,  // 配网 / 激活 QR（L5 全屏互斥独占）
    kControlCenter = 4,  // 控制中心（下拉手势 · 6 宫格已实现）
    kPomodoro      = 5,  // 番茄钟界面（拟新增）
    kTodo          = 6,  // 待办清单（拟新增）
    kRoleSwitcher  = 7,  // 角色切换（拟新增）
};

inline const char* SceneTypeToString(SceneType s) {
    switch (s) {
        case SceneType::kClock:         return "Clock";
        case SceneType::kChat:          return "Chat";
        case SceneType::kPlayer:        return "Player";
        case SceneType::kConfigQr:      return "ConfigQr";
        case SceneType::kControlCenter: return "ControlCenter";
        case SceneType::kPomodoro:      return "Pomodoro";
        case SceneType::kTodo:          return "Todo";
        case SceneType::kRoleSwitcher:  return "RoleSwitcher";
    }
    return "?";
}

#endif  // _SCENE_TYPE_H_
