#include "control_center.h"
#include <esp_log.h>
#include <cstdio>

#define TAG "ControlCenter"

// 信号图标从 mmap assets 加载
#include "ui_img_paths.h"
#include "ui_image_manager.h"

// 颜色定义（iOS 深色主题）
#define CC_BG_COLOR         lv_color_hex(0x1C1C1E)  // 深灰背景
#define CC_BTN_OFF_COLOR    lv_color_hex(0x3A3A3C)  // 按钮关闭状态
#define CC_BTN_ON_COLOR     lv_color_hex(0x0A84FF)  // 按钮开启状态（蓝色）
#define CC_BTN_WARN_COLOR   lv_color_hex(0xFF9500)  // 警告状态（橙色，4G）
#define CC_BTN_EXIT_COLOR   lv_color_hex(0xFF453A)  // 退出按钮颜色（红色）
#define CC_SLIDER_BG_COLOR  lv_color_hex(0x48484A)  // 滑块背景
#define CC_SLIDER_IND_COLOR lv_color_hex(0xFFFFFF)  // 滑块指示器
#define CC_ICON_COLOR       lv_color_hex(0xFFFFFF)  // 图标颜色
#define CC_TEXT_COLOR       lv_color_hex(0xAAAAAA)  // 文字颜色

// 字体声明
// 字体: 直接用外部 BUILTIN_TEXT_FONT
LV_FONT_DECLARE(BUILTIN_TEXT_FONT);

