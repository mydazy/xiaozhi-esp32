/*
 * SPDX-FileCopyrightText: 2026 mydazy
 * SPDX-License-Identifier: Apache-2.0
 *
 * WS1850S NFC 极简驱动 — v2.0.0
 *
 * 设计：纯 C API + 静态单例，零 handle 传递；底层协议层保留（nfc.c / ws1850_iic.c）。
 */

#include "esp_nfc_ws1850s.h"

extern "C" {
#include "nfc.h"
#include "ws1850_iic.h"
}

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <atomic>
#include <cstdio>
#include <cstring>

static const char *TAG = "nfc";

/* ========================================================================
 * 内部静态状态（项目内 P31 仅一个 NFC 实例）
 * ====================================================================== */
namespace {

constexpr int64_t kDedupUs        = 1500000;  // 1.5 s 去重（儿童快刷友好）
constexpr int     kIdleResetCount = 3;        // 离场 3 帧 → 重置 last_uid

struct NfcState {
    nfc_card_cb_t          cb;
    void                  *ctx;
    uint32_t               interval_ms;
    TaskHandle_t           task;
    std::atomic<bool>      running{false};
    bool                   initialized;
};

NfcState s_nfc{};

}  // namespace

/* ========================================================================
 * Helper
 * ====================================================================== */

size_t mydazy_nfc_uid_to_str(const nfc_uid_t *uid, char *buf, size_t buf_size)
{
    if (!uid || !buf || buf_size < 1) return 0;
    int pos = 0;
    for (uint8_t i = 0; i < uid->length && i < 10 && pos < (int)buf_size - 3; i++) {
        pos += snprintf(buf + pos, buf_size - pos, "%s%02X",
                        i > 0 ? ":" : "", uid->bytes[i]);
    }
    return (size_t)pos;
}

/* ========================================================================
 * ISO14443A 检测 + 防冲突 + UID 提取（剥级联标签）
 * ====================================================================== */
static nfc_card_type_t detect_one(nfc_uid_t *out)
{
    unsigned char ct[2];
    if (PcdRequest(PICC_REQALL, ct) != MI_OK) return NFC_CARD_NONE;

    /* 16 字节防 cascade 3 写 ida[7..11] 越界（v1 P0 修复） */
    unsigned char ida[16] = {};
    unsigned char sak = 0;

    if (pcd_cascaded_anticoll(PICC_ANTICOLL1, 0, &ida[0]) != MI_OK) return NFC_CARD_NONE;
    out->length = 4;
    if (pcd_cascaded_select(PICC_ANTICOLL1, &ida[0], &sak) != MI_OK) return NFC_CARD_NONE;

    if (sak & 0x04) {  /* UID 不完整，进二级 cascade */
        if (pcd_cascaded_anticoll(PICC_ANTICOLL2, 0, &ida[4]) != MI_OK) return NFC_CARD_NONE;
        out->length = 7;
        if (pcd_cascaded_select(PICC_ANTICOLL2, &ida[4], &sak) != MI_OK) return NFC_CARD_NONE;
    }
    if (sak & 0x04) {  /* 三级 cascade（10B UID） */
        if (pcd_cascaded_anticoll(PICC_ANTICOLL3, 0, &ida[7]) != MI_OK) return NFC_CARD_NONE;
        out->length = 10;
        if (pcd_cascaded_select(PICC_ANTICOLL3, &ida[7], &sak) != MI_OK) return NFC_CARD_NONE;
    }

    /* ISO14443-3 cascade UID 剥级联标签（cascade1/2 跳 0x88，cascade3 直读） */
    if (out->length == 4) {
        memcpy(out->bytes, &ida[0], 4);
    } else if (out->length == 7) {
        memcpy(out->bytes,     &ida[1], 3);
        memcpy(out->bytes + 3, &ida[4], 4);
    } else {  /* 10B */
        memcpy(out->bytes,     &ida[1], 3);
        memcpy(out->bytes + 3, &ida[5], 3);
        memcpy(out->bytes + 6, &ida[7], 4);
    }

    /* SAK → 卡类型 */
    if (sak == 0x08)              return NFC_CARD_MIFARE_1K;
    if (sak == 0x18)              return NFC_CARD_MIFARE_4K;
    if (sak == 0x00)              return NFC_CARD_ULTRALIGHT;
    if (sak == 0x10 || sak == 0x11) return NFC_CARD_MIFARE_PLUS;
    if (sak & 0x20)               return NFC_CARD_ISO14443A4;
    return NFC_CARD_UNKNOWN;
}

