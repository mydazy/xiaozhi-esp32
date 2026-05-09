# 教育卡 LVGL 升级 Plan（v1 → v8）

> 现有 `UiDisplay::ShowEduCard()` (ui_display.cc:1009) 是 v1 实现（main 48 / bottom 48）
> 升级到 v8 启蒙定版（main 56 / top 30 / sub 20 + 激活校验 + 跑马灯）

## 一、改造范围（5 处）

| # | 改造点 | 文件 | 行 | 工作量 |
|---|---|---|---|---|
| 1 | 加 56/48 px 字体加载 | ui_display.cc/h | EnsureDisplayFonts | 30 min |
| 2 | should_activate_card 校验 | ui_display.cc | ShowEduCard 入口 | 30 min |
| 3 | EduRow 字号锚点重排 | ui_display.cc | BuildEduCard | 1 h |
| 4 | row-explain 跑马灯（新）| ui_display.cc/h | 新增成员 + 函数 | 2 h |
| 5 | MCP show_word_card 注册 | mcp_server.cc | AddCommonTools | 1 h |

**总：约 5 小时**

## 二、字体接入（改造点 1）

**新增 3 个字体成员**（ui_display.h:117 附近）：

```cpp
// 教育卡 v8 三档字号
const lv_font_t* edu_main_56_font_  = nullptr;  // 主秀 56 Bold（中+英+拼音）
const lv_font_t* edu_main_48_font_  = nullptr;  // EN 兜底 48 Bold（仅 ASCII）
const lv_font_t* edu_top_30_font_   = nullptr;  // 顶部 30 SemiBold（ASCII+拼音）
// 副位 20 复用现有 clock_text_font_（已有 30），新加 20 px：
const lv_font_t* edu_sub_20_font_   = nullptr;  // 副位 20 Regular（中+英）
```

**在 EnsureDisplayFonts() 中加载**：

```cpp
extern const lv_font_t font_maru_56_4;
extern const lv_font_t font_maru_eng_48_4;
extern const lv_font_t font_maru_30_4;
extern const lv_font_t font_maru_20_4;

if (!edu_main_56_font_) edu_main_56_font_ = &font_maru_56_4;
if (!edu_main_48_font_) edu_main_48_font_ = &font_maru_eng_48_4;
if (!edu_top_30_font_)  edu_top_30_font_  = &font_maru_30_4;
if (!edu_sub_20_font_)  edu_sub_20_font_  = &font_maru_20_4;
```

## 三、激活校验（改造点 2）

**新增辅助函数**（ui_display.cc 文件顶部 anonymous namespace 内）：

```cpp
namespace {

// 判定 left 是拼音（含声调字符）还是英文（纯 ASCII）
bool IsPinyin(const char* text) {
    if (!text) return false;
    while (*text) {
        unsigned char c = (unsigned char)*text;
        if (c >= 0x80) return true;  // UTF-8 多字节 = 拼音声调
        text++;
    }
    return false;  // 纯 ASCII = 英文
}

// UTF-8 字符数（不是字节数）
int Utf8CharCount(const char* text) {
    if (!text) return 0;
    int count = 0;
    while (*text) {
        if ((*text & 0xC0) != 0x80) count++;
        text++;
    }
    return count;
}

// 主秀字号选择 + 激活校验
// 返回 nullptr → 跳过激活
const lv_font_t* PickMainFont(const char* main_text, bool is_pinyin_mode,
                               const lv_font_t* font_56, const lv_font_t* font_48) {
    int n = Utf8CharCount(main_text);

    if (is_pinyin_mode) {
        // PY-mode: ≤ 4 字汉字（已在 should_activate 校验过 3000 内）
        if (n < 1 || n > 4) return nullptr;
        return font_56;
    } else {
        // EN-mode: 56 → 48 → 跳过
        if (n < 1 || n > 12) return nullptr;
        // 实测 56 是否装下
        int w56 = lv_text_get_width(main_text, strlen(main_text), font_56, 0);
        if (w56 <= 280) return font_56;
        // 兜底 48
        int w48 = lv_text_get_width(main_text, strlen(main_text), font_48, 0);
        if (w48 <= 280) return font_48;
        return nullptr;  // 48 也装不下，跳过
    }
}

}  // anonymous namespace
```

