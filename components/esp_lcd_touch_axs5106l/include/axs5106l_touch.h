/*
 * AXS5106L 触摸屏驱动
 *
 * 适配: MyDazy P30 系列（ESP32-S3 + JD9853 284×240 + AXS5106L），I2C 地址 0x63
 * 职责: chip 初始化 + 固件升级 + I2C 轮询读坐标 + host 层手势识别 + LVGL 接入
 *
 * 设计原则（见 README.md）：
 *   - 坐标 1:1 直出（chip V2905 固件已做旋转，直接输出 landscape 像素）
 *   - 轻量去抖：INT 持续 3ms + 连续 2 帧释放确认 + 速度滤波 2000px/s
 *   - 不做滑动平均（延迟 >收益），不做稳定性多帧确认（触发灵敏度优先）
 *   - LVGL 纯轮询，不用 GPIO 中断（轮询频率 30ms 对 1 指触摸够用）
 */

#pragma once

#include <esp_lvgl_port.h>
#include <driver/i2c_master.h>
#include <driver/gpio.h>
#include <functional>
#include <cstdint>
#include <atomic>

// 触摸校准调试开关（量产保持 0）。=1 时屏幕会画四条 2px 红色边缘线 + 红色跟手圆点，
// 并每 20 次触摸打印一次 chip 原始输出范围，用于观察 edge suppression 覆盖。
#ifndef AXS5106L_TOUCH_DEBUG_OVERLAY
#define AXS5106L_TOUCH_DEBUG_OVERLAY 1   // 临时开启测试边界，测完改回 0
#endif

enum class TouchGesture {
    None,
    SingleClick,
    DoubleClick,
    LongPress,
    LongPressRelease,
    SwipeUp,
    SwipeDown,
    SwipeLeft,
    SwipeRight,
};

using TouchGestureCallback = std::function<void(TouchGesture, int16_t x, int16_t y)>;

class Axs5106lTouch {
public:
    Axs5106lTouch(i2c_master_bus_handle_t i2c_bus,
                  gpio_num_t rst_gpio,
                  gpio_num_t int_gpio,
                  uint16_t width,
                  uint16_t height,
                  bool swap_xy,
                  bool mirror_x,
                  bool mirror_y);
    ~Axs5106lTouch();

    // 分两阶段：InitializeHardware 在 LCD 初始化前（可能拉共享 LDO），
    // InitializeInput 在 LVGL 启动后再注册输入设备。
    bool InitializeHardware();
    bool InitializeInput();

    void Sleep();
    void Resume();
    void Cleanup();

    // 用户按下时触发（LVGL 回调里检测到新 press 的那一帧）
    void SetWakeCallback(std::function<void()> cb) { wake_callback_ = std::move(cb); }
    // 识别到 click/double/long/swipe 时触发
    void SetGestureCallback(TouchGestureCallback cb) { gesture_callback_ = std::move(cb); }

    lv_indev_t* GetLvglDevice() const { return lvgl_indev_; }

private:
    // 硬件
    i2c_master_bus_handle_t i2c_bus_;
    i2c_master_dev_handle_t i2c_handle_ = nullptr;
    gpio_num_t rst_gpio_;
    gpio_num_t int_gpio_;
    uint16_t width_;
    uint16_t height_;
    bool swap_xy_;
    bool mirror_x_;
    bool mirror_y_;

    // LVGL
    lv_indev_t* lvgl_indev_ = nullptr;
    std::atomic<bool> sleeping_{false};

    // 回调
    std::function<void()> wake_callback_;
    TouchGestureCallback gesture_callback_;

    // 触摸状态（LVGL 上下文，单线程）
    struct {
        bool pressed = false;           // 当前是否已确认按下
        uint8_t release_count = 0;      // 连续释放帧计数（2 帧确认）
        int16_t last_x = 0;             // 最后有效坐标（释放去抖期间保持给 LVGL）
        int16_t last_y = 0;
        uint64_t last_time = 0;         // 最后一帧时间（速度滤波）
        uint64_t int_low_since = 0;     // INT 首次拉低时间（非阻塞去抖）
    } touch_;

    // 手势状态
    struct {
        bool pressed = false;
        bool long_fired = false;
        int16_t start_x = 0;
        int16_t start_y = 0;
        int16_t last_x = 0;
        int16_t last_y = 0;
        uint64_t press_time = 0;
        uint64_t last_click_time = 0;
        int16_t last_click_x = 0;
        int16_t last_click_y = 0;
    } gesture_;

#if AXS5106L_TOUCH_DEBUG_OVERLAY
    struct {
        uint16_t raw_min_x = 0xFFFF;
        uint16_t raw_max_x = 0;
        uint16_t raw_min_y = 0xFFFF;
        uint16_t raw_max_y = 0;
        uint32_t sample_count = 0;
    } raw_stats_;
    lv_obj_t* debug_dot_ = nullptr;
    lv_obj_t* debug_edges_[4] = {nullptr, nullptr, nullptr, nullptr};
#endif

    // 私有方法
    bool ResetChip();
    bool CheckAndUpgradeFirmware();
    bool ReadRegister(uint8_t reg, uint8_t* data, size_t len);
    bool WriteRegister(uint8_t reg, const uint8_t* data, size_t len);
    static void LvglReadCallback(lv_indev_t* indev, lv_indev_data_t* data);
    // 返回 true：本帧有有效按下坐标（输出到 x/y）
    bool ReadTouch(uint16_t& x, uint16_t& y);
    void RecognizeGesture(int16_t x, int16_t y, bool pressed);
};