/* ========================================================================
 * 后台检测任务
 * ====================================================================== */
static void detect_task_fn(void *)
{
    nfc_uid_t last = {};
    int64_t   last_us = 0;
    int       idle_count = 0;

    ESP_LOGI(TAG, "detection task started (Core %d)", xPortGetCoreID());

    while (s_nfc.running.load(std::memory_order_acquire)) {
        nfc_uid_t uid = {};
        nfc_card_type_t type = detect_one(&uid);

        if (type != NFC_CARD_NONE) {
            idle_count = 0;
            int64_t now = esp_timer_get_time();

            bool same_uid = (uid.length == last.length) &&
                            (memcmp(uid.bytes, last.bytes, uid.length) == 0);
            if (!same_uid || (now - last_us) > kDedupUs) {
                if (s_nfc.cb) s_nfc.cb(type, &uid, s_nfc.ctx);
                last    = uid;
                last_us = now;
            }
            vTaskDelay(pdMS_TO_TICKS(s_nfc.interval_ms));
        } else {
            /* 离场重置：3 帧无卡 → 清 last_uid，让"拿走再刷"算新事件 */
            if (++idle_count == kIdleResetCount) {
                last = nfc_uid_t{};
            }
            /* 无卡时逐渐降频：interval → ×3 → ×6 */
            uint32_t delay_ms = (idle_count < 10)  ? s_nfc.interval_ms
                              : (idle_count < 30)  ? s_nfc.interval_ms * 3
                                                   : s_nfc.interval_ms * 6;
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
        }
    }

    ESP_LOGI(TAG, "detection task exiting");
    s_nfc.task = nullptr;
    vTaskDelete(nullptr);
}

/* ========================================================================
 * Public API
 * ====================================================================== */

esp_err_t mydazy_nfc_init(i2c_master_bus_handle_t bus,
                          nfc_card_cb_t on_card,
                          void *ctx,
                          uint32_t poll_interval_ms)
{
    if (!bus || !on_card)                      return ESP_ERR_INVALID_ARG;
    if (s_nfc.running.load())                  return ESP_ERR_INVALID_STATE;

    /* 底层 IIC + chip 初始化 */
    ws_iic_set_pins(GPIO_NUM_NC, GPIO_NUM_NC);
    ws_iic_init(bus);
    PcdReset();
    vTaskDelay(pdMS_TO_TICKS(50));

    uint8_t ver = ReadRawRC(VersionReg);
    if (ver == 0x00 || ver == 0xFF) {
        ESP_LOGE(TAG, "WS1850S 通信失败 (ver=0x%02X)", ver);
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "WS1850S ready (ver=0x%02X)", ver);

    PcdConfig('A');
    PcdAntennaOn();

    s_nfc.cb           = on_card;
    s_nfc.ctx          = ctx;
    s_nfc.interval_ms  = poll_interval_ms ? poll_interval_ms : 300;
    s_nfc.initialized  = true;
    s_nfc.running.store(true, std::memory_order_release);

    /* Pin Core 0 P1 + 4KB 内部 RAM 栈
       注释：Core 0 选择基于 NVS/OTA 期间 cache 禁用安全 + ISO14443-4 RATS
       调用栈在 4KB 内可控（量产实测峰值 ~2.5KB） */
    BaseType_t ok = xTaskCreatePinnedToCore(
        detect_task_fn, "nfc_detect", 4096, nullptr, 1, &s_nfc.task, 0);
    if (ok != pdPASS) {
        s_nfc.running.store(false);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t mydazy_nfc_pause(void)
{
    if (!s_nfc.running.load()) return ESP_OK;

    s_nfc.running.store(false, std::memory_order_release);

    /* 等 task 自删（带 1s 超时，防 UB；TaskHandle_t 不再访问，由 task 内部清空） */
    for (int i = 0; i < 20 && s_nfc.task; i++) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    PcdAntennaOff();   /* 关天线，整机静态电流 -50 mA */
    ESP_LOGI(TAG, "paused (antenna off)");
    return ESP_OK;
}

esp_err_t mydazy_nfc_resume(void)
{
    if (s_nfc.running.load())   return ESP_OK;
    if (!s_nfc.initialized)     return ESP_ERR_INVALID_STATE;

    PcdAntennaOn();
    s_nfc.running.store(true, std::memory_order_release);

    BaseType_t ok = xTaskCreatePinnedToCore(
        detect_task_fn, "nfc_detect", 4096, nullptr, 1, &s_nfc.task, 0);
    if (ok != pdPASS) {
        s_nfc.running.store(false);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "resumed");
    return ESP_OK;
}
