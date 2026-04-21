/*
 * AXS5106L Touchscreen Driver — public API
 *
 * See axs5106l_touch.cc for hardware notes and design decisions.
 */

#pragma once

#include <esp_lvgl_port.h>
#include <driver/i2c_master.h>
#include <driver/gpio.h>
#include <functional>
#include <cstdint>
#include <atomic>

// Set to 1 to enable debug overlay: red tracking dot + raw-coordinate serial log.
// Keep 0 for production builds.
#ifndef AXS5106L_TOUCH_DEBUG_OVERLAY
#define AXS5106L_TOUCH_DEBUG_OVERLAY 0
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
                  bool swap_xy  = false,
                  bool mirror_x = false,
                  bool mirror_y = false);
    ~Axs5106lTouch();

    // Phase 1: call before LVGL starts (configures GPIO, runs firmware upgrade)
    bool InitializeHardware();
    // Phase 2: call after LVGL starts (registers input device)
    bool InitializeInput();

    void Sleep();
    void Resume();
    void Cleanup();

    // Fired on the first press frame (use for screen wake-up)
    void SetWakeCallback(std::function<void()> cb) { wake_callback_ = std::move(cb); }
    // Fired when a gesture is recognized
    void SetGestureCallback(TouchGestureCallback cb) { gesture_callback_ = std::move(cb); }

    lv_indev_t* GetLvglDevice() const { return lvgl_indev_; }

private:
    i2c_master_bus_handle_t i2c_bus_;
    i2c_master_dev_handle_t i2c_handle_ = nullptr;
    gpio_num_t rst_gpio_;
    gpio_num_t int_gpio_;
    uint16_t   width_;
    uint16_t   height_;
    bool swap_xy_;
    bool mirror_x_;
    bool mirror_y_;

    lv_indev_t*           lvgl_indev_ = nullptr;
    std::atomic<bool>     sleeping_{false};
    std::function<void()> wake_callback_;
    TouchGestureCallback  gesture_callback_;

    // I2C bus-recovery counter: when 4G RF locks SDA, consecutive read failures
    // accumulate; on threshold, i2c_master_bus_reset() sends 9 SCL pulses to unlock.
    uint8_t i2c_err_streak_ = 0;

    struct TouchState {
        bool     pressed      = false;
        uint8_t  release_count = 0;
        int16_t  last_x       = 0;
        int16_t  last_y       = 0;
        uint64_t last_time    = 0;   // for velocity filter
        uint64_t int_low_since = 0;  // for INT debounce
    } touch_;

    struct GestureState {
        bool     pressed    = false;
        bool     long_fired = false;
        int16_t  start_x   = 0, start_y   = 0;
        int16_t  last_x    = 0, last_y    = 0;
        uint64_t press_time      = 0;
        uint64_t last_click_time = 0;
        int16_t  last_click_x   = 0, last_click_y = 0;
    } gesture_;

#if AXS5106L_TOUCH_DEBUG_OVERLAY
    struct {
        uint16_t raw_min_x    = 0xFFFF;
        uint16_t raw_max_x    = 0;
        uint16_t raw_min_y    = 0xFFFF;
        uint16_t raw_max_y    = 0;
        uint32_t sample_count = 0;
    } raw_stats_;
    lv_obj_t* debug_dot_ = nullptr;
#endif

    void ResetChip();
    bool CheckAndUpgradeFirmware();
    bool WriteRegister(uint8_t reg, const uint8_t* data, size_t len);
    bool ReadRegister(uint8_t reg, uint8_t* data, size_t len);
    bool ReadTouch(uint16_t& x, uint16_t& y);
    void RecognizeGesture(int16_t x, int16_t y, bool pressed);

    static void LvglReadCallback(lv_indev_t* indev, lv_indev_data_t* data);
};
