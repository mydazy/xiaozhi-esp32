#ifndef __SC7A20H_H__
#define __SC7A20H_H__

#include "i2c_device.h"
#include <functional>

/**
 * @brief SC7A20H 三轴加速度计传感器类
 * 
 * 简化版本，专注于基本的运动检测和唤醒功能
 */
class Sc7a20h : public I2cDevice {
public:
    // 简化的中断回调函数类型
    using WakeupCallback = std::function<void()>;

public:
    /**
     * @brief 构造函数
     * @param i2c_bus I2C总线句柄
     * @param addr 设备地址 (默认 0x19)
     */
    Sc7a20h(i2c_master_bus_handle_t i2c_bus, uint8_t addr = 0x19);

    /**
     * @brief 析构函数
     */
    ~Sc7a20h();

    /**
     * @brief 初始化传感器
     * @return true 成功, false 失败
     */
    bool Initialize();

    /**
     * @brief 启用/禁用运动检测中断
     * @param enable true启用, false禁用
     * @return true 成功, false 失败
     */
    bool SetMotionDetection(bool enable);

    /**
     * @brief 设置唤醒回调
     * @param callback 回调函数
     */
    void SetWakeupCallback(WakeupCallback callback);

    /**
     * @brief 进入低功耗模式
     * @return true 成功, false 失败
     */
    bool EnterPowerDown();

    /**
     * @brief 退出低功耗模式
     * @return true 成功, false 失败
     */
    bool ExitPowerDown();

private:
    // 私有成员变量
    bool initialized_;
    bool motion_detection_enabled_;
    
    // 回调函数
    WakeupCallback wakeup_callback_;

    // 私有方法
    bool CheckDeviceId();
    bool ConfigureSensor();
};

#endif // __SC7A20H_H__
