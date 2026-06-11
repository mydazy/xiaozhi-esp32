#ifndef CONTROL_CENTER_H
#define CONTROL_CENTER_H

#include <lvgl.h>
#include <functional>
#include "managed_timer.h"

// iOS 风格控制中心组件（3x2 宫格布局，WiFi/4G 版位置统一）
// 第一排：网络（4G版=切WiFi/4G · WiFi版=进配网，均二次确认）、打断(AEC)、休眠
// 第二排：退出、亮度、关于
// 音量不进控制中心（物理按键 + 语音调节）
class ControlCenter {
public:
    using ToggleCallback = std::function<void(bool enabled)>;
    using SliderCallback = std::function<void(int value)>;
    using VoidCallback = std::function<void()>;

    ControlCenter(lv_obj_t* parent, int width, int height);
    ~ControlCenter();

    // 显示/隐藏控制中心
    void Show();
    void Hide();
    void Raise();   // 可见时重新提顶（对话期间 bottom_bar/status_bar 提顶会压住本面板）
    bool IsVisible() const { return is_visible_; }
    void Toggle() { is_visible_ ? Hide() : Show(); }

    // 设置回调
    void SetExitCallback(VoidCallback cb) { exit_callback_ = cb; }
    void SetNetworkCallback(VoidCallback cb) { network_callback_ = cb; }       // 二次确认通过后触发
    void SetAecToggleCallback(VoidCallback cb) { aec_toggle_callback_ = cb; }  // 仅请求切换，实际状态以 SetAecState 回写为准
    void SetSleepCallback(ToggleCallback cb) { sleep_callback_ = cb; }
    void SetBrightnessCallback(SliderCallback cb) { brightness_callback_ = cb; }
    void SetAboutCallback(VoidCallback cb) { about_callback_ = cb; }

    // 更新状态（外部调用）
    void SetNetworkMode(int mode);      // 4G 版：0=WiFi, 1=4G（按钮文案"切换"）
    void SetNetworkAsProvision();       // WiFi 版：按钮语义=进配网（按钮文案"配网"）
    void SetAecState(bool on);          // AEC 实际状态回写（开/关 文字 + 高亮）
    void SetAecEnabled(bool enabled);   // false=置灰不可点（4G 版 AEC 未放开）
    void SetSleepState(bool on);
    void SetBrightness(int value);      // 15-100

private:
    void CreateUI();
    void CreateGridButton(int col, int row, int start_x, int start_y,
                         const char* symbol, const char* text,
                         lv_obj_t** btn, lv_obj_t** icon, lv_obj_t** label,
                         bool large_icon = false, bool use_text_font = false);
    void CreateSliderArea();
    void ShowSlider();                  // 亮度滑块（控制中心唯一滑块）
    void HideSlider();
    void UpdateButtonStyle(lv_obj_t* btn, bool active);
    void UpdateBrightnessLabel();
    void UpdateSleepLabel();
    void UpdateNetworkIcon();
    void StartSliderTimer();
    void StopSliderTimer();
    void ResetNetworkConfirm();         // 取消二次确认态，恢复按钮原样

    // 事件处理
    static void OnExitClicked(lv_event_t* e);
    static void OnNetworkClicked(lv_event_t* e);
    static void OnAecClicked(lv_event_t* e);
    static void OnSleepClicked(lv_event_t* e);
    static void OnBrightnessClicked(lv_event_t* e);
    static void OnAboutClicked(lv_event_t* e);
    static void OnSliderChanged(lv_event_t* e);
    static void OnSliderTimer(lv_timer_t* timer);
    static void OnNetworkConfirmTimer(lv_timer_t* timer);
    static void OnButtonPressed(lv_event_t* e);   // 按下诊断：名字+触摸坐标+按钮矩形
    void AddPressFeedback(lv_obj_t* btn, const char* name);

    lv_obj_t* parent_ = nullptr;
    lv_obj_t* container_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    bool is_visible_ = false;

    // 第一行：网络、打断(AEC)、休眠
    lv_obj_t* network_btn_ = nullptr;
    lv_obj_t* network_icon_ = nullptr;      // 图片控件（WiFi/4G 图标）
    lv_obj_t* network_label_ = nullptr;

    lv_obj_t* aec_btn_ = nullptr;
    lv_obj_t* aec_icon_ = nullptr;          // 按钮中间"开/关"文字
    lv_obj_t* aec_label_ = nullptr;

    lv_obj_t* sleep_btn_ = nullptr;
    lv_obj_t* sleep_icon_ = nullptr;
    lv_obj_t* sleep_label_ = nullptr;

    // 第二行：退出、亮度、关于
    lv_obj_t* exit_btn_ = nullptr;
    lv_obj_t* exit_icon_ = nullptr;
    lv_obj_t* exit_label_ = nullptr;

    lv_obj_t* brightness_btn_ = nullptr;
    lv_obj_t* brightness_icon_ = nullptr;
    lv_obj_t* brightness_label_ = nullptr;

    lv_obj_t* about_btn_ = nullptr;
    lv_obj_t* about_icon_ = nullptr;
    lv_obj_t* about_label_ = nullptr;

    // 上方亮度滑块区域
    lv_obj_t* slider_container_ = nullptr;
    lv_obj_t* slider_title_ = nullptr;
    lv_obj_t* slider_ = nullptr;
    lv_obj_t* slider_value_label_ = nullptr;
    ManagedTimer slider_timer_;             // 滑块自动关闭定时器
    ManagedTimer network_confirm_timer_;    // 网络二次确认超时回退定时器

    // 状态值
    int network_mode_ = 0;                  // 0=WiFi, 1=4G
    bool network_is_provision_ = false;     // true=WiFi 版（按钮=进配网）
    bool network_confirm_pending_ = false;  // 网络按钮处于"再点确认"态
    uint32_t network_confirm_tick_ = 0;     // 进入确认态的 lv_tick（防抖动连击穿透）
    bool aec_on_ = false;
    bool aec_enabled_ = true;
    bool sleep_on_ = true;
    int current_brightness_ = 35;   // 与系统默认亮度一致（backlight.cc GetInt("brightness", 35)）

    // 回调
    VoidCallback exit_callback_;
    VoidCallback network_callback_;
    VoidCallback aec_toggle_callback_;
    ToggleCallback sleep_callback_;
    SliderCallback brightness_callback_;
    VoidCallback about_callback_;
};

#endif // CONTROL_CENTER_H
