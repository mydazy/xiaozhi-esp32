#ifndef UI_DISPLAY_H
#define UI_DISPLAY_H

#include "lcd_display.h"
#include "../scene_type.h"
#include <lvgl.h>
#include <functional>

// [量产稳定期] ControlCenter 已下线，恢复时取消注释 forward decl 与 unique_ptr 字段
// class ControlCenter;

/**
 * UiDisplay — 带交互式 UI 的 LCD 显示（仅 mydazy-p30-4g / mydazy-p31 使用）
 *
 * 继承: Display → LvglDisplay → LcdDisplay → SpiLcdDisplay → UiDisplay
 *
 * 承载三个自绘页面（全部内联在 .cc 中，不再拆子类）：
 *   ├─ 时钟主屏  (idle)  — 88px 时间 + 日期 + 星期
 *   ├─ 配网 QR 页        — 蓝牙/热点模式切换 + QR + 左右色条
 *   └─ 激活绑定页        — URL QR + 6 位激活码
 *
 * 附带：
 *   ├─ 全局状态栏（clock/chat 共享，PNG 图标）
 *   ├─ 开机动画（logo 渐入 → 持续显示 → Idle 时由状态机驱动 fade_out → 切时钟）
 *   └─ 控制中心（下拉手势触发，懒加载）
 */
class UiDisplay : public SpiLcdDisplay {
public:
    UiDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
              int width, int height, int offset_x, int offset_y,
              bool mirror_x, bool mirror_y, bool swap_xy);
    ~UiDisplay() override;

    // ===== SetupUI override =====
    void SetupUI() override;
    void UpdateStatusBar(bool update_all = false) override;

    // ===== 通用二维码页（覆盖配网/绑定/付费等所有场景） =====
    // 第 2 参数 = 加亮核心信息（设备名/激活码/金额）→ 蓝色大字突出
    void ShowQrCode(const char* qr_content,
                    const char* highlight = nullptr,
                    const char* top = nullptr,
                    const char* bottom = nullptr,
                    const char* left_label = nullptr,
                    const char* right_label = nullptr,
                    bool active_left = true,
                    std::function<void()> on_double_click = nullptr) override;
    void HideQrCode() override;

    // ===== 主屏切换 =====
    void SwitchToClockMode();    // idle → 时钟主屏
    void SwitchToChatMode();     // 对话 → 聊天 UI
    bool IsClockMode() const { return current_scene_ == SceneType::kClock; }

    // ===== 音乐播放器页（极简：曲名 + Play/Pause + 进度条）=====
    using PlayerPauseToggleCb = std::function<void()>;
    void SwitchToPlayerMode(const char* title);
    void SwitchOutPlayerMode();
    void UpdatePlayerProgress(int position_ms, int total_ms);
    void SetPlayerPaused(bool paused);
    void OnPlayerPauseToggle(PlayerPauseToggleCb cb) { on_player_pause_toggle_ = std::move(cb); }
    bool IsPlayerMode() const { return current_scene_ == SceneType::kPlayer; }

    // 三维心智模型（C 维度）查询入口：UI 场景互斥维度（替代 is_clock_mode_ / is_player_mode_）
    SceneType GetCurrentScene() const { return current_scene_; }

    // 开机引导结束：logo fade_out → SwitchToClockMode（幂等）
    // 仅由 Application::HandleStateChangedEvent(Idle) 调用，确保联网+激活完成才切时钟
    void FinishBootAndShowClock();

    // 用 PSRAM GIF buffer 替换 emoji_collection 中 "font" 槽位（识字笔画动画）
    // gif_buffer 由 heap_caps_malloc(MALLOC_CAP_SPIRAM) 分配，所有权转移给 EmojiCollection。
    // 调用方完成调用后不要再 free buffer。
    void UpdateFontGif(uint8_t* gif_buffer, size_t size);

    // 教育卡专属：当前为 font 时跳过 neutral（application.cc 进 listening 默认注入），
    // 其他 emotion 正常替换；font 模式额外隐藏 bottom_bar_ 让出底部视觉。
    // 写字 GIF 原始尺寸 200×200，在 284×240 屏上居中即可，不做缩放。
    void SetEmotion(const char* emotion) override;

    // font 模式（识字写字）静默丢弃字幕——避免 LLM 中途推送 SetChatMessage
    // 把 bottom_bar_ 重新 unhide 出来，破坏写字 GIF 的沉浸感。
    void SetChatMessage(const char* role, const char* content) override;

    // ===== 控制中心 =====
    // [量产稳定期] ControlCenter 整体下线，保留接口为 stub 维持三个 board 的调用兼容
    void ShowControlCenter() {}
    void HideControlCenter() {}
    bool IsControlCenterVisible() const { return false; }

    // ===== 教育卡（统一接口）=====
    // overlay 模式，与 QR 同 — 不占 SceneType（C 维），盖在 chat/clock 之上
    // 触屏点击退出。top/bottom 可空字符串（该行不显示）。
    //
    // category 取值（5-10 岁三年级以下儿童被动跟读场景）：
    //   "word"   英文单词/拼写： top=拼读/音标, main=英文单词(金黄, 字距 4),     bottom=中文释义（带分隔线）
    //   "hanzi"  汉字组词：       top=拼音,       main=汉字(白色, 字距 8 加宽),     bottom=组词
    //   "pinyin" 拼音/声韵母：    top=类别,       main=声韵母(橙红, 字距 6),         bottom=例字
    // 主体一律 48px（edu_main_font_，CJK 走 fallback 链）/ 副字一律 20px（g_text_font 含 GB2312 全字 fallback）
    void ShowEduCard(const char* category, const char* main_text,
                     const char* top, const char* bottom);
    void HideEduCard();
    bool IsEduCardActive() const {
        return edu_card_overlay_ != nullptr &&
               !lv_obj_has_flag(edu_card_overlay_, LV_OBJ_FLAG_HIDDEN);
    }

