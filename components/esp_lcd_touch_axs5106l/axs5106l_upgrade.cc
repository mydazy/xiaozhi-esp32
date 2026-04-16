/*
 * AXS5106L 触摸屏固件升级模块
 *
 * 移植自: esp_lcd_axs5106l 组件的 axs_upgrade.c
 */

#include "axs5106l_upgrade.h"
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cstring>

static const char* TAG = "Axs5106lUpgrade";

// 固件数据（从 firmware_flash.i 导入）
static const uint8_t kFirmwareData[] = {
#include "axs5106l_firmware.h"
};

// 固件版本偏移量
#define FIRMWARE_VERSION_OFFSET  0x400

// I2C 参数
#define I2C_TIMEOUT_MS           100
#define I2C_MAX_RETRIES          3

// 升级参数
#define UPGRADE_RETRY_TIMES      1
#define DEBUG_MODE_RETRY_TIMES   3
#define ERASE_TIMEOUT_MS         300
#define WRITE_TIMEOUT_MS         10

Axs5106lUpgrade::Axs5106lUpgrade(i2c_master_dev_handle_t i2c_handle, gpio_num_t rst_gpio)
    : i2c_handle_(i2c_handle), rst_gpio_(rst_gpio) {
}

void Axs5106lUpgrade::DelayMs(uint16_t ms) {
    vTaskDelay(pdMS_TO_TICKS(ms));
}

void Axs5106lUpgrade::DelayUs(uint16_t us) {
    uint64_t start = esp_timer_get_time();
    while (esp_timer_get_time() - start < us) {
        // 忙等待
    }
}

bool Axs5106lUpgrade::WriteRegister(uint8_t reg, const uint8_t* data, size_t len) {
    if (i2c_handle_ == nullptr || len > 64) return false;

    uint8_t buf[65];
    buf[0] = reg;
    memcpy(&buf[1], data, len);

    for (int retry = 0; retry < I2C_MAX_RETRIES; retry++) {
        esp_err_t ret = i2c_master_transmit(i2c_handle_, buf, len + 1, I2C_TIMEOUT_MS);
        if (ret == ESP_OK) return true;
        DelayMs(5);
    }
    return false;
}

bool Axs5106lUpgrade::ReadRegister(uint8_t reg, uint8_t* data, size_t len) {
    if (i2c_handle_ == nullptr) return false;

    for (int retry = 0; retry < I2C_MAX_RETRIES; retry++) {
        esp_err_t ret = i2c_master_transmit(i2c_handle_, &reg, 1, I2C_TIMEOUT_MS);
        if (ret != ESP_OK) {
            DelayMs(5);
            continue;
        }

        ret = i2c_master_receive(i2c_handle_, data, len, I2C_TIMEOUT_MS);
        if (ret == ESP_OK) return true;

        DelayMs(5);
    }
    return false;
}

bool Axs5106lUpgrade::WriteRegisters(const uint8_t* reg, size_t reg_len, const uint8_t* data, size_t data_len) {
    if (i2c_handle_ == nullptr) return false;

    // 合并寄存器地址和数据
    size_t total_len = reg_len + data_len;
    if (total_len > 600) return false;

    uint8_t buf[600];
    memcpy(buf, reg, reg_len);
    memcpy(buf + reg_len, data, data_len);

    for (int retry = 0; retry < I2C_MAX_RETRIES; retry++) {
        esp_err_t ret = i2c_master_transmit(i2c_handle_, buf, total_len, I2C_TIMEOUT_MS);
        if (ret == ESP_OK) return true;
        DelayMs(5);
    }
    return false;
}

bool Axs5106lUpgrade::ReadRegisters(const uint8_t* reg, size_t reg_len, uint8_t* data, size_t data_len) {
    if (i2c_handle_ == nullptr) return false;

    for (int retry = 0; retry < I2C_MAX_RETRIES; retry++) {
        esp_err_t ret = i2c_master_transmit(i2c_handle_, reg, reg_len, I2C_TIMEOUT_MS);
        if (ret != ESP_OK) {
            DelayMs(5);
            continue;
        }

        ret = i2c_master_receive(i2c_handle_, data, data_len, I2C_TIMEOUT_MS);
        if (ret == ESP_OK) return true;

        DelayMs(5);
    }
    return false;
}

void Axs5106lUpgrade::HardwareReset() {
    gpio_set_level(rst_gpio_, 1);
    DelayUs(50);
    gpio_set_level(rst_gpio_, 0);
    DelayUs(50);
    DelayMs(20);
    gpio_set_level(rst_gpio_, 1);
}

void Axs5106lUpgrade::SoftwareReset() {
    uint8_t rst_cmd[5] = {0xB3, 0x55, 0xAA, 0x34, 0x01};
    WriteRegister(0xF0, rst_cmd, 5);
    HardwareReset();
}

