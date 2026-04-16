/**
 * @file esp_nfc_ws1850s.h
 * @brief WS1850S NFC 读卡器驱动（MFRC522 兼容）
 *
 * 典型用法（后台自动检测）:
 * @code
 * NfcWs1850s nfc(i2c_bus);
 * nfc.Initialize();
 * nfc.SetCardCallback([](NfcCardType type, const NfcUid& uid) {
 *     ESP_LOGI("NFC", "Card: %s", uid.ToString().c_str());
 * });
 * nfc.StartDetection();
 * @endcode
 *
 * @note I2C 地址固定 0x28，与音频/触摸共享 I2C 总线
 */

#pragma once

#include <driver/i2c_master.h>
#include <esp_err.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>

/// NFC 卡类型（基于 SAK 字节判断）
enum class NfcCardType : uint8_t {
    kNone = 0,
    kMifareClassic1K,  // SAK=0x08, MIFARE Classic 1K (S50)
    kMifareClassic4K,  // SAK=0x18, MIFARE Classic 4K (S70)
    kUltralight,       // SAK=0x00, MIFARE Ultralight / NTAG
    kMifarePlus,       // SAK=0x10/0x11
    kIso14443A4,       // SAK bit5=1, ISO14443-4 compliant
    kUnknown,          // 其他 SAK 值
};

/// NFC UID（4/7/10 字节）
struct NfcUid {
    uint8_t bytes[10] = {};
    uint8_t length = 0;

    std::string ToString() const;

    bool operator==(const NfcUid& o) const {
        return length == o.length && memcmp(bytes, o.bytes, length) == 0;
    }
    bool operator!=(const NfcUid& o) const { return !(*this == o); }
};

/// 卡片检测回调
using NfcCardCallback = std::function<void(NfcCardType type, const NfcUid& uid)>;

/**
 * @brief WS1850S NFC 驱动
 *
 * 提供三种使用方式:
 * 1. SetCardCallback + StartDetection — 后台自动检测（推荐）
 * 2. DetectOnce — 手动单次检测
 * 3. 底层读写通过 C API 直接调用（高级用途）
 */
class NfcWs1850s {
public:
    explicit NfcWs1850s(i2c_master_bus_handle_t i2c_bus);
    ~NfcWs1850s();

    NfcWs1850s(const NfcWs1850s&) = delete;
    NfcWs1850s& operator=(const NfcWs1850s&) = delete;

    /// 初始化芯片（软复位 + 配置 + 天线开启）
    esp_err_t Initialize();

    /// 芯片版本（0x18=WS1850S-T, 0x15=WS1850S-S）
    uint8_t GetChipVersion() const;

    /// 设置卡片检测回调（在检测任务上下文中调用）
    void SetCardCallback(NfcCardCallback callback);

    /// 启动后台检测（PSRAM 栈，P1 优先级，Core0）
    esp_err_t StartDetection(uint32_t interval_ms = 300);

    /// 停止后台检测
    void StopDetection();

    /// 手动单次检测（阻塞，~10ms）
    NfcCardType DetectOnce(NfcUid* uid = nullptr);

private:
    i2c_master_bus_handle_t i2c_bus_;
    bool initialized_ = false;
    NfcCardCallback card_callback_;

    TaskHandle_t detect_task_ = nullptr;
    std::atomic<bool> detect_running_{false};

    /// ISO14443A 防冲突 + 选卡，返回卡类型和 UID
    NfcCardType SelectTypeA(NfcUid& uid);

    static void DetectTaskFunc(void* arg);
};
