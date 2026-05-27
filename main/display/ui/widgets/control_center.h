#ifndef CONTROL_CENTER_H
#define CONTROL_CENTER_H

#include <lvgl.h>
#include <functional>
#include <string>
#include "managed_timer.h"

// iOS 风格控制中心组件（3x2 宫格布局）
// 第一排：退出、网络切换(WiFi/4G)、AEC开关
// 第二排：休眠、音量、亮度
class ControlCenter {
public:
    // 回调函数类型定义
    using ToggleCallback = std::function<void(bool enabled)>;
    using SliderCallback = std::function<void(int value)>;
    using NetworkCallback = std::function<void(int mode)>;  // 0=WiFi, 1=4G
    using VoidCallback = std::function<void()>;

    ControlCenter(lv_obj_t* parent, int width, int height);
    ~ControlCenter();

    // 显示/隐藏控制中心
    void Show();
    void Hide();
    bool IsVisible() const { return is_visible_; }
    void Toggle() { is_visible_ ? Hide() : Show(); }

    // 设置回调
    void SetExitCallback(VoidCallback cb) { exit_callback_ = cb; }
    void SetNetworkCallback(NetworkCallback cb) { network_callback_ = cb; }
    void SetAecCallback(ToggleCallback cb) { aec_callback_ = cb; }
    void SetAecToggleCallback(ToggleCallback cb) { aec_toggle_callback_ = cb; }  // WiFi 版网络槽位作 AEC 开关
    void SetSleepCallback(ToggleCallback cb) { sleep_callback_ = cb; }
    void SetBrightnessCallback(SliderCallback cb) { brightness_callback_ = cb; }
    void SetVolumeCallback(SliderCallback cb) { volume_callback_ = cb; }

    // 更新状态（外部调用）
    void SetNetworkMode(int mode);      // 0=WiFi, 1=4G
    void UseNetworkSlotAsAec(bool initial_on);  // WiFi 版：网络槽位改作 AEC 开关（无 4G 可切）
    void UpdateNetworkSlotAec(bool on);         // 每次打开时同步 AEC 当前状态（仅 AEC 槽位生效）
    void SetSignalLevel(int level);     // 0-4 信号强度（4G: 0-4格, WiFi: 0-3格）
    void SetAecState(bool on);
    void SetSleepState(bool on);
    void SetBrightness(int value);      // 15-100
    void SetVolume(int value);          // 0-100

private:
    void CreateUI();
    void CreateGridButton(int col, int row, int start_x, int start_y,
                         const char* symbol, const char* text,
                         lv_obj_t** btn, lv_obj_t** icon, lv_obj_t** label,
                         bool large_icon = false, bool use_text_font = false);
    void CreateSliderArea();
    void ShowSlider(bool is_brightness);
    void HideSlider();
    void UpdateButtonStyle(lv_obj_t* btn, bool active);
    void UpdateVolumeLabel();
    void UpdateBrightnessLabel();
    void UpdateSleepLabel();
    void UpdateAecLabel();
    void UpdateNetworkIcon();
    void StartSliderTimer();
    void StopSliderTimer();

    // 事件处理
    static void OnExitClicked(lv_event_t* e);
    static void OnNetworkClicked(lv_event_t* e);
    static void OnAecClicked(lv_event_t* e);
    static void OnSleepClicked(lv_event_t* e);
    static void OnBrightnessClicked(lv_event_t* e);
    static void OnVolumeClicked(lv_event_t* e);
    static void OnSliderChanged(lv_event_t* e);
    static void OnSliderTimer(lv_timer_t* timer);

    lv_obj_t* parent_ = nullptr;
    lv_obj_t* container_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    bool is_visible_ = false;

    // 第一行：退出、网络、AEC
    lv_obj_t* exit_btn_ = nullptr;
    lv_obj_t* exit_icon_ = nullptr;
    lv_obj_t* exit_label_ = nullptr;

    lv_obj_t* network_btn_ = nullptr;
    lv_obj_t* network_icon_ = nullptr;      // 图片控件（显示信号图标）
    lv_obj_t* network_label_ = nullptr;
    lv_obj_t* aec_state_label_ = nullptr;   // AEC 模式下 network_btn_ 上的"开/关"文字

    lv_obj_t* aec_btn_ = nullptr;
    lv_obj_t* aec_icon_ = nullptr;
    lv_obj_t* aec_label_ = nullptr;

    // 第二行：休眠、音量、亮度
    lv_obj_t* sleep_btn_ = nullptr;
    lv_obj_t* sleep_icon_ = nullptr;
    lv_obj_t* sleep_label_ = nullptr;

    lv_obj_t* volume_btn_ = nullptr;
    lv_obj_t* volume_icon_ = nullptr;
    lv_obj_t* volume_label_ = nullptr;

    lv_obj_t* brightness_btn_ = nullptr;
    lv_obj_t* brightness_icon_ = nullptr;
    lv_obj_t* brightness_label_ = nullptr;

    // 上方滑块区域
    lv_obj_t* slider_container_ = nullptr;
    lv_obj_t* slider_title_ = nullptr;
    lv_obj_t* slider_ = nullptr;
    lv_obj_t* slider_value_label_ = nullptr;
    ManagedTimer slider_timer_;  // 滑块自动关闭定时器（安全封装）
    bool slider_is_brightness_ = true;

    // 状态值
    int network_mode_ = 0;          // 0=WiFi, 1=4G
    bool network_is_aec_ = false;   // true=网络槽位作 AEC 开关（WiFi 版）
    int signal_level_ = 0;          // 0-4 信号强度
    bool aec_on_ = true;
    bool sleep_on_ = true;
    int current_brightness_ = 80;
    int current_volume_ = 70;

    // 回调
    VoidCallback exit_callback_;
    NetworkCallback network_callback_;
    ToggleCallback aec_toggle_callback_;   // WiFi 版网络槽位 AEC 开关回调（传入新状态）
    ToggleCallback aec_callback_;
    ToggleCallback sleep_callback_;
    SliderCallback brightness_callback_;
    SliderCallback volume_callback_;
};

#endif // CONTROL_CENTER_H