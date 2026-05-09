# 教育卡独立文件化 + Kconfig 可选编译 设计

> 把教育卡逻辑从 ui_display.cc 搬出，独立成 `EducationCard` 类，方便维护 + 部分设备可关闭。

## 一、改动文件

| 文件 | 状态 | 改动内容 |
|---|---|---|
| `main/display/education_card.h` | 🆕 新建 | EducationCard 类声明 + EduRow 移入 |
| `main/display/education_card.cc` | 🆕 新建 | ShowEduCard / BuildEduCard / 字体选档 / 跑马灯 全部实现 |
| `main/display/ui_display.h` | ✏ 修改 | 删教育卡相关字段 / 函数声明，改持有 unique_ptr<EducationCard> |
| `main/display/ui_display.cc` | ✏ 修改 | ShowEduCard 委托给 card_->Show(...)；EnsureDisplayFonts 加 88 字体加载 |
| `main/Kconfig.projbuild` | ✏ 修改 | 加 `CONFIG_USE_EDUCATION_CARD`（默认 ON for P30/P31） |
| `main/CMakeLists.txt` | ✏ 修改 | 条件编译 education_card.cc |

## 二、education_card.h 接口设计

```cpp
#pragma once
#include <lvgl.h>
#include <functional>
#include "lcd_display.h"

class EducationCard {
public:
    enum class Mode {
        kNormal,      // 默认主秀 56 px（被动触发：故事/对话）
        kSuperSize    // 主动学习超大档：≤ 5 字符英文用 88，否则降 56
    };

    explicit EducationCard(LcdDisplay* parent_display);
    ~EducationCard();

    // 字体加载（由 UiDisplay::EnsureDisplayFonts() 调用注入）
    void SetFonts(const lv_font_t* main_56,
                  const lv_font_t* main_48,
                  const lv_font_t* main_88,
                  const lv_font_t* top_30,
                  const lv_font_t* sub_20);

    // v8 接口（参数与原 ShowEduCard 兼容）
    void Show(const char* category, const char* main_text,
              const char* top, const char* bottom,
              Mode mode = Mode::kNormal,
              const char* explanation = nullptr,
              int duration_ms = 0);

    void Hide();
    bool IsActive() const;

    // 自动隐藏定时器
    void ScheduleAutoHide(int duration_ms);

private:
    struct EduRow {
        const char* text;
        const lv_font_t* font;
        uint32_t color;
        int letter_space;
        int height;
    };

    LcdDisplay* parent_;  // 用于 DisplayLockGuard 互斥
    lv_obj_t* overlay_       = nullptr;
    lv_obj_t* top_label_     = nullptr;
    lv_obj_t* main_label_    = nullptr;
    lv_obj_t* sub_label_     = nullptr;
    lv_obj_t* explain_label_ = nullptr;  // 跑马灯（PR-5）
    lv_timer_t* auto_hide_timer_ = nullptr;

    const lv_font_t* main_56_ = nullptr;
    const lv_font_t* main_48_ = nullptr;
    const lv_font_t* main_88_ = nullptr;
    const lv_font_t* top_30_  = nullptr;
    const lv_font_t* sub_20_  = nullptr;

    void EnsureOverlay();
    void Build(const EduRow& top, const EduRow& main_row, const EduRow& bottom,
               const char* explanation);
    void UpdateRow(lv_obj_t* lbl, const EduRow& row, int y);
    void UpdateRowAtBottom(lv_obj_t* lbl, const EduRow& row, int dist_from_bottom);
    void UpdateExplain(const char* text);
    static void OnClicked(lv_event_t* e);

    // 字号选档（含 88 档兜底链）
    const lv_font_t* PickMainFont(const char* text, bool is_py_mode, Mode mode);
};
```

## 三、Kconfig 配置

```kconfig
# main/Kconfig.projbuild

config USE_EDUCATION_CARD
    bool "Enable education card overlay (3-9 岁启蒙教育卡)"
    default y if BOARD_TYPE_MYDAZY_P30_4G || BOARD_TYPE_MYDAZY_P30_WIFI || BOARD_TYPE_MYDAZY_P31
    default n
    help
      启用教育卡 overlay 显示。
      - P30/P31 启蒙设备：开启（默认）
      - 通用 xiaozhi 设备：关闭（不需要教育卡）

      关闭后：
      - 节省 ~1.4 MB Flash（56 主秀字体 + 48 兜底）
      - 节省 ~10 KB 应用代码（EducationCard 类）
      - ShowEduCard MCP tool 静默 no-op
```

## 四、CMakeLists 条件编译

```cmake
# main/CMakeLists.txt

if(CONFIG_USE_EDUCATION_CARD)
    list(APPEND SOURCES "display/education_card.cc")
endif()
```

