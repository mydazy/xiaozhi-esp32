/**
 * UiDisplay — 交互式 UI 显示（mydazy-p30-4g / mydazy-p31 专用）
 *
 * 所有 UI 页面内联实现，不再拆子类：
 *   - 时钟主屏  (CreateClockPage + UpdateClockTime)
 *   - 配网 QR 页 (ShowWifiQrCode)
 *   - 激活绑定页 (ShowActivationPage)
 *   - 全局状态栏 + 开机动画 + 控制中心
 */

#include "ui_display.h"

#include "ui/resources/ui_image_manager.h"
#include "ui/resources/ui_img_paths.h"
#include "ui/widgets/control_center.h"
#include "ui/theme/ui_config.h"

#include "application.h"
#include "audio_codec.h"
#include "backlight.h"
#include "board.h"
#include "assets.h"
#include "lvgl_theme.h"

#include <cstring>
#include <time.h>
#include <font_awesome.h>
#include <esp_log.h>
#include <cbin_font.h>

// FontAwesome 网络图标 → PNG 资源映射。
// 同时作为 4G/WiFi 默认图区分入口：
//   P30-4G（modem 未 ready） → board.GetNetworkStateIcon() == SIGNAL_OFF → 返回 4G_1（4G 无信号）
//   P30-WiFi（未连）          → board.GetNetworkStateIcon() == WIFI_SLASH → 返回 WIFI_0（WiFi 无信号）
//   未知 fa：兜底 WIFI_0（不丢屏，但代码里所有 WiFi/4G 状态都已枚举）
static const lv_image_dsc_t* MapNetworkIconFa(const char* fa_icon) {
    if (!fa_icon) return UI_IMG(IMG_FILE_SIGNAL_WIFI_0);
    if (strcmp(fa_icon, FONT_AWESOME_WIFI) == 0)          return UI_IMG(IMG_FILE_SIGNAL_WIFI);
    if (strcmp(fa_icon, FONT_AWESOME_WIFI_FAIR) == 0)     return UI_IMG(IMG_FILE_SIGNAL_WIFI_1);
    if (strcmp(fa_icon, FONT_AWESOME_WIFI_WEAK) == 0)     return UI_IMG(IMG_FILE_SIGNAL_WIFI_0);
    if (strcmp(fa_icon, FONT_AWESOME_WIFI_SLASH) == 0)    return UI_IMG(IMG_FILE_SIGNAL_WIFI_0);
    if (strcmp(fa_icon, FONT_AWESOME_SIGNAL) == 0)        return UI_IMG(IMG_FILE_SIGNAL_4G);
    if (strcmp(fa_icon, FONT_AWESOME_SIGNAL_STRONG) == 0) return UI_IMG(IMG_FILE_SIGNAL_4G_4);
    if (strcmp(fa_icon, FONT_AWESOME_SIGNAL_GOOD) == 0)   return UI_IMG(IMG_FILE_SIGNAL_4G_3);
    if (strcmp(fa_icon, FONT_AWESOME_SIGNAL_FAIR) == 0)   return UI_IMG(IMG_FILE_SIGNAL_4G_2);
    if (strcmp(fa_icon, FONT_AWESOME_SIGNAL_WEAK) == 0)   return UI_IMG(IMG_FILE_SIGNAL_4G_1);
    if (strcmp(fa_icon, FONT_AWESOME_SIGNAL_OFF) == 0)    return UI_IMG(IMG_FILE_SIGNAL_4G_1);
    return UI_IMG(IMG_FILE_SIGNAL_WIFI_0);
}

// 电量档位 → PNG 图标（5 档）。充电不影响档位选择（充电靠 recolor 染色区分）。
static const char* MapBatteryFile(int level) {
    if (level > 80) return IMG_FILE_ICON_BATTERY_4;
    if (level > 60) return IMG_FILE_ICON_BATTERY_3;
    if (level > 40) return IMG_FILE_ICON_BATTERY_2;
    if (level > 20) return IMG_FILE_ICON_BATTERY_1;
    return IMG_FILE_ICON_BATTERY_0;
}

#define TAG "UiDisplay"

LV_FONT_DECLARE(BUILTIN_TEXT_FONT);
LV_IMG_DECLARE(ui_img_start_logo_png);

// ============================================================
// 构造 / 析构
// ============================================================

