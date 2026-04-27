#ifndef UI_DISPLAY_H
#define UI_DISPLAY_H

#include "lcd_display.h"
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

    // ===== 配网 QR 页（蓝牙/热点切换） =====
    // override Display 基类 6 参数虚函数，缺第 6 参数会回退到基类空实现，UI 不显示。
    void ShowWifiQrCode(const char* qr_content, const char* hint = nullptr,
                        const char* left_label = nullptr,
                        const char* right_label = nullptr,
                        bool active_left = true,
                        std::function<void()> on_double_click = nullptr) override;
    void HideWifiQrCode() override;

    // ===== 激活绑定页（URL QR + 6 位激活码） =====
    void ShowActivationPage(const char* bind_url, const char* activation_code);
    void HideActivationPage();

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

private:
    // 全局状态栏（常驻 screen 顶部，clock / chat 共享）
    lv_obj_t* global_status_bar_   = nullptr;
    lv_obj_t* status_network_icon_ = nullptr;
    lv_obj_t* status_battery_icon_ = nullptr;
    bool cached_battery_charging_  = false;

    // 时钟主屏（内联实现，无独立页面类）
    lv_obj_t* clock_container_  = nullptr;
    lv_obj_t* clock_time_label_ = nullptr;
    lv_obj_t* clock_date_label_ = nullptr;
    lv_obj_t* clock_week_label_ = nullptr;
    lv_timer_t* clock_tick_     = nullptr;
    const lv_font_t* clock_big_font_  = nullptr;   // 88px cbin（assets 就绪后加载）
    const lv_font_t* clock_text_font_ = nullptr;   // 30px cbin

    // 配网 / 激活 overlay（同时只有一个）
    lv_obj_t* wifi_qr_overlay_   = nullptr;
    lv_obj_t* activation_overlay_ = nullptr;

    // 配网双击切换：lambda 来自 WifiBoard::MakeSwitchCallback，必须保活直到 HideWifiQrCode
    std::function<void()> wifi_qr_double_click_cb_;
    uint64_t              wifi_qr_last_click_us_ = 0;
    static void OnWifiQrClicked(lv_event_t* e);

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
    void CreateGlobalStatusBar();
    void UpdateGlobalStatusIcons();

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
