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

#include "application.h"
#include "audio_codec.h"
#include "audio/music_player.h"
#include "backlight.h"
#include "board.h"
#include "dual_network_board.h"
#include "system_info.h"
#include "assets.h"
#include "lvgl_theme.h"
#include "ui/widgets/control_center.h"

#include <cstring>
#include <string>
#include <time.h>
#include <font_awesome.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_app_desc.h>

LV_FONT_DECLARE(BUILTIN_ICON_FONT);
#include <cbin_font.h>
#include "text_font.h"

#define TAG "UiDisplay"

LV_FONT_DECLARE(BUILTIN_TEXT_FONT);
LV_IMG_DECLARE(ui_img_start_logo_png);

namespace {

inline bool ContainsCjk(const char* text) {
    if (!text) return false;
    while (*text) {
        unsigned char b0 = (unsigned char)*text;
        if ((b0 & 0xF0) == 0xE0) {
            // 3 字节 UTF-8（U+0800-U+FFFF，CJK 在此范围内）
            unsigned char b1 = (unsigned char)text[1];
            unsigned char b2 = (unsigned char)text[2];
            if (b1 == 0 || b2 == 0) break;
            uint32_t cp = ((b0 & 0x0F) << 12) | ((b1 & 0x3F) << 6) | (b2 & 0x3F);
            if (cp >= 0x4E00 && cp <= 0x9FFF) return true;
            text += 3;
        } else if ((b0 & 0xE0) == 0xC0) {
            text += 2;     // 2 字节 UTF-8（拉丁补充 / 拼音声调 / Phonics 中点 U+00B7 等，不算 CJK）
        } else if ((b0 & 0xF8) == 0xF0) {
            text += 4;     // 4 字节（极少见）
        } else {
            text++;        // 1 字节 ASCII
        }
    }
    return false;
}

inline int Utf8CharCount(const char* text) {
    if (!text) return 0;
    int count = 0;
    while (*text) {
        if ((*text & 0xC0) != 0x80) count++;
        text++;
    }
    return count;
}

inline const lv_font_t* PickFont(const char* main_text, bool is_cjk,
                                  const lv_font_t* font_56,
                                  const lv_font_t* font_48) {
    if (!main_text || !main_text[0] || !font_56) return nullptr;
    int n = Utf8CharCount(main_text);
    if (n < 1) return nullptr;

    if (is_cjk) {
        return n <= 4 ? font_56 : nullptr;
    }

    if (n > 12) return nullptr;

    lv_point_t sz;
    lv_text_get_size(&sz, main_text, font_56, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_EXPAND);
    if (sz.x <= 280) return font_56;

    if (!font_48) return nullptr;
    lv_text_get_size(&sz, main_text, font_48, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_EXPAND);
    if (sz.x <= 280) return font_48;
    return nullptr;
}

}  // anonymous namespace


UiDisplay::UiDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                     int width, int height, int offset_x, int offset_y,
                     bool mirror_x, bool mirror_y, bool swap_xy)
    : SpiLcdDisplay(panel_io, panel, width, height, offset_x, offset_y,
                    mirror_x, mirror_y, swap_xy) {
}

UiDisplay::~UiDisplay() {
}


void UiDisplay::SetupUI() {
    InitTextFontProxy();

    SpiLcdDisplay::SetupUI();
    DisplayLockGuard lock(this);

    if (emoji_label_) {
        lv_label_set_text(emoji_label_, "");
        lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
    }
    if (emoji_image_) {
        lv_image_set_src(emoji_image_, &ui_img_start_logo_png);
        lv_obj_remove_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
    }
    if (emoji_box_) {
        lv_obj_set_style_opa(emoji_box_, LV_OPA_TRANSP, 0);
        lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(emoji_box_, OnFontExitClicked, LV_EVENT_CLICKED, this);
    }

    if (auto* screen = lv_screen_active()) {
        boot_brand_label_ = lv_label_create(screen);
        lv_label_set_text(boot_brand_label_, "MyDazy");
        lv_obj_set_style_text_color(boot_brand_label_, lv_color_hex(0xFFFFFF), 0);
        lv_obj_align(boot_brand_label_, LV_ALIGN_CENTER, 0, 70);
        lv_obj_set_style_opa(boot_brand_label_, LV_OPA_TRANSP, 0);
    }

    if (status_label_)       lv_label_set_recolor(status_label_, true);
    if (notification_label_) lv_label_set_recolor(notification_label_, true);

    // 3. 开机动画
    StartBootAnimation();

    // 4. 文本字体补字 fallback：BUILTIN_TEXT_FONT 仅链入 ~600 常字，
    LoadFallbackTextFont();
}

lv_font_t g_text_font;        // RAM-resident proxy，extern 于 text_font.h
static bool s_text_font_proxy_initialized = false;

void InitTextFontProxy() {
    if (s_text_font_proxy_initialized) return;
    g_text_font = BUILTIN_TEXT_FONT;   // 按值复制 lv_font_t（callbacks/cmaps 仍指向 Flash rodata，OK）
    s_text_font_proxy_initialized = true;
}

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


void UiDisplay::UpdateStatusBar(bool update_all) {
    if (active_scene_ == SceneType::kClock) {
        last_status_update_time_ = std::chrono::system_clock::now();
    }
    LvglDisplay::UpdateStatusBar(update_all);
    DisplayLockGuard lock(this);

    // clock 模式：1s tick 刷新时钟（cbin 字体尚未就绪时延迟加载）
    if (active_scene_ == SceneType::kClock && clock_time_label_) {
        if (!clock_big_font_ || !clock_text_font_) EnsureDisplayFonts();
        UpdateClockTime();
    }
}

namespace {
// 三段文字以屏幕中心为基准做 y 偏移；x 留 0 = 水平居中。
constexpr int16_t kClockTimeOffsetY = -26;   // 大字 HH:MM 距中心 -26px（原 -36，下移 10px）
constexpr int16_t kClockDateOffsetY =  40;   // 日期距中心 +40px
constexpr int16_t kClockWeekOffsetY =  76;   // 星期距中心 +76px
constexpr int16_t kClockOffsetX     =   0;   // 三段统一水平居中

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

