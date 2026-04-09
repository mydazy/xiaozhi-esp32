/*
 * AXS5106L 触摸屏驱动
 * 芯片: AXS5106L (I2C 地址 0x63)
 * 适用: MyDazy P30 系列开发板 (284x240 分辨率)
 * 功能: 单击、双击、长按、上滑、下滑、左滑、右滑
 *
 * 特性:
 * - 自动检测并升级触摸芯片固件
 * - 手势识别（单击、双击、长按、滑动）
 * - 抗 4G 电磁干扰滤波
 */

#pragma once

#include <esp_lvgl_port.h>
#include <driver/i2c_master.h>
#include <driver/gpio.h>
#include <functional>
#include <cstdint>

// 前向声明
class Axs5106lUpgrade;

// 手势类型枚举
enum class TouchGesture {
    None,           // 无手势
    SingleClick,    // 单击
    DoubleClick,    // 双击
    LongPress,      // 长按
    SwipeUp,        // 上滑
    SwipeDown,      // 下滑
    SwipeLeft,      // 左滑
    SwipeRight      // 右滑
};

// 手势回调函数类型
using TouchGestureCallback = std::function<void(TouchGesture gesture, int16_t x, int16_t y)>;

/**
 * @brief AXS5106L 触摸屏驱动类 (精简版)
 */
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

    bool Initialize();
    // 共享 LCD/Touch 复位线时，硬件初始化必须在 LCD 点亮前完成。
    bool InitializeHardware();
    // LVGL 输入设备注册依赖显示和 LVGL 已经初始化完成。
    bool InitializeInput();
    void Sleep();
    void Resume();
    void SetTouchCallback(std::function<void()> callback);
    void SetGestureCallback(TouchGestureCallback callback);
    // 共享复位线场景下，这里只释放资源，不主动拉低 RST。
    void Cleanup();
    lv_indev_t* GetLvglDevice() const { return lvgl_indev_; }

private:
    // 硬件配置
    i2c_master_bus_handle_t i2c_bus_;
    i2c_master_dev_handle_t i2c_handle_;
    gpio_num_t rst_gpio_;
    gpio_num_t int_gpio_;

    // 显示参数
    uint16_t width_;
    uint16_t height_;
    bool swap_xy_;
    bool mirror_x_;
    bool mirror_y_;

    // LVGL 集成
    lv_indev_t* lvgl_indev_;

    // 回调
    std::function<void()> touch_callback_;
    TouchGestureCallback gesture_callback_;

    // 中断标志
    bool interrupt_installed_;
    // 用于把“芯片上电/复位”和“LVGL 输入注册”拆成两个阶段。
    bool hardware_initialized_;
    bool reset_gpio_configured_;
    bool int_gpio_configured_;

    // 手势识别状态
    struct {
        bool is_pressed;
        bool long_press_triggered;  // 长按已触发标志（避免释放时重复触发）
        int16_t start_x;
        int16_t start_y;
        int16_t last_x;
        int16_t last_y;
        uint64_t press_time;
        // 双击检测
        uint64_t last_click_time;   // 上次单击时间
        int16_t last_click_x;       // 上次单击位置
        int16_t last_click_y;
        // 抗干扰滤波
        uint64_t last_touch_time;   // 上次触摸时间（任何触摸）
        int16_t stable_x;           // 稳定的触摸坐标
        int16_t stable_y;
        uint8_t stable_count;       // 连续稳定读取次数
    } gesture_;

    // 滑动平均滤波器（3点滤波，平滑坐标抖动）
    struct {
        int16_t x_buf[3];           // X坐标缓冲
        int16_t y_buf[3];           // Y坐标缓冲
        uint8_t idx;                // 当前索引
        uint8_t count;              // 有效数据数量
        int16_t last_x;             // 上次有效坐标（用于速度计算）
        int16_t last_y;
        uint64_t last_time;         // 上次有效时间
    } filter_;

    // 私有方法
    bool ResetChip();
    bool CheckAndUpgradeFirmware();  // 检查并升级固件
    bool ReadRegister(uint8_t reg, uint8_t* data, size_t len);
    bool WriteRegister(uint8_t reg, const uint8_t* data, size_t len);
    static void LvglReadCallback(lv_indev_t* indev, lv_indev_data_t* data);
    static void IRAM_ATTR GpioIsrHandler(void* arg);
    void HandleTouchInterrupt();
    bool ParseTouchData(uint16_t& x, uint16_t& y, bool& pressed);
    void RecognizeGesture(int16_t x, int16_t y, bool pressed);
};
