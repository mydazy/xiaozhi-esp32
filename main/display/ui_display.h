#ifndef UI_DISPLAY_H
#define UI_DISPLAY_H

#include "lcd_display.h"
#include "../scene_type.h"
#include <lvgl.h>
#include <memory>
#include <functional>

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
    bool IsClockMode() const { return is_clock_mode_; }

    // ===== 音乐播放器页（极简：曲名 + Play/Pause + 进度条）=====
    using PlayerPauseToggleCb = std::function<void()>;
    void SwitchToPlayerMode(const char* title);
    void SwitchOutPlayerMode();
    void UpdatePlayerProgress(int position_ms, int total_ms);
    void SetPlayerPaused(bool paused);
    void OnPlayerPauseToggle(PlayerPauseToggleCb cb) { on_player_pause_toggle_ = std::move(cb); }
    bool IsPlayerMode() const { return is_player_mode_; }

    // 开机引导结束：logo fade_out → SwitchToClockMode（幂等）
    // 仅由 Application::HandleStateChangedEvent(Idle) 调用，确保联网+激活完成才切时钟
    void FinishBootAndShowClock();

    // ===== 控制中心 =====
    void ShowControlCenter();
    void HideControlCenter();
    bool IsControlCenterVisible() const;

    /**
     * 三维心智模型 · UI 场景维度（C）查询入口
     * 详见 docs/p30-architecture.html § 一.5
     *
     * 替代 IsClockMode() / IsPlayerMode() / IsControlCenterVisible() 等散落判断。
     * 优先级：ConfigQr（互斥独占） > Player > ControlCenter > Clock > Emoji。
     */
    SceneType GetCurrentScene() const;

private:
    // 状态栏：完全沿用父类 SpiLcdDisplay top_bar_（已透明背景 · 已含 network_label_/battery_label_）
    // SwitchToClockMode 时 top_bar_ remove HIDDEN + move_foreground 浮在 clock_container_ 之上；
    // SwitchToChatMode 时 top_bar_ HIDDEN（chat 期间 emoji 满屏不要状态栏）。

    // 时钟主屏（内联实现，无独立页面类）
    lv_obj_t* clock_container_  = nullptr;
    lv_obj_t* clock_time_label_ = nullptr;
    lv_obj_t* clock_date_label_ = nullptr;
    lv_obj_t* clock_week_label_ = nullptr;
    lv_timer_t* clock_tick_     = nullptr;
    const lv_font_t* clock_big_font_  = nullptr;   // 88px cbin（assets 就绪后加载）
    const lv_font_t* clock_text_font_ = nullptr;   // 30px cbin

    // 通用二维码 overlay（配网 / 绑定 / 付费等场景共享，同时只有一个）
    lv_obj_t* qr_overlay_ = nullptr;

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
    bool is_player_mode_         = false;
    bool is_player_paused_       = false;
    PlayerPauseToggleCb on_player_pause_toggle_;

    // 开机动画：logo 持续显示，结束时机由状态机驱动（FinishBootAndShowClock）

    // 控制中心（懒加载）
    std::unique_ptr<ControlCenter> control_center_;

    // BUILTIN_TEXT_FONT 的补字字体（覆盖 basic 缺失的 5000+ 常用汉字，如打/断/休/眠/亮/退/分 等）
    const lv_font_t* puhui_common_font_ = nullptr;

    // 状态
    bool is_clock_mode_ = false;

    // ===== 内部方法 =====
    void CreateClockPage();
    void UpdateClockTime();
    void LoadClockFonts();          // cbin 字体延迟加载（assets 就绪后）
    void LoadPuhuiCommonFont();     // puhui_common 补字字体加载 + 注入 BUILTIN_TEXT_FONT.fallback
    static void ClockTickCb(lv_timer_t* t);

    // 音乐播放器页内部方法
    void CreatePlayerPage();
    static void OnPlayerPlayPauseClicked(lv_event_t* e);
    static void PlayerTickCb(lv_timer_t* t);
    static void FormatTime(int ms, char* buf, size_t buf_size);

    void StartBootAnimation();

    void EnsureControlCenter();
};

#endif  // UI_DISPLAY_H
