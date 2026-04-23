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
//   WiFi: _0(弱) / _1(中) / _2(满格)   共 3 档 + 无网
//   4G:   _1(弱) / _2 / _3 / _4(满格)  共 4 档 + 无网
static const lv_image_dsc_t* MapNetworkIconFa(const char* fa_icon) {
    if (!fa_icon) return UI_IMG(IMG_FILE_SIGNAL_WIFI);  // 兜底：无网图
    if (strcmp(fa_icon, FONT_AWESOME_WIFI) == 0)          return UI_IMG(IMG_FILE_SIGNAL_WIFI_2);   // 满格
    if (strcmp(fa_icon, FONT_AWESOME_WIFI_FAIR) == 0)     return UI_IMG(IMG_FILE_SIGNAL_WIFI_1);
    if (strcmp(fa_icon, FONT_AWESOME_WIFI_WEAK) == 0)     return UI_IMG(IMG_FILE_SIGNAL_WIFI_0);
    if (strcmp(fa_icon, FONT_AWESOME_WIFI_SLASH) == 0)    return UI_IMG(IMG_FILE_SIGNAL_WIFI);     // 未连/无网
    if (strcmp(fa_icon, FONT_AWESOME_SIGNAL) == 0)        return UI_IMG(IMG_FILE_SIGNAL_4G);       // 4G 无等级/兜底
    if (strcmp(fa_icon, FONT_AWESOME_SIGNAL_STRONG) == 0) return UI_IMG(IMG_FILE_SIGNAL_4G_4);
    if (strcmp(fa_icon, FONT_AWESOME_SIGNAL_GOOD) == 0)   return UI_IMG(IMG_FILE_SIGNAL_4G_3);
    if (strcmp(fa_icon, FONT_AWESOME_SIGNAL_FAIR) == 0)   return UI_IMG(IMG_FILE_SIGNAL_4G_2);
    if (strcmp(fa_icon, FONT_AWESOME_SIGNAL_WEAK) == 0)   return UI_IMG(IMG_FILE_SIGNAL_4G_1);
    if (strcmp(fa_icon, FONT_AWESOME_SIGNAL_OFF) == 0)    return UI_IMG(IMG_FILE_SIGNAL_4G);       // csq=-1/modem 未就绪
    return UI_IMG(IMG_FILE_SIGNAL_WIFI);  // 未知 fa：兜底无网
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

UiDisplay::~UiDisplay() = default;

// ============================================================
// SetupUI override：先调父类，然后注入 UI 扩展
// ============================================================

void UiDisplay::SetupUI() {
    SpiLcdDisplay::SetupUI();
    DisplayLockGuard lock(this);

    // 开机 Logo：替换父类 emoji_label_ 的 FONT_AWESOME_MICROCHIP_AI 为 start_logo 图片
    // 静态即显，logo 一直显示到 application.cc:HandleStateChangedEvent 在 Idle 状态
    // 主动调 SwitchToClockMode（联网激活完成）→ 隐藏 container_/emoji_box_ 切到时钟主屏
    if (emoji_label_) {
        lv_label_set_text(emoji_label_, "");
        lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
    }
    if (emoji_image_) {
        lv_image_set_src(emoji_image_, &ui_img_start_logo_png);
        lv_obj_remove_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
    }

    // 状态栏 PNG 图标（PNG image 挂父类 top_bar_，clock/chat 共享）
    CreateGlobalStatusBar();

    // 聊天时底部消息气泡背景透明（父类默认 LV_OPA_50 带背景色，容易遮住表情）
    if (bottom_bar_) {
        lv_obj_set_style_bg_opa(bottom_bar_, LV_OPA_TRANSP, 0);
    }

    // 关闭顶部聊天状态栏（status_label_/notification_label_）
    // chat 状态改由底部 bottom_bar_ 的 chat_message_label_ 承载，避免顶部重复 + 覆盖
    if (status_bar_) {
        lv_obj_add_flag(status_bar_, LV_OBJ_FLAG_HIDDEN);
    }
}

// ============================================================
// 状态栏 PNG 图标：直接挂父类 top_bar_ / right_icons，不再新建容器
// - 复用父类半透明背景条、flex row space-between 布局、pad 设置
// - clock/chat 模式均由父类 top_bar_ 的显隐决定（当前两模式都显示）
// ============================================================

void UiDisplay::CreateGlobalStatusBar() {
    if (!top_bar_) return;

    // 非 emote 分支下父类 top_bar_ 是 container_ 的子（lcd_display.cc:395）
    // clock 模式会 HIDDEN container_ 导致状态栏跟着消失
    // 把 top_bar_ reparent 到 screen，脱离 container_ 的显隐控制（不新建容器，复用现有 top_bar_）
    auto* screen = lv_screen_active();
    if (screen && lv_obj_get_parent(top_bar_) != screen) {
        lv_obj_set_parent(top_bar_, screen);
        lv_obj_align(top_bar_, LV_ALIGN_TOP_MID, 0, 0);
    }
    // 背景透明（父类默认 LV_OPA_50 带色条，reparent 后直接透明叠在内容上更干净）
    lv_obj_set_style_bg_opa(top_bar_, LV_OPA_TRANSP, 0);

    // battery_label_ 的父容器 = 父类 SetupUI 中的 right_icons 局部变量（无成员引用，反查）
    lv_obj_t* right_icons = battery_label_ ? lv_obj_get_parent(battery_label_) : nullptr;

    // 删除父类 FA 文字 label（被 PNG image 替代；父类 UpdateStatusBar 对 nullptr 有 guard，自动 skip）
    if (network_label_) { lv_obj_del(network_label_); network_label_ = nullptr; }
    if (battery_label_) { lv_obj_del(battery_label_); battery_label_ = nullptr; }

    // 网络 PNG：挂 top_bar_ 首位（flex row space-between 的左侧位置，原 network_label_ 所在）
    // ⚠ SetupUI 此时 UiImageManager::LoadAll 尚未调用（application.cc:407 才执行），
    //    UI_IMG 可能返回 nullptr。cache 只在 set_src 成功后赋值，
    //    让 UpdateStatusBar 的 10s tick 在 LoadAll 之后重新补上。
    status_network_icon_ = lv_image_create(top_bar_);
    lv_obj_move_to_index(status_network_icon_, 0);
    const char* init_fa = Board::GetInstance().GetNetworkStateIcon();
    if (auto* init_img = MapNetworkIconFa(init_fa)) {
        lv_image_set_src(status_network_icon_, init_img);
        cached_net_fa_ = init_fa;
    }

    // 电量 PNG：挂 right_icons（与 mute_label_ 并列，保持原 battery_label_ 位置）
    if (right_icons) {
        status_battery_icon_ = lv_image_create(right_icons);
        int init_level = 100; bool init_charging = false, init_dis = false;
        Board::GetInstance().GetBatteryLevel(init_level, init_charging, init_dis);
        const char* init_file = MapBatteryFile(init_level);
        if (auto* init_img = UI_IMG(init_file)) {
            lv_image_set_src(status_battery_icon_, init_img);
            cached_battery_file_ = init_file;
        }
        if (init_charging) {
            lv_obj_set_style_image_recolor(status_battery_icon_, lv_color_hex(0x4CAF50), 0);
            lv_obj_set_style_image_recolor_opa(status_battery_icon_, LV_OPA_70, 0);
        }
        cached_battery_charging_ = init_charging;
    }
}

void UiDisplay::UpdateStatusBar(bool update_all) {
    // 基于父类 LvglDisplay::UpdateStatusBar（每秒由 clock_timer 驱动）扩展
    // 父类已负责：mute_label_ / battery_label_ / network_label_ / status_label_ / low_battery_popup_
    LvglDisplay::UpdateStatusBar(update_all);
    DisplayLockGuard lock(this);

    // === 扩展 1：PNG 状态栏 icon 刷新（10s 节流，避免每秒 AT+CSQ 压 4G UART）===
    // top_bar_ 的显隐由父类管理，PNG image 随之跟随；此处只判 image 是否已创建
    if (status_network_icon_ && status_battery_icon_) {
        static int sec_counter = 0;
        bool tick_10s = (sec_counter++ % 10 == 0);
        if (tick_10s) ESP_LOGI(TAG, "UpdateStatusBar tick_10s: net_icon=%p batt_icon=%p", status_network_icon_, status_battery_icon_);
        auto& board = Board::GetInstance();

        if (tick_10s) {
            const char* fa_icon = board.GetNetworkStateIcon();
            if (fa_icon && fa_icon != cached_net_fa_) {
                cached_net_fa_ = fa_icon;
                if (auto* net_img = MapNetworkIconFa(fa_icon)) {
                    lv_image_set_src(status_network_icon_, net_img);
                }
            }
        }

        int level = 0;
        bool charging = false, discharging = false;
        if (board.GetBatteryLevel(level, charging, discharging)) {
            // 档位 10s 节流 + 文件名 cache
            if (tick_10s) {
                const char* file = MapBatteryFile(level);
                if (file && file != cached_battery_file_) {
                    auto* img = UI_IMG(file);
                    if (img) {
                        cached_battery_file_ = file;
                        lv_image_set_src(status_battery_icon_, img);
                        ESP_LOGI(TAG, "电池档位切换: level=%d file=%s OK", level, file);
                    } else {
                        ESP_LOGW(TAG, "电池图 UI_IMG(%s) 返回 nullptr，LoadAll 未就绪 or 解析失败", file);
                    }
                }
            }
            // 充电 recolor 每秒响应（插拔瞬时反馈，不涉及 AT）
            if (charging != cached_battery_charging_) {
                cached_battery_charging_ = charging;
                lv_obj_set_style_image_recolor(status_battery_icon_,
                                               charging ? lv_color_hex(0x4CAF50) : lv_color_white(), 0);
                lv_obj_set_style_image_recolor_opa(status_battery_icon_,
                                                   charging ? LV_OPA_70 : LV_OPA_0, 0);
            }
        }
    }

    // === 扩展 2：clock 主屏时钟（每秒；assets 字体尚未就绪时延迟加载）===
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
    // 时间每秒刷新由 UpdateStatusBar（clock_timer 驱动）负责，无需独立 lv_timer
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
    // top_bar_ 由父类 SetupUI 创建并挂 screen，clock/chat 两模式都常驻显示，不必专门管
    if (top_bar_) lv_obj_move_foreground(top_bar_);
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
    // status_bar_ 在 SetupUI 时已常隐（顶部聊天状态关闭），chat 模式不再打开
    if (emoji_box_) {
        lv_obj_remove_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(emoji_box_);
    }
    // bottom_bar_ 挂在 screen 上（与 container_ 同级 sibling），必须显式提顶否则
    // 被 move_foreground(container_) 盖住 —— SetChatMessage 即使 remove HIDDEN 也看不见。
    if (bottom_bar_) lv_obj_move_foreground(bottom_bar_);

    // top_bar_ 复用父类 + PNG 图标挂在里面，chat 模式自然显示网络/电量/mute
    if (top_bar_)            lv_obj_move_foreground(top_bar_);
    if (wifi_qr_overlay_)    lv_obj_move_foreground(wifi_qr_overlay_);
    if (activation_overlay_) lv_obj_move_foreground(activation_overlay_);

    is_clock_mode_ = false;
    ESP_LOGI(TAG, "Switched to chat mode");
}

// ============================================================
// 配网 QR 页（蓝牙/热点切换）
// ============================================================

void UiDisplay::ShowWifiQrCode(const char* qr_content, const char* hint,
                                const char* left_label, const char* right_label,
                                bool active_left,
                                std::function<void()> on_double_click) {
    DisplayLockGuard lock(this);
    HideWifiQrCode();

    auto* screen = lv_screen_active();
    if (!screen) return;

    bool is_blufi = active_left;
    wifi_qr_on_switch_ = std::move(on_double_click);
    wifi_qr_last_click_ms_ = 0;

    wifi_qr_overlay_ = lv_obj_create(screen);
    lv_obj_set_size(wifi_qr_overlay_, LV_HOR_RES, LV_VER_RES);
    lv_obj_align(wifi_qr_overlay_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_remove_flag(wifi_qr_overlay_, LV_OBJ_FLAG_SCROLLABLE);
    // 双击切换：overlay 需可点击（仅在设置了回调时启用，避免误拦截）
    if (wifi_qr_on_switch_) {
        lv_obj_add_flag(wifi_qr_overlay_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(wifi_qr_overlay_, [](lv_event_t* e) {
            auto* self = static_cast<UiDisplay*>(lv_event_get_user_data(e));
            if (!self || !self->wifi_qr_on_switch_) return;
            int64_t now_ms = esp_timer_get_time() / 1000;
            // 500ms 内第二次点击 = 双击
            if (self->wifi_qr_last_click_ms_ != 0 &&
                (now_ms - self->wifi_qr_last_click_ms_) < 500) {
                self->wifi_qr_last_click_ms_ = 0;
                auto cb = self->wifi_qr_on_switch_;
                cb();  // 回调里可能会再次调用 ShowWifiQrCode 覆盖 overlay
            } else {
                self->wifi_qr_last_click_ms_ = now_ms;
            }
        }, LV_EVENT_CLICKED, this);
    } else {
        lv_obj_remove_flag(wifi_qr_overlay_, LV_OBJ_FLAG_CLICKABLE);
    }
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
    if (is_blufi || is_url) {
        // BLUFI 模式：caller 传入载荷原样使用（小程序协议载荷）
        // URL：原样显示（用于绑定页）
        snprintf(qr_data, sizeof(qr_data), "%s", qr_content);
    } else {
        // AP 模式：按 WiFi QR 标准封装 SSID（open 网络，T:nopass）
        snprintf(qr_data, sizeof(qr_data), "WIFI:T:nopass;S:%s;;", qr_content);
    }

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
    wifi_qr_on_switch_ = nullptr;
    wifi_qr_last_click_ms_ = 0;
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

    // activation_overlay_ 全屏覆盖 top_bar_，无需显式 move_foreground（激活页本就要盖住状态栏）
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