bool Axs5106lUpgrade::GetChipFirmwareVersion(uint16_t& version) {
    uint8_t fw_ver[2] = {0};
    if (!ReadRegister(0x05, fw_ver, 2)) {
        return false;
    }
    version = (fw_ver[0] << 8) | fw_ver[1];
    return true;
}

uint16_t Axs5106lUpgrade::GetEmbeddedFirmwareVersion() const {
    if (sizeof(kFirmwareData) < FIRMWARE_VERSION_OFFSET + 2) {
        return 0;
    }
    return (kFirmwareData[FIRMWARE_VERSION_OFFSET] << 8) | kFirmwareData[FIRMWARE_VERSION_OFFSET + 1];
}

bool Axs5106lUpgrade::EnterDebugMode() {
    uint8_t debug_cmd[1] = {0x55};
    uint8_t write_buf[3] = {0x80, 0x7f, 0xd1};
    uint8_t read_buf[1] = {0x00};

    for (int retry = 0; retry < DEBUG_MODE_RETRY_TIMES; retry++) {
        // 复位芯片
        SoftwareReset();

        // 等待 500us < delay < 4ms
        DelayUs(800);

        // 发送调试模式命令
        WriteRegister(0xAA, debug_cmd, 1);

        // delay >= 50us
        DelayUs(100);

        // 检查是否进入调试模式
        if (ReadRegisters(write_buf, 3, read_buf, 1)) {
            if (read_buf[0] == 0x28) {
                ESP_LOGI(TAG, "进入调试模式成功");
                return true;
            }
        }
    }

    ESP_LOGE(TAG, "进入调试模式失败");
    return false;
}

void Axs5106lUpgrade::ExitDebugMode() {
    uint8_t cmd[1] = {0x5F};
    WriteRegister(0xA0, cmd, 1);
}

bool Axs5106lUpgrade::UnlockFlash() {
    uint8_t unlock_cmd[3] = {0x6F, 0xFF, 0xFF};
    WriteRegister(0x90, unlock_cmd, 3);

    unlock_cmd[1] = 0xDA;
    unlock_cmd[2] = 0x18;
    WriteRegister(0x90, unlock_cmd, 3);

    return true;
}

bool Axs5106lUpgrade::EraseFlash() {
    uint8_t clear_flag[3] = {0x6F, 0xD9, 0x0C};
    uint8_t erase_cmd[3] = {0x6F, 0xD6, 0x77};
    uint8_t write_buf[3] = {0x80, 0x7F, 0xD9};
    uint8_t read_buf[1] = {0x00};

    WriteRegister(0x90, clear_flag, 3);
    WriteRegister(0x90, erase_cmd, 3);

    // 等待擦除完成 (最多 300ms)
    for (int i = 0; i < 30; i++) {
        DelayMs(WRITE_TIMEOUT_MS);

        if (ReadRegisters(write_buf, 3, read_buf, 1)) {
            if (read_buf[0] & 0x04) {  // bit2 == 1 表示完成
                erase_cmd[2] = 0x00;
                WriteRegister(0x90, erase_cmd, 3);
                ESP_LOGI(TAG, "Flash 擦除成功");
                return true;
            }
        }
    }

    erase_cmd[2] = 0x00;
    WriteRegister(0x90, erase_cmd, 3);
    ESP_LOGE(TAG, "Flash 擦除超时");
    return false;
}

bool Axs5106lUpgrade::WriteFlash(const uint8_t* data, size_t len) {
    uint8_t cmd[3] = {0x6F, 0xD4, 0x00};

    // 设置写入参数
    WriteRegister(0x90, cmd, 3);

    cmd[1] = 0xD5;
    WriteRegister(0x90, cmd, 3);

    cmd[1] = 0xD2;
    cmd[2] = (len - 1) & 0xFF;
    WriteRegister(0x90, cmd, 3);

    cmd[1] = 0xD3;
    cmd[2] = ((len - 1) >> 8) & 0xFF;
    WriteRegister(0x90, cmd, 3);

    cmd[1] = 0xD6;
    cmd[2] = 0xF4;
    WriteRegister(0x90, cmd, 3);

    // 逐字节写入（慢速模式，兼容性更好）
    cmd[1] = 0xD7;
    for (size_t i = 0; i < len; i++) {
        cmd[2] = data[i];
        WriteRegister(0x90, cmd, 3);

        // 每 1KB 打印进度
        if ((i + 1) % 1024 == 0) {
            ESP_LOGI(TAG, "写入进度: %zu / %zu", i + 1, len);
        }
    }

    cmd[1] = 0xD6;
    cmd[2] = 0x00;
    WriteRegister(0x90, cmd, 3);

    ESP_LOGI(TAG, "固件写入完成，共 %zu 字节", len);
    return true;
}

