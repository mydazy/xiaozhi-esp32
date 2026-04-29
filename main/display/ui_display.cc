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
#include "audio/music_player.h"
#include "backlight.h"
#include "board.h"
#include "assets.h"
#include "lvgl_theme.h"

#include <cstring>
#include <time.h>
#include <font_awesome.h>
#include <esp_log.h>
#include <esp_timer.h>

// 同 lcd_display.cc / oled_display.cc 风格：让 BUILTIN_ICON_FONT 宏展开的符号在本文件可见
LV_FONT_DECLARE(BUILTIN_ICON_FONT);
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
    // 注：SetupUI 阶段 assets 分区还未挂载（UiImageManager.LoadAll 在 ActivationTask 才跑），
    //     UI_IMG 此时返回 nullptr —— 没关系，UpdateGlobalStatusIcons 每秒重试，assets 就绪后自动显示。
    const char* init_fa = Board::GetInstance().GetNetworkStateIcon();
    if (auto* init = MapNetworkIconFa(init_fa)) lv_image_set_src(status_network_icon_, init);
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
    // 不缓存 fa_icon 指针：CreateGlobalStatusBar 时 assets 未就绪，UI_IMG 返回 nullptr，
    // 必须每秒重试直到 assets 加载完成才能显示。lv_image_set_src 内部对相同 src 短路，开销可忽略。
    // （电池图标也是同样无缓存逻辑，二者对齐。）
    if (fa_icon) {
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
    // P0 修复：MP3 播放期间 OnMusicPlay 流程会先 CloseAudioChannel → 触发 STATE_CHANGED(Idle)
    // → HandleStateChangedEvent::kDeviceStateIdle → FinishBootAndShowClock → SwitchToClockMode，
    // 会盖住刚切好的 Player UI。Player 模式下拒绝切时钟，由 SwitchOutPlayerMode 显式退出。
    if (is_player_mode_) return;

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
    if (qr_overlay_) lv_obj_move_foreground(qr_overlay_);

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
    if (qr_overlay_) lv_obj_move_foreground(qr_overlay_);

    is_clock_mode_ = false;
    ESP_LOGI(TAG, "Switched to chat mode");
}

// ============================================================
// 开机动画（Logo 渐入 → 持续显示 → Idle 时由状态机触发渐出 → 切时钟）
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
}