UiDisplay::UiDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                     int width, int height, int offset_x, int offset_y,
                     bool mirror_x, bool mirror_y, bool swap_xy)
    : SpiLcdDisplay(panel_io, panel, width, height, offset_x, offset_y,
                    mirror_x, mirror_y, swap_xy) {
}

UiDisplay::~UiDisplay() {
    if (boot_timer_) { lv_timer_del(boot_timer_); boot_timer_ = nullptr; }
    if (clock_tick_) { lv_timer_del(clock_tick_); clock_tick_ = nullptr; }
}

// ============================================================
// SetupUI override：先调父类，然后注入 UI 扩展
// ============================================================

void UiDisplay::SetupUI() {
    SpiLcdDisplay::SetupUI();
    DisplayLockGuard lock(this);

    // 1. 开机 Logo：替换父类的 FONT_AWESOME_MICROCHIP_AI 为 start_logo 图片
    if (emoji_label_) {
        lv_label_set_text(emoji_label_, "");
        lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
    }
    if (emoji_image_) {
        lv_image_set_src(emoji_image_, &ui_img_start_logo_png);
        lv_obj_remove_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
    }
    if (emoji_box_) lv_obj_set_style_opa(emoji_box_, LV_OPA_TRANSP, 0);

    // 2. 全局状态栏（clock / chat 共享）
    CreateGlobalStatusBar();

    // 3. 开机动画
    StartBootAnimation();
}

void UiDisplay::LoadPuhuiCommonFont() {
    // 空实现，保留签名以免改 .h
}

// ============================================================
// 全局状态栏（挂 screen 常驻）
// ============================================================

