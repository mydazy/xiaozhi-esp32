#ifndef SCREEN_CONFIG_H
#define SCREEN_CONFIG_H

#include <cstdint>

// ============================================================================
// MyDazy P30 设计系统配置
// 基于 Clay 风格适配 284x240 LVGL（见 DESIGN.md）
// ============================================================================

namespace ScreenConfig {

// 屏幕尺寸
constexpr int WIDTH = 284;
constexpr int HEIGHT = 240;
constexpr int RADIUS = 25;              // 4 角圆角半径（避让区半径）

// ----------------------------------------------------------------------------
// 布局常量
// ----------------------------------------------------------------------------
constexpr int HEADER_HEIGHT = 36;       // 标题栏高度（284x240 屏幕紧凑布局）
constexpr int ITEM_HEIGHT = 56;         // 列表项高度
constexpr int ITEM_GAP = 8;             // 列表项间距
constexpr int CONTENT_PADDING = 8;      // 内容区边距
constexpr int BUTTON_RADIUS = 16;       // 按钮圆角

// 2x2 宫格布局（284x240 屏幕适配）
// 水平: 2*72 + 36 = 180px, 左右边距各 52px
// 垂直: header(40) + 2*(72+2+20) + 8 = 40+188+8 = 236px, 底部余 4px
constexpr int GRID_BTN_SIZE = 72;       // 宫格按钮尺寸
constexpr int GRID_SPACING_X = 36;      // 宫格水平间距
constexpr int GRID_SPACING_Y = 8;       // 宫格垂直间距
constexpr int GRID_LABEL_HEIGHT = 20;   // 宫格标签高度
constexpr int GRID_LABEL_GAP = 2;       // 标签与按钮间距

// ----------------------------------------------------------------------------
// 颜色主题
// ----------------------------------------------------------------------------
namespace Colors {
    // 背景色
    constexpr uint32_t BG_PRIMARY = 0x000000;       // 主背景（纯黑）
    constexpr uint32_t BG_CARD = 0x1E1E1E;          // 卡片背景
    constexpr uint32_t BG_CARD_PRESSED = 0x2A2A2A;  // 卡片按下
    constexpr uint32_t BG_OVERLAY = 0x333333;       // 遮罩层

    // 强调色
    constexpr uint32_t ACCENT_BLUE = 0x64B5F6;      // 浅蓝（完成状态）
    constexpr uint32_t ACCENT_RED = 0xE53935;       // 红色（删除/警告）
    constexpr uint32_t ACCENT_GREEN = 0x4CAF50;     // 绿色（成功）
    constexpr uint32_t ACCENT_ORANGE = 0xFB8C00;    // 橙色（番茄钟）

    // 文字色
    constexpr uint32_t TEXT_PRIMARY = 0xFFFFFF;     // 主文字（白）
    constexpr uint32_t TEXT_SECONDARY = 0xCCCCCC;   // 次要文字（亮灰，宫格标签更清晰）
    constexpr uint32_t TEXT_DISABLED = 0x888888;    // 禁用文字（WCAG AA ~5.3:1 on black）
    constexpr uint32_t TEXT_HINT = 0x666666;        // 提示文字

    // 边框色
    constexpr uint32_t BORDER_DEFAULT = 0x555555;   // 默认边框
    constexpr uint32_t BORDER_ACTIVE = 0x64B5F6;    // 激活边框
}

// 教育模式配色（CHILD Profile 专用，3-8岁儿童友好）
namespace EduColors {
    // 跟读反馈
    constexpr uint32_t CORRECT_GREEN = 0x66BB6A;    // 答对（柔和绿，避免刺眼）
    constexpr uint32_t CORRECT_BG = 0x1B3A1B;       // 答对背景（深绿）
    constexpr uint32_t WRONG_ORANGE = 0xFFA726;     // 答错（温柔橙，不用红色避免挫败感）
    constexpr uint32_t WRONG_BG = 0x3A2A1B;         // 答错背景（深橙）