> ⚠ 注：`all_in_3000(text)` 暂不实现（依赖 3000 字哈希表），改用"56 字体能渲染就用，渲染不了 LVGL 自动 fallback 到 20"——通过字体覆盖范围天然过滤。

## 四、布局重排（改造点 3）

**新版 BuildEduCard 锚点逻辑**（替换现有 ui_display.cc:980-1007）：

```cpp
void UiDisplay::BuildEduCard(const EduRow& top, const EduRow& main_row,
                              const EduRow& bottom, const char* explanation) {
    DisplayLockGuard lock(this);
    HideQrCode();
    EnsureEduCardOverlay();
    if (!edu_card_overlay_) return;

    // v8 锚点（240 px 屏高）：
    //   PY-mode: 顶 50 / 主 100 / (无副位) — 间距 20
    //   EN-mode: (无顶部) 主 80 / 副 bottom:64 — 间距 20
    //   EN+Phonics: 顶 28 / 主 80 / 副 bottom:64 — 间距 22/20
    bool has_top = top.text && top.text[0];
    bool has_bottom = bottom.text && bottom.text[0];

    int main_top, top_y;
    if (has_bottom) {
        // EN-mode（含副位）
        top_y = 28;  // 顶部 Phonics（如有）
        main_top = 80;
    } else {
        // PY-mode（仅顶 + 主，无副位）
        top_y = 50;
        main_top = 100;
    }

    UpdateEduRow(edu_top_label_, top, top_y);
    UpdateEduRow(edu_main_label_, main_row, main_top);

    if (has_bottom) {
        // 副位用 bottom 定位（距底 64 → 距主行 20）
        UpdateEduRowBottom(edu_bottom_label_, bottom, 64);
    } else {
        lv_obj_add_flag(edu_bottom_label_, LV_OBJ_FLAG_HIDDEN);
    }

    // 跑马灯讲解（仅 explanation 非空时）
    UpdateEduExplain(explanation);

    lv_obj_remove_flag(edu_card_overlay_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(edu_card_overlay_);
}
```

**新增 UpdateEduRowBottom**（用 LV_ALIGN_BOTTOM_MID）：

```cpp
void UiDisplay::UpdateEduRowBottom(lv_obj_t* lbl, const EduRow& row, int dist_from_bottom) {
    if (!lbl) return;
    if (!row.text || !row.text[0]) {
        lv_obj_add_flag(lbl, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_label_set_text(lbl, row.text);
    lv_obj_set_style_text_font(lbl, row.font, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(row.color), 0);
    lv_obj_set_style_text_letter_space(lbl, row.letter_space, 0);
    lv_obj_align(lbl, LV_ALIGN_BOTTOM_MID, 0, -dist_from_bottom);
    lv_obj_remove_flag(lbl, LV_OBJ_FLAG_HIDDEN);
}
```

## 五、跑马灯讲解（改造点 4）

**新增成员**（ui_display.h）：

```cpp
lv_obj_t* edu_explain_label_ = nullptr;  // 底部跑马灯（explanation 非空时显示）
lv_timer_t* edu_auto_hide_timer_ = nullptr;  // duration_ms 后自动隐藏
```

**新增函数**（ui_display.cc）：

