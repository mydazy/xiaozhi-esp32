/**
 * @file esp_nfc_ws1850s.cc
 * @brief WS1850S NFC C++ 驱动（桥接底层 C 寄存器操作）
 */

#include "esp_nfc_ws1850s.h"

extern "C" {
#include "nfc.h"
#include "ws1850_iic.h"
}

#include <esp_log.h>
#include <esp_timer.h>
#include <cstring>
#include <cstdio>

static const char* TAG = "NfcWs1850s";

// ============================================================
// NfcUid
// ============================================================

std::string NfcUid::ToString() const {
    char buf[32];
    int pos = 0;
    for (uint8_t i = 0; i < length && i < 10; i++) {
        pos += snprintf(buf + pos, sizeof(buf) - pos, "%s%02X", i > 0 ? ":" : "", bytes[i]);
    }
    return std::string(buf);
}

// ============================================================
// 构造 / 析构
// ============================================================

NfcWs1850s::NfcWs1850s(i2c_master_bus_handle_t i2c_bus)
    : i2c_bus_(i2c_bus) {}

NfcWs1850s::~NfcWs1850s() {
    StopDetection();
}

// ============================================================
// 初始化
// ============================================================

esp_err_t NfcWs1850s::Initialize() {
    // 底层 GPIO 设为 NC（P30 无独立 RST/IRQ 引脚）
    ws_iic_set_pins(GPIO_NUM_NC, GPIO_NUM_NC);
    ws_iic_init(i2c_bus_);

    PcdReset();
    vTaskDelay(pdMS_TO_TICKS(50));

    uint8_t ver = ReadRawRC(VersionReg);
    if (ver == 0x00 || ver == 0xFF) {
        ESP_LOGE(TAG, "芯片通信失败 (版本=0x%02X)", ver);
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "WS1850S 初始化成功 (版本=0x%02X)", ver);

    PcdConfig('A');
    PcdAntennaOn();

    initialized_ = true;
    return ESP_OK;
}

uint8_t NfcWs1850s::GetChipVersion() const {
    return ReadRawRC(VersionReg);
}

// ============================================================
// ISO14443A 防冲突 + 选卡（核心检测逻辑）
// ============================================================

NfcCardType NfcWs1850s::SelectTypeA(NfcUid& uid) {
    unsigned char ct[2];
    if (PcdRequest(PICC_REQALL, ct) != MI_OK) return NfcCardType::kNone;

    // 一级防冲突
    unsigned char ida[10] = {};
    unsigned char sak = 0;
    if (pcd_cascaded_anticoll(PICC_ANTICOLL1, 0, &ida[0]) != MI_OK) return NfcCardType::kNone;

    uid.length = 4;
    if (pcd_cascaded_select(PICC_ANTICOLL1, &ida[0], &sak) != MI_OK) return NfcCardType::kNone;

    // 二级防冲突（SAK bit2=1 表示 UID 不完整）
    if (sak & 0x04) {
        if (pcd_cascaded_anticoll(PICC_ANTICOLL2, 0, &ida[4]) != MI_OK) return NfcCardType::kNone;
        uid.length = 7;
        if (pcd_cascaded_select(PICC_ANTICOLL2, &ida[4], &sak) != MI_OK) return NfcCardType::kNone;
    }

    // 三级防冲突
    if (sak & 0x04) {
        if (pcd_cascaded_anticoll(PICC_ANTICOLL3, 0, &ida[7]) != MI_OK) return NfcCardType::kNone;
        uid.length = 10;
        if (pcd_cascaded_select(PICC_ANTICOLL3, &ida[7], &sak) != MI_OK) return NfcCardType::kNone;
    }

    // 提取 UID（跳过级联标签 0x88）
    if (uid.length == 4) {
        memcpy(uid.bytes, &ida[0], 4);
    } else if (uid.length == 7) {
        // 一级的前 3 字节（跳 ida[0]=0x88）+ 二级 4 字节
        memcpy(uid.bytes, &ida[1], 3);
        memcpy(uid.bytes + 3, &ida[4], 4);
    } else {
        memcpy(uid.bytes, ida, 10);
    }

    // SAK → 卡类型
    if (sak == 0x08) return NfcCardType::kMifareClassic1K;
    if (sak == 0x18) return NfcCardType::kMifareClassic4K;
    if (sak == 0x00) return NfcCardType::kUltralight;
    if (sak == 0x10 || sak == 0x11) return NfcCardType::kMifarePlus;
    if (sak & 0x20) return NfcCardType::kIso14443A4;
    return NfcCardType::kUnknown;
}

// ============================================================
// 检测接口
// ============================================================

NfcCardType NfcWs1850s::DetectOnce(NfcUid* uid) {
    if (!initialized_) return NfcCardType::kNone;

    NfcUid detected = {};
    NfcCardType type = SelectTypeA(detected);
    if (type != NfcCardType::kNone && uid) {
        *uid = detected;
    }
    return type;
}

void NfcWs1850s::SetCardCallback(NfcCardCallback callback) {
    card_callback_ = std::move(callback);
}

esp_err_t NfcWs1850s::StartDetection(uint32_t interval_ms) {
    if (detect_running_.load()) return ESP_ERR_INVALID_STATE;
    detect_running_.store(true);

    // PSRAM 栈，P1 优先级，Core0
    BaseType_t ret = xTaskCreatePinnedToCoreWithCaps(
        DetectTaskFunc, "nfc_detect", 4096, this, 1, &detect_task_, 0,
        MALLOC_CAP_SPIRAM);

    if (ret != pdPASS) {
        detect_running_.store(false);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void NfcWs1850s::StopDetection() {
    detect_running_.store(false);
    if (detect_task_) {
        for (int i = 0; i < 30 && eTaskGetState(detect_task_) != eDeleted; i++) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        detect_task_ = nullptr;
    }
}

// ============================================================
// 后台检测任务
// ============================================================

void NfcWs1850s::DetectTaskFunc(void* arg) {
    auto* self = static_cast<NfcWs1850s*>(arg);
    NfcUid last_uid = {};
    int64_t last_trigger_us = 0;
    constexpr int64_t kDedupUs = 5000000;  // 相同 UID 5 秒去重
    int idle_count = 0;

    while (self->detect_running_.load()) {
        NfcUid uid = {};
        NfcCardType type = self->DetectOnce(&uid);

        if (type != NfcCardType::kNone) {
            idle_count = 0;
            int64_t now = esp_timer_get_time();

            if (uid != last_uid || (now - last_trigger_us) > kDedupUs) {
                ESP_LOGI(TAG, "卡片: type=%d UID=%s", (int)type, uid.ToString().c_str());
                if (self->card_callback_) {
                    self->card_callback_(type, uid);
                }
                last_uid = uid;
                last_trigger_us = now;
            }
            vTaskDelay(pdMS_TO_TICKS(500));
        } else {
            idle_count++;
            // 无卡时逐渐降频: 500ms → 1s → 2s
            int delay = (idle_count < 10) ? 500 : (idle_count < 30) ? 1000 : 2000;
            vTaskDelay(pdMS_TO_TICKS(delay));
        }
    }

    ESP_LOGI(TAG, "检测任务退出");
    vTaskDelete(nullptr);
}