bool Axs5106lUpgrade::VerifyFlash(const uint8_t* data, size_t len) {
    // 使用逐字节读取验证
    uint8_t cmd[3] = {0x6F, 0xD4, 0x00};
    uint8_t write_buf[3] = {0x80, 0x7F, 0xD7};
    uint8_t read_buf[1] = {0x00};

    WriteRegister(0x90, cmd, 3);

    cmd[1] = 0xD5;
    WriteRegister(0x90, cmd, 3);

    cmd[1] = 0xD2;
    cmd[2] = (len - 1) & 0xFF;
    WriteRegister(0x90, cmd, 3);

    cmd[1] = 0xD3;
    cmd[2] = ((len - 1) >> 8) & 0xFF;
    WriteRegister(0x90, cmd, 3);

    cmd[1] = 0xD6;
    cmd[2] = 0xF1;
    WriteRegister(0x90, cmd, 3);

    for (size_t i = 0; i < len; i++) {
        if (!ReadRegisters(write_buf, 3, read_buf, 1)) {
            ESP_LOGE(TAG, "验证读取失败，位置: %zu", i);
            goto verify_exit;
        }

        if (read_buf[0] != data[i]) {
            ESP_LOGE(TAG, "验证失败，位置: %zu, 期望: 0x%02X, 实际: 0x%02X",
                     i, data[i], read_buf[0]);
            goto verify_exit;
        }

        // 每 1KB 打印进度
        if ((i + 1) % 1024 == 0) {
            ESP_LOGI(TAG, "验证进度: %zu / %zu", i + 1, len);
        }
    }

    cmd[1] = 0xD6;
    cmd[2] = 0x00;
    WriteRegister(0x90, cmd, 3);

    ESP_LOGI(TAG, "固件验证成功");
    return true;

verify_exit:
    cmd[1] = 0xD6;
    cmd[2] = 0x00;
    WriteRegister(0x90, cmd, 3);
    return false;
}

bool Axs5106lUpgrade::DoUpgrade() {
    ESP_LOGI(TAG, "开始升级，固件大小: %zu 字节", sizeof(kFirmwareData));

    // 1. 进入调试模式
    if (!EnterDebugMode()) {
        return false;
    }

    // 2. 解锁 Flash
    if (!UnlockFlash()) {
        ESP_LOGE(TAG, "解锁 Flash 失败");
        return false;
    }

    // 3. 擦除 Flash
    if (!EraseFlash()) {
        return false;
    }

    // 4. 写入固件
    if (!WriteFlash(kFirmwareData, sizeof(kFirmwareData))) {
        return false;
    }

    // 5. 验证固件（可选，耗时较长）
    // 注意：验证是可选的，如果时间紧迫可以跳过
    // if (!VerifyFlash(kFirmwareData, sizeof(kFirmwareData))) {
    //     return false;
    // }

    return true;
}

Axs5106lUpgradeResult Axs5106lUpgrade::CheckAndUpgrade() {
    // 1. 获取芯片当前固件版本
    uint16_t chip_version = 0;
    if (!GetChipFirmwareVersion(chip_version)) {
        ESP_LOGW(TAG, "无法读取芯片固件版本，尝试升级");
        // 继续尝试升级，可能是全新芯片
    } else {
        ESP_LOGI(TAG, "芯片固件版本: V%u", chip_version);
    }

    // 2. 获取内嵌固件版本
    uint16_t embedded_version = GetEmbeddedFirmwareVersion();
    ESP_LOGI(TAG, "内嵌固件版本: V%u", embedded_version);

    // 3. 比较版本
    if (chip_version == embedded_version && chip_version != 0) {
        ESP_LOGI(TAG, "固件版本相同，无需升级");
        return Axs5106lUpgradeResult::NotNeeded;
    }

    // 4. 执行升级
    ESP_LOGI(TAG, "开始固件升级: V%u -> V%u", chip_version, embedded_version);

    for (int retry = 0; retry < UPGRADE_RETRY_TIMES; retry++) {
        if (DoUpgrade()) {
            // 5. 升级成功，复位芯片
            SoftwareReset();
            DelayMs(50);

            // 6. 验证新版本
            uint16_t new_version = 0;
            if (GetChipFirmwareVersion(new_version)) {
                if (new_version == embedded_version) {
                    ESP_LOGI(TAG, "固件升级成功，新版本: V%u", new_version);
                    return Axs5106lUpgradeResult::Success;
                } else {
                    ESP_LOGW(TAG, "版本不匹配: 期望 V%u, 实际 V%u", embedded_version, new_version);
                }
            }

            ESP_LOGW(TAG, "升级后版本验证失败，重试...");
        }
    }

    ESP_LOGE(TAG, "固件升级失败");
    return Axs5106lUpgradeResult::Failed;
}