// 布局参数（3x2 宫格，284x240 全屏优化）
#define GRID_COLS       3
#define GRID_ROWS       2
#define BTN_SIZE        75      // 按钮尺寸（稍大）
#define BTN_SPACING_X   12      // 水平间距
#define BTN_SPACING_Y   16      // 垂直间距
#define TOP_MARGIN      12      // 顶部边距（去掉标题后减少）
#define ICON_FONT_SIZE  24      // 图标字体大小
#define LABEL_MARGIN    4       // 文字与图标间距
#define LABEL_HEIGHT    22      // 标签高度
#define SLIDER_HEIGHT   100     // 滑块区域高度（覆盖第一行按钮）
#define BRIGHTNESS_MIN  15      // 亮度最小值（避免黑屏）
#define SLIDER_STEP     5       // 滑块调节步进

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

    // 计算 3x2 宫格布局（284x240 屏幕，无标题，充分利用空间）
    // 每行高度 = 按钮(75) + 标签(22) + 间距(8) = 105
    // 两行总高度 = 105 * 2 = 210, 剩余 30 分配给上下边距各 15
    int total_width = GRID_COLS * BTN_SIZE + (GRID_COLS - 1) * BTN_SPACING_X;  // 3*75 + 2*12 = 249
    int total_height = GRID_ROWS * (BTN_SIZE + LABEL_HEIGHT) + (GRID_ROWS - 1) * BTN_SPACING_Y;  // 2*(75+22) + 8 = 202
    int start_x = (width_ - total_width) / 2;   // (284-249)/2 = 17
    int start_y = (height_ - total_height) / 2; // (240-202)/2 = 19

    // 第一行：网络、打断、休眠
    // 网络按钮（特殊处理：使用图片而非文字）
    {
        int x = start_x + 0 * (BTN_SIZE + BTN_SPACING_X);
        int y = start_y + 0 * (BTN_SIZE + BTN_SPACING_Y + 16);

        network_btn_ = lv_button_create(container_);
        lv_obj_set_size(network_btn_, BTN_SIZE, BTN_SIZE);
        lv_obj_set_pos(network_btn_, x, y);
        lv_obj_set_style_bg_color(network_btn_, CC_BTN_OFF_COLOR, 0);
        lv_obj_set_style_bg_opa(network_btn_, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(network_btn_, 16, 0);
        lv_obj_set_style_border_width(network_btn_, 0, 0);
        lv_obj_set_style_shadow_width(network_btn_, 0, 0);
        lv_obj_set_style_bg_opa(network_btn_, LV_OPA_70, LV_STATE_PRESSED);

        // 信号图标（图片控件，统一白色）
        network_icon_ = lv_image_create(network_btn_);
        const lv_image_dsc_t* wifi_icon = &ui_img_icon_signal_wifi_png;
        if (wifi_icon) {
            lv_image_set_src(network_icon_, wifi_icon);
        }
        lv_obj_align(network_icon_, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_image_recolor(network_icon_, lv_color_white(), 0);
        lv_obj_set_style_image_recolor_opa(network_icon_, LV_OPA_COVER, 0);

        // 文字标签
        network_label_ = lv_label_create(container_);
        lv_label_set_text(network_label_, "切换");
        lv_obj_set_style_text_font(network_label_, &BUILTIN_TEXT_FONT, 0);
        lv_obj_set_style_text_color(network_label_, CC_TEXT_COLOR, 0);
        lv_obj_align_to(network_label_, network_btn_, LV_ALIGN_OUT_BOTTOM_MID, 0, 2);
    }
    lv_obj_add_event_cb(network_btn_, OnNetworkClicked, LV_EVENT_CLICKED, this);

    CreateGridButton(1, 0, start_x, start_y, "开", "打断",
                     &aec_btn_, &aec_icon_, &aec_label_, false, true);  // 中文文字
    lv_obj_add_event_cb(aec_btn_, OnAecClicked, LV_EVENT_CLICKED, this);

    CreateGridButton(2, 0, start_x, start_y, "5分", "休眠",
                     &sleep_btn_, &sleep_icon_, &sleep_label_, false, true);  // 中文文字
    lv_obj_add_event_cb(sleep_btn_, OnSleepClicked, LV_EVENT_CLICKED, this);

    // 第二行：退出、音量、亮度
    CreateGridButton(0, 1, start_x, start_y, "关", "退出",
                     &exit_btn_, &exit_icon_, &exit_label_, false, true);  // 中文"关"字
    lv_obj_set_style_bg_color(exit_btn_, CC_BTN_EXIT_COLOR, 0);
    lv_obj_add_event_cb(exit_btn_, OnExitClicked, LV_EVENT_CLICKED, this);

    CreateGridButton(1, 1, start_x, start_y, "70%", "音量",
                     &volume_btn_, &volume_icon_, &volume_label_, false, true);  // 中文文字
    lv_obj_add_event_cb(volume_btn_, OnVolumeClicked, LV_EVENT_CLICKED, this);

    CreateGridButton(2, 1, start_x, start_y, "80%", "亮度",
                     &brightness_btn_, &brightness_icon_, &brightness_label_, false, true);  // 中文文字
    lv_obj_add_event_cb(brightness_btn_, OnBrightnessClicked, LV_EVENT_CLICKED, this);

    // 滑块区域（悬浮在第一行按钮上方，最后创建以确保在最上层）
    CreateSliderArea();

    // 默认隐藏控制中心
    lv_obj_add_flag(container_, LV_OBJ_FLAG_HIDDEN);

    // 设置默认状态
    UpdateButtonStyle(network_btn_, true);   // 网络默认开启
    UpdateButtonStyle(aec_btn_, true);       // AEC默认开启
    UpdateButtonStyle(sleep_btn_, true);     // 休眠默认开启
    UpdateVolumeLabel();
    UpdateBrightnessLabel();
    UpdateSleepLabel();

    ESP_LOGI(TAG, "控制中心UI创建完成 (3x2宫格布局)");
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
        lv_obj_align(*icon, LV_ALIGN_CENTER, 0, 0);  // 居中
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
    // 位置与第一行按钮重叠（TOP_MARGIN 是按钮起始位置）
    lv_obj_align(slider_container_, LV_ALIGN_TOP_MID, 0, TOP_MARGIN);
    lv_obj_set_style_bg_color(slider_container_, lv_color_hex(0x2C2C2E), 0);  // 稍亮的背景
    lv_obj_set_style_bg_opa(slider_container_, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(slider_container_, 20, 0);  // 更大的圆角
    lv_obj_set_style_border_width(slider_container_, 0, 0);
    lv_obj_set_style_shadow_width(slider_container_, 20, 0);  // 添加阴影
    lv_obj_set_style_shadow_color(slider_container_, lv_color_black(), 0);
    lv_obj_set_style_shadow_opa(slider_container_, LV_OPA_50, 0);
    lv_obj_set_style_pad_left(slider_container_, 20, 0);
    lv_obj_set_style_pad_right(slider_container_, 20, 0);
    lv_obj_remove_flag(slider_container_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(slider_container_, LV_OBJ_FLAG_HIDDEN);

    // 滑块标题（左上）
    slider_title_ = lv_label_create(slider_container_);
    lv_obj_set_style_text_font(slider_title_, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(slider_title_, lv_color_white(), 0);
    lv_obj_align(slider_title_, LV_ALIGN_TOP_LEFT, 0, 12);

    // 滑块数值标签（右上）
    slider_value_label_ = lv_label_create(slider_container_);
    lv_obj_set_style_text_font(slider_value_label_, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(slider_value_label_, lv_color_white(), 0);
    lv_obj_align(slider_value_label_, LV_ALIGN_TOP_RIGHT, 0, 12);

    // 滑块（更大更粗，居中偏下）
    slider_ = lv_slider_create(slider_container_);
    lv_obj_set_size(slider_, width_ - 80, 16);  // 更粗的滑块
    lv_obj_align(slider_, LV_ALIGN_CENTER, 0, 16);  // 居中偏下
    lv_slider_set_range(slider_, 0, 100);

    // 滑块样式 - 更大气
    lv_obj_set_style_bg_color(slider_, lv_color_hex(0x636366), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(slider_, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(slider_, 8, LV_PART_MAIN);

    lv_obj_set_style_bg_color(slider_, CC_SLIDER_IND_COLOR, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(slider_, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(slider_, 8, LV_PART_INDICATOR);

    lv_obj_set_style_bg_color(slider_, CC_SLIDER_IND_COLOR, LV_PART_KNOB);
    lv_obj_set_style_bg_opa(slider_, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_radius(slider_, 14, LV_PART_KNOB);  // 更大的滑块手柄
    lv_obj_set_style_pad_all(slider_, 8, LV_PART_KNOB);

    lv_obj_add_event_cb(slider_, OnSliderChanged, LV_EVENT_VALUE_CHANGED, this);
}

void ControlCenter::ShowSlider(bool is_brightness) {
    slider_is_brightness_ = is_brightness;
    char buf[16];

    if (is_brightness) {
        lv_label_set_text(slider_title_, "亮度");
        lv_slider_set_range(slider_, BRIGHTNESS_MIN, 100);
        // 对齐到步进值
        int aligned_value = (current_brightness_ / SLIDER_STEP) * SLIDER_STEP;
        if (aligned_value < BRIGHTNESS_MIN) aligned_value = BRIGHTNESS_MIN;
        lv_slider_set_value(slider_, aligned_value, LV_ANIM_OFF);
        snprintf(buf, sizeof(buf), "%d", aligned_value);
        lv_label_set_text(slider_value_label_, buf);
    } else {
        lv_label_set_text(slider_title_, "音量");
        lv_slider_set_range(slider_, 0, 100);
        // 对齐到步进值
        int aligned_value = (current_volume_ / SLIDER_STEP) * SLIDER_STEP;
        lv_slider_set_value(slider_, aligned_value, LV_ANIM_OFF);
        snprintf(buf, sizeof(buf), "%d", aligned_value);
        lv_label_set_text(slider_value_label_, buf);
    }

    lv_obj_remove_flag(slider_container_, LV_OBJ_FLAG_HIDDEN);
    StartSliderTimer();  // 启动自动关闭定时器
}

void ControlCenter::HideSlider() {
    StopSliderTimer();  // 停止定时器
    lv_obj_add_flag(slider_container_, LV_OBJ_FLAG_HIDDEN);
}

void ControlCenter::UpdateButtonStyle(lv_obj_t* btn, bool active) {
    if (btn == exit_btn_) {
        // 退出按钮始终红色
        lv_obj_set_style_bg_color(btn, CC_BTN_EXIT_COLOR, 0);
    } else if (btn == network_btn_) {
        // 网络按钮：WiFi/4G 统一蓝色
        lv_obj_set_style_bg_color(btn, active ? CC_BTN_ON_COLOR : CC_BTN_OFF_COLOR, 0);
        UpdateNetworkIcon();
    } else {
        lv_obj_set_style_bg_color(btn, active ? CC_BTN_ON_COLOR : CC_BTN_OFF_COLOR, 0);
    }
}

void ControlCenter::UpdateNetworkIcon() {
    if (!network_icon_) return;

    // 控制中心"切换"按钮只表达"当前是哪种联网方式"，不反映信号强度
    // 用编译内置的满格 PNG（同主时钟状态栏那套），避免 assets 分区依赖
    const lv_image_dsc_t* icon_dsc = (network_mode_ == 0)
                                       ? &ui_img_icon_signal_wifi_png
                                       : &ui_img_icon_signal_4g_4_png;
    lv_image_set_src(network_icon_, icon_dsc);
}

void ControlCenter::UpdateVolumeLabel() {
    // 更新按钮中间的数值显示
    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", current_volume_);
    lv_label_set_text(volume_icon_, buf);
}

void ControlCenter::UpdateBrightnessLabel() {
    // 更新按钮中间的数值显示
    char buf[16];
    snprintf(buf, sizeof(buf), "%d%%", current_brightness_);
    lv_label_set_text(brightness_icon_, buf);
}

void ControlCenter::UpdateSleepLabel() {
    // 更新按钮中间的显示（5分/无）
    lv_label_set_text(sleep_icon_, sleep_on_ ? "5分" : "无");
}

void ControlCenter::UpdateAecLabel() {
    // 更新按钮中间的显示（开/关）
    lv_label_set_text(aec_icon_, aec_on_ ? "开" : "关");
}

void ControlCenter::StartSliderTimer() {
    // 使用 ManagedTimer 创建单次定时器，3秒后自动关闭滑块
    slider_timer_.CreateOnce(3000, OnSliderTimer, this);
}

void ControlCenter::StopSliderTimer() {
    slider_timer_.Delete();
}

void ControlCenter::OnSliderTimer(lv_timer_t* timer) {
    auto* self = static_cast<ControlCenter*>(lv_timer_get_user_data(timer));
    // ManagedTimer 的 CreateOnce 设置了 repeat_count=1，触发后 LVGL 会自动停止
    // 但不会自动删除，所以 HideSlider 中会调用 Delete() 清理
    self->HideSlider();
    ESP_LOGI(TAG, "滑块自动关闭");
}

void ControlCenter::Show() {
    if (container_) {
        HideSlider();  // 显示时隐藏滑块
        lv_obj_remove_flag(container_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(container_);
        is_visible_ = true;
        ESP_LOGI(TAG, "控制中心显示");
    }
}

void ControlCenter::Hide() {
    if (container_) {
        HideSlider();
        lv_obj_add_flag(container_, LV_OBJ_FLAG_HIDDEN);
        is_visible_ = false;
        ESP_LOGI(TAG, "控制中心隐藏");
    }
}

void ControlCenter::SetNetworkMode(int mode) {
    network_mode_ = mode;
    UpdateButtonStyle(network_btn_, true);
}

void ControlCenter::SetSignalLevel(int level) {
    signal_level_ = level;
    UpdateNetworkIcon();
}

void ControlCenter::SetAecState(bool on) {
    aec_on_ = on;
    UpdateButtonStyle(aec_btn_, on);
    UpdateAecLabel();
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
    if (slider_is_brightness_ && !lv_obj_has_flag(slider_container_, LV_OBJ_FLAG_HIDDEN)) {
        lv_slider_set_value(slider_, current_brightness_, LV_ANIM_ON);
        char buf[8];
        snprintf(buf, sizeof(buf), "%d", current_brightness_);
        lv_label_set_text(slider_value_label_, buf);
    }
}

void ControlCenter::SetVolume(int value) {
    // 对齐到步进值
    int aligned = (value / SLIDER_STEP) * SLIDER_STEP;
    if (aligned < 0) aligned = 0;
    if (aligned > 100) aligned = 100;
    current_volume_ = aligned;
    UpdateVolumeLabel();
    if (!slider_is_brightness_ && !lv_obj_has_flag(slider_container_, LV_OBJ_FLAG_HIDDEN)) {
        lv_slider_set_value(slider_, current_volume_, LV_ANIM_ON);
        char buf[8];
        snprintf(buf, sizeof(buf), "%d", current_volume_);
        lv_label_set_text(slider_value_label_, buf);
    }
}

// 事件回调
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
    self->network_mode_ = (self->network_mode_ + 1) % 2;
    self->UpdateButtonStyle(self->network_btn_, true);
    ESP_LOGI(TAG, "网络模式切换: %s", self->network_mode_ == 0 ? "WiFi" : "4G");
    if (self->network_callback_) {
        self->network_callback_(self->network_mode_);
    }
}

void ControlCenter::OnAecClicked(lv_event_t* e) {
    auto* self = static_cast<ControlCenter*>(lv_event_get_user_data(e));
    self->HideSlider();
    self->aec_on_ = !self->aec_on_;
    self->UpdateButtonStyle(self->aec_btn_, self->aec_on_);
    self->UpdateAecLabel();
    ESP_LOGI(TAG, "打断: %s", self->aec_on_ ? "开" : "关");
    if (self->aec_callback_) {
        self->aec_callback_(self->aec_on_);
    }
}

void ControlCenter::OnSleepClicked(lv_event_t* e) {
    auto* self = static_cast<ControlCenter*>(lv_event_get_user_data(e));
    self->HideSlider();
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
    // 切换亮度滑块显示
    if (!lv_obj_has_flag(self->slider_container_, LV_OBJ_FLAG_HIDDEN) && self->slider_is_brightness_) {
        self->HideSlider();
    } else {
        self->ShowSlider(true);
    }
}

void ControlCenter::OnVolumeClicked(lv_event_t* e) {
    auto* self = static_cast<ControlCenter*>(lv_event_get_user_data(e));
    // 切换音量滑块显示
    if (!lv_obj_has_flag(self->slider_container_, LV_OBJ_FLAG_HIDDEN) && !self->slider_is_brightness_) {
        self->HideSlider();
    } else {
        self->ShowSlider(false);
    }
}

void ControlCenter::OnSliderChanged(lv_event_t* e) {
    auto* self = static_cast<ControlCenter*>(lv_event_get_user_data(e));
    int raw_value = lv_slider_get_value(self->slider_);

    // 重置自动关闭定时器
    self->StartSliderTimer();

    // 对齐到步进值
    int value = (raw_value / SLIDER_STEP) * SLIDER_STEP;

    char buf[16];

    if (self->slider_is_brightness_) {
        // 亮度最低15
        if (value < BRIGHTNESS_MIN) value = BRIGHTNESS_MIN;
        self->current_brightness_ = value;
        snprintf(buf, sizeof(buf), "%d", value);
        lv_label_set_text(self->slider_value_label_, buf);
        self->UpdateBrightnessLabel();
        ESP_LOGD(TAG, "亮度: %d", value);
        if (self->brightness_callback_) {
            self->brightness_callback_(value);
        }
    } else {
        self->current_volume_ = value;
        snprintf(buf, sizeof(buf), "%d", value);
        lv_label_set_text(self->slider_value_label_, buf);
        self->UpdateVolumeLabel();
        ESP_LOGD(TAG, "音量: %d", value);
        if (self->volume_callback_) {
            self->volume_callback_(value);
        }
    }
}