#ifndef UI_DISPLAY_H
#define UI_DISPLAY_H

#include "lcd_display.h"
#include <lvgl.h>
#include <memory>

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
 *   ├─ 开机动画（logo 渐入 → 3s → 渐出 → 切时钟）
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

    // ===== 配网 QR 页（蓝牙/热点切换），非虚，仅 UiDisplay 暴露 =====
    void ShowWifiQrCode(const char* qr_content, const char* hint = nullptr,
                        const char* left_label = nullptr,
                        const char* right_label = nullptr,
                        bool active_left = true);
    void HideWifiQrCode();

    // ===== 激活绑定页（URL QR + 6 位激活码） =====
    void ShowActivationPage(const char* bind_url, const char* activation_code);
    void HideActivationPage();

    // ===== 主屏切换 =====
    void SwitchToClockMode();    // idle → 时钟主屏
    void SwitchToChatMode();     // 对话 → 聊天 UI
    bool IsClockMode() const { return is_clock_mode_; }

    // ===== 控制中心 =====
    void ShowControlCenter();
    void HideControlCenter();
    bool IsControlCenterVisible() const;

private:
    // 全局状态栏（常驻 screen 顶部，clock / chat 共享）
    lv_obj_t* global_status_bar_   = nullptr;
    lv_obj_t* status_network_icon_ = nullptr;
    lv_obj_t* status_battery_icon_ = nullptr;
    const char* cached_net_fa_     = nullptr;
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

    // 开机动画
    lv_timer_t* boot_timer_ = nullptr;

    // 控制中心（懒加载）
    std::unique_ptr<ControlCenter> control_center_;

    // 状态
    bool is_clock_mode_ = false;

    // ===== 内部方法 =====
    void CreateGlobalStatusBar();
    void UpdateGlobalStatusIcons();

    void CreateClockPage();
    void UpdateClockTime();
    void LoadClockFonts();          // cbin 字体延迟加载（assets 就绪后）
    static void ClockTickCb(lv_timer_t* t);

    void StartBootAnimation();
    static void BootTimerCallback(lv_timer_t* t);

    void EnsureControlCenter();
};

#endif  // UI_DISPLAY_H