private:

    // 时钟主屏（内联实现，无独立页面类）
    lv_obj_t* clock_container_  = nullptr;
    lv_obj_t* clock_time_label_ = nullptr;
    lv_obj_t* clock_date_label_ = nullptr;
    lv_obj_t* clock_week_label_ = nullptr;
    const lv_font_t* clock_big_font_  = nullptr;   // 88px cbin · 时钟数字 0-9 + : · v8 同时承载主动学习超大主秀 (英文+大小写+加减乘除·~89 KB)
    const lv_font_t* clock_text_font_ = nullptr;   // 30px cbin · 主屏日期/星期 + 教育卡顶部拼音/Phonics + IPA (~79 KB)
    const lv_font_t* edu_main_font_   = nullptr;   // 48px cbin · v8 EN 兜底 11-12 字符英文（仅 ASCII+拼音+Phonics·77 KB·v3 已砍中文）
    const lv_font_t* edu_main_56_font_ = nullptr;  // 56px cbin · v8 主秀 Bold · GB 2312 一级 3755 字 + ASCII + 拼音 + Phonics · 1bpp 压缩 (~1.3 MB)

    // 通用二维码 overlay（配网 / 绑定 / 付费等场景共享，同时只有一个）
    lv_obj_t* qr_overlay_ = nullptr;

    // 教育卡 overlay（单词 / 拼音 / 汉字组词）— 复用单一 overlay + 3 个 label
    // 设计：第一次 ShowEduCard 时懒创建，后续仅更新 label 内容/字体/位置；
    //      HideEduCard 只 set HIDDEN flag，不删 overlay → 杜绝 lv_obj_del 与 label 异步
    //      layout cb 的 race，从根本上避免 LVGL 9 的 lv_event_mark_deleted UAF。
    lv_obj_t* edu_card_overlay_ = nullptr;
    lv_obj_t* edu_top_label_    = nullptr;
    lv_obj_t* edu_main_label_   = nullptr;
    lv_obj_t* edu_bottom_label_ = nullptr;
    static void OnEduCardClicked(lv_event_t* e);

    // 清场 helper：font 模式（GIF 笔画）退出 + 状态切换前调用
    // 防止 font 状态在 SwitchTo* 之间残留导致 emoji_image 仍显示 GIF
    void ResetFontMode();

    // GIF 笔画字触屏退出回调（与教育卡 OnEduCardClicked 同款机制）
    // 单击 emoji_box 区域 → ResetFontMode → 切回 neutral 表情
    // 仅 font 模式生效（其他 emotion 时回调内 early return）
    static void OnFontExitClicked(lv_event_t* e);

    // 教育卡布局：[top 30px] + main 48px + bottom 48px（top 可空 · 整体上下居中）
    //   MCP 调用 show_card (category="word"/"pinyin") 传 top → 三行（拼读/拼音）
    //   聊天正则提取 [part1-part2] 传 top="" → 两行（更紧凑）
    struct EduRow {
        const char* text;            // null 或空字符串则该行不渲染
        const lv_font_t* font;
        uint32_t color;              // 0xRRGGBB
        int letter_space;
        int height;                  // 字体高度 px，用于布局计算
    };
    void BuildEduCard(const EduRow& top, const EduRow& main_row, const EduRow& bottom);
    void EnsureEduCardOverlay();                                        // 懒创建 overlay + 3 label 槽
    void UpdateEduRow(lv_obj_t* lbl, const EduRow& row, int y);         // 更新单个 label 槽（LV_ALIGN_TOP_MID）
    void UpdateEduRowAtBottom(lv_obj_t* lbl, const EduRow& row, int dist_from_bottom);  // 副位定位（LV_ALIGN_BOTTOM_MID）

    // 整页双击 callback（仅显示左右色条时有效，用于切换模式）
    // 必须保活直到 HideQrCode，否则 lambda 析构后 click 事件触发 UAF
    std::function<void()> qr_double_click_cb_;
    uint64_t              qr_last_click_us_ = 0;
    static void OnQrClicked(lv_event_t* e);

    // 音乐播放器页（懒加载，首次 SwitchToPlayerMode 时构建）
    lv_obj_t* player_container_  = nullptr;
    lv_obj_t* player_title_      = nullptr;
    lv_obj_t* player_btn_play_   = nullptr;
    lv_obj_t* player_play_icon_  = nullptr;
    lv_obj_t* player_progress_   = nullptr;     // lv_bar，仅显示不可拖
    lv_obj_t* player_time_cur_   = nullptr;
    lv_obj_t* player_time_total_ = nullptr;
    lv_timer_t* player_tick_     = nullptr;     // 200ms 自动刷新进度
    bool is_player_paused_       = false;
    PlayerPauseToggleCb on_player_pause_toggle_;

    // 开机动画：logo 持续显示，结束时机由状态机驱动（FinishBootAndShowClock）

    // [量产稳定期] ControlCenter 字段已下线（恢复时改回 std::unique_ptr<ControlCenter> control_center_）

    // BUILTIN_TEXT_FONT 的补字字体：与主字体同名约定（font_maru_common_20_4.bin），
    // 链入主字体仅 ~600 字常用文案，cbin 字体补 GB 2312 全字（7000+），LVGL 缺字自动 fallback。
    const lv_font_t* fallback_text_font_ = nullptr;

    // 状态：UI 场景维度（详见 docs/p30-architecture.html § 一.5 三维心智模型 · C 维度）
    // 默认 kEmoji（启动时 emoji_box 显 logo · 进 chat 后显表情都属此场景）
    SceneType current_scene_ = SceneType::kEmoji;

    // 当前是否在显示 font GIF（仅用于 SetEmotion 判 "跳过 neutral"）
    bool current_is_font_ = false;

    // ===== 内部方法 =====
    void CreateClockPage();
    void UpdateClockTime();
    void EnsureDisplayFonts();          // cbin 字体延迟加载（assets 就绪后）
    void LoadFallbackTextFont();    // BUILTIN_TEXT_FONT 缺字 fallback（GB 2312 全字 cbin）

    // 音乐播放器页内部方法
    void CreatePlayerPage();
    static void OnPlayerPlayPauseClicked(lv_event_t* e);
    static void PlayerTickCb(lv_timer_t* t);
    static void FormatTime(int ms, char* buf, size_t buf_size);

    void StartBootAnimation();

    // 三个 SwitchTo* 共享的状态栏管理 helper（去除重复）
    void SetTopBarIconsVisible(bool visible);   // network/battery/mute 三 icon 一次切换
    void RaiseStatusBar();                       // remove HIDDEN + opa COVER + move_foreground

    // [量产稳定期] EnsureControlCenter / EnableStatusBarTapForControlCenter / OnStatusBarClicked 已下线
};

#endif  // UI_DISPLAY_H
