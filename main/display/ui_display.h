#ifndef UI_DISPLAY_H
#define UI_DISPLAY_H

#include "lcd_display.h"
#include "../scene_type.h"
#include <lvgl.h>
#include <functional>
#include <atomic>

class ControlCenter;

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

    // ===== 番茄钟页（参考时钟：88px 倒计时 + 64×64 启停圆按钮）=====
    using PomodoroToggleCb = std::function<void()>;
    void SwitchToPomodoroMode(uint32_t remain_sec, bool running);
    void SwitchOutPomodoroMode();
    void UpdatePomodoro(uint32_t remain_sec, bool running);
    void OnPomodoroToggle(PomodoroToggleCb cb) { on_pomodoro_toggle_ = std::move(cb); }
    bool IsPomodoroMode() const { return active_scene_ == SceneType::kPomodoro; }

    SceneType GetCurrentScene() const { return active_scene_; }

    void FinishBootAndShowClock();

    // 显示笔画 GIF 动画（写字识字）——把 PSRAM buffer 装入 emoji_collection "font" 槽位
    void FontGif(uint8_t* gif_buffer, size_t size, uint32_t request_id = 0);

    void HideFontGif();

    // 申请一次"笔画 GIF 装载"许可：
    uint32_t BeginFontPending();
    void CancelFontPending(uint32_t request_id);

    // 笔画 GIF 是否有"待装载"的下载窗口（in_font_mode_ 之前的中间态）
    bool IsFontPending() const { return font_pending_.load(); }

    void SetEmotion(const char* emotion) override;
    void SetChatMessage(const char* role, const char* content) override;
    void ShowControlCenter();
    void HideControlCenter();
    bool IsControlCenterVisible() const;

    // 显示教育卡（overlay 模式，单一两行布局 · 不分类）
    //   main: 主秀大字（56 默认 / 48 EN 兜底 · 自动判定 CJK/英文）
    //   top:  顶部辅助文字（30 px 薄荷绿 · 拼音 / 中文释义 / 类别）· 可空
    // 触屏点击退出。in_font_mode_ 时屏蔽。
    void ShowEduCard(const char* main, const char* top);
    void HideEduCard();
    bool IsEduCardActive() const {
        return edu_card_overlay_ != nullptr &&
               !lv_obj_has_flag(edu_card_overlay_, LV_OBJ_FLAG_HIDDEN);
    }

    // 关于/设备信息页（overlay 只读 · 控制中心入口触发 · 点按任意处返回）
    void ShowAboutPage();
    void HideAboutPage();
    bool IsAboutPageActive() const {
        return about_overlay_ != nullptr &&
               !lv_obj_has_flag(about_overlay_, LV_OBJ_FLAG_HIDDEN);
    }

    // 笔画 GIF 是否显示中（供 application.cc 教育卡链式弹卡判定·避免 FontGIF 期间空转）
    bool IsFontGifActive() const { return in_font_mode_; }

private:

    // 时钟主屏（内联实现，无独立页面类）
    lv_obj_t* clock_container_  = nullptr;
    lv_obj_t* clock_time_label_ = nullptr;
    lv_obj_t* clock_date_label_ = nullptr;
    lv_obj_t* clock_week_label_ = nullptr;
    const lv_font_t* clock_big_font_  = nullptr;   // 88px cbin · 时钟主屏时间专用（数字 0-9 + :）· 不参与教育卡
    const lv_font_t* clock_text_font_ = nullptr;   // 30px cbin · 主屏日期/星期 + 教育卡顶部拼音/中文释义
    const lv_font_t* edu_main_font_   = nullptr;   // 48px cbin · 教育卡 EN 兜底 11-12 字符英文
    const lv_font_t* edu_main_56_font_ = nullptr;  // 56px cbin · 教育卡主秀 Bold · GB 2312 + ASCII

    lv_obj_t* qr_overlay_ = nullptr;
    lv_obj_t* edu_card_overlay_ = nullptr;
    lv_obj_t* edu_top_label_    = nullptr;
    lv_obj_t* edu_main_label_   = nullptr;
    static void OnEduCardClicked(lv_event_t* e);

    lv_obj_t* about_overlay_   = nullptr;
    lv_obj_t* about_net_value_ = nullptr;   // 网络状态值（Show 时刷新）
    static void OnAboutClicked(lv_event_t* e);

    lv_obj_t* boot_brand_label_ = nullptr;   // 开机品牌字 "MyDazy"（SetupUI 创建）

    static void OnFontExitClicked(lv_event_t* e);

    // bottom_bar（屏幕底部字幕条 · "长按说话"等）显隐
    //   font 模式下隐藏让用户聚焦写字 GIF；其他场景显示
    void ShowBottomBar();
    void HideBottomBar();
    struct EduRow {
        const char* text;            // null 或空字符串则该行不渲染
        const lv_font_t* font;
        uint32_t color;              // 0xRRGGBB
        int letter_space;
        int height;                  // 字体高度 px，用于布局计算
    };
    void RenderEduCardLayout(const EduRow& top, const EduRow& main_row);
    void EnsureEduCardOverlay();                                        // 懒创建 overlay + 3 label 槽
    void UpdateEduRow(lv_obj_t* lbl, const EduRow& row, int y);         // 更新单个 label 槽（LV_ALIGN_TOP_MID）
    

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

    // 番茄钟页（懒加载，首次 SwitchToPomodoroMode 时构建）
    lv_obj_t* pomodoro_container_  = nullptr;
    lv_obj_t* pomodoro_time_label_ = nullptr;   // 88px 倒计时 "MM:SS"
    lv_obj_t* pomodoro_btn_        = nullptr;   // 64×64 启停圆按钮
    lv_obj_t* pomodoro_btn_icon_   = nullptr;   // FONT_AWESOME_PAUSE / PLAY
    bool      pomodoro_is_running_ = false;
    PomodoroToggleCb on_pomodoro_toggle_;

    const lv_font_t* fallback_text_font_ = nullptr;

    SceneType active_scene_ = SceneType::kChat;

    bool in_font_mode_ = false;
    std::atomic<bool>     font_pending_{false};
    std::atomic<uint32_t> font_request_next_{0};
    std::atomic<uint32_t> font_request_active_{0};

    std::unique_ptr<ControlCenter> control_center_;

    // ===== 内部方法 =====
    void EnsureControlCenter();             // 懒创建 ControlCenter + 绑定 6 个回调

    void CreateClockPage();
    void UpdateClockTime();
    void EnsureDisplayFonts();          // cbin 字体延迟加载（assets 就绪后）
    void LoadFallbackTextFont();    // BUILTIN_TEXT_FONT 缺字 fallback（GB 2312 全字 cbin）

    // 音乐播放器页内部方法
    void CreatePlayerPage();
    static void OnPlayerPlayPauseClicked(lv_event_t* e);
    static void PlayerTickCb(lv_timer_t* t);
    static void FormatTime(int ms, char* buf, size_t buf_size);

    // 番茄钟页内部方法
    void CreatePomodoroPage();
    static void OnPomodoroBtnClicked(lv_event_t* e);
    static void FormatPomodoroTime(uint32_t remain_sec, char* buf, size_t buf_size);

    void StartBootAnimation();

    void SetTopBarIconsVisible(bool visible);   // network/battery/mute 三 icon 一次切换
    void RaiseStatusBar();                       // remove HIDDEN + opa COVER + move_foreground

};

#endif  // UI_DISPLAY_H
