#ifndef _SCENE_TYPE_H_
#define _SCENE_TYPE_H_

#include <cstdint>

/**
 * SceneType — UI 场景维度（C 维度）
 *
 * 详见 docs/p30-architecture.html § 一.5（三维心智模型）。
 *
 * 替代 UiDisplay 内部 is_clock_mode_ / is_player_mode_ / IsControlCenterVisible() 等
 * 散落 mode flag 的对外查询入口。复用 LVGL screen 抽象，不引入 SceneNavigator 基类。
 *
 * 当前已实现（✅）：Clock / Emoji / Player / ConfigQr / ControlCenter
 * 拟新增（后续 PR）：Pomodoro / Todo / RoleSwitcher
 *
 * 量产期纪律：本 enum 仅作类型标识，不抽 Scene 基类。
 * 真有第 6 个全屏 Scene 且共享逻辑出现时再抽 SceneFactory 工厂（见 § 一.5.6 阶段 3）。
 */
enum class SceneType : uint8_t {
    kClock         = 0,  // 时钟主屏（Idle 默认）· 88px 时间 + 30px 日期
    kEmoji         = 1,  // 对话表情（Listening / Speaking）· emoji_box
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
        case SceneType::kEmoji:         return "Emoji";
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