void UiDisplay::FinishBootAndShowClock() {
    DisplayLockGuard lock(this);
    if (is_clock_mode_) return;          // 幂等：已切时钟，重复 Idle 事件忽略
    if (!setup_ui_called_) return;
    // 同 SwitchToClockMode：Player 模式下不切回时钟（OnMusicPlay 路径会触发 Idle 状态事件）
    if (is_player_mode_) return;

    // 清理可能残留的开机引导覆盖层（激活码 / 配网 QR），避免切到时钟后仍被 overlay 遮挡。
    HideQrCode();

    if (!emoji_box_) {
        SwitchToClockMode();
        return;
    }

    lv_anim_t fade_out;
    lv_anim_init(&fade_out);
    lv_anim_set_var(&fade_out, emoji_box_);
    lv_anim_set_values(&fade_out, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_time(&fade_out, 400);
    lv_anim_set_exec_cb(&fade_out, boot_opa_cb);
    lv_anim_set_user_data(&fade_out, this);
    lv_anim_set_completed_cb(&fade_out, [](lv_anim_t* a) {
        auto* d = static_cast<UiDisplay*>(lv_anim_get_user_data(a));
        d->SwitchToClockMode();
    });
    lv_anim_start(&fade_out);

    if (status_bar_) {
        lv_anim_t status_out;
        lv_anim_init(&status_out);
        lv_anim_set_var(&status_out, status_bar_);
        lv_anim_set_values(&status_out, LV_OPA_COVER, LV_OPA_TRANSP);
        lv_anim_set_time(&status_out, 400);
        lv_anim_set_exec_cb(&status_out, boot_opa_cb);
        lv_anim_start(&status_out);
    }
}

// ============================================================
// 通用二维码页（合并配网 BLUFI/AP + 设备绑定，未来支持付费等扩展）
// ============================================================

void UiDisplay::ShowQrCode(const char* qr_content,
                            const char* highlight, const char* top,
                            const char* bottom,
                            const char* left_label, const char* right_label,
                            bool active_left,
                            std::function<void()> on_double_click) {
    DisplayLockGuard lock(this);
    HideQrCode();

    auto* screen = lv_screen_active();
    if (!screen || !qr_content) return;

    // 保活双击 callback（仅显示色条时启用 click 事件）
    qr_double_click_cb_ = std::move(on_double_click);
    qr_last_click_us_   = 0;

    // 全屏白底 overlay
    qr_overlay_ = lv_obj_create(screen);
    lv_obj_set_size(qr_overlay_, LV_HOR_RES, LV_VER_RES);
    lv_obj_align(qr_overlay_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_remove_flag(qr_overlay_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(qr_overlay_, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(qr_overlay_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(qr_overlay_, 0, 0);
    lv_obj_set_style_radius(qr_overlay_, 0, 0);
    lv_obj_set_style_pad_all(qr_overlay_, 0, 0);

    if (qr_double_click_cb_) {
        lv_obj_add_flag(qr_overlay_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(qr_overlay_, OnQrClicked, LV_EVENT_CLICKED, this);
    } else {
        lv_obj_remove_flag(qr_overlay_, LV_OBJ_FLAG_CLICKABLE);
    }

    constexpr int kBarW = 40;
    const bool has_bars = (left_label && left_label[0]) || (right_label && right_label[0]);
    const int kCenterW = has_bars ? (LV_HOR_RES - kBarW * 2) : LV_HOR_RES;
    const lv_color_t color_on  = lv_color_hex(0x2196F3);
    const lv_color_t color_off = lv_color_hex(0xE0E0E0);

    // 左右竖排色条 lambda（UTF-8 逐字符竖排）— 仅 left/right 非空时画
    auto make_bar = [&](lv_align_t align, const char* text, bool on) {
        if (!text || !text[0]) return;
        lv_obj_t* bar = lv_obj_create(qr_overlay_);
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
    make_bar(LV_ALIGN_TOP_LEFT,  left_label,  active_left);
    make_bar(LV_ALIGN_TOP_RIGHT, right_label, !active_left);

    // 居中文本 label 创建辅助 lambda
    auto make_label = [&](const char* text, lv_color_t color,
                          lv_align_t align, int y_off, bool scroll = false) -> lv_obj_t* {
        if (!text || !text[0]) return nullptr;
        lv_obj_t* lbl = lv_label_create(qr_overlay_);
        lv_label_set_text(lbl, text);
        lv_obj_set_style_text_font(lbl, &BUILTIN_TEXT_FONT, 0);
        lv_obj_set_style_text_color(lbl, color, 0);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(lbl, kCenterW);
        if (scroll) lv_label_set_long_mode(lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_align(lbl, align, 0, y_off);
        return lbl;
    };

    // 配网模式顶部固定提示（仅在显示左右色条时有效）
    if (has_bars) {
        make_label("双击切换模式", lv_color_hex(0xAAAAAA), LV_ALIGN_TOP_MID, 2);
    }
    // 顶部业务提示词（配网模式下移避开"双击切换"提示）
    make_label(top, lv_color_hex(0x333333), LV_ALIGN_TOP_MID, has_bars ? 22 : 6);

    // 中央二维码（调用方拼好内容，内部不做格式判断）
#if CONFIG_LV_USE_QRCODE
    constexpr int kQrSize = 160;
    lv_obj_t* qr = lv_qrcode_create(qr_overlay_);
    lv_qrcode_set_size(qr, kQrSize);
    lv_qrcode_set_dark_color(qr, lv_color_black());
    lv_qrcode_set_light_color(qr, lv_color_white());
    lv_qrcode_update(qr, qr_content, strlen(qr_content));
    lv_obj_align(qr, LV_ALIGN_CENTER, 0, -10);
#else
    make_label(qr_content, lv_color_hex(0x333333), LV_ALIGN_CENTER, 0);
#endif

    // 高亮大字（蓝色，如激活码 / 付款金额）
    int bottom_y = -3;
    if (highlight && highlight[0]) {
        make_label(highlight, lv_color_hex(0x2196F3), LV_ALIGN_BOTTOM_MID, -25);
        bottom_y = -6;  // 给高亮让出空间
    }

    // 底部辅助文字
    make_label(bottom, lv_color_hex(0x999999), LV_ALIGN_BOTTOM_MID, bottom_y, true);

    // QR 页全屏独占，overlay 必须在最前层（避免顶部 top 文字被全局状态栏 36px 遮挡）
    lv_obj_move_foreground(qr_overlay_);
    ESP_LOGI(TAG, "QR页: content=%s top=%s bottom=%s highlight=%s",
             qr_content, top ? top : "", bottom ? bottom : "", highlight ? highlight : "");
}

void UiDisplay::HideQrCode() {
    DisplayLockGuard lock(this);
    if (qr_overlay_) {
        lv_obj_del(qr_overlay_);
        qr_overlay_ = nullptr;
        ESP_LOGI(TAG, "隐藏 QR 页");
    }
    qr_double_click_cb_ = nullptr;
    qr_last_click_us_   = 0;
}

// LVGL click 事件 → 用上次时间戳判定 < 500ms 双击
void UiDisplay::OnQrClicked(lv_event_t* e) {
    auto* self = static_cast<UiDisplay*>(lv_event_get_user_data(e));
    if (!self || !self->qr_double_click_cb_) return;

    const uint64_t now  = esp_timer_get_time();
    const uint64_t last = self->qr_last_click_us_;
    constexpr uint64_t kDoubleClickWindowUs = 500 * 1000;

    if (last != 0 && (now - last) < kDoubleClickWindowUs) {
        // 拷贝 cb 后调，防止 cb 内部 HideQrCode 把 cb 自己析构
        auto cb = self->qr_double_click_cb_;
        self->qr_last_click_us_ = 0;
        cb();
    } else {
        self->qr_last_click_us_ = now;
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

// ============================================================
// 三维心智模型 · UI 场景维度（C）查询
// 详见 docs/p30-architecture.html § 一.5
//
// 阶段 0 实现：基于现有 mode flag + DeviceState 推断，不引入新成员变量。
// 优先级：ConfigQr（互斥独占）> Player > ControlCenter > Clock > Emoji
// ============================================================
SceneType UiDisplay::GetCurrentScene() const {
    // 配网 / 激活 QR 互斥独占（L5 全屏）
    auto state = Application::GetInstance().GetDeviceState();
    if (state == kDeviceStateWifiConfiguring ||
        state == kDeviceStateActivating) {
        return SceneType::kConfigQr;
    }
    // MP3 播放器页（is_player_mode_ 标志）
    if (is_player_mode_) {
        return SceneType::kPlayer;
    }
    // 控制中心（懒加载 + IsVisible）
    if (control_center_ && control_center_->IsVisible()) {
        return SceneType::kControlCenter;
    }
    // 时钟主屏（Idle 默认显示）
    if (is_clock_mode_) {
        return SceneType::kClock;
    }
    // 默认对话表情（Listening / Speaking 期）
    return SceneType::kEmoji;
}

// ============================================================
// 音乐播放器页（极简：曲名 + Play/Pause 圆按钮 + 时间 + 进度条）
// 布局参考上游 xiaozhi-esp32-189 player_page，去掉 prev/next 简化版。
// ============================================================

void UiDisplay::FormatTime(int ms, char* buf, size_t buf_size) {
    if (ms < 0) ms = 0;
    int total_sec = ms / 1000;
    int min = total_sec / 60;
    int sec = total_sec % 60;
    snprintf(buf, buf_size, "%d:%02d", min, sec);
}

void UiDisplay::CreatePlayerPage() {
    if (player_container_) return;
    auto* screen = lv_screen_active();
    if (!screen) return;

    using namespace ScreenConfig;

    player_container_ = lv_obj_create(screen);
    lv_obj_set_size(player_container_, WIDTH, HEIGHT);
    lv_obj_align(player_container_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_remove_flag(player_container_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(player_container_, lv_color_hex(Colors::BG_PRIMARY), 0);
    lv_obj_set_style_bg_opa(player_container_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(player_container_, 0, 0);
    lv_obj_set_style_radius(player_container_, RADIUS, 0);
    lv_obj_set_style_pad_all(player_container_, 0, 0);

    // 曲名（标题栏下方，长文本截断省略）— 不指定字体，跟随 LVGL 默认/theme 字体
    player_title_ = lv_label_create(player_container_);
    lv_label_set_text(player_title_, "正在播放");
    lv_obj_set_style_text_color(player_title_, lv_color_hex(Colors::TEXT_PRIMARY), 0);
    lv_obj_set_width(player_title_, WIDTH - 48);
    lv_obj_set_style_text_align(player_title_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(player_title_, LV_LABEL_LONG_DOT);
    lv_obj_align(player_title_, LV_ALIGN_TOP_MID, 0, HEADER_HEIGHT + 16);

    // Play/Pause 圆按钮（屏幕中央，64×64）
    constexpr int PLAY_SIZE = 64;
    constexpr int PLAY_CENTER_Y = 130;
    player_btn_play_ = lv_button_create(player_container_);
    lv_obj_set_size(player_btn_play_, PLAY_SIZE, PLAY_SIZE);
    lv_obj_set_pos(player_btn_play_, WIDTH / 2 - PLAY_SIZE / 2, PLAY_CENTER_Y - PLAY_SIZE / 2);
    lv_obj_set_style_bg_color(player_btn_play_, lv_color_hex(Colors::ACCENT_BLUE), 0);
    lv_obj_set_style_bg_opa(player_btn_play_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(player_btn_play_, PLAY_SIZE / 2, 0);
    lv_obj_set_style_border_width(player_btn_play_, 0, 0);
    lv_obj_set_style_shadow_width(player_btn_play_, 0, 0);
    lv_obj_set_style_bg_opa(player_btn_play_, LV_OPA_70, LV_STATE_PRESSED);

    player_play_icon_ = lv_label_create(player_btn_play_);
    lv_label_set_text(player_play_icon_, FONT_AWESOME_PAUSE);  // 默认播放中 → 显示 Pause 图标
    lv_obj_set_style_text_font(player_play_icon_, &BUILTIN_ICON_FONT, 0);
    lv_obj_set_style_text_color(player_play_icon_, lv_color_hex(Colors::TEXT_PRIMARY), 0);
    lv_obj_center(player_play_icon_);
    lv_obj_add_event_cb(player_btn_play_, OnPlayerPlayPauseClicked, LV_EVENT_CLICKED, this);

    // 时间标签（当前 | 总时长）
    constexpr int TIME_Y = 192;
    player_time_cur_ = lv_label_create(player_container_);
    lv_label_set_text(player_time_cur_, "0:00");
    lv_obj_set_style_text_font(player_time_cur_, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(player_time_cur_, lv_color_hex(Colors::TEXT_SECONDARY), 0);
    lv_obj_set_pos(player_time_cur_, 28, TIME_Y);

    player_time_total_ = lv_label_create(player_container_);
    lv_label_set_text(player_time_total_, "0:00");
    lv_obj_set_style_text_font(player_time_total_, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(player_time_total_, lv_color_hex(Colors::TEXT_SECONDARY), 0);
    lv_obj_align(player_time_total_, LV_ALIGN_TOP_RIGHT, -28, TIME_Y);

    // 进度条（lv_bar，仅显示）
    constexpr int BAR_Y = TIME_Y + 26;
    player_progress_ = lv_bar_create(player_container_);
    lv_obj_set_size(player_progress_, WIDTH - 56, 6);
    lv_obj_align(player_progress_, LV_ALIGN_TOP_MID, 0, BAR_Y);
    lv_bar_set_range(player_progress_, 0, 1000);
    lv_bar_set_value(player_progress_, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(player_progress_, lv_color_hex(0x333333), 0);
    lv_obj_set_style_bg_color(player_progress_, lv_color_hex(Colors::ACCENT_BLUE), LV_PART_INDICATOR);
    lv_obj_set_style_radius(player_progress_, 3, 0);
    lv_obj_set_style_radius(player_progress_, 3, LV_PART_INDICATOR);

    // 默认隐藏，SwitchToPlayerMode 时再显示
    lv_obj_add_flag(player_container_, LV_OBJ_FLAG_HIDDEN);
}

void UiDisplay::OnPlayerPlayPauseClicked(lv_event_t* e) {
    auto* self = static_cast<UiDisplay*>(lv_event_get_user_data(e));
    if (!self || !self->on_player_pause_toggle_) return;
    // 关键：LVGL task 上下文 + DisplayLockGuard 已持，直接调 MusicPlayer::Pause →
    // audio_codec data_if_mutex_ 与 LVGL 锁可能形成顺序倒置（watchdog reset）。
    // 投到主任务 Schedule 执行，立即返回让 LVGL 释放锁。
    auto cb = self->on_player_pause_toggle_;   // copy std::function 到 lambda capture
    Application::GetInstance().Schedule([cb]() {
        if (cb) cb();
    });
}

// 200ms tick：直接从 MusicPlayer 拉进度/暂停状态，自动刷新 UI
void UiDisplay::PlayerTickCb(lv_timer_t* t) {
    auto* self = static_cast<UiDisplay*>(lv_timer_get_user_data(t));
    if (!self || !self->is_player_mode_) return;
    auto& mp = MusicPlayer::GetInstance();
    self->UpdatePlayerProgress(mp.GetPositionMs(), mp.GetTotalDurationMs());
    self->SetPlayerPaused(mp.IsPaused());
}

void UiDisplay::SwitchToPlayerMode(const char* title) {
    DisplayLockGuard lock(this);
    if (!setup_ui_called_) return;
    if (!player_container_) CreatePlayerPage();
    if (!player_container_) return;

    if (player_title_) {
        const char* t = (title && title[0]) ? title : "正在播放";
        lv_label_set_text(player_title_, t);
        lv_obj_invalidate(player_title_);
        ESP_LOGI(TAG, "Player title: %s", t);
    }
    is_player_paused_ = false;
    if (player_play_icon_) lv_label_set_text(player_play_icon_, FONT_AWESOME_PAUSE);
    if (player_progress_)  lv_bar_set_value(player_progress_, 0, LV_ANIM_OFF);
    if (player_time_cur_)  lv_label_set_text(player_time_cur_, "0:00");
    if (player_time_total_) lv_label_set_text(player_time_total_, "0:00");

    // 隐藏其他模式的 widget tree
    if (clock_container_) lv_obj_add_flag(clock_container_, LV_OBJ_FLAG_HIDDEN);
    if (content_)         lv_obj_add_flag(content_, LV_OBJ_FLAG_HIDDEN);
    if (container_)       lv_obj_add_flag(container_, LV_OBJ_FLAG_HIDDEN);
    if (status_bar_)      lv_obj_add_flag(status_bar_, LV_OBJ_FLAG_HIDDEN);
    if (emoji_box_)       lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);

    lv_obj_remove_flag(player_container_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(player_container_);

    // 全局状态栏保持显示（顶部 36px），与时钟模式一致
    if (global_status_bar_) {
        lv_obj_remove_flag(global_status_bar_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(global_status_bar_);
    }
    if (qr_overlay_) lv_obj_move_foreground(qr_overlay_);

    is_player_mode_ = true;
    is_clock_mode_  = false;   // 复用 clock 状态字段，与 SwitchToChatMode 保持互斥

    // 启动 200ms 进度刷新 timer（懒创建）
    if (!player_tick_) player_tick_ = lv_timer_create(PlayerTickCb, 200, this);
    else               lv_timer_resume(player_tick_);

    ESP_LOGI(TAG, "Switched to player mode: %s", title ? title : "");
}

void UiDisplay::SwitchOutPlayerMode() {
    DisplayLockGuard lock(this);
    if (!is_player_mode_) return;
    if (player_container_) lv_obj_add_flag(player_container_, LV_OBJ_FLAG_HIDDEN);
    if (player_tick_) lv_timer_pause(player_tick_);
    is_player_mode_ = false;
    // 退出时回时钟主屏（idle 状态）
    SwitchToClockMode();
    ESP_LOGI(TAG, "Switched out player mode");
}

void UiDisplay::UpdatePlayerProgress(int position_ms, int total_ms) {
    DisplayLockGuard lock(this);
    if (!is_player_mode_ || !player_progress_) return;

    char buf[16];
    if (player_time_cur_) {
        FormatTime(position_ms, buf, sizeof(buf));
        lv_label_set_text(player_time_cur_, buf);
    }
    if (player_time_total_) {
        FormatTime(total_ms, buf, sizeof(buf));
        lv_label_set_text(player_time_total_, buf);
    }
    int v = (total_ms > 0) ? (int)((int64_t)position_ms * 1000 / total_ms) : 0;
    if (v < 0) v = 0; else if (v > 1000) v = 1000;
    lv_bar_set_value(player_progress_, v, LV_ANIM_OFF);
}

void UiDisplay::SetPlayerPaused(bool paused) {
    DisplayLockGuard lock(this);
    if (!player_play_icon_) return;
    if (is_player_paused_ == paused) return;
    is_player_paused_ = paused;
    lv_label_set_text(player_play_icon_, paused ? FONT_AWESOME_PLAY : FONT_AWESOME_PAUSE);
}