void UiDisplay::CreateGlobalStatusBar() {
    auto* screen = lv_screen_active();
    if (!screen) return;

    // 父类 top_bar_ 里的 FontAwesome label 隐藏（父类 UpdateStatusBar 仍继续 set_text 无副作用）
    if (network_label_) lv_obj_add_flag(network_label_, LV_OBJ_FLAG_HIDDEN);
    if (battery_label_) lv_obj_add_flag(battery_label_, LV_OBJ_FLAG_HIDDEN);

    global_status_bar_ = lv_obj_create(screen);
    lv_obj_set_size(global_status_bar_, LV_HOR_RES, 36);
    lv_obj_align(global_status_bar_, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_opa(global_status_bar_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(global_status_bar_, 0, 0);
    lv_obj_set_style_pad_all(global_status_bar_, 0, 0);
    lv_obj_remove_flag(global_status_bar_, LV_OBJ_FLAG_SCROLLABLE);

    status_network_icon_ = lv_image_create(global_status_bar_);
    // 初始 src 按 board 类型自动区分（4G 板 → 4G 无信号图；WiFi 板 → WiFi 无信号图）
    const char* init_fa = Board::GetInstance().GetNetworkStateIcon();
    if (auto* init = MapNetworkIconFa(init_fa)) lv_image_set_src(status_network_icon_, init);
    cached_net_fa_ = init_fa;  // 同步 cache，避免下一秒 tick 时重复 set_src
    lv_obj_align(status_network_icon_, LV_ALIGN_LEFT_MID, 16, 0);

    status_battery_icon_ = lv_image_create(global_status_bar_);
    // 初始 src 用 board 真实电量（PowerManager 在 board 构造里已读 ADC，此时已可用）
    int init_level = 100; bool init_charging = false, init_dis = false;
    Board::GetInstance().GetBatteryLevel(init_level, init_charging, init_dis);
    if (auto* init = UI_IMG(MapBatteryFile(init_level))) lv_image_set_src(status_battery_icon_, init);
    // 充电时染黄（LVGL recolor），实时标识充电状态，无需新图素材
    if (init_charging) {
        lv_obj_set_style_image_recolor(status_battery_icon_, lv_color_hex(0x4CAF50), 0);
        lv_obj_set_style_image_recolor_opa(status_battery_icon_, LV_OPA_70, 0);
    }
    cached_battery_charging_ = init_charging;
    lv_obj_align(status_battery_icon_, LV_ALIGN_RIGHT_MID, -16, 0);

    lv_obj_move_foreground(global_status_bar_);
}

void UiDisplay::UpdateGlobalStatusIcons() {
    if (!status_network_icon_ || !status_battery_icon_) return;
    // chat 模式已隐藏全局状态栏（SwitchToChatMode 主动 add HIDDEN），早退避免空跑
    if (global_status_bar_ && lv_obj_has_flag(global_status_bar_, LV_OBJ_FLAG_HIDDEN)) return;

    auto& board = Board::GetInstance();
    const char* fa_icon = board.GetNetworkStateIcon();
    if (fa_icon && fa_icon != cached_net_fa_) {
        cached_net_fa_ = fa_icon;
        if (auto* net_img = MapNetworkIconFa(fa_icon)) {
            lv_image_set_src(status_network_icon_, net_img);
        }
    }

    int level = 0;
    bool charging = false, discharging = false;
    if (board.GetBatteryLevel(level, charging, discharging)) {
        // 电量档位图：按 level 走，充电不切换档位（保留档位信息）
        if (auto* img = UI_IMG(MapBatteryFile(level))) {
            lv_image_set_src(status_battery_icon_, img);
        }
        // 充电状态：用 LVGL recolor 染黄，替代"独占充电图"方案 —— 档位 + 充电同时可见
        if (charging != cached_battery_charging_) {
            cached_battery_charging_ = charging;
            lv_obj_set_style_image_recolor(status_battery_icon_,
                                           charging ? lv_color_hex(0x4CAF50) : lv_color_white(), 0);
            lv_obj_set_style_image_recolor_opa(status_battery_icon_,
                                               charging ? LV_OPA_70 : LV_OPA_0, 0);
        }
    }
}

void UiDisplay::UpdateStatusBar(bool update_all) {
    LvglDisplay::UpdateStatusBar(update_all);
    DisplayLockGuard lock(this);
    UpdateGlobalStatusIcons();
    // clock 模式下每秒触发，顺便刷新时间（assets 字体尚未就绪时延迟加载）
    if (is_clock_mode_ && clock_time_label_) {
        if (!clock_big_font_ || !clock_text_font_) LoadClockFonts();
        UpdateClockTime();
    }
}

// ============================================================
// 时钟主屏（内联实现）
// ============================================================

void UiDisplay::CreateClockPage() {
    if (clock_container_) return;
    auto* screen = lv_screen_active();
    if (!screen) return;

    clock_container_ = lv_obj_create(screen);
    lv_obj_set_size(clock_container_, ScreenConfig::WIDTH, ScreenConfig::HEIGHT);
    lv_obj_align(clock_container_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_remove_flag(clock_container_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(clock_container_, lv_color_hex(ScreenConfig::Colors::BG_PRIMARY), 0);
    lv_obj_set_style_bg_opa(clock_container_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(clock_container_, 0, 0);
    lv_obj_set_style_radius(clock_container_, ScreenConfig::RADIUS, 0);
    lv_obj_set_style_pad_all(clock_container_, 0, 0);

    clock_time_label_ = lv_label_create(clock_container_);
    lv_obj_set_style_text_color(clock_time_label_, lv_color_hex(ScreenConfig::Colors::TEXT_PRIMARY), 0);
    lv_label_set_text(clock_time_label_, "--:--");
    lv_obj_align(clock_time_label_, LV_ALIGN_CENTER, 0, -36);

    LoadClockFonts();
    if (!clock_big_font_) lv_obj_set_style_text_font(clock_time_label_, &BUILTIN_TEXT_FONT, 0);

    const lv_font_t* text_font = clock_text_font_ ? clock_text_font_ : &BUILTIN_TEXT_FONT;

    clock_date_label_ = lv_label_create(clock_container_);
    lv_obj_set_style_text_font(clock_date_label_, text_font, 0);
    lv_obj_set_style_text_color(clock_date_label_, lv_color_hex(ScreenConfig::Colors::TEXT_SECONDARY), 0);
    lv_label_set_text(clock_date_label_, "----年--月--日");
    lv_obj_align(clock_date_label_, LV_ALIGN_CENTER, 0, 40);

    clock_week_label_ = lv_label_create(clock_container_);
    lv_obj_set_style_text_font(clock_week_label_, text_font, 0);
    lv_obj_set_style_text_color(clock_week_label_, lv_color_hex(ScreenConfig::Colors::TEXT_DISABLED), 0);
    lv_label_set_text(clock_week_label_, "星期--");
    lv_obj_align(clock_week_label_, LV_ALIGN_CENTER, 0, 76);

    UpdateClockTime();

    // 1s 定时器刷新时间（UpdateStatusBar 里已按秒触发，ClockTick 冗余可删；保留 1s tick 避免 status_bar 卡住时时钟不动）
    clock_tick_ = lv_timer_create(ClockTickCb, 1000, this);
}

void UiDisplay::LoadClockFonts() {
    auto load = [](const char* name, const lv_font_t*& dst) {
        if (dst) return;
        void* ptr = nullptr; size_t size = 0;
        if (!Assets::GetInstance().GetAssetData(name, ptr, size) || !ptr) return;
        dst = cbin_font_create(static_cast<uint8_t*>(ptr));
        if (dst) const_cast<lv_font_t*>(dst)->fallback = &BUILTIN_TEXT_FONT;
    };
    load("font_maru_88_4.bin", clock_big_font_);
    load("font_maru_30_4.bin", clock_text_font_);

    if (clock_big_font_  && clock_time_label_) lv_obj_set_style_text_font(clock_time_label_, clock_big_font_, 0);
    if (clock_text_font_ && clock_date_label_) lv_obj_set_style_text_font(clock_date_label_, clock_text_font_, 0);
    if (clock_text_font_ && clock_week_label_) lv_obj_set_style_text_font(clock_week_label_, clock_text_font_, 0);
}

void UiDisplay::UpdateClockTime() {
    if (!clock_time_label_) return;
    time_t now = time(nullptr);
    struct tm tm_info;
    localtime_r(&now, &tm_info);

    if (tm_info.tm_year + 1900 < 2020) {
        lv_label_set_text(clock_time_label_, "--:--");
        lv_label_set_text(clock_date_label_, "----年--月--日");
        lv_label_set_text(clock_week_label_, "星期--");
        return;
    }

    char buf[48];
    snprintf(buf, sizeof(buf), "%02d:%02d", tm_info.tm_hour, tm_info.tm_min);
    lv_label_set_text(clock_time_label_, buf);
    snprintf(buf, sizeof(buf), "%d年%02d月%02d日",
             tm_info.tm_year + 1900, tm_info.tm_mon + 1, tm_info.tm_mday);
    lv_label_set_text(clock_date_label_, buf);

    static const char* days[] = {"星期日","星期一","星期二","星期三","星期四","星期五","星期六"};
    lv_label_set_text(clock_week_label_,
        (tm_info.tm_wday >= 0 && tm_info.tm_wday < 7) ? days[tm_info.tm_wday] : "");
}

void UiDisplay::ClockTickCb(lv_timer_t* t) {
    auto* self = static_cast<UiDisplay*>(lv_timer_get_user_data(t));
    if (!self->clock_big_font_ || !self->clock_text_font_) self->LoadClockFonts();
    self->UpdateClockTime();
}

// ============================================================
// 主屏切换
// ============================================================

void UiDisplay::SwitchToClockMode() {
    DisplayLockGuard lock(this);
    if (is_clock_mode_) return;
    if (!setup_ui_called_) return;

    if (!clock_container_) CreateClockPage();

    if (content_)     lv_obj_add_flag(content_, LV_OBJ_FLAG_HIDDEN);
    if (container_)   lv_obj_add_flag(container_, LV_OBJ_FLAG_HIDDEN);
    if (status_bar_)  lv_obj_add_flag(status_bar_, LV_OBJ_FLAG_HIDDEN);
    if (emoji_box_)   lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);

    if (clock_container_) {
        lv_obj_remove_flag(clock_container_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(clock_container_);
    }
    if (global_status_bar_) {
        lv_obj_remove_flag(global_status_bar_, LV_OBJ_FLAG_HIDDEN);  // clock 模式主动显示
        lv_obj_move_foreground(global_status_bar_);
    }
    if (wifi_qr_overlay_)    lv_obj_move_foreground(wifi_qr_overlay_);
    if (activation_overlay_) lv_obj_move_foreground(activation_overlay_);

    is_clock_mode_ = true;
    ESP_LOGI(TAG, "Switched to clock mode");
}

void UiDisplay::SwitchToChatMode() {
    DisplayLockGuard lock(this);
    if (!is_clock_mode_) return;

    if (clock_container_) {
        lv_obj_add_flag(clock_container_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_background(clock_container_);
    }

    if (container_) {
        lv_obj_remove_flag(container_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(container_);
    }
    if (content_) lv_obj_remove_flag(content_, LV_OBJ_FLAG_HIDDEN);
    if (status_bar_) {
        lv_obj_remove_flag(status_bar_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_opa(status_bar_, LV_OPA_COVER, 0);
    }
    if (emoji_box_) {
        lv_obj_remove_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_opa(emoji_box_, LV_OPA_COVER, 0);  // 关键：boot fade_out 把 opa=TRANSP
        lv_obj_move_foreground(emoji_box_);
    }
    // bottom_bar_ 挂在 screen 上（与 container_ 同级 sibling），必须显式提顶否则
    // 被 move_foreground(container_) 盖住 —— SetChatMessage 即使 remove HIDDEN 也看不见。
    if (bottom_bar_) lv_obj_move_foreground(bottom_bar_);

    // chat 模式主动隐藏全局状态栏（信号 + 电池）—— 不依赖 z-order 巧合，语义明确且 UpdateGlobalStatusIcons 能早退省 CPU
    if (global_status_bar_)  lv_obj_add_flag(global_status_bar_, LV_OBJ_FLAG_HIDDEN);
    if (wifi_qr_overlay_)    lv_obj_move_foreground(wifi_qr_overlay_);
    if (activation_overlay_) lv_obj_move_foreground(activation_overlay_);

    is_clock_mode_ = false;
    ESP_LOGI(TAG, "Switched to chat mode");
}

// ============================================================
// 开机动画（Logo 渐入 → 3s → 渐出 → 切时钟）
// ============================================================

static void boot_opa_cb(void* obj, int32_t v) {
    lv_obj_set_style_opa(static_cast<lv_obj_t*>(obj), v, 0);
}

void UiDisplay::StartBootAnimation() {
    if (!emoji_box_) return;

    lv_anim_t fade_in;
    lv_anim_init(&fade_in);
    lv_anim_set_var(&fade_in, emoji_box_);
    lv_anim_set_values(&fade_in, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_time(&fade_in, 800);
    lv_anim_set_exec_cb(&fade_in, boot_opa_cb);
    lv_anim_start(&fade_in);

    if (status_bar_) {
        lv_obj_set_style_opa(status_bar_, LV_OPA_TRANSP, 0);
        lv_anim_t status_in;
        lv_anim_init(&status_in);
        lv_anim_set_var(&status_in, status_bar_);
        lv_anim_set_values(&status_in, LV_OPA_TRANSP, LV_OPA_COVER);
        lv_anim_set_time(&status_in, 800);
        lv_anim_set_delay(&status_in, 200);
        lv_anim_set_exec_cb(&status_in, boot_opa_cb);
        lv_anim_start(&status_in);
    }

    boot_timer_ = lv_timer_create(BootTimerCallback, 3000, this);
    lv_timer_set_repeat_count(boot_timer_, 1);
}

void UiDisplay::BootTimerCallback(lv_timer_t* t) {
    auto* self = static_cast<UiDisplay*>(lv_timer_get_user_data(t));
    self->boot_timer_ = nullptr;

    if (self->emoji_box_) {
        lv_anim_t fade_out;
        lv_anim_init(&fade_out);
        lv_anim_set_var(&fade_out, self->emoji_box_);
        lv_anim_set_values(&fade_out, LV_OPA_COVER, LV_OPA_TRANSP);
        lv_anim_set_time(&fade_out, 400);
        lv_anim_set_exec_cb(&fade_out, boot_opa_cb);
        lv_anim_set_user_data(&fade_out, self);
        lv_anim_set_completed_cb(&fade_out, [](lv_anim_t* a) {
            auto* d = static_cast<UiDisplay*>(lv_anim_get_user_data(a));
            d->SwitchToClockMode();
        });
        lv_anim_start(&fade_out);
    }

    if (self->status_bar_) {
        lv_anim_t status_out;
        lv_anim_init(&status_out);
        lv_anim_set_var(&status_out, self->status_bar_);
        lv_anim_set_values(&status_out, LV_OPA_COVER, LV_OPA_TRANSP);
        lv_anim_set_time(&status_out, 400);
        lv_anim_set_exec_cb(&status_out, boot_opa_cb);
        lv_anim_start(&status_out);
    }
}

// ============================================================
// 配网 QR 页（蓝牙/热点切换）
// ============================================================

void UiDisplay::ShowWifiQrCode(const char* qr_content, const char* hint,
                                const char* left_label, const char* right_label,
                                bool active_left) {
    DisplayLockGuard lock(this);
    HideWifiQrCode();

    auto* screen = lv_screen_active();
    if (!screen) return;

    bool is_blufi = active_left;

    wifi_qr_overlay_ = lv_obj_create(screen);
    lv_obj_set_size(wifi_qr_overlay_, LV_HOR_RES, LV_VER_RES);
    lv_obj_align(wifi_qr_overlay_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_remove_flag(wifi_qr_overlay_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(wifi_qr_overlay_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(wifi_qr_overlay_, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(wifi_qr_overlay_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(wifi_qr_overlay_, 0, 0);
    lv_obj_set_style_radius(wifi_qr_overlay_, 0, 0);
    lv_obj_set_style_pad_all(wifi_qr_overlay_, 0, 0);

    constexpr int kBarW = 40;
    const int kCenterW = LV_HOR_RES - kBarW * 2;
    const lv_color_t color_on  = lv_color_hex(0x2196F3);
    const lv_color_t color_off = lv_color_hex(0xE0E0E0);

    // 左右竖排色条（UTF-8 逐字符）
    auto make_bar = [&](lv_align_t align, const char* text, bool on) {
        if (!text || !text[0]) return;
        lv_obj_t* bar = lv_obj_create(wifi_qr_overlay_);
        lv_obj_set_size(bar, kBarW, LV_VER_RES);
        lv_obj_align(bar, align, 0, 0);
        lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(bar, on ? color_on : color_off, 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(bar, 0, 0);
        lv_obj_set_style_radius(bar, 0, 0);
        lv_obj_set_style_pad_all(bar, 0, 0);

        lv_color_t tc = on ? lv_color_white() : lv_color_hex(0x666666);
        int n = 0;
        for (const char* p = text; *p; n++) {
            p += (*p & 0x80) == 0 ? 1 : (*p & 0xE0) == 0xC0 ? 2 : (*p & 0xF0) == 0xE0 ? 3 : 4;
        }
        int lh = 22;
        int sy = (LV_VER_RES - n * lh) / 2;
        int idx = 0;
        for (const char* p = text; *p; idx++) {
            int b = (*p & 0x80) == 0 ? 1 : (*p & 0xE0) == 0xC0 ? 2 : (*p & 0xF0) == 0xE0 ? 3 : 4;
            char buf[5] = {};
            memcpy(buf, p, b);
            lv_obj_t* ch = lv_label_create(bar);
            lv_label_set_text(ch, buf);
            lv_obj_set_style_text_font(ch, &BUILTIN_TEXT_FONT, 0);
            lv_obj_set_style_text_color(ch, tc, 0);
            lv_obj_align(ch, LV_ALIGN_TOP_MID, 0, sy + idx * lh);
            p += b;
        }
    };

    make_bar(LV_ALIGN_TOP_LEFT, left_label, active_left);
    make_bar(LV_ALIGN_TOP_RIGHT, right_label, !active_left);

    bool has_bars = (left_label && left_label[0]) || (right_label && right_label[0]);
    if (has_bars) {
        lv_obj_t* tip = lv_label_create(wifi_qr_overlay_);
        lv_label_set_text(tip, "双击切换模式");
        lv_obj_set_style_text_font(tip, &BUILTIN_TEXT_FONT, 0);
        lv_obj_set_style_text_color(tip, lv_color_hex(0xAAAAAA), 0);
        lv_obj_set_style_text_align(tip, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(tip, kCenterW);
        lv_obj_align(tip, LV_ALIGN_TOP_MID, 0, 2);
    }

    // QR 码
#if CONFIG_LV_USE_QRCODE
    constexpr int kQrSize = 160;
    const bool is_url = (strncmp(qr_content, "http", 4) == 0);
    char qr_data[256];
    if (is_url) snprintf(qr_data, sizeof(qr_data), "%s", qr_content);
    else        snprintf(qr_data, sizeof(qr_data), "WIFI:T:nopass;S:%s;;", qr_content);

    lv_obj_t* qr = lv_qrcode_create(wifi_qr_overlay_);
    lv_qrcode_set_size(qr, kQrSize);
    lv_qrcode_set_dark_color(qr, lv_color_black());
    lv_qrcode_set_light_color(qr, lv_color_white());
    lv_qrcode_update(qr, qr_data, strlen(qr_data));
    lv_obj_align(qr, LV_ALIGN_CENTER, 0, -15);

    const char* sub_text = is_blufi ? "扫码蓝牙配网" :
                           is_url   ? "扫码绑定设备" : "扫码连接热点";
    lv_obj_t* sub = lv_label_create(wifi_qr_overlay_);
    lv_label_set_text(sub, sub_text);
    lv_obj_set_style_text_font(sub, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(sub, lv_color_hex(0x666666), 0);
    lv_obj_set_style_text_align(sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(sub, kCenterW);
    lv_obj_align_to(sub, qr, LV_ALIGN_OUT_BOTTOM_MID, 0, 4);
#else
    lv_obj_t* fallback = lv_label_create(wifi_qr_overlay_);
    lv_obj_set_style_text_font(fallback, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(fallback, lv_color_hex(0x333333), 0);
    lv_obj_set_style_text_align(fallback, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(fallback, kCenterW);
    lv_label_set_text(fallback, is_blufi ? "打开小程序蓝牙配网" : qr_content);
    lv_obj_align(fallback, LV_ALIGN_CENTER, 0, 0);
#endif

    const char* h = (hint && hint[0]) ? hint : qr_content;
    lv_obj_t* hl = lv_label_create(wifi_qr_overlay_);
    lv_label_set_text(hl, h);
    lv_obj_set_style_text_font(hl, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_width(hl, kCenterW);
    lv_obj_set_style_text_align(hl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(hl, lv_color_hex(0x999999), 0);
    lv_label_set_long_mode(hl, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_align(hl, LV_ALIGN_BOTTOM_MID, 0, -3);

    ESP_LOGI(TAG, "配网页: mode=%s content=%s", is_blufi ? "BluFi" : "AP", qr_content);
}

void UiDisplay::HideWifiQrCode() {
    DisplayLockGuard lock(this);
    if (wifi_qr_overlay_) {
        lv_obj_del(wifi_qr_overlay_);
        wifi_qr_overlay_ = nullptr;
        ESP_LOGI(TAG, "隐藏配网 QR");
    }
}

// ============================================================
// 激活绑定页（URL QR + 6 位激活码）
// ============================================================

void UiDisplay::ShowActivationPage(const char* bind_url, const char* activation_code) {
    DisplayLockGuard lock(this);
    HideActivationPage();

    auto* screen = lv_screen_active();
    if (!screen || !bind_url) return;

    activation_overlay_ = lv_obj_create(screen);
    lv_obj_set_size(activation_overlay_, LV_HOR_RES, LV_VER_RES);
    lv_obj_align(activation_overlay_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_remove_flag(activation_overlay_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(activation_overlay_, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(activation_overlay_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(activation_overlay_, 0, 0);
    lv_obj_set_style_radius(activation_overlay_, 0, 0);
    lv_obj_set_style_pad_all(activation_overlay_, 0, 0);

    // 顶部标题
    lv_obj_t* title = lv_label_create(activation_overlay_);
    lv_label_set_text(title, "绑定设备");
    lv_obj_set_style_text_font(title, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x333333), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);

    // QR 码
#if CONFIG_LV_USE_QRCODE
    constexpr int kQrSize = 140;
    lv_obj_t* qr = lv_qrcode_create(activation_overlay_);
    lv_qrcode_set_size(qr, kQrSize);
    lv_qrcode_set_dark_color(qr, lv_color_black());
    lv_qrcode_set_light_color(qr, lv_color_white());
    lv_qrcode_update(qr, bind_url, strlen(bind_url));
    lv_obj_align(qr, LV_ALIGN_CENTER, 0, -18);
#else
    lv_obj_t* fallback = lv_label_create(activation_overlay_);
    lv_label_set_text(fallback, bind_url);
    lv_obj_set_style_text_font(fallback, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(fallback, lv_color_hex(0x333333), 0);
    lv_obj_set_style_text_align(fallback, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(fallback, LV_HOR_RES - 20);
    lv_obj_align(fallback, LV_ALIGN_CENTER, 0, 0);
#endif

    // 激活码（大字号醒目显示）
    if (activation_code && activation_code[0]) {
        lv_obj_t* code_lbl = lv_label_create(activation_overlay_);
        lv_label_set_text(code_lbl, activation_code);
        lv_obj_set_style_text_font(code_lbl, &BUILTIN_TEXT_FONT, 0);
        lv_obj_set_style_text_color(code_lbl, lv_color_hex(0x2196F3), 0);
        lv_obj_set_style_text_align(code_lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(code_lbl, LV_ALIGN_BOTTOM_MID, 0, -28);

        lv_obj_t* tip = lv_label_create(activation_overlay_);
        lv_label_set_text(tip, "扫码或输入激活码");
        lv_obj_set_style_text_font(tip, &BUILTIN_TEXT_FONT, 0);
        lv_obj_set_style_text_color(tip, lv_color_hex(0x999999), 0);
        lv_obj_align(tip, LV_ALIGN_BOTTOM_MID, 0, -6);
    } else {
        lv_obj_t* tip = lv_label_create(activation_overlay_);
        lv_label_set_text(tip, "扫码绑定设备");
        lv_obj_set_style_text_font(tip, &BUILTIN_TEXT_FONT, 0);
        lv_obj_set_style_text_color(tip, lv_color_hex(0x666666), 0);
        lv_obj_align(tip, LV_ALIGN_BOTTOM_MID, 0, -10);
    }

    if (global_status_bar_) lv_obj_move_foreground(global_status_bar_);
    ESP_LOGI(TAG, "激活页: %s%s%s", bind_url,
             activation_code ? " code=" : "", activation_code ? activation_code : "");
}

void UiDisplay::HideActivationPage() {
    DisplayLockGuard lock(this);
    if (activation_overlay_) {
        lv_obj_del(activation_overlay_);
        activation_overlay_ = nullptr;
        ESP_LOGI(TAG, "隐藏激活页");
    }
}

// ============================================================
// 控制中心（下拉手势触发，懒加载）
// ============================================================

void UiDisplay::EnsureControlCenter() {
    if (control_center_) return;
    auto* screen = lv_screen_active();
    if (!screen) return;

    control_center_ = std::make_unique<ControlCenter>(screen, LV_HOR_RES, LV_VER_RES);
    control_center_->Hide();

    auto& app = Application::GetInstance();
    auto& board = Board::GetInstance();

    control_center_->SetExitCallback([this]() { HideControlCenter(); });

    if (auto* codec = board.GetAudioCodec())    control_center_->SetVolume(codec->output_volume());
    if (auto* bk    = board.GetBacklight())     control_center_->SetBrightness(bk->brightness());

    control_center_->SetVolumeCallback([&board](int v) {
        if (auto* codec = board.GetAudioCodec()) codec->SetOutputVolume(v);
    });
    control_center_->SetBrightnessCallback([&board](int v) {
        if (auto* bk = board.GetBacklight()) bk->SetBrightness(v);
    });
    control_center_->SetAecCallback([&app](bool on) {
        app.SetAecMode(on ? kAecOnDeviceSide : kAecOff);
    });
    control_center_->SetSleepCallback([](bool on) {
        ESP_LOGI(TAG, "休眠: %s", on ? "ON" : "OFF");
    });
    control_center_->SetNetworkCallback([](int mode) {
        ESP_LOGI(TAG, "网络切换: %s", mode == 0 ? "WiFi" : "4G");
    });
}

void UiDisplay::ShowControlCenter() {
    DisplayLockGuard lock(this);
    EnsureControlCenter();
    if (!control_center_) return;
    auto& board = Board::GetInstance();
    if (auto* codec = board.GetAudioCodec()) control_center_->SetVolume(codec->output_volume());
    if (auto* bk    = board.GetBacklight())  control_center_->SetBrightness(bk->brightness());
    control_center_->Show();
}

void UiDisplay::HideControlCenter() {
    DisplayLockGuard lock(this);
    if (control_center_) control_center_->Hide();
}

bool UiDisplay::IsControlCenterVisible() const {
    return control_center_ && control_center_->IsVisible();
}
