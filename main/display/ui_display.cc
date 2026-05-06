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

// [量产稳定期] 切断 ui_display 对 main/display/ui 子目录的依赖
// 恢复时取消下列 include 并恢复 ScreenConfig::* / ControlCenter 实现块
// #include "ui/resources/ui_image_manager.h"
// #include "ui/resources/ui_img_paths.h"
// #include "ui/widgets/control_center.h"
// #include "ui/theme/ui_config.h"

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

LV_FONT_DECLARE(BUILTIN_ICON_FONT);
#include <cbin_font.h>
#include "text_font.h"

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
    // clock_tick_ 已移除：时间刷新沿用 1Hz CLOCK_TICK → UpdateStatusBar 链路
}

// ============================================================
// SetupUI override：先调父类，然后注入 UI 扩展
// ============================================================

void UiDisplay::SetupUI() {
    // 0. RAM proxy 初始化（必须在任何 lv_obj_set_style_text_font(&g_text_font, ...) 调用前）
    InitTextFontProxy();

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

    // 2. 启用 status_label_ / notification_label_ 的 recolor 富文本
    //    业务可在文本中嵌入颜色：display->SetStatus("#FF3030 ●# 录音中")
    //    ⚠️ 启用后业务文本不能裸出现 '#'，需要时用 '##' 转义。
    if (status_label_)       lv_label_set_recolor(status_label_, true);
    if (notification_label_) lv_label_set_recolor(notification_label_, true);

    // 3. 开机动画
    StartBootAnimation();

    // 4. 文本字体补字 fallback：BUILTIN_TEXT_FONT 仅链入 ~600 常字，
    //    通过 cbin 加载 GB 2312 全字（7000+），缺字时由 LVGL 自动 fallback。
    LoadFallbackTextFont();

    // [量产稳定期] 第 5 步原本是 EnableStatusBarTapForControlCenter()——已与 ControlCenter 一并下线
}

// BUILTIN_TEXT_FONT 直接位于 Flash rodata（不可写），无法写它的 fallback 字段。
// 我们在 RAM 中维护一个按值复制的 proxy，所有 UI 代码以 g_text_font 替代 BUILTIN_TEXT_FONT
// 引用，proxy 的 fallback 字段在 cbin 字体加载完成后才被写入。
// 详细说明见 main/display/text_font.h。
lv_font_t g_text_font;        // RAM-resident proxy，extern 于 text_font.h
static bool s_text_font_proxy_initialized = false;

void InitTextFontProxy() {
    if (s_text_font_proxy_initialized) return;
    g_text_font = BUILTIN_TEXT_FONT;   // 按值复制 lv_font_t（callbacks/cmaps 仍指向 Flash rodata，OK）
    s_text_font_proxy_initialized = true;
}

// 同名加载约定：cbin 文件名 = BUILTIN_TEXT_FONT 名 + ".bin"。
// CMakeLists 改主字体名时，build_default_assets.py 和此处自动联动，无需多处同步。
#define _STR_HELPER(x) #x
#define _STR(x) _STR_HELPER(x)
static constexpr const char* kFallbackFontAsset = _STR(BUILTIN_TEXT_FONT) ".bin";

void UiDisplay::LoadFallbackTextFont() {
    if (fallback_text_font_) return;
    InitTextFontProxy();             // 防御：保证 proxy 已就绪

    void* ptr = nullptr; size_t size = 0;
    if (!Assets::GetInstance().GetAssetData(kFallbackFontAsset, ptr, size) || !ptr) {
        ESP_LOGW(TAG, "Fallback cbin font asset not found: %s — out-of-glyph chars will render as boxes",
                 kFallbackFontAsset);
        return;
    }
    fallback_text_font_ = cbin_font_create(static_cast<uint8_t*>(ptr));
    if (!fallback_text_font_) {
        ESP_LOGE(TAG, "cbin_font_create failed for %s", kFallbackFontAsset);
        return;
    }
    ESP_LOGI(TAG, "Fallback text font loaded: %s (%u bytes)", kFallbackFontAsset, (unsigned)size);
    g_text_font.fallback = fallback_text_font_;       // 写 RAM proxy（rodata 主字体不可写）
}

