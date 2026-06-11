#include "control_center.h"
#include <esp_log.h>
#include <cstdio>
#include <font_awesome.h>

#define TAG "ControlCenter"

LV_IMG_DECLARE(ui_img_icon_signal_wifi_png);
LV_IMG_DECLARE(ui_img_icon_signal_4g_png);
LV_FONT_DECLARE(BUILTIN_ICON_FONT);

// 颜色定义（iOS 深色主题）
#define CC_BG_COLOR         lv_color_hex(0x1C1C1E)  // 深灰背景
#define CC_BTN_OFF_COLOR    lv_color_hex(0x3A3A3C)  // 按钮关闭状态
#define CC_BTN_ON_COLOR     lv_color_hex(0x0A84FF)  // 按钮开启状态（蓝色）
#define CC_BTN_WARN_COLOR   lv_color_hex(0xFF9500)  // 警告状态（橙色，二次确认）
#define CC_BTN_EXIT_COLOR   lv_color_hex(0xFF453A)  // 退出按钮颜色（红色）
#define CC_SLIDER_BG_COLOR  lv_color_hex(0x48484A)  // 滑块背景
#define CC_SLIDER_IND_COLOR lv_color_hex(0xFFFFFF)  // 滑块指示器
#define CC_ICON_COLOR       lv_color_hex(0xFFFFFF)  // 图标颜色
#define CC_TEXT_COLOR       lv_color_hex(0xAAAAAA)  // 文字颜色
#define CC_DISABLED_COLOR   lv_color_hex(0x666666)  // 置灰文字颜色

// 字体声明
LV_FONT_DECLARE(BUILTIN_TEXT_FONT);

// 布局参数（3x2 宫格，284x240 全屏优化）
#define GRID_COLS       3
#define GRID_ROWS       2
#define BTN_SIZE        75      // 按钮尺寸
#define BTN_SPACING_X   12      // 水平间距
#define BTN_SPACING_Y   16      // 垂直间距
#define TOP_MARGIN      12      // 顶部边距
#define LABEL_HEIGHT    22      // 标签高度
#define SLIDER_HEIGHT   100     // 滑块区域高度（覆盖第一行按钮）
#define BRIGHTNESS_MIN  15      // 亮度最小值（避免黑屏）
#define SLIDER_STEP     5       // 滑块调节步进
#define CONFIRM_TIMEOUT_MS 3000 // 网络二次确认超时（超时自动回退）
#define CONFIRM_MIN_GAP_MS 600  // 二次确认最小间隔（防触摸抖动连击穿透）

ControlCenter::ControlCenter(lv_obj_t* parent, int width, int height)
    : parent_(parent), width_(width), height_(height) {
    CreateUI();
}

ControlCenter::~ControlCenter() {
    if (container_) {
        lv_obj_del(container_);
        container_ = nullptr;
    }
}