```cpp
void UiDisplay::UpdateEduExplain(const char* explanation) {
    if (!edu_explain_label_) {
        edu_explain_label_ = lv_label_create(edu_card_overlay_);
        lv_label_set_long_mode(edu_explain_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_set_width(edu_explain_label_, 280);
        lv_obj_set_style_text_align(edu_explain_label_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(edu_explain_label_, edu_sub_20_font_, 0);
        lv_obj_set_style_text_color(edu_explain_label_,
                                     lv_color_white(), 0);
        lv_obj_set_style_text_opa(edu_explain_label_, LV_OPA_60, 0);  // 半透明白
        lv_obj_set_style_text_letter_space(edu_explain_label_, 2, 0);
        lv_obj_set_style_anim_speed(edu_explain_label_, 40, 0);  // 40 px/s
        lv_obj_align(edu_explain_label_, LV_ALIGN_BOTTOM_MID, 0, -28);
    }

    if (explanation && explanation[0]) {
        lv_label_set_text(edu_explain_label_, explanation);
        lv_obj_remove_flag(edu_explain_label_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(edu_explain_label_, LV_OBJ_FLAG_HIDDEN);
    }
}

// duration_ms 后自动 HideEduCard
void UiDisplay::ScheduleAutoHide(int duration_ms) {
    if (edu_auto_hide_timer_) {
        lv_timer_delete(edu_auto_hide_timer_);
        edu_auto_hide_timer_ = nullptr;
    }
    if (duration_ms > 0) {
        edu_auto_hide_timer_ = lv_timer_create(
            [](lv_timer_t* t) {
                auto* self = static_cast<UiDisplay*>(lv_timer_get_user_data(t));
                self->HideEduCard();
                lv_timer_delete(t);
                self->edu_auto_hide_timer_ = nullptr;
            },
            duration_ms, this);
        lv_timer_set_repeat_count(edu_auto_hide_timer_, 1);
    }
}
```

## 六、ShowEduCard 升级（改造点 3 收尾）

**新签名**（向后兼容）：

```cpp
// ui_display.h:100 替换
void ShowEduCard(const char* category, const char* main_text,
                 const char* top, const char* bottom,
                 const char* explanation = nullptr,   // 新增：跑马灯
                 int duration_ms = 0);                 // 新增：0=不自动隐藏
```

**实现**（替换 ui_display.cc:1009-1049）：

```cpp
void UiDisplay::ShowEduCard(const char* category, const char* main_text,
                             const char* top, const char* bottom,
                             const char* explanation, int duration_ms) {
    if (!main_text || !main_text[0]) return;
    EnsureDisplayFonts();
    if (!edu_main_56_font_ || !edu_top_30_font_ || !edu_sub_20_font_) {
        ESP_LOGW(TAG, "EduCard skipped: v8 fonts not loaded");
        return;
    }
    ResetFontMode();

    // 模式判定
    bool is_py = (top && top[0] && IsPinyin(top));  // top 是拼音 → PY-mode

    // 字号 + 激活校验
    const lv_font_t* main_font = PickMainFont(
        main_text, is_py, edu_main_56_font_, edu_main_48_font_);
    if (!main_font) {
        ESP_LOGI(TAG, "EduCard skipped: text=%s 超激活范围", main_text);
        return;  // 跳过激活，保持当前画面
    }

    // 字号选 56 还是 48
    int main_h = (main_font == edu_main_56_font_) ? 56 : 48;
    int main_ls = is_py ? 3 : 0;  // 圆角字距 PY 3 / EN 0

    // 三行 EduRow（v8 颜色：金 #FFCA28 / 绿 #A5D6A7）
    EduRow t{top,       edu_top_30_font_, 0xA5D6A7, 2, 30};   // 顶部薄荷绿
    EduRow m{main_text, main_font,        0xFFCA28, main_ls, main_h};  // 主秀亮金
    EduRow b{bottom,    edu_sub_20_font_, 0xA5D6A7, 3, 20};   // 副位薄荷绿

    BuildEduCard(t, m, b, explanation);
    if (duration_ms > 0) ScheduleAutoHide(duration_ms);

    ESP_LOGI(TAG, "EduCard v8[%s] main=%s(%d) top=%s expl=%s dur=%d",
             category, main_text, main_h, top ? top : "",
             explanation ? "Y" : "N", duration_ms);
}
```

## 七、MCP show_word_card tool（改造点 5）

**注册**（mcp_server.cc 的 `AddCommonTools()` 内）：