// ============================================================
// 子类只 override UpdateStatusBar 用来 1s tick 刷时钟，不再自建 status bar
// ============================================================

void UiDisplay::UpdateStatusBar(bool update_all) {
    if (is_clock_mode_) {
        last_status_update_time_ = std::chrono::system_clock::now();
    }
    LvglDisplay::UpdateStatusBar(update_all);
    DisplayLockGuard lock(this);

    // clock 模式：1s tick 刷新时钟（cbin 字体尚未就绪时延迟加载）
    if (is_clock_mode_ && clock_time_label_) {
        if (!clock_big_font_ || !clock_text_font_) LoadClockFonts();
        UpdateClockTime();
    }
}

// ============================================================
// 时钟主屏（内联实现）
// ============================================================

namespace {
// 三段文字以屏幕中心为基准做 y 偏移；x 留 0 = 水平居中。
constexpr int16_t kClockTimeOffsetY = -36;   // 大字 HH:MM 距中心 -36px（偏上）
constexpr int16_t kClockDateOffsetY =  40;   // 日期距中心 +40px
constexpr int16_t kClockWeekOffsetY =  76;   // 星期距中心 +76px
constexpr int16_t kClockOffsetX     =   0;   // 三段统一水平居中

// [量产稳定期] 从 display/ui/theme/ui_config.h 内联（切断对 ui/ 目录依赖）
// 恢复时改回 ScreenConfig::WIDTH / HEIGHT / RADIUS / HEADER_HEIGHT / Colors::*
constexpr int      kScreenWidth        = 284;
constexpr int      kScreenHeight       = 240;
constexpr int      kScreenRadius       = 25;
constexpr int      kHeaderHeight       = 36;
constexpr uint32_t kColorBgPrimary     = 0x000000;
constexpr uint32_t kColorTextPrimary   = 0xFFFFFF;
constexpr uint32_t kColorTextSecondary = 0xCCCCCC;
constexpr uint32_t kColorTextDisabled  = 0x888888;
constexpr uint32_t kColorAccentBlue    = 0x64B5F6;
}  // namespace