void ControlCenter::CreateUI() {
    // 创建全屏容器（深色背景，无标题）
    container_ = lv_obj_create(parent_);
    lv_obj_set_size(container_, width_, height_);
    lv_obj_center(container_);
    lv_obj_set_style_bg_color(container_, CC_BG_COLOR, 0);
    lv_obj_set_style_bg_opa(container_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(container_, 0, 0);
    lv_obj_set_style_radius(container_, 20, 0);
    lv_obj_set_style_pad_all(container_, 0, 0);
    lv_obj_remove_flag(container_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(container_, LV_OBJ_FLAG_CLICKABLE);

    // 计算 3x2 宫格布局（284x240 屏幕）
    int total_width = GRID_COLS * BTN_SIZE + (GRID_COLS - 1) * BTN_SPACING_X;  // 3*75 + 2*12 = 249
    int total_height = GRID_ROWS * (BTN_SIZE + LABEL_HEIGHT) + (GRID_ROWS - 1) * BTN_SPACING_Y;  // 202
    int start_x = (width_ - total_width) / 2;   // (284-249)/2 = 17
    int start_y = (height_ - total_height) / 2; // (240-202)/2 = 19

    // ── 第一行：网络（左上）、打断 AEC（上中）、休眠（右上）──
    // 网络按钮单独创建（中间是图片图标，不是文字）
    {
        int x = start_x;
        int y = start_y;

        network_btn_ = lv_button_create(container_);
        lv_obj_set_size(network_btn_, BTN_SIZE, BTN_SIZE);
        lv_obj_set_pos(network_btn_, x, y);
        lv_obj_set_style_bg_color(network_btn_, CC_BTN_OFF_COLOR, 0);
        lv_obj_set_style_bg_opa(network_btn_, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(network_btn_, 16, 0);
        lv_obj_set_style_border_width(network_btn_, 0, 0);
        lv_obj_set_style_shadow_width(network_btn_, 0, 0);
        lv_obj_set_style_bg_opa(network_btn_, LV_OPA_70, LV_STATE_PRESSED);

        // 信号图标（图片控件，统一白色，只表达联网方式不表达信号强度）
        network_icon_ = lv_image_create(network_btn_);
        lv_image_set_src(network_icon_, &ui_img_icon_signal_wifi_png);
        lv_obj_align(network_icon_, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_image_recolor(network_icon_, lv_color_white(), 0);
        lv_obj_set_style_image_recolor_opa(network_icon_, LV_OPA_COVER, 0);

        network_label_ = lv_label_create(container_);
        lv_label_set_text(network_label_, "切换");
        lv_obj_set_style_text_font(network_label_, &BUILTIN_TEXT_FONT, 0);
        lv_obj_set_style_text_color(network_label_, CC_TEXT_COLOR, 0);
        lv_obj_align_to(network_label_, network_btn_, LV_ALIGN_OUT_BOTTOM_MID, 0, 2);
    }
    lv_obj_add_event_cb(network_btn_, OnNetworkClicked, LV_EVENT_CLICKED, this);

    CreateGridButton(1, 0, start_x, start_y, "关", "打断",
                     &aec_btn_, &aec_icon_, &aec_label_, false, true);
    lv_obj_add_event_cb(aec_btn_, OnAecClicked, LV_EVENT_CLICKED, this);

    CreateGridButton(2, 0, start_x, start_y, "5分", "休眠",
                     &sleep_btn_, &sleep_icon_, &sleep_label_, false, true);
    lv_obj_add_event_cb(sleep_btn_, OnSleepClicked, LV_EVENT_CLICKED, this);

    // ── 第二行：退出（左下）、亮度（中下）、关于（右下）──
    CreateGridButton(0, 1, start_x, start_y, "关", "退出",
                     &exit_btn_, &exit_icon_, &exit_label_, false, true);
    lv_obj_set_style_bg_color(exit_btn_, CC_BTN_EXIT_COLOR, 0);
    lv_obj_add_event_cb(exit_btn_, OnExitClicked, LV_EVENT_CLICKED, this);

    CreateGridButton(1, 1, start_x, start_y, "35%", "亮度",
                     &brightness_btn_, &brightness_icon_, &brightness_label_, false, true);
    lv_obj_add_event_cb(brightness_btn_, OnBrightnessClicked, LV_EVENT_CLICKED, this);

    CreateGridButton(2, 1, start_x, start_y, FONT_AWESOME_CIRCLE_INFO, "关于",
                     &about_btn_, &about_icon_, &about_label_, false, false);
    lv_obj_set_style_text_font(about_icon_, &BUILTIN_ICON_FONT, 0);
    lv_obj_align(about_icon_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_event_cb(about_btn_, OnAboutClicked, LV_EVENT_CLICKED, this);

    // 按下反馈（白描边 + 日志：名字/触摸坐标/按钮矩形）——定位"点A响B"类触控问题
    AddPressFeedback(network_btn_, "网络");
    AddPressFeedback(aec_btn_, "打断");
    AddPressFeedback(sleep_btn_, "休眠");
    AddPressFeedback(exit_btn_, "退出");
    AddPressFeedback(brightness_btn_, "亮度");
    AddPressFeedback(about_btn_, "关于");
    lv_obj_add_event_cb(container_, OnButtonPressed, LV_EVENT_PRESSED, (void*)"空白区");

    // 滑块区域（悬浮在第一行按钮上方，最后创建以确保在最上层）
    CreateSliderArea();

    // 默认隐藏控制中心
    lv_obj_add_flag(container_, LV_OBJ_FLAG_HIDDEN);

    // 设置默认状态（AEC 实际状态由外部 SetAecState 回写）
    UpdateButtonStyle(network_btn_, true);   // 网络常亮（表达当前联网方式）
    UpdateButtonStyle(aec_btn_, false);
    UpdateButtonStyle(sleep_btn_, true);
    UpdateBrightnessLabel();
    UpdateSleepLabel();

    ESP_LOGI(TAG, "控制中心UI创建完成 (3x2: 网络/打断/休眠 + 退出/亮度/关于)");
}

void ControlCenter::CreateGridButton(int col, int row, int start_x, int start_y,
                                      const char* symbol, const char* text,
                                      lv_obj_t** btn, lv_obj_t** icon, lv_obj_t** label,
                                      bool large_icon, bool use_text_font) {
    int x = start_x + col * (BTN_SIZE + BTN_SPACING_X);
    int y = start_y + row * (BTN_SIZE + BTN_SPACING_Y + 16);  // 16 = 文字高度

    // 按钮容器（包含图标和文字）
    *btn = lv_button_create(container_);
    lv_obj_set_size(*btn, BTN_SIZE, BTN_SIZE);
    lv_obj_set_pos(*btn, x, y);
    lv_obj_set_style_bg_color(*btn, CC_BTN_OFF_COLOR, 0);
    lv_obj_set_style_bg_opa(*btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(*btn, 16, 0);
    lv_obj_set_style_border_width(*btn, 0, 0);
    lv_obj_set_style_shadow_width(*btn, 0, 0);
    lv_obj_set_style_bg_opa(*btn, LV_OPA_70, LV_STATE_PRESSED);

    // 图标/文字（按钮中间）
    *icon = lv_label_create(*btn);
    lv_label_set_text(*icon, symbol);
    lv_obj_set_style_text_color(*icon, CC_ICON_COLOR, 0);

    if (use_text_font) {
        // 使用中文字体显示文字
        lv_obj_set_style_text_font(*icon, &BUILTIN_TEXT_FONT, 0);
        lv_obj_align(*icon, LV_ALIGN_CENTER, 0, 0);
    } else if (large_icon) {
        // 放大图标（使用默认字体但缩放显示）
        lv_obj_set_style_text_font(*icon, lv_font_get_default(), 0);
        lv_obj_set_style_transform_scale(*icon, 384, 0);  // 1.5倍 = 256 * 1.5
        lv_obj_align(*icon, LV_ALIGN_CENTER, 0, 0);
    } else {
        lv_obj_set_style_text_font(*icon, lv_font_get_default(), 0);
        lv_obj_align(*icon, LV_ALIGN_CENTER, 0, -8);
    }

    // 文字标签（在按钮下方）
    *label = lv_label_create(container_);
    lv_label_set_text(*label, text);
    lv_obj_set_style_text_font(*label, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(*label, CC_TEXT_COLOR, 0);
    lv_obj_align_to(*label, *btn, LV_ALIGN_OUT_BOTTOM_MID, 0, 2);
}

void ControlCenter::CreateSliderArea() {
    // 滑块容器（悬浮覆盖在第一行按钮上方，默认隐藏）
    slider_container_ = lv_obj_create(container_);
    lv_obj_set_size(slider_container_, width_ - 24, SLIDER_HEIGHT);
    lv_obj_align(slider_container_, LV_ALIGN_TOP_MID, 0, TOP_MARGIN);
    lv_obj_set_style_bg_color(slider_container_, lv_color_hex(0x2C2C2E), 0);
    lv_obj_set_style_bg_opa(slider_container_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(slider_container_, 20, 0);
    lv_obj_set_style_border_width(slider_container_, 0, 0);
    lv_obj_set_style_shadow_width(slider_container_, 20, 0);
    lv_obj_set_style_shadow_color(slider_container_, lv_color_black(), 0);
    lv_obj_set_style_shadow_opa(slider_container_, LV_OPA_50, 0);
    lv_obj_set_style_pad_left(slider_container_, 20, 0);
    lv_obj_set_style_pad_right(slider_container_, 20, 0);
    lv_obj_remove_flag(slider_container_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(slider_container_, LV_OBJ_FLAG_HIDDEN);

    // 滑块标题（左上）
    slider_title_ = lv_label_create(slider_container_);
    lv_label_set_text(slider_title_, "亮度");
    lv_obj_set_style_text_font(slider_title_, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(slider_title_, lv_color_white(), 0);
    lv_obj_align(slider_title_, LV_ALIGN_TOP_LEFT, 0, 12);

    // 滑块数值标签（右上）
    slider_value_label_ = lv_label_create(slider_container_);
    lv_obj_set_style_text_font(slider_value_label_, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(slider_value_label_, lv_color_white(), 0);
    lv_obj_align(slider_value_label_, LV_ALIGN_TOP_RIGHT, 0, 12);

    // 滑块（亮度专用，范围固定 BRIGHTNESS_MIN-100）
    slider_ = lv_slider_create(slider_container_);
    lv_obj_set_size(slider_, width_ - 80, 16);
    lv_obj_align(slider_, LV_ALIGN_CENTER, 0, 16);
    lv_slider_set_range(slider_, BRIGHTNESS_MIN, 100);

    lv_obj_set_style_bg_color(slider_, lv_color_hex(0x636366), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(slider_, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(slider_, 8, LV_PART_MAIN);

    lv_obj_set_style_bg_color(slider_, CC_SLIDER_IND_COLOR, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(slider_, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(slider_, 8, LV_PART_INDICATOR);

    lv_obj_set_style_bg_color(slider_, CC_SLIDER_IND_COLOR, LV_PART_KNOB);
    lv_obj_set_style_bg_opa(slider_, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_radius(slider_, 14, LV_PART_KNOB);
    lv_obj_set_style_pad_all(slider_, 8, LV_PART_KNOB);

    lv_obj_add_event_cb(slider_, OnSliderChanged, LV_EVENT_VALUE_CHANGED, this);
}

void ControlCenter::ShowSlider() {
    int aligned_value = (current_brightness_ / SLIDER_STEP) * SLIDER_STEP;
    if (aligned_value < BRIGHTNESS_MIN) aligned_value = BRIGHTNESS_MIN;
    lv_slider_set_value(slider_, aligned_value, LV_ANIM_OFF);

    char buf[16];
    snprintf(buf, sizeof(buf), "%d", aligned_value);
    lv_label_set_text(slider_value_label_, buf);

    lv_obj_remove_flag(slider_container_, LV_OBJ_FLAG_HIDDEN);
    StartSliderTimer();  // 启动自动关闭定时器
}

void ControlCenter::HideSlider() {
    StopSliderTimer();
    lv_obj_add_flag(slider_container_, LV_OBJ_FLAG_HIDDEN);
}

void ControlCenter::UpdateButtonStyle(lv_obj_t* btn, bool active) {
    if (btn == exit_btn_) {
        // 退出按钮始终红色
        lv_obj_set_style_bg_color(btn, CC_BTN_EXIT_COLOR, 0);
        return;
    }
    if (btn == aec_btn_ && !aec_enabled_) {
        // 置灰态不随 active 变色
        lv_obj_set_style_bg_color(btn, CC_BTN_OFF_COLOR, 0);
        return;
    }
    lv_obj_set_style_bg_color(btn, active ? CC_BTN_ON_COLOR : CC_BTN_OFF_COLOR, 0);
    if (btn == network_btn_) UpdateNetworkIcon();
}

void ControlCenter::UpdateNetworkIcon() {
    if (!network_icon_) return;
    // 只表达"当前是哪种联网方式"，不反映信号强度
    const lv_image_dsc_t* icon_dsc = (network_mode_ == 0)
                                       ? &ui_img_icon_signal_wifi_png
                                       : &ui_img_icon_signal_4g_png;
    lv_image_set_src(network_icon_, icon_dsc);
}

void ControlCenter::UpdateBrightnessLabel() {
    char buf[16];
    snprintf(buf, sizeof(buf), "%d%%", current_brightness_);
    lv_label_set_text(brightness_icon_, buf);
}

void ControlCenter::UpdateSleepLabel() {
    lv_label_set_text(sleep_icon_, sleep_on_ ? "5分" : "无");
}

void ControlCenter::StartSliderTimer() {
    slider_timer_.CreateOnce(3000, OnSliderTimer, this);
}

void ControlCenter::StopSliderTimer() {
    slider_timer_.Delete();
}

void ControlCenter::OnSliderTimer(lv_timer_t* timer) {
    auto* self = static_cast<ControlCenter*>(lv_timer_get_user_data(timer));
    self->HideSlider();
    ESP_LOGI(TAG, "滑块自动关闭");
}

// ── 网络按钮二次确认（防误触：单击切换=重启级操作）──

void ControlCenter::ResetNetworkConfirm() {
    network_confirm_timer_.Delete();
    if (!network_confirm_pending_) return;
    network_confirm_pending_ = false;
    if (network_label_) {
        lv_label_set_text(network_label_, network_is_provision_ ? "配网" : "切换");
        lv_obj_align_to(network_label_, network_btn_, LV_ALIGN_OUT_BOTTOM_MID, 0, 2);
    }
    if (network_btn_) lv_obj_set_style_bg_color(network_btn_, CC_BTN_ON_COLOR, 0);
}

void ControlCenter::OnNetworkConfirmTimer(lv_timer_t* timer) {
    auto* self = static_cast<ControlCenter*>(lv_timer_get_user_data(timer));
    self->ResetNetworkConfirm();
    ESP_LOGI(TAG, "网络切换确认超时，自动取消");
}

void ControlCenter::Show() {
    if (container_) {
        HideSlider();
        ResetNetworkConfirm();
        lv_obj_remove_flag(container_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(container_);
        is_visible_ = true;
        ESP_LOGI(TAG, "控制中心显示");
    }
}

void ControlCenter::Raise() {
    if (container_ && is_visible_) {
        lv_obj_move_foreground(container_);
    }
}

void ControlCenter::Hide() {
    if (container_) {
        HideSlider();
        ResetNetworkConfirm();
        lv_obj_add_flag(container_, LV_OBJ_FLAG_HIDDEN);
        is_visible_ = false;
        ESP_LOGI(TAG, "控制中心隐藏");
    }
}

void ControlCenter::SetNetworkMode(int mode) {
    network_mode_ = mode;
    network_is_provision_ = false;
    ResetNetworkConfirm();
    UpdateButtonStyle(network_btn_, true);
}

void ControlCenter::SetNetworkAsProvision() {
    network_is_provision_ = true;
    network_mode_ = 0;   // WiFi 图标
    ResetNetworkConfirm();
    if (network_label_) {
        lv_label_set_text(network_label_, "配网");
        lv_obj_align_to(network_label_, network_btn_, LV_ALIGN_OUT_BOTTOM_MID, 0, 2);
    }
    UpdateButtonStyle(network_btn_, true);
}

void ControlCenter::SetAecState(bool on) {
    aec_on_ = on;
    if (aec_icon_) lv_label_set_text(aec_icon_, on ? "开" : "关");
    UpdateButtonStyle(aec_btn_, on);
}

void ControlCenter::SetAecEnabled(bool enabled) {
    aec_enabled_ = enabled;
    lv_color_t text_color = enabled ? CC_ICON_COLOR : CC_DISABLED_COLOR;
    lv_color_t label_color = enabled ? CC_TEXT_COLOR : CC_DISABLED_COLOR;
    if (aec_icon_)  lv_obj_set_style_text_color(aec_icon_, text_color, 0);
    if (aec_label_) lv_obj_set_style_text_color(aec_label_, label_color, 0);
    UpdateButtonStyle(aec_btn_, aec_on_);
}

void ControlCenter::SetSleepState(bool on) {
    sleep_on_ = on;
    UpdateButtonStyle(sleep_btn_, on);
    UpdateSleepLabel();
}

void ControlCenter::SetBrightness(int value) {
    // 对齐到步进值，并确保不低于最小值
    int aligned = (value / SLIDER_STEP) * SLIDER_STEP;
    if (aligned < BRIGHTNESS_MIN) aligned = BRIGHTNESS_MIN;
    current_brightness_ = aligned;
    UpdateBrightnessLabel();
    if (!lv_obj_has_flag(slider_container_, LV_OBJ_FLAG_HIDDEN)) {
        lv_slider_set_value(slider_, current_brightness_, LV_ANIM_ON);
        char buf[8];
        snprintf(buf, sizeof(buf), "%d", current_brightness_);
        lv_label_set_text(slider_value_label_, buf);
    }
}

// ── 按下诊断反馈 ──

void ControlCenter::AddPressFeedback(lv_obj_t* btn, const char* name) {
    if (!btn) return;
    // 视觉反馈走按钮自带的按压变暗（LV_OPA_70），仅保留串口按下日志用于排障
    lv_obj_add_event_cb(btn, OnButtonPressed, LV_EVENT_PRESSED, (void*)name);
}

void ControlCenter::OnButtonPressed(lv_event_t* e) {
    auto* name = static_cast<const char*>(lv_event_get_user_data(e));
    lv_point_t p = {-1, -1};
    lv_indev_t* indev = lv_indev_active();
    if (indev) lv_indev_get_point(indev, &p);
    auto* obj = static_cast<lv_obj_t*>(lv_event_get_target(e));
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    ESP_LOGI(TAG, "[press] %s · 触点(%d,%d) · 控件区[%d,%d %dx%d]",
             name, (int)p.x, (int)p.y, (int)a.x1, (int)a.y1,
             (int)lv_area_get_width(&a), (int)lv_area_get_height(&a));
}

// ── 事件回调 ──

void ControlCenter::OnExitClicked(lv_event_t* e) {
    auto* self = static_cast<ControlCenter*>(lv_event_get_user_data(e));
    ESP_LOGI(TAG, "退出控制中心");
    self->Hide();
    if (self->exit_callback_) {
        self->exit_callback_();
    }
}

void ControlCenter::OnNetworkClicked(lv_event_t* e) {
    auto* self = static_cast<ControlCenter*>(lv_event_get_user_data(e));
    self->HideSlider();

    if (!self->network_confirm_pending_) {
        // 第一次点击：进入确认态（橙色 + "再点确认"），超时自动回退
        self->network_confirm_pending_ = true;
        self->network_confirm_tick_ = lv_tick_get();
        lv_obj_set_style_bg_color(self->network_btn_, CC_BTN_WARN_COLOR, 0);
        lv_label_set_text(self->network_label_, "再点确认");
        lv_obj_align_to(self->network_label_, self->network_btn_, LV_ALIGN_OUT_BOTTOM_MID, 0, 2);
        self->network_confirm_timer_.CreateOnce(CONFIRM_TIMEOUT_MS, OnNetworkConfirmTimer, self);
        ESP_LOGI(TAG, "网络按钮：等待二次确认 (%s)", self->network_is_provision_ ? "进配网" : "切网络");
        return;
    }

    // 防触摸抖动/连击穿透：进入确认态后 600ms 内的点击不算"确认"
    //（真人二次确认不会快过 600ms；4G RF 触摸抖动的多投递都在 ~200ms 内）
    if (lv_tick_elaps(self->network_confirm_tick_) < CONFIRM_MIN_GAP_MS) {
        ESP_LOGW(TAG, "网络按钮：确认间隔过短(%ums)，疑似抖动连击，忽略",
                 (unsigned)lv_tick_elaps(self->network_confirm_tick_));
        return;
    }

    // 第二次点击：确认执行（实际切换/重启由外部回调走主线程）
    self->ResetNetworkConfirm();
    ESP_LOGI(TAG, "网络按钮：确认执行 (%s)", self->network_is_provision_ ? "进配网" : "切网络");
    if (self->network_callback_) {
        self->network_callback_();
    }
}

void ControlCenter::OnAecClicked(lv_event_t* e) {
    auto* self = static_cast<ControlCenter*>(lv_event_get_user_data(e));
    self->HideSlider();
    self->ResetNetworkConfirm();
    if (!self->aec_enabled_) {
        ESP_LOGI(TAG, "AEC 开关置灰（本版本未放开），忽略点击");
        return;
    }
    // 只发请求，不预翻 UI 状态；实际状态由外部 SetAecState 回写（防显示与真实状态脱钩）
    ESP_LOGI(TAG, "AEC 开关：请求切换 (当前=%s)", self->aec_on_ ? "开" : "关");
    if (self->aec_toggle_callback_) self->aec_toggle_callback_();
}

void ControlCenter::OnSleepClicked(lv_event_t* e) {
    auto* self = static_cast<ControlCenter*>(lv_event_get_user_data(e));
    self->HideSlider();
    self->ResetNetworkConfirm();
    self->sleep_on_ = !self->sleep_on_;
    self->UpdateButtonStyle(self->sleep_btn_, self->sleep_on_);
    self->UpdateSleepLabel();
    ESP_LOGI(TAG, "休眠: %s", self->sleep_on_ ? "5分钟" : "无");
    if (self->sleep_callback_) {
        self->sleep_callback_(self->sleep_on_);
    }
}

void ControlCenter::OnBrightnessClicked(lv_event_t* e) {
    auto* self = static_cast<ControlCenter*>(lv_event_get_user_data(e));
    self->ResetNetworkConfirm();
    // 切换亮度滑块显示
    if (!lv_obj_has_flag(self->slider_container_, LV_OBJ_FLAG_HIDDEN)) {
        self->HideSlider();
    } else {
        self->ShowSlider();
    }
}

void ControlCenter::OnAboutClicked(lv_event_t* e) {
    auto* self = static_cast<ControlCenter*>(lv_event_get_user_data(e));
    self->HideSlider();
    self->ResetNetworkConfirm();
    ESP_LOGI(TAG, "打开关于页");
    if (self->about_callback_) {
        self->about_callback_();
    }
}

void ControlCenter::OnSliderChanged(lv_event_t* e) {
    auto* self = static_cast<ControlCenter*>(lv_event_get_user_data(e));
    int raw_value = lv_slider_get_value(self->slider_);

    // 重置自动关闭定时器
    self->StartSliderTimer();

    // 对齐到步进值
    int value = (raw_value / SLIDER_STEP) * SLIDER_STEP;
    if (value < BRIGHTNESS_MIN) value = BRIGHTNESS_MIN;
    self->current_brightness_ = value;

    char buf[16];
    snprintf(buf, sizeof(buf), "%d", value);
    lv_label_set_text(self->slider_value_label_, buf);
    self->UpdateBrightnessLabel();
    ESP_LOGD(TAG, "亮度: %d", value);
    if (self->brightness_callback_) {
        self->brightness_callback_(value);
    }
}