```cpp
AddTool(
    "self.education.show_word_card",
    "Show an education card with English word + Chinese + Phonics syllables. "
    "Use during storytelling to teach vocabulary in context for 3-9 year olds.",
    PropertyList({
        Property("word", kPropertyTypeString),
        Property("chinese", kPropertyTypeString),
        Property("syllables", kPropertyTypeString, std::string("")),
        Property("story_context", kPropertyTypeString, std::string("")),
        Property("duration_ms", kPropertyTypeInteger, 4000, 1000, 10000),
    }),
    [](const PropertyList& props) -> ReturnValue {
        auto* board = Board::GetInstance();
        auto* display = static_cast<UiDisplay*>(board->GetDisplay());
        if (!display) return false;
        std::string word = props["word"].value<std::string>();
        std::string chinese = props["chinese"].value<std::string>();
        std::string syllables = props["syllables"].value<std::string>();
        std::string ctx = props["story_context"].value<std::string>();
        int duration = props["duration_ms"].value<int>();
        display->ShowEduCard(
            "word", word.c_str(),
            syllables.empty() ? nullptr : syllables.c_str(),
            chinese.c_str(),
            ctx.empty() ? nullptr : ctx.c_str(),
            duration);
        return true;
    });
```

## 八、PY-mode 接入（改造点 5 续）

**已存在的 `self.education.show_card` tool**（如有）继续兼容，触发条件：
- top = 拼音（"māo"）→ IsPinyin() = true → PY-mode
- main = 汉字（"猫"）
- bottom 留空 → 不显示副位

服务端继续用现有协议下发，无需改动。

## 九、量产纪律（红线）

- ❌ **不新增 SceneType / DeviceState 枚举** — 教育卡是 overlay，不占场景维度（已落档）
- ❌ **不动 EduRow 数据结构** — 仅扩 ShowEduCard 形参（向后兼容默认值）
- ❌ **不改主屏 / QR / Player 等其他 page** — 改造严格限于 ShowEduCard 调用链
- ✅ **edu_auto_hide_timer_ 必须 lv_timer_delete** — 防 LVGL 9 dangling timer
- ✅ **跑马灯标签首次创建后复用** — 跟 edu_card_overlay_ 同生命周期，不删
- ✅ **激活校验失败静默 return** — 保持当前画面（emoji/对话）

## 十、分阶段提交

| PR | 内容 | 验证 |
|---|---|---|
| **PR-1** | gen_edu_fonts.py 跑通 + 三个 .bin 文件入库 | 字体生成成功，体积符合预期 |
| **PR-2** | EnsureDisplayFonts 加载新字体 + 现有 ShowEduCard 临时改 56 主字 | 真机看 "猫"、"apple" 是 56 px |
| **PR-3** | should_activate_card 激活校验 + 跳过路径 | 真机推 "饕"/"communication" 不显示 |
| **PR-4** | 锚点重排 + EN 副位上提 | 真机看 "apple|苹果" 间距 20 px |
| **PR-5** | 跑马灯 + auto-hide | 真机推 explanation 看跑马灯 |
| **PR-6** | MCP show_word_card 注册 | 服务端测试触发 |

每 PR 单独烧板验证，避免一次性大改导致定位困难。

## 十一、上游 rebase 友好

**改动文件**：
- `ui_display.h` — 加 4 个字体成员 + ShowEduCard 形参扩
- `ui_display.cc` — 改 BuildEduCard / ShowEduCard / 加 4 个 helper
- `mcp_server.cc` — 加 1 个 tool 注册（独立块）
- `78__xiaozhi-fonts/scripts/gen_edu_fonts.py` — 新文件
- `78__xiaozhi-fonts/cbin/font_maru_*.bin` — 新文件
- `78__xiaozhi-fonts/include/cbin_font.h` — 加字体声明

**上游 rebase 时易合并**：上游 xiaozhi-esp32 没有 ShowEduCard，本仓库私有功能，不冲突。