void UiDisplay::CreateClockPage() {
    if (clock_container_) return;
    auto* screen = lv_screen_active();
    if (!screen) return;

    clock_container_ = lv_obj_create(screen);
    lv_obj_set_size(clock_container_, kScreenWidth, kScreenHeight);
    lv_obj_align(clock_container_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_remove_flag(clock_container_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(clock_container_, lv_color_hex(kColorBgPrimary), 0);
    lv_obj_set_style_bg_opa(clock_container_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(clock_container_, 0, 0);
    lv_obj_set_style_radius(clock_container_, kScreenRadius, 0);
    lv_obj_set_style_pad_all(clock_container_, 0, 0);

    clock_time_label_ = lv_label_create(clock_container_);
    lv_obj_set_style_text_color(clock_time_label_, lv_color_hex(kColorTextPrimary), 0);
    lv_label_set_text(clock_time_label_, "--:--");
    lv_obj_align(clock_time_label_, LV_ALIGN_CENTER, kClockOffsetX, kClockTimeOffsetY);

    LoadClockFonts();
    if (!clock_big_font_) lv_obj_set_style_text_font(clock_time_label_, &g_text_font, 0);

    const lv_font_t* text_font = clock_text_font_ ? clock_text_font_ : &g_text_font;

    clock_date_label_ = lv_label_create(clock_container_);
    lv_obj_set_style_text_font(clock_date_label_, text_font, 0);
    lv_obj_set_style_text_color(clock_date_label_, lv_color_hex(kColorTextSecondary), 0);
    lv_label_set_text(clock_date_label_, "----年--月--日");
    lv_obj_align(clock_date_label_, LV_ALIGN_CENTER, kClockOffsetX, kClockDateOffsetY);

    clock_week_label_ = lv_label_create(clock_container_);
    lv_obj_set_style_text_font(clock_week_label_, text_font, 0);
    lv_obj_set_style_text_color(clock_week_label_, lv_color_hex(kColorTextDisabled), 0);
    lv_label_set_text(clock_week_label_, "星期--");
    lv_obj_align(clock_week_label_, LV_ALIGN_CENTER, kClockOffsetX, kClockWeekOffsetY);

    UpdateClockTime();
}

void UiDisplay::LoadClockFonts() {
    auto load = [](const char* name, const lv_font_t*& dst) {
        if (dst) return;
        void* ptr = nullptr; size_t size = 0;
        if (!Assets::GetInstance().GetAssetData(name, ptr, size) || !ptr) return;
        dst = cbin_font_create(static_cast<uint8_t*>(ptr));
        if (dst) const_cast<lv_font_t*>(dst)->fallback = &g_text_font;
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
    if (is_player_mode_) return;

    if (!clock_container_) CreateClockPage();

    if (content_)     lv_obj_add_flag(content_, LV_OBJ_FLAG_HIDDEN);
    if (container_)   lv_obj_add_flag(container_, LV_OBJ_FLAG_HIDDEN);
    if (emoji_box_)   lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);

    if (clock_container_) {
        lv_obj_remove_flag(clock_container_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(clock_container_);
    }
    if (top_bar_) {
        lv_obj_remove_flag(top_bar_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(top_bar_);
    }
    if (status_bar_) {
        lv_obj_remove_flag(status_bar_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_opa(status_bar_, LV_OPA_COVER, 0);  // 防 boot fade_out 残留 TRANSP 致 notification 不可见
        lv_obj_move_foreground(status_bar_);
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
    if (emoji_box_) {
        lv_obj_remove_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_opa(emoji_box_, LV_OPA_COVER, 0);  // 关键：boot fade_out 把 opa=TRANSP
        lv_obj_move_foreground(emoji_box_);
    }
    // bottom_bar_ 挂在 screen 上（与 container_ 同级 sibling），必须显式提顶否则
    // 被 move_foreground(container_) 盖住 —— SetChatMessage 即使 remove HIDDEN 也看不见。
    if (bottom_bar_) lv_obj_move_foreground(bottom_bar_);

    // chat 模式：top_bar_ HIDDEN（信号/电量图标让位 emoji 满屏沉浸感 · 产品决策）
    //           status_bar_ 显示（含 status_label_ 对话状态文字 + ShowNotification 通知通道）
    if (top_bar_) lv_obj_add_flag(top_bar_, LV_OBJ_FLAG_HIDDEN);
    if (status_bar_) {
        lv_obj_remove_flag(status_bar_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_opa(status_bar_, LV_OPA_COVER, 0);
    }
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
// 动态 GIF 注入（识字笔画等场景）：替换 emoji_collection "font" 槽位
// ============================================================
void UiDisplay::UpdateFontGif(uint8_t* gif_buffer, size_t size) {
    if (!gif_buffer || size == 0) return;
    auto* lvgl_theme = static_cast<LvglTheme*>(GetTheme());
    if (!lvgl_theme) {
        ESP_LOGW(TAG, "UpdateFontGif: no theme, free buffer");
        heap_caps_free(gif_buffer);
        return;
    }
    auto emoji_collection = lvgl_theme->emoji_collection();
    if (!emoji_collection) {
        ESP_LOGW(TAG, "UpdateFontGif: no emoji_collection, free buffer");
        heap_caps_free(gif_buffer);
        return;
    }
    // 所有权转移给 LvglRawImage（其析构会 free buffer 含 GIF 解码资源）
    DisplayLockGuard lock(this);
    emoji_collection->ReplaceEmoji("font", new LvglRawImage(gif_buffer, size));
    ESP_LOGI(TAG, "Font GIF replaced with PSRAM buffer (%u bytes)", (unsigned)size);
}

// ============================================================
// SetEmotion override：当前为 font 时仅跳过 neutral；其他 emotion 都允许替换
// ============================================================
void UiDisplay::SetEmotion(const char* emotion) {
    if (!emotion) return;

    // 当前显示 font GIF 时跳过 neutral（application.cc 进 listening 默认强制注入），
    // happy/laughing/sad 等由 LLM 判定的情绪正常通过。
    if (current_is_font_ && strcmp(emotion, "neutral") == 0) {
        return;
    }

    bool is_font = (strcmp(emotion, "font") == 0);
    LcdDisplay::SetEmotion(emotion);

    // font: 按 GIF 实际宽度等比缩放到 kFontEmojiSizePx；其他 emoji: 复位原尺寸
    if (emoji_image_) {
        DisplayLockGuard lock(this);
        int32_t zoom = kDefaultEmojiZoom;
        if (is_font && gif_controller_ && gif_controller_->IsLoaded()) {
            uint16_t w = gif_controller_->width();
            if (w > 0) {
                zoom = (int32_t)kFontEmojiSizePx * 256 / w;
            }
        }
        lv_image_set_scale(emoji_image_, zoom);
    }

    current_is_font_ = is_font;
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
            lv_obj_set_style_text_font(ch, &g_text_font, 0);
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
        lv_obj_set_style_text_font(lbl, &g_text_font, 0);
        lv_obj_set_style_text_color(lbl, color, 0);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(lbl, kCenterW);
        if (scroll) lv_label_set_long_mode(lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_align(lbl, align, 0, y_off);
        return lbl;
    };

    // 顶部一行（调用方传什么显什么：配网传"双击切换模式"，激活传"绑定设备"...）
    make_label(top, lv_color_hex(0x333333), LV_ALIGN_TOP_MID, 6);

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
// [量产稳定期] 控制中心（下拉手势触发）整体下线
// ----------------------------------------------------------------
// 原实现：EnsureControlCenter / ShowControlCenter / HideControlCenter
//        IsControlCenterVisible / EnableStatusBarTapForControlCenter
//        OnStatusBarClicked
// 公共三方法已在 ui_display.h stub 化，三个 board 调用兼容（运行时无操作）
// 恢复时：
//   1. main/CMakeLists.txt 取消注释 control_center.cc / ui_image_manager.cc /
//      ui_img_icon_signal_*_png.c / INCLUDE_DIRS 子目录
//   2. ui_display.h 恢复 forward decl + unique_ptr 字段 + 私有方法声明
//   3. ui_display.cc 恢复 ui/* include + 此实现块 + Setup() 第 5 步
// ============================================================

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

    // [量产稳定期] 原 `using namespace ScreenConfig;` 已切断，改用本文件匿名 namespace 内联常量

    player_container_ = lv_obj_create(screen);
    lv_obj_set_size(player_container_, kScreenWidth, kScreenHeight);
    lv_obj_align(player_container_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_remove_flag(player_container_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(player_container_, lv_color_hex(kColorBgPrimary), 0);
    lv_obj_set_style_bg_opa(player_container_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(player_container_, 0, 0);
    lv_obj_set_style_radius(player_container_, kScreenRadius, 0);
    lv_obj_set_style_pad_all(player_container_, 0, 0);

    // 曲名（标题栏下方，长文本截断省略）— 不指定字体，跟随 LVGL 默认/theme 字体
    player_title_ = lv_label_create(player_container_);
    lv_label_set_text(player_title_, "正在播放");
    lv_obj_set_style_text_color(player_title_, lv_color_hex(kColorTextPrimary), 0);
    lv_obj_set_width(player_title_, kScreenWidth - 48);
    lv_obj_set_style_text_align(player_title_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(player_title_, LV_LABEL_LONG_DOT);
    lv_obj_align(player_title_, LV_ALIGN_TOP_MID, 0, kHeaderHeight + 16);

    // Play/Pause 圆按钮（屏幕中央，64×64）
    constexpr int PLAY_SIZE = 64;
    constexpr int PLAY_CENTER_Y = 130;
    player_btn_play_ = lv_button_create(player_container_);
    lv_obj_set_size(player_btn_play_, PLAY_SIZE, PLAY_SIZE);
    lv_obj_set_pos(player_btn_play_, kScreenWidth / 2 - PLAY_SIZE / 2, PLAY_CENTER_Y - PLAY_SIZE / 2);
    lv_obj_set_style_bg_color(player_btn_play_, lv_color_hex(kColorAccentBlue), 0);
    lv_obj_set_style_bg_opa(player_btn_play_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(player_btn_play_, PLAY_SIZE / 2, 0);
    lv_obj_set_style_border_width(player_btn_play_, 0, 0);
    lv_obj_set_style_shadow_width(player_btn_play_, 0, 0);
    lv_obj_set_style_bg_opa(player_btn_play_, LV_OPA_70, LV_STATE_PRESSED);

    player_play_icon_ = lv_label_create(player_btn_play_);
    lv_label_set_text(player_play_icon_, FONT_AWESOME_PAUSE);  // 默认播放中 → 显示 Pause 图标
    lv_obj_set_style_text_font(player_play_icon_, &BUILTIN_ICON_FONT, 0);
    lv_obj_set_style_text_color(player_play_icon_, lv_color_hex(kColorTextPrimary), 0);
    lv_obj_center(player_play_icon_);
    // RF 抗扰核心是 PRESS_LOCK：阻止 swipe drag-into-click（swipe 起点不在按钮上
    // 即使终点落在按钮也不触发 click）。这一道闸已能挡住实测最常见的假触模式。
    // CLICKED 单击即响应，保持按钮灵敏度。如真机仍偶发假触，再考虑 LONG_PRESSED。
    lv_obj_add_flag(player_btn_play_, LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_add_event_cb(player_btn_play_, OnPlayerPlayPauseClicked, LV_EVENT_CLICKED, this);

    // 时间标签（当前 | 总时长）
    constexpr int TIME_Y = 192;
    player_time_cur_ = lv_label_create(player_container_);
    lv_label_set_text(player_time_cur_, "0:00");
    lv_obj_set_style_text_font(player_time_cur_, &g_text_font, 0);
    lv_obj_set_style_text_color(player_time_cur_, lv_color_hex(kColorTextSecondary), 0);
    lv_obj_set_pos(player_time_cur_, 28, TIME_Y);

    player_time_total_ = lv_label_create(player_container_);
    lv_label_set_text(player_time_total_, "0:00");
    lv_obj_set_style_text_font(player_time_total_, &g_text_font, 0);
    lv_obj_set_style_text_color(player_time_total_, lv_color_hex(kColorTextSecondary), 0);
    lv_obj_align(player_time_total_, LV_ALIGN_TOP_RIGHT, -28, TIME_Y);

    // 进度条（lv_bar，仅显示）
    constexpr int BAR_Y = TIME_Y + 26;
    player_progress_ = lv_bar_create(player_container_);
    lv_obj_set_size(player_progress_, kScreenWidth - 56, 6);
    lv_obj_align(player_progress_, LV_ALIGN_TOP_MID, 0, BAR_Y);
    lv_bar_set_range(player_progress_, 0, 1000);
    lv_bar_set_value(player_progress_, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(player_progress_, lv_color_hex(0x333333), 0);
    lv_obj_set_style_bg_color(player_progress_, lv_color_hex(kColorAccentBlue), LV_PART_INDICATOR);
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

    // 父类 top_bar_ 保持显示并提顶，与时钟模式一致
    if (top_bar_) {
        lv_obj_remove_flag(top_bar_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(top_bar_);
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