```cpp
// main/display/ui_display.cc
#ifdef CONFIG_USE_EDUCATION_CARD
#include "education_card.h"
#endif

class UiDisplay : public SpiLcdDisplay {
private:
#ifdef CONFIG_USE_EDUCATION_CARD
    std::unique_ptr<EducationCard> edu_card_;
#endif
    // ...
};

void UiDisplay::ShowEduCard(const char* category, const char* main_text,
                             const char* top, const char* bottom) {
#ifdef CONFIG_USE_EDUCATION_CARD
    if (!edu_card_) {
        edu_card_ = std::make_unique<EducationCard>(this);
        edu_card_->SetFonts(edu_main_56_font_, edu_main_font_,
                            clock_big_font_,    // 88 px
                            clock_text_font_, &g_text_font);
    }
    edu_card_->Show(category, main_text, top, bottom);
#else
    (void)category; (void)main_text; (void)top; (void)bottom;  // no-op
#endif
}
```

## 五、88 px 接入策略（同步落到独立类内）

`PickMainFont` 实现：

```cpp
const lv_font_t* EducationCard::PickMainFont(const char* text, bool is_py_mode, Mode mode) {
    int n = utf8_char_count(text);

    if (is_py_mode) {
        // PY-mode (汉字主秀): ≤ 4 字 → 56；> 4 字 → nullptr (跳过激活)
        // 88 档汉字目前未实现（无 CJK 字符集），即使 kSuperSize 也走 56
        if (n < 1 || n > 4) return nullptr;
        return main_56_;
    }

    // EN-mode：56 → 48 兜底 → 跳过
    if (n < 1 || n > 12) return nullptr;

    // ⭐ 主动学习场景（kSuperSize）：≤ 5 字符英文优先 88
    if (mode == Mode::kSuperSize && n <= 5 && main_88_) {
        int w88 = lv_text_get_width(text, strlen(text), main_88_, 0);
        if (w88 <= 280) return main_88_;
    }

    // 标准 EN-mode 选档
    int w56 = lv_text_get_width(text, strlen(text), main_56_, 0);
    if (w56 <= 280) return main_56_;
    int w48 = lv_text_get_width(text, strlen(text), main_48_, 0);
    if (w48 <= 280) return main_48_;
    return nullptr;
}
```

**调用方何时用 kSuperSize**：

```cpp
// 主动学习场景（云端 LLM 通过 MCP tool 显式传 prefer_super_size=true）
edu_card_->Show("letter", "Aa", nullptr, "苹果",
                EducationCard::Mode::kSuperSize, "apple, ant", 5000);
// 结果：Aa 主秀 88 px (~75 px 宽，占屏 26%)，启蒙感冲击拉满

// 被动触发（默认）
edu_card_->Show("word", "apple", "ap·ple", "苹果");
// 结果：apple 主秀 56 px（不抢戏）
```

## 六、收益对照

| 维度 | 当前 | 独立化后 |
|---|---|---|
| 教育卡代码位置 | ui_display.cc 散落 200+ 行 | education_card.cc 集中 250 行 |
| 维护性 | ❌ 跟时钟/QR/Player 挤在一起 | ✅ 独立类，单一职责 |
| 不需要的板子 | ❌ 强制编译进固件 | ✅ Kconfig 关掉省 ~10 KB |
| 88 主动学习档 | ❌ 没接入 | ✅ Mode 参数控制 |
| 跑马灯讲解（PR-5）| ❌ 未实现 | ✅ explain_label_ + 自动隐藏 |
| MCP show_word_card | ❌ 未实现 | ✅ 直接调 edu_card_->Show(... kSuperSize) |
| 上游 rebase | 中（动 ui_display.cc 风险高）| 低（独立文件，主路径不动）|

## 七、实施工作量

| 阶段 | 内容 | 工作量 |
|---|---|---|
| Phase 1 | 新建 education_card.h（声明 + EduRow 移入）| 30 min |
| Phase 2 | 新建 education_card.cc（搬运 + 88 档接入）| 1.5 h |
| Phase 3 | ui_display.h/cc 委托改造 | 1 h |
| Phase 4 | Kconfig + CMakeLists 条件编译 | 30 min |
| Phase 5 | 编译验证 + 真机烧录 | 1 h |
| **总** | | **4-5 小时** |

## 八、Risk

- **R1**：lv_obj 销毁顺序——overlay 是 screen 子节点，析构需要在 ~UiDisplay 之前。**对策**：unique_ptr 在 UiDisplay 字段中，析构顺序天然倒序。
- **R2**：DisplayLockGuard 跨类引用——EducationCard 持有 LcdDisplay*，调用 `DisplayLockGuard lock(parent_)`。**对策**：parent_ 必须保活——EducationCard 的生命周期由 UiDisplay 管理，不会先于 parent 析构。
- **R3**：教育卡关闭时仍要让 ShowEduCard 接口可调用（避免 application.cc 编译失败）。**对策**：`#ifndef CONFIG_USE_EDUCATION_CARD` 时 ShowEduCard 实现为空函数（no-op）。
