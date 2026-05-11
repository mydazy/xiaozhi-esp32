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
    bool IsClockMode() const { return active_scene_ == SceneType::kClock; }

    // ===== 音乐播放器页（极简：曲名 + Play/Pause + 进度条）=====
    using PlayerPauseToggleCb = std::function<void()>;
    void SwitchToPlayerMode(const char* title);
    void SwitchOutPlayerMode();
    void UpdatePlayerProgress(int position_ms, int total_ms);
    void SetPlayerPaused(bool paused);
    void OnPlayerPauseToggle(PlayerPauseToggleCb cb) { on_player_pause_toggle_ = std::move(cb); }
    bool IsPlayerMode() const { return active_scene_ == SceneType::kPlayer; }

    SceneType GetCurrentScene() const { return active_scene_; }

    void FinishBootAndShowClock();

    // 显示笔画 GIF 动画（写字识字）——把 PSRAM buffer 装入 emoji_collection "font" 槽位
    // gif_buffer 由 heap_caps_malloc(MALLOC_CAP_SPIRAM) 分配，所有权转移给 EmojiCollection
    void FontGif(uint8_t* gif_buffer, size_t size);

    void SetEmotion(const char* emotion) override;
    void SetChatMessage(const char* role, const char* content) override;
    void ShowControlCenter() {}
    void HideControlCenter() {}
    bool IsControlCenterVisible() const { return false; }

    // 显示教育卡（overlay 模式，与 QR 同槽，盖在 chat/clock 之上）
    // category 取值（MCP 协议字段）：
    //   被动 56px 主秀：word / hanzi / pinyin / poem / topic / color
    //   主动 88px 超大：letter / phonics / math
    // top/bottom 可空字符串（该行不显示）。触屏点击退出。
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

    lv_obj_t* qr_overlay_ = nullptr;
    lv_obj_t* edu_card_overlay_ = nullptr;
    lv_obj_t* edu_top_label_    = nullptr;
    lv_obj_t* edu_main_label_   = nullptr;
    lv_obj_t* edu_bottom_label_ = nullptr;
    static void OnEduCardClicked(lv_event_t* e);

    void ResetFontMode();
    static void OnFontExitClicked(lv_event_t* e);
    struct EduRow {
        const char* text;            // null 或空字符串则该行不渲染
        const lv_font_t* font;
        uint32_t color;              // 0xRRGGBB
        int letter_space;
        int height;                  // 字体高度 px，用于布局计算
    };
    void RenderEduCardLayout(const EduRow& top, const EduRow& main_row, const EduRow& bottom);
    void EnsureEduCardOverlay();                                        // 懒创建 overlay + 3 label 槽
    void UpdateEduRow(lv_obj_t* lbl, const EduRow& row, int y);         // 更新单个 label 槽（LV_ALIGN_TOP_MID）
    void UpdateEduRowAtBottom(lv_obj_t* lbl, const EduRow& row, int dist_from_bottom);  // 副位定位（LV_ALIGN_BOTTOM_MID）

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

    const lv_font_t* fallback_text_font_ = nullptr;

    SceneType active_scene_ = SceneType::kChat;

    bool in_font_mode_ = false;

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

    void SetTopBarIconsVisible(bool visible);   // network/battery/mute 三 icon 一次切换
    void RaiseStatusBar();                       // remove HIDDEN + opa COVER + move_foreground

};

#endif  // UI_DISPLAY_H
