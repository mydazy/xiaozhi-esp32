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

    // ===== 配网 QR 页（蓝牙/热点切换），override Display 虚方法 =====
    void ShowWifiQrCode(const char* qr_content, const char* hint,
                        const char* left_label,
                        const char* right_label,
                        bool active_left,
                        std::function<void()> on_double_click) override;
    void HideWifiQrCode() override;

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
    // 状态栏 PNG 图标：直接挂父类 top_bar_（左 network）+ right_icons（右 battery）
    // 复用父类 flex row space-between 布局 + 半透明背景，无额外容器
    lv_obj_t* status_network_icon_ = nullptr;
    lv_obj_t* status_battery_icon_ = nullptr;
    const char* cached_net_fa_      = nullptr;
    const char* cached_battery_file_ = nullptr;
    bool cached_battery_charging_  = false;

    // 时钟主屏（内联实现，无独立页面类）
    lv_obj_t* clock_container_  = nullptr;
    lv_obj_t* clock_time_label_ = nullptr;
    lv_obj_t* clock_date_label_ = nullptr;
    lv_obj_t* clock_week_label_ = nullptr;
    const lv_font_t* clock_big_font_  = nullptr;   // 88px cbin（assets 就绪后加载）
    const lv_font_t* clock_text_font_ = nullptr;   // 30px cbin

    // 配网 / 激活 overlay（同时只有一个）
    lv_obj_t* wifi_qr_overlay_   = nullptr;
    lv_obj_t* activation_overlay_ = nullptr;

    // 配网双击切换状态
    std::function<void()> wifi_qr_on_switch_;
    int64_t wifi_qr_last_click_ms_ = 0;

    // 控制中心（懒加载）
    std::unique_ptr<ControlCenter> control_center_;

    // 状态
    bool is_clock_mode_ = false;

    // UiDisplay 有独立 clock_time_label_，禁用父类往 status_label_ 写 HH:MM
    // 避免 chat 模式 10s 用时间覆盖"聆听中"等状态文字
    bool ShouldShowTimeInStatusLabel() const override { return false; }

    // ===== 内部方法 =====
    void CreateGlobalStatusBar();

    void CreateClockPage();
    void UpdateClockTime();
    void LoadClockFonts();          // cbin 字体延迟加载（assets 就绪后）

    void EnsureControlCenter();
};

#endif  // UI_DISPLAY_H
