/*
 * AXS5106L 触摸屏固件升级模块
 *
 * 功能: 检查并升级触摸芯片固件
 * 移植自: esp_lcd_axs5106l 组件
 */

#pragma once

#include <driver/i2c_master.h>
#include <driver/gpio.h>
#include <cstdint>

/**
 * @brief AXS5106L 固件升级结果
 */
enum class Axs5106lUpgradeResult {
    Success = 0,           // 升级成功
    NotNeeded = -1,        // 不需要升级（版本已最新）
    Failed = -2,           // 升级失败
    I2cError = -3,         // I2C 通信错误
};

/**
 * @brief AXS5106L 固件升级类
 *
 * 该类负责检查触摸芯片固件版本，并在需要时升级到内嵌的最新固件。
 *
 * 使用方法:
 * 1. 创建实例时传入 I2C 句柄和复位 GPIO
 * 2. 调用 CheckAndUpgrade() 执行升级检查
 * 3. 如果返回 Success，需要重新复位芯片
 */
class Axs5106lUpgrade {
public:
    /**
     * @brief 构造函数
     * @param i2c_handle I2C 设备句柄
     * @param rst_gpio 复位引脚
     */
    Axs5106lUpgrade(i2c_master_dev_handle_t i2c_handle, gpio_num_t rst_gpio);

    /**
     * @brief 检查并升级固件
     * @return 升级结果
     */
    Axs5106lUpgradeResult CheckAndUpgrade();

    /**
     * @brief 获取芯片固件版本
     * @param version 输出版本号
     * @return 是否成功
     */
    bool GetChipFirmwareVersion(uint16_t& version);

    /**
     * @brief 获取内嵌固件版本
     * @return 内嵌固件版本号
     */
    uint16_t GetEmbeddedFirmwareVersion() const;

private:
    i2c_master_dev_handle_t i2c_handle_;
    gpio_num_t rst_gpio_;

    // I2C 通信
    bool WriteRegister(uint8_t reg, const uint8_t* data, size_t len);
    bool ReadRegister(uint8_t reg, uint8_t* data, size_t len);
    bool WriteRegisters(const uint8_t* reg, size_t reg_len, const uint8_t* data, size_t data_len);
    bool ReadRegisters(const uint8_t* reg, size_t reg_len, uint8_t* data, size_t data_len);

    // 复位
    void HardwareReset();
    void SoftwareReset();

    // 升级流程
    bool EnterDebugMode();
    void ExitDebugMode();
    bool UnlockFlash();
    bool EraseFlash();
    bool WriteFlash(const uint8_t* data, size_t len);
    bool VerifyFlash(const uint8_t* data, size_t len);
    bool DoUpgrade();

    // 延时
    static void DelayMs(uint16_t ms);
    static void DelayUs(uint16_t us);
};