    EnsureDisplayFonts();
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

void UiDisplay::EnsureDisplayFonts() {
    auto load = [](const char* name, const lv_font_t*& dst) {
        if (dst) return;
        void* ptr = nullptr; size_t size = 0;
        if (!Assets::GetInstance().GetAssetData(name, ptr, size) || !ptr) {
            ESP_LOGW(TAG, "[font] asset miss: %s — 检查 assets 分区是否已烧 (idf.py flash)", name);
            return;
        }
        dst = cbin_font_create(static_cast<uint8_t*>(ptr));
        if (!dst) {
            ESP_LOGE(TAG, "[font] cbin_font_create failed: %s (size=%u)", name, (unsigned)size);
            return;
        }
        const_cast<lv_font_t*>(dst)->fallback = &g_text_font;
        ESP_LOGI(TAG, "[font] loaded: %s (size=%u, line_h=%d)",
                 name, (unsigned)size, (int)dst->line_height);
    };
    load("font_maru_88_4.bin", clock_big_font_);
    load("font_maru_30_4.bin", clock_text_font_);
    load("font_maru_48_4.bin", edu_main_font_);
    load("font_maru_56_4.bin", edu_main_56_font_);

    if (edu_main_font_ && clock_text_font_) {
        const_cast<lv_font_t*>(edu_main_font_)->fallback = clock_text_font_;
    }
    if (edu_main_56_font_ && clock_text_font_) {
        const_cast<lv_font_t*>(edu_main_56_font_)->fallback = clock_text_font_;
    }

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
    if (active_scene_ == SceneType::kClock) return;
    if (!setup_ui_called_) return;
    if (active_scene_ == SceneType::kPlayer) return;
    if (active_scene_ == SceneType::kPomodoro) return;

    HideEduCard();   // 状态切换清场，防止教育卡遮挡时钟主屏
    HideFontGif();   // 同步清 font 状态，防止 GIF 笔画残留

    if (!clock_container_) CreateClockPage();

    if (content_)     lv_obj_add_flag(content_, LV_OBJ_FLAG_HIDDEN);
    if (container_)   lv_obj_add_flag(container_, LV_OBJ_FLAG_HIDDEN);
    if (emoji_box_)   lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
    if (pomodoro_container_) lv_obj_add_flag(pomodoro_container_, LV_OBJ_FLAG_HIDDEN);

    if (clock_container_) {
        lv_obj_remove_flag(clock_container_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(clock_container_);
    }
    if (top_bar_) {
        lv_obj_remove_flag(top_bar_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(top_bar_);
    }
    SetTopBarIconsVisible(true);   // 恢复 chat 模式下隐藏的三个图标
    RaiseStatusBar();
    if (qr_overlay_) lv_obj_move_foreground(qr_overlay_);

    active_scene_ = SceneType::kClock;
    ESP_LOGI(TAG, "Switched to clock mode");
}

void UiDisplay::SwitchToChatMode() {
    DisplayLockGuard lock(this);
    if (active_scene_ != SceneType::kClock && active_scene_ != SceneType::kPomodoro) return;

    HideEduCard();   // 状态切换清场，防止教育卡遮挡 emoji 表情
    HideFontGif();   // 同步清 font 状态，防止 GIF 笔画残留

    if (clock_container_) {
        lv_obj_add_flag(clock_container_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_background(clock_container_);
    }
    // 软独占：从 Pomodoro 切到 Chat（用户唤醒对话）· 隐藏番茄钟容器，让 emoji_box 接管
    if (pomodoro_container_) lv_obj_add_flag(pomodoro_container_, LV_OBJ_FLAG_HIDDEN);

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

    SetTopBarIconsVisible(false);
    RaiseStatusBar();
    if (qr_overlay_) lv_obj_move_foreground(qr_overlay_);

    active_scene_ = SceneType::kChat;   // chat 主 widget = emoji_box
    ESP_LOGI(TAG, "Switched to chat mode");
}

// ============================================================
// 开机 Logo（静态显示 · 无动画 · Idle 时由状态机触发渐出 → 切时钟）
// ============================================================

static void boot_opa_cb(void* obj, int32_t v) {
    lv_obj_set_style_opa(static_cast<lv_obj_t*>(obj), v, 0);
}

void UiDisplay::StartBootAnimation() {
    if (!emoji_box_) return;
    lv_obj_set_style_opa(emoji_box_, LV_OPA_COVER, 0);
    if (status_bar_) lv_obj_set_style_opa(status_bar_, LV_OPA_COVER, 0);
}

void UiDisplay::FinishBootAndShowClock() {
    DisplayLockGuard lock(this);
    if (active_scene_ == SceneType::kClock) return;   // 幂等：已切时钟，重复 Idle 事件忽略
    if (!setup_ui_called_) return;
    if (active_scene_ == SceneType::kPlayer) return;

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

uint32_t UiDisplay::BeginFontPending() {
    uint32_t id = font_request_next_.fetch_add(1, std::memory_order_relaxed) + 1;
    font_request_active_.store(id, std::memory_order_relaxed);
    font_pending_.store(true, std::memory_order_release);
    return id;
}

void UiDisplay::CancelFontPending(uint32_t request_id) {
    if (request_id == 0) return;
    uint32_t active = font_request_active_.load(std::memory_order_relaxed);
    if (request_id != active) return;  // stale, 让最新 token 接管 pending
    font_pending_.store(false, std::memory_order_release);
}

void UiDisplay::FontGif(uint8_t* gif_buffer, size_t size, uint32_t request_id) {
    if (!gif_buffer || size == 0) return;

    // 并发去重：晚到的 stale 请求丢弃 · 只允许最新 token 装载
    if (request_id != 0) {
        uint32_t active = font_request_active_.load(std::memory_order_relaxed);
        if (request_id != active) {
            ESP_LOGI(TAG, "FontGif: stale token %u (active=%u), drop %u bytes",
                     (unsigned)request_id, (unsigned)active, (unsigned)size);
            heap_caps_free(gif_buffer);
            return;
        }
    }

    auto* lvgl_theme = static_cast<LvglTheme*>(GetTheme());
    if (!lvgl_theme) {
        ESP_LOGW(TAG, "FontGif: no theme, free buffer");
        heap_caps_free(gif_buffer);
        CancelFontPending(request_id);
        return;
    }
    auto emoji_collection = lvgl_theme->emoji_collection();
    if (!emoji_collection) {
        ESP_LOGW(TAG, "FontGif: no emoji_collection, free buffer");
        heap_caps_free(gif_buffer);
        CancelFontPending(request_id);
        return;
    }

    DisplayLockGuard lock(this);

    auto* raw = new (std::nothrow) LvglRawImage(gif_buffer, size, /*owns_data=*/true);
    if (!raw) {
        ESP_LOGW(TAG, "FontGif: LvglRawImage alloc failed, free buffer");
        heap_caps_free(gif_buffer);
        CancelFontPending(request_id);
        return;
    }

    // ① 先装载 + 切 src + 拉前 → emoji_box 覆盖底层场景显示新 GIF
    emoji_collection->ReplaceEmoji("font", raw);
    LcdDisplay::SetEmotion("font");                              // 切表情 src（绕守护直调父类）
    if (emoji_box_) {
        // 🔴 番茄钟/时钟/Player 主屏 emoji_box 被 HIDDEN · FontGif 必须显式解开才能可见
        //   历史只在 kChat 触发（emoji_box 本就显示）所以没暴露 · 软独占改造后需补
        lv_obj_remove_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_opa(emoji_box_, LV_OPA_COVER, 0);       // 防 boot fade_out 残留 TRANSP
        lv_obj_move_foreground(emoji_box_);                      // z-order 拉到最前盖住底层
    }
    in_font_mode_ = true;
    font_pending_.store(false, std::memory_order_release);       // pending → active

    HideEduCard();
    HideQrCode();
    HideBottomBar();
    ESP_LOGI(TAG, "Font GIF loaded (%u bytes)", (unsigned)size);
}

// SetEmotion override：仅切表情 src · font 模式时屏蔽所有外部 emotion 切换
// 原 4 职责（守护 / 切 src / bottom_bar / 互斥清场）已解耦：副作用搬到 FontGif/HideFontGif
void UiDisplay::SetEmotion(const char* emotion) {
    if (!emotion) return;
    // 守护：font 模式不响应外部 emotion（孩子聚焦写字时不被 LLM emotion 推送打断）
    if (in_font_mode_) return;
    LcdDisplay::SetEmotion(emotion);
}

// font 模式静默丢弃字幕。常规模式下根据文本宽度自适应 long_mode：
// 单屏容得下 → LONG_WRAP 静态多行；超出 → LONG_SCROLL_CIRCULAR 横向跑马灯。
// 速度参数 kChatScrollSpeedPps：3-10 岁孩子推荐 ~28 px/s（约 1.4 字/秒）。
//
// 🔴 多行文本压平规则：LLM 朗诵诗 / 多段对话含 \n，bottom_bar LV_SIZE_CONTENT 模式
//   会按 \n 撑高 5-6 行 → 遮挡 emoji_box 表情区。
//   策略：清洗 \n/\r/\t/连续空格 → 单空格 · 让长文本统一走横向滚动展示。
//   产品意图：1.83" 小屏 + 孩子辅助字幕，不保留格式，只保留语义。
void UiDisplay::SetChatMessage(const char* role, const char* content) {
    if (in_font_mode_) {
        ESP_LOGI(TAG, "[chat_msg] dropped (in_font_mode): role=%s len=%d",
                 role ? role : "?", content ? (int)strlen(content) : 0);
        return;
    }

    // 清洗换行/连续空白 → 单空格（避免 LONG_WRAP 按 \n 撑高 bottom_bar 遮挡表情）
    std::string cleaned;
    if (content && content[0]) {
        cleaned.reserve(strlen(content));
        bool prev_space = false;
        for (const char* p = content; *p; ++p) {
            unsigned char c = (unsigned char)*p;
            bool is_ws = (c == '\n' || c == '\r' || c == '\t' || c == ' ');
            if (is_ws) {
                if (!prev_space && !cleaned.empty()) {
                    cleaned += ' ';
                    prev_space = true;
                }
            } else {
                cleaned += (char)c;
                prev_space = false;
            }
        }
        while (!cleaned.empty() && cleaned.back() == ' ') cleaned.pop_back();
    }
    const char* display_content = cleaned.c_str();   // 空字符串时 c_str()=""，安全传给父类

    if (chat_message_label_ && display_content[0]) {
        DisplayLockGuard lock(this);
        constexpr int kChatScrollSpeedPps = 30;  // pixel/second ≈ 2 字/秒
        const lv_font_t* font = lv_obj_get_style_text_font(chat_message_label_, LV_PART_MAIN);
        if (!font) font = &g_text_font;

        // 用 lv_text_get_size 精确测宽（替代 chars×line_height 的粗估）
        //   旧逻辑用行高当字宽：CJK ≈ 行高 OK，但 ASCII 实际宽 ≈ 0.5 行高 → 长 ASCII 错走 SCROLL
        //   现在 LVGL 自己按 cmap 算每个字符精确宽度，混合文本（中英数字标点）都准确
        lv_point_t sz;
        lv_text_get_size(&sz, display_content, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_EXPAND);
        int total_w = sz.x;
        int label_w = lv_obj_get_width(chat_message_label_);

        if (total_w > label_w) {
            lv_label_set_long_mode(chat_message_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
            // LVGL 9 不再有 set_style_anim_speed，用 anim_duration（一轮滚动总时长 ms）等效
            // duration_ms = total_w * 1000 / pps → 30 pps 即 30 px/s
            int duration_ms = (total_w * 1000) / kChatScrollSpeedPps;
            lv_obj_set_style_anim_duration(chat_message_label_, duration_ms, LV_PART_MAIN);
        } else {
            lv_label_set_long_mode(chat_message_label_, LV_LABEL_LONG_WRAP);
        }
    }

    LcdDisplay::SetChatMessage(role, display_content);   // 传清洗后的文本给父类
}

// ============================================================
// 三个 SwitchTo* 共享的状态栏管理 helper
// 调用方需自持 DisplayLockGuard
// ============================================================
void UiDisplay::HideFontGif() {
    font_pending_.store(false, std::memory_order_release);
    font_request_active_.store(0, std::memory_order_release);

    DisplayLockGuard lock(this);
    bool was_in_font = in_font_mode_;
    in_font_mode_ = false;

    ShowBottomBar();
    LcdDisplay::SetEmotion("neutral");

    // FontGif 借用 emoji_box 作为 GIF 载体（非真 overlay），退出后需按 active_scene_
    auto restore_container = [this](lv_obj_t* container) {
        if (emoji_box_) lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
        if (container) lv_obj_move_foreground(container);
    };
    switch (active_scene_) {
        case SceneType::kClock:    restore_container(clock_container_);    break;
        case SceneType::kPlayer:   restore_container(player_container_);   break;
        case SceneType::kPomodoro: restore_container(pomodoro_container_); break;
        default: break;  // kChat 保持现状
    }

    ESP_LOGI(TAG, "[font_gif] hide: was_in_font=%d scene=%d → bottom_bar shown + emotion=neutral",
             was_in_font, (int)active_scene_);
}

// bottom_bar 显示（其他场景下让字幕条可见）
void UiDisplay::ShowBottomBar() {
    if (!bottom_bar_) return;
    DisplayLockGuard lock(this);
    lv_obj_remove_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(bottom_bar_);
}

// bottom_bar 隐藏（font 模式让用户聚焦写字 GIF）
void UiDisplay::HideBottomBar() {
    if (!bottom_bar_) return;
    DisplayLockGuard lock(this);
    lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
}

// 触屏点击 emoji_box 退出 font 模式
// 用 Schedule 异步：避免在 LVGL 事件回调内修改 emoji src 引发事件链问题
void UiDisplay::OnFontExitClicked(lv_event_t* e) {
    auto* self = static_cast<UiDisplay*>(lv_event_get_user_data(e));
    if (!self || !self->in_font_mode_) return;
    Application::GetInstance().Schedule([self]() { self->HideFontGif(); });
}

void UiDisplay::SetTopBarIconsVisible(bool v) {
    if (network_label_) (v ? lv_obj_remove_flag : lv_obj_add_flag)(network_label_, LV_OBJ_FLAG_HIDDEN);
    if (battery_label_) (v ? lv_obj_remove_flag : lv_obj_add_flag)(battery_label_, LV_OBJ_FLAG_HIDDEN);
    if (mute_label_)    (v ? lv_obj_remove_flag : lv_obj_add_flag)(mute_label_, LV_OBJ_FLAG_HIDDEN);
}

void UiDisplay::RaiseStatusBar() {
    if (!status_bar_) return;
    lv_obj_remove_flag(status_bar_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_opa(status_bar_, LV_OPA_COVER, 0);  // 防 boot fade_out 残留 TRANSP
    lv_obj_move_foreground(status_bar_);
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

    make_label(top, lv_color_hex(0x333333), LV_ALIGN_TOP_MID, 6);

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

    int bottom_y = -3;
    if (highlight && highlight[0]) {
        make_label(highlight, lv_color_hex(0x2196F3), LV_ALIGN_BOTTOM_MID, -30);
        bottom_y = -6;  // 给高亮让出空间
    }

    // 底部辅助文字
    make_label(bottom, lv_color_hex(0x999999), LV_ALIGN_BOTTOM_MID, bottom_y, true);

    lv_obj_move_foreground(qr_overlay_);
    ESP_LOGI(TAG, "QR页: content=%s top=%s bottom=%s highlight=%s",
             qr_content, top ? top : "", bottom ? bottom : "", highlight ? highlight : "");
}

void UiDisplay::HideQrCode() {
    DisplayLockGuard lock(this);
    if (qr_overlay_) {
        lv_obj_del_async(qr_overlay_);
        qr_overlay_ = nullptr;
        ESP_LOGI(TAG, "隐藏 QR 页");
    }
    qr_double_click_cb_ = nullptr;
    qr_last_click_us_   = 0;
}

void UiDisplay::OnQrClicked(lv_event_t* e) {
    auto* self = static_cast<UiDisplay*>(lv_event_get_user_data(e));
    if (!self || !self->qr_double_click_cb_) return;

    const uint64_t now  = esp_timer_get_time();
    const uint64_t last = self->qr_last_click_us_;
    constexpr uint64_t kDoubleClickWindowUs = 500 * 1000;

    if (last != 0 && (now - last) < kDoubleClickWindowUs) {
        auto cb = self->qr_double_click_cb_;
        self->qr_last_click_us_ = 0;
        cb();
    } else {
        self->qr_last_click_us_ = now;
    }
}

void UiDisplay::EnsureControlCenter() {
    if (control_center_) return;
    auto* screen = lv_screen_active();
    if (!screen) return;

    control_center_ = std::make_unique<ControlCenter>(screen, LV_HOR_RES, LV_VER_RES);
    control_center_->Hide();

    auto& board = Board::GetInstance();

    control_center_->SetExitCallback([this]() { HideControlCenter(); });

    control_center_->SetVolumeCallback([&board](int v) {
        if (auto* codec = board.GetAudioCodec()) codec->SetOutputVolume(v);
    });
    control_center_->SetBrightnessCallback([&board](int v) {
        if (auto* bk = board.GetBacklight()) bk->SetBrightness(v);
    });
    // 原 AEC 格已让位为"关于"入口：收起控制中心 → 弹关于页（AEC 仍可经按键/MCP/语音）
    control_center_->SetAecCallback([this](bool /*unused*/) {
        HideControlCenter();
        ShowAboutPage();
    });
    control_center_->SetSleepCallback([](bool on) {
        Board::GetInstance().EnableAutoSleep(on);
    });
    control_center_->SetNetworkCallback([](int /*mode*/) {
        auto& b = Board::GetInstance();
        if (b.CanSwitchNetwork()) b.SwitchNetwork();
    });
    if (auto* dnb = dynamic_cast<DualNetworkBoard*>(&board)) {
        control_center_->SetNetworkMode(dnb->GetNetworkType() == NetworkType::ML307 ? 1 : 0);
    }
    control_center_->SetSleepState(board.IsAutoSleepEnabled());
}

void UiDisplay::ShowControlCenter() {
    DisplayLockGuard lock(this);
    EnsureControlCenter();
    if (!control_center_) return;
    auto& board = Board::GetInstance();
    if (auto* codec = board.GetAudioCodec()) control_center_->SetVolume(codec->output_volume());
    if (auto* bk = board.GetBacklight())     control_center_->SetBrightness(bk->brightness());
    control_center_->Show();
}

void UiDisplay::HideControlCenter() {
    DisplayLockGuard lock(this);
    if (control_center_) control_center_->Hide();
}

bool UiDisplay::IsControlCenterVisible() const {
    return control_center_ && control_center_->IsVisible();
}

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
    auto cb = self->on_player_pause_toggle_;   // copy std::function 到 lambda capture
    Application::GetInstance().Schedule([cb]() {
        if (cb) cb();
    });
}

// 200ms tick：直接从 MusicPlayer 拉进度/暂停状态，自动刷新 UI
void UiDisplay::PlayerTickCb(lv_timer_t* t) {
    auto* self = static_cast<UiDisplay*>(lv_timer_get_user_data(t));
    if (!self || self->active_scene_ != SceneType::kPlayer) return;
    auto& mp = MusicPlayer::GetInstance();
    self->UpdatePlayerProgress(mp.GetPositionMs(), mp.GetTotalDurationMs());
    self->SetPlayerPaused(mp.IsPaused());
}

void UiDisplay::SwitchToPlayerMode(const char* title) {
    DisplayLockGuard lock(this);
    if (!setup_ui_called_) return;
    if (!player_container_) CreatePlayerPage();
    if (!player_container_) return;

    HideEduCard();   // 状态切换清场，防止教育卡遮挡 player UI
    HideFontGif();   // 同步清 font 状态，防止 GIF 笔画残留

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
    // status_bar_ 永久不隐藏（产品决策）：Player 模式也显示顶部状态栏
    if (emoji_box_)       lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
    if (pomodoro_container_) lv_obj_add_flag(pomodoro_container_, LV_OBJ_FLAG_HIDDEN);

    lv_obj_remove_flag(player_container_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(player_container_);

    // 父类 top_bar_ 保持显示并提顶，与时钟模式一致
    if (top_bar_) {
        lv_obj_remove_flag(top_bar_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(top_bar_);
    }
    SetTopBarIconsVisible(true);  // chat → player 路径恢复三个图标
    RaiseStatusBar();              // status_bar_ 永久不隐藏，必须提顶否则被 player_container_ 遮挡
    if (qr_overlay_) lv_obj_move_foreground(qr_overlay_);

    active_scene_ = SceneType::kPlayer;   // 与 Clock / Chat 互斥

    // 启动 200ms 进度刷新 timer（懒创建）
    if (!player_tick_) player_tick_ = lv_timer_create(PlayerTickCb, 200, this);
    else               lv_timer_resume(player_tick_);

    ESP_LOGI(TAG, "Switched to player mode: %s", title ? title : "");
}

void UiDisplay::SwitchOutPlayerMode() {
    DisplayLockGuard lock(this);
    if (active_scene_ != SceneType::kPlayer) return;
    if (player_container_) lv_obj_add_flag(player_container_, LV_OBJ_FLAG_HIDDEN);
    if (player_tick_) lv_timer_pause(player_tick_);
    // 先退出 Player 场景，让 SwitchToClockMode 内的 kPlayer 互斥判定通过
    active_scene_ = SceneType::kChat;
    SwitchToClockMode();
    ESP_LOGI(TAG, "Switched out player mode");
}

void UiDisplay::UpdatePlayerProgress(int position_ms, int total_ms) {
    DisplayLockGuard lock(this);
    if (active_scene_ != SceneType::kPlayer || !player_progress_) return;

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

void UiDisplay::FormatPomodoroTime(uint32_t remain_sec, char* buf, size_t buf_size) {
    uint32_t min = remain_sec / 60;
    uint32_t sec = remain_sec % 60;
    snprintf(buf, buf_size, "%02u:%02u", (unsigned)min, (unsigned)sec);
}

void UiDisplay::CreatePomodoroPage() {
    if (pomodoro_container_) return;
    auto* screen = lv_screen_active();
    if (!screen) return;

    pomodoro_container_ = lv_obj_create(screen);
    lv_obj_set_size(pomodoro_container_, kScreenWidth, kScreenHeight);
    lv_obj_align(pomodoro_container_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_remove_flag(pomodoro_container_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(pomodoro_container_, lv_color_hex(kColorBgPrimary), 0);
    lv_obj_set_style_bg_opa(pomodoro_container_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(pomodoro_container_, 0, 0);
    lv_obj_set_style_radius(pomodoro_container_, kScreenRadius, 0);
    lv_obj_set_style_pad_all(pomodoro_container_, 0, 0);

    // 88px 倒计时（与时钟主屏完全同款布局：y=-26）
    pomodoro_time_label_ = lv_label_create(pomodoro_container_);
    lv_obj_set_style_text_color(pomodoro_time_label_, lv_color_hex(kColorTextPrimary), 0);
    lv_label_set_text(pomodoro_time_label_, "25:00");
    lv_obj_align(pomodoro_time_label_, LV_ALIGN_CENTER, kClockOffsetX, kClockTimeOffsetY);

    EnsureDisplayFonts();
    if (clock_big_font_) {
        lv_obj_set_style_text_font(pomodoro_time_label_, clock_big_font_, 0);
    } else {
        lv_obj_set_style_text_font(pomodoro_time_label_, &g_text_font, 0);
    }

    // 启停圆按钮（屏幕底部偏上 · 64×64 · 上移 20px 给底部呼吸空间）
    constexpr int BTN_SIZE = 64;
    constexpr int BTN_CENTER_Y = 168;   // 距屏底 ~72px · 原 188 上移 20px
    pomodoro_btn_ = lv_button_create(pomodoro_container_);
    lv_obj_set_size(pomodoro_btn_, BTN_SIZE, BTN_SIZE);
    lv_obj_set_pos(pomodoro_btn_, kScreenWidth / 2 - BTN_SIZE / 2, BTN_CENTER_Y - BTN_SIZE / 2);
    // 番茄色（ui_config.h ACCENT_ORANGE=0xFB8C00）
    lv_obj_set_style_bg_color(pomodoro_btn_, lv_color_hex(0xFB8C00), 0);
    lv_obj_set_style_bg_opa(pomodoro_btn_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(pomodoro_btn_, BTN_SIZE / 2, 0);
    lv_obj_set_style_border_width(pomodoro_btn_, 0, 0);
    lv_obj_set_style_shadow_width(pomodoro_btn_, 0, 0);
    lv_obj_set_style_bg_opa(pomodoro_btn_, LV_OPA_70, LV_STATE_PRESSED);

    pomodoro_btn_icon_ = lv_label_create(pomodoro_btn_);
    lv_label_set_text(pomodoro_btn_icon_, FONT_AWESOME_PAUSE);  // 默认 running → ⏸
    lv_obj_set_style_text_font(pomodoro_btn_icon_, &BUILTIN_ICON_FONT, 0);
    lv_obj_set_style_text_color(pomodoro_btn_icon_, lv_color_hex(kColorTextPrimary), 0);
    lv_obj_center(pomodoro_btn_icon_);

    // PRESS_LOCK 防 swipe 假触（同 Player）
    lv_obj_add_flag(pomodoro_btn_, LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_add_event_cb(pomodoro_btn_, OnPomodoroBtnClicked, LV_EVENT_CLICKED, this);

    lv_obj_add_flag(pomodoro_container_, LV_OBJ_FLAG_HIDDEN);  // 默认隐藏
}

void UiDisplay::OnPomodoroBtnClicked(lv_event_t* e) {
    auto* self = static_cast<UiDisplay*>(lv_event_get_user_data(e));
    if (!self || !self->on_pomodoro_toggle_) return;
    // 同 Player：LVGL 锁 + esp_timer 锁可能顺序倒置 → Schedule 到主线程
    auto cb = self->on_pomodoro_toggle_;
    Application::GetInstance().Schedule([cb]() {
        if (cb) cb();
    });
}

void UiDisplay::SwitchToPomodoroMode(uint32_t remain_sec, bool running) {
    DisplayLockGuard lock(this);
    if (!setup_ui_called_) return;
    if (!pomodoro_container_) CreatePomodoroPage();
    if (!pomodoro_container_) return;

    HideEduCard();
    HideFontGif();

    // 同步显示
    char buf[8];
    FormatPomodoroTime(remain_sec, buf, sizeof(buf));
    if (pomodoro_time_label_) lv_label_set_text(pomodoro_time_label_, buf);
    pomodoro_is_running_ = running;
    if (pomodoro_btn_icon_) {
        lv_label_set_text(pomodoro_btn_icon_, running ? FONT_AWESOME_PAUSE : FONT_AWESOME_PLAY);
    }

    // 隐藏其他模式
    if (clock_container_)  lv_obj_add_flag(clock_container_, LV_OBJ_FLAG_HIDDEN);
    if (player_container_) lv_obj_add_flag(player_container_, LV_OBJ_FLAG_HIDDEN);
    if (content_)          lv_obj_add_flag(content_, LV_OBJ_FLAG_HIDDEN);
    if (container_)        lv_obj_add_flag(container_, LV_OBJ_FLAG_HIDDEN);
    if (emoji_box_)        lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);

    lv_obj_remove_flag(pomodoro_container_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(pomodoro_container_);

    // top_bar / status_bar 提顶（与 Player 同套路）
    if (top_bar_) {
        lv_obj_remove_flag(top_bar_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(top_bar_);
    }
    SetTopBarIconsVisible(true);
    RaiseStatusBar();
    if (qr_overlay_) lv_obj_move_foreground(qr_overlay_);

    active_scene_ = SceneType::kPomodoro;
    ESP_LOGI(TAG, "Switched to pomodoro mode: %s remain=%u", running ? "running" : "paused", (unsigned)remain_sec);
}

void UiDisplay::SwitchOutPomodoroMode() {
    DisplayLockGuard lock(this);
    if (active_scene_ != SceneType::kPomodoro) return;
    if (pomodoro_container_) lv_obj_add_flag(pomodoro_container_, LV_OBJ_FLAG_HIDDEN);
    // 先脱离 Pomodoro 场景，让 SwitchToClockMode 的 kPomodoro 互斥判定通过
    active_scene_ = SceneType::kChat;
    SwitchToClockMode();
    ESP_LOGI(TAG, "Switched out pomodoro mode");
}

void UiDisplay::UpdatePomodoro(uint32_t remain_sec, bool running) {
    DisplayLockGuard lock(this);
    if (active_scene_ != SceneType::kPomodoro) return;
    if (pomodoro_time_label_) {
        char buf[8];
        FormatPomodoroTime(remain_sec, buf, sizeof(buf));
        lv_label_set_text(pomodoro_time_label_, buf);
    }
    if (pomodoro_is_running_ != running) {
        pomodoro_is_running_ = running;
        if (pomodoro_btn_icon_) {
            lv_label_set_text(pomodoro_btn_icon_, running ? FONT_AWESOME_PAUSE : FONT_AWESOME_PLAY);
        }
    }
}


void UiDisplay::OnEduCardClicked(lv_event_t* e) {
    auto* self = static_cast<UiDisplay*>(lv_event_get_user_data(e));
    if (!self) return;
    self->HideEduCard();
}

// ============================================================
// 关于/设备信息页（参考 P30-V2 brain_info 布局 · 适配 284×240 无滚动 · 内联 overlay）
// ============================================================

void UiDisplay::OnAboutClicked(lv_event_t* e) {
    auto* self = static_cast<UiDisplay*>(lv_event_get_user_data(e));
    if (self) self->HideAboutPage();
}

void UiDisplay::HideAboutPage() {
    DisplayLockGuard lock(this);
    if (about_overlay_ && !lv_obj_has_flag(about_overlay_, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_add_flag(about_overlay_, LV_OBJ_FLAG_HIDDEN);
        if (status_bar_) lv_obj_move_foreground(status_bar_);
        ESP_LOGI(TAG, "隐藏关于页");
    }
}

void UiDisplay::ShowAboutPage() {
    DisplayLockGuard lock(this);
    auto* screen = lv_screen_active();
    if (!screen) return;

    if (!about_overlay_) {
        about_overlay_ = lv_obj_create(screen);
        lv_obj_set_size(about_overlay_, LV_HOR_RES, LV_VER_RES);
        lv_obj_align(about_overlay_, LV_ALIGN_CENTER, 0, 0);
        lv_obj_remove_flag(about_overlay_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_color(about_overlay_, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(about_overlay_, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(about_overlay_, 0, 0);
        lv_obj_set_style_radius(about_overlay_, 0, 0);
        lv_obj_set_style_pad_all(about_overlay_, 0, 0);
        lv_obj_add_flag(about_overlay_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(about_overlay_, OnAboutClicked, LV_EVENT_CLICKED, this);

        lv_obj_t* title = lv_label_create(about_overlay_);
        lv_label_set_text(title, "关于设备");
        lv_obj_set_style_text_color(title, lv_color_white(), 0);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 12);

        lv_obj_t* card = lv_obj_create(about_overlay_);
        lv_obj_set_size(card, 256, 168);
        lv_obj_align(card, LV_ALIGN_CENTER, 0, 6);
        lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_color(card, lv_color_hex(0x1C1C1C), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(card, 16, 0);
        lv_obj_set_style_border_width(card, 0, 0);
        lv_obj_set_style_pad_all(card, 0, 0);
        lv_obj_add_flag(card, LV_OBJ_FLAG_EVENT_BUBBLE);  // 点卡片冒泡到 overlay → 返回

        char ver[48];
        snprintf(ver, sizeof(ver), "V%s", esp_app_get_description()->version);
        static std::string mac  = SystemInfo::GetMacAddress();
        static std::string chip = SystemInfo::GetChipModelName();

        const int ROW_Y[5] = {14, 44, 74, 104, 134};
        const uint32_t TXT = 0xA4A6A6, DIV = 0x3A3A3A;
        struct { const char* k; const char* v; } rows[5] = {
            {"型号",   "MYDAZY/P30"},
            {"版本",   ver},
            {"设备ID", mac.c_str()},
            {"芯片",   chip.c_str()},
            {"网络",   "检测中"},
        };
        for (int i = 0; i < 5; i++) {
            lv_obj_t* k = lv_label_create(card);
            lv_obj_set_pos(k, 16, ROW_Y[i]);
            lv_label_set_text(k, rows[i].k);
            lv_obj_set_style_text_color(k, lv_color_hex(TXT), 0);
            lv_obj_t* v = lv_label_create(card);
            lv_obj_align(v, LV_ALIGN_TOP_RIGHT, -16, ROW_Y[i]);
            lv_label_set_text(v, rows[i].v);
            lv_obj_set_style_text_color(v, lv_color_hex(TXT), 0);
            lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
            if (i == 4) about_net_value_ = v;
            if (i < 4) {
                lv_obj_t* d = lv_obj_create(card);
                lv_obj_set_size(d, 224, 1);
                lv_obj_align(d, LV_ALIGN_TOP_MID, 0, ROW_Y[i] + 22);
                lv_obj_remove_flag(d, LV_OBJ_FLAG_SCROLLABLE);
                lv_obj_set_style_radius(d, 0, 0);
                lv_obj_set_style_border_width(d, 0, 0);
                lv_obj_set_style_bg_color(d, lv_color_hex(DIV), 0);
                lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
            }
        }

        lv_obj_t* tip = lv_label_create(about_overlay_);
        lv_label_set_text(tip, "点按任意处返回");
        lv_obj_set_style_text_color(tip, lv_color_hex(0x888888), 0);
        lv_obj_align(tip, LV_ALIGN_BOTTOM_MID, 0, -6);
    }

    if (about_net_value_) {
        const char* icon = Board::GetInstance().GetNetworkStateIcon();
        lv_label_set_text(about_net_value_, (icon && icon[0]) ? "已连接" : "未连接");
    }

    lv_obj_remove_flag(about_overlay_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(about_overlay_);
    ESP_LOGI(TAG, "显示关于页");
}

void UiDisplay::HideEduCard() {
    DisplayLockGuard lock(this);
    if (edu_card_overlay_ && !lv_obj_has_flag(edu_card_overlay_, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_add_flag(edu_card_overlay_, LV_OBJ_FLAG_HIDDEN);
        if (bottom_bar_) lv_obj_move_foreground(bottom_bar_);
        if (status_bar_) lv_obj_move_foreground(status_bar_);
        ESP_LOGI(TAG, "隐藏教育卡");
    }
}

void UiDisplay::EnsureEduCardOverlay() {
    if (edu_card_overlay_) return;
    auto* screen = lv_screen_active();
    if (!screen) return;

    edu_card_overlay_ = lv_obj_create(screen);
    lv_obj_set_size(edu_card_overlay_, LV_HOR_RES, LV_VER_RES);
    lv_obj_align(edu_card_overlay_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_remove_flag(edu_card_overlay_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(edu_card_overlay_, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(edu_card_overlay_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(edu_card_overlay_, 0, 0);
    lv_obj_set_style_radius(edu_card_overlay_, 0, 0);
    lv_obj_set_style_pad_all(edu_card_overlay_, 0, 0);
    lv_obj_add_flag(edu_card_overlay_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(edu_card_overlay_, OnEduCardClicked, LV_EVENT_CLICKED, this);
    lv_obj_add_flag(edu_card_overlay_, LV_OBJ_FLAG_HIDDEN);

    auto make_label = [this]() {
        lv_obj_t* lbl = lv_label_create(edu_card_overlay_);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_CLIP);
        lv_obj_set_width(lbl, 270);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_add_flag(lbl, LV_OBJ_FLAG_HIDDEN);
        return lbl;
    };
    edu_top_label_  = make_label();
    edu_main_label_ = make_label();
}

void UiDisplay::UpdateEduRow(lv_obj_t* lbl, const EduRow& row, int y) {
    if (!lbl) return;
    if (!row.text || !row.text[0]) {
        lv_obj_add_flag(lbl, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_label_set_text(lbl, row.text);
    lv_obj_set_style_text_font(lbl, row.font, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(row.color), 0);
    lv_obj_set_style_text_letter_space(lbl, row.letter_space, 0);
    lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_remove_flag(lbl, LV_OBJ_FLAG_HIDDEN);
}

void UiDisplay::RenderEduCardLayout(const EduRow& top, const EduRow& main_row) {
    DisplayLockGuard lock(this);
    HideQrCode();      // 与 QR overlay 互斥

    EnsureEduCardOverlay();
    if (!edu_card_overlay_) {
        ESP_LOGE(TAG, "[edu] RenderEduCardLayout: overlay 创建失败 (lv_screen_active=%p)",
                 lv_screen_active());
        return;
    }

    int main_y = 100 - main_row.height / 2;
    int top_y  = main_y - 20 - top.height;
    UpdateEduRow(edu_top_label_,  top,      top_y);
    UpdateEduRow(edu_main_label_, main_row, main_y);

    lv_obj_remove_flag(edu_card_overlay_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(edu_card_overlay_);

    // overlay 盖屏，但 bottom_bar（"长按说话"）和 status_bar 必须保留可见
    if (bottom_bar_) lv_obj_move_foreground(bottom_bar_);
    if (status_bar_) lv_obj_move_foreground(status_bar_);

    ESP_LOGI(TAG, "[edu] Render done: overlay=%p top_lbl=%p main_lbl=%p main_h=%d top='%s' main='%s'",
             edu_card_overlay_, edu_top_label_, edu_main_label_, main_row.height,
             top.text ? top.text : "", main_row.text ? main_row.text : "");
}

// 显示教育卡（极简两行 · 不分类）
void UiDisplay::ShowEduCard(const char* main_text, const char* top) {
    if (!main_text || !main_text[0]) {
        ESP_LOGW(TAG, "[edu] ShowEduCard 入参 main 为空");
        return;
    }

    ESP_LOGI(TAG, "[edu] ShowEduCard enter: main='%s' top='%s'", main_text, top ? top : "");

    if (in_font_mode_) {
        ESP_LOGI(TAG, "[edu] skip @ShowEduCard: in_font_mode (font GIF rendering)");
        return;
    }
    if (font_pending_.load(std::memory_order_acquire)) {
        ESP_LOGI(TAG, "[edu] skip @ShowEduCard: font_pending (GIF download window)");
        return;
    }

    EnsureDisplayFonts();
    ESP_LOGI(TAG, "[edu] font ptrs: 56=%p 48=%p 30=%p 88=%p fb=%p",
             edu_main_56_font_, edu_main_font_, clock_text_font_, clock_big_font_, fallback_text_font_);
    if (!edu_main_font_ || !clock_text_font_) {
        ESP_LOGW(TAG, "[edu] skip @ShowEduCard: 必需字体未加载 (edu_48=%p text_30=%p) — 检查 assets 分区",
                 edu_main_font_, clock_text_font_);
        return;
    }

    bool is_cjk = ContainsCjk(main_text);
    int n_chars = Utf8CharCount(main_text);
    const lv_font_t* font_56 = edu_main_56_font_ ? edu_main_56_font_ : edu_main_font_;
    const lv_font_t* main_font = PickFont(main_text, is_cjk, font_56, edu_main_font_);
    if (!main_font) {
        // 细化失败原因：CJK >4 字 / EN >12 字符 / EN 宽度超限
        const char* reason = is_cjk
            ? (n_chars > 4  ? "CJK 字数 > 4"     : "CJK 字符为空")
            : (n_chars > 12 ? "EN 字符数 > 12"   : "EN 宽度 > 280px（48 也兜不住）");
        ESP_LOGI(TAG, "[edu] skip @PickFont: %s · main='%s' is_cjk=%d n=%d",
                 reason, main_text, (int)is_cjk, n_chars);
        return;
    }

    int main_ls = is_cjk ? 3 : 0;
    int main_h  = (main_font == edu_main_56_font_) ? 56 : 48;
    ESP_LOGI(TAG, "[edu] PickFont -> font_h=%d (56_ptr=%p picked=%p · %s降级)",
             main_h, edu_main_56_font_, main_font,
             (edu_main_56_font_ == nullptr) ? "56未加载所以" : (main_font != edu_main_56_font_ ? "宽度超限" : "未"));

    bool top_is_cjk = top && top[0] && ContainsCjk(top);
    const lv_font_t* top_font = top_is_cjk ? &g_text_font : clock_text_font_;
    int top_h  = top_is_cjk ? 20 : 30;
    int top_ls = top_is_cjk ? 1  : 2;

    constexpr uint32_t kMainColor = 0xFFCA28;   // 主秀亮金
    constexpr uint32_t kHintColor = 0xA5D6A7;   // 顶部薄荷绿

    EduRow t{top,       top_font,  kHintColor, top_ls,  top_h};
    EduRow m{main_text, main_font, kMainColor, main_ls, main_h};

    RenderEduCardLayout(t, m);
    ESP_LOGI(TAG, "EduCard mode=%s main=%s(%d) top=%s(%d)",
             is_cjk ? "CJK" : "EN", main_text, main_h,
             top ? top : "(none)", top_h);
}

