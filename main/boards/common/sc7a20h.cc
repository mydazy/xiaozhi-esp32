#include "sc7a20h.h"
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define TAG "Sc7a20h"

// SC7A20H 寄存器定义
#define SC7A20H_WHO_AM_I        0x0F
#define SC7A20H_CTRL_REG1       0x20
#define SC7A20H_CTRL_REG2       0x21
#define SC7A20H_CTRL_REG3       0x22
#define SC7A20H_CTRL_REG4       0x23
#define SC7A20H_CTRL_REG5       0x24
#define SC7A20H_CTRL_REG6       0x25
#define SC7A20H_INT1_CFG        0x30
#define SC7A20H_INT1_THS        0x32
#define SC7A20H_INT1_DURATION   0x33

// 设备ID
#define SC7A20H_DEVICE_ID       0x11

// 中断配置
#define SC7A20H_INT1_ENABLE     0x40
#define SC7A20H_MOTION_DETECT   0x02
#define SC7A20H_COLLISION_DETECT 0x01

Sc7a20h::Sc7a20h(i2c_master_bus_handle_t i2c_bus, uint8_t addr) 
    : I2cDevice(i2c_bus, addr), 
      initialized_(false),
      motion_detection_enabled_(false) {
}

Sc7a20h::~Sc7a20h() {
    if (initialized_) {
        EnterPowerDown();
    }
}

bool Sc7a20h::Initialize() {
    ESP_LOGI(TAG, "Initializing SC7A20H sensor");
    
    // 检查设备ID
    if (!CheckDeviceId()) {
        ESP_LOGE(TAG, "Device ID check failed");
        return false;
    }
    
    // 配置传感器
    if (!ConfigureSensor()) {
        ESP_LOGE(TAG, "Sensor configuration failed");
        return false;
    }
    
    initialized_ = true;
    ESP_LOGI(TAG, "SC7A20H initialization completed successfully");
    return true;
}

bool Sc7a20h::CheckDeviceId() {
    uint8_t device_id = ReadReg(SC7A20H_WHO_AM_I);
    
    ESP_LOGI(TAG, "Device ID: 0x%02X", device_id);
    
    if (device_id != SC7A20H_DEVICE_ID) {
        ESP_LOGE(TAG, "Invalid device ID: 0x%02X, expected: 0x%02X", device_id, SC7A20H_DEVICE_ID);
        return false;
    }
    
    return true;
}

bool Sc7a20h::ConfigureSensor() {
    ESP_LOGI(TAG, "Configuring SC7A20H sensor");
    
    // 进入低功耗模式进行配置
    WriteReg(SC7A20H_CTRL_REG1, 0x00);
    vTaskDelay(pdMS_TO_TICKS(10));
    
    // 配置量程为±4g
    WriteReg(SC7A20H_CTRL_REG4, 0x90);
    
    // 配置高通滤波器禁用
    WriteReg(SC7A20H_CTRL_REG2, 0x01);
    
    // 配置FIFO禁用
    WriteReg(SC7A20H_CTRL_REG5, 0x80);
    
    // 配置中断
    WriteReg(SC7A20H_CTRL_REG3, SC7A20H_INT1_ENABLE);
    
    // 配置中断电平为高电平有效
    WriteReg(SC7A20H_CTRL_REG6, 0x02);
    
    // 启用XYZ轴，设置100Hz输出数据率
    WriteReg(SC7A20H_CTRL_REG1, 0x5F);
    vTaskDelay(pdMS_TO_TICKS(10));
    
    ESP_LOGI(TAG, "Sensor configuration completed");
    return true;
}


bool Sc7a20h::SetMotionDetection(bool enable) {
    if (!initialized_) {
        ESP_LOGE(TAG, "Sensor not initialized");
        return false;
    }
    
    motion_detection_enabled_ = enable;
    
    if (enable) {
        // 使用默认阈值和持续时间
        WriteReg(SC7A20H_INT1_THS, 0x0C);        // 默认阈值  hsf   0x08 --> 0x10
        WriteReg(SC7A20H_INT1_DURATION, 0x03);   // 默认持续时间  hsf   0x02 --> 0x04
        WriteReg(SC7A20H_INT1_CFG, SC7A20H_MOTION_DETECT);
        ESP_LOGI(TAG, "Motion detection enabled");
    } else {
        WriteReg(SC7A20H_INT1_CFG, 0x00);
        ESP_LOGI(TAG, "Motion detection disabled");
    }
    
    return true;
}

void Sc7a20h::SetWakeupCallback(WakeupCallback callback) {
    wakeup_callback_ = callback;
}

bool Sc7a20h::EnterPowerDown() {
    if (!initialized_) {
        return false;
    }
    
    WriteReg(SC7A20H_CTRL_REG1, 0x00);
    vTaskDelay(pdMS_TO_TICKS(10));
    
    ESP_LOGI(TAG, "Entered power down mode");
    return true;
}

bool Sc7a20h::ExitPowerDown() {
    if (!initialized_) {
        return false;
    }
    
    WriteReg(SC7A20H_CTRL_REG1, 0x5F);
    vTaskDelay(pdMS_TO_TICKS(10));
    
    ESP_LOGI(TAG, "Exited power down mode");
    return true;
}

// 移除ProcessInterrupts方法，简化代码