    // 进度与成就
    constexpr uint32_t STAR_GOLD = 0xFFD54F;        // 星星（金色）
    constexpr uint32_t STREAK_FIRE = 0xFF7043;      // 连续天数火焰（橙红）
    constexpr uint32_t PROGRESS_BLUE = 0x42A5F5;    // 进度条（天蓝）
    constexpr uint32_t PROGRESS_BG = 0x1A2A3A;      // 进度条背景

    // 卡片
    constexpr uint32_t CARD_WORD_BG = 0x1A237E;     // 单词卡背景（深靛蓝）
    constexpr uint32_t CARD_HANZI_BG = 0x4A148C;    // 汉字卡背景（深紫）
    constexpr uint32_t CARD_REVIEW_BG = 0x1B5E20;   // 复习卡背景（深绿）

    // PTT 跟读按钮
    constexpr uint32_t PTT_IDLE = 0x42A5F5;         // 待跟读（天蓝）
    constexpr uint32_t PTT_RECORDING = 0xEF5350;    // 录音中（红色脉动）
    constexpr uint32_t PTT_TEXT = 0xFFFFFF;          // 按钮文字（白）

    // 庆祝动效
    constexpr uint32_t CELEBRATE_1 = 0xFFD54F;      // 庆祝色1（金）
    constexpr uint32_t CELEBRATE_2 = 0xFF7043;      // 庆祝色2（橙）
    constexpr uint32_t CELEBRATE_3 = 0x66BB6A;      // 庆祝色3（绿）
}

// ----------------------------------------------------------------------------
// 字体大小（对应 font_puhui_XX_4）
// ----------------------------------------------------------------------------
namespace FontSize {
    constexpr int SMALL = 14;
    constexpr int NORMAL = 20;
    constexpr int LARGE = 30;
    constexpr int XLARGE = 88;          // 大时钟数字

    // 教育模式字体（cbin 动态加载，PSRAM 存储）
    constexpr int EDU_WORD = 48;        // 单词/汉字大字展示（卡片主体）
    constexpr int EDU_PHONETIC = 30;    // 音标/拼音（卡片辅助）
    constexpr int EDU_MEANING = 20;     // 释义（复用 NORMAL）
    constexpr int EDU_EMOJI = 36;       // emoji 配图尺寸
}

// ----------------------------------------------------------------------------
// 动画时长（毫秒）
// ----------------------------------------------------------------------------
namespace Animation {
    constexpr int FAST = 150;
    constexpr int NORMAL = 300;
    constexpr int SLOW = 500;
}

// ----------------------------------------------------------------------------
// 教育模式布局常量（284x240 屏幕）
// ----------------------------------------------------------------------------
namespace EduLayout {
    // EduCardPage 布局
    constexpr int CARD_AREA_Y = 36;            // 卡片区起始Y（header下方）
    constexpr int CARD_AREA_HEIGHT = 134;      // 卡片区高度
    constexpr int ACTION_BAR_Y = 174;          // 操作区起始Y
    constexpr int ACTION_BAR_HEIGHT = 62;      // 操作区高度
    constexpr int PTT_BTN_WIDTH = 140;         // PTT跟读按钮宽度
    constexpr int PTT_BTN_HEIGHT = 48;         // PTT跟读按钮高度
    constexpr int NAV_BTN_SIZE = 48;           // 翻页导航按钮

    // EduTodoPage 布局
    constexpr int TODO_ITEM_HEIGHT = 52;       // 任务条目高度
    constexpr int TODO_ITEM_GAP = 6;           // 条目间距
    constexpr int TODO_ICON_SIZE = 32;         // 任务图标

    // 进度条
    constexpr int PROGRESS_BAR_HEIGHT = 8;     // 进度条高度
    constexpr int PROGRESS_BAR_RADIUS = 4;     // 进度条圆角

    // 反馈动效
    constexpr int STAR_SIZE = 24;              // 星星尺寸
    constexpr int STAR_COUNT = 5;              // 答对时飞出星星数
    constexpr int FEEDBACK_DURATION_MS = 800;  // 反馈动效时长
    constexpr int COMBO_THRESHOLD = 3;         // 三连击庆祝阈值
}

}  // namespace ScreenConfig

#endif // SCREEN_CONFIG_H