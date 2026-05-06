/*
 * SPDX-FileCopyrightText: 2026 mydazy
 * SPDX-License-Identifier: Apache-2.0
 *
 * WS1850S NFC 驱动 — v2.0.0 极简版（C API）
 *
 * 设计哲学：判断越少问题越少，保留高频刚需，删除运行时切换。
 *
 * 仅 3 个 API 覆盖项目所有用例：
 *   1) mydazy_nfc_init     — 一行初始化（注册 + chip 初始化 + 启动后台 detection）
 *   2) mydazy_nfc_pause    — 进深睡前停 task + 关天线（电流 50mA→0）
 *   3) mydazy_nfc_resume   — 唤醒后重开天线 + 重启 task
 *
 * 项目硬编码（编译期常量化）：
 *   - I2C 地址 0x28（WS1850S 默认）
 *   - I2C 速率 100 kHz（默认）
 *   - 检测周期默认 300 ms
 *   - 去重时间 1.5 s（儿童快刷友好）
 *   - 离场重置 3 帧（拿走再刷算新事件）
 *
 * 高频使用场景：
 *   - 教育卡识别：UID 上报，服务端取动物/绘本内容
 *   - 关卡/积分卡：UID 业务回调
 *   - 家长解锁卡：UID 白名单进家长模式
 *
 * 单例语义：项目仅 P31 用 NFC，板内 1 个实例 → 内部静态状态，零 handle 传递。
 *
 * 删除的旧 API（v1.x → v2.0）：
 *   - NfcWs1850s C++ class（含 ctor/dtor/Initialize 等 8 方法）→ 全部 C 函数
 *   - SetCardCallback / StartDetection / StopDetection / DetectOnce → 合并到 init/pause/resume
 *   - GetChipVersion → 启动日志已打印，不再单独 API
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <driver/i2c_master.h>
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
 * Public types
 * ====================================================================== */

/// NFC 卡类型（基于 SAK 字节判断）
typedef enum {
    NFC_CARD_NONE         = 0,
    NFC_CARD_MIFARE_1K,         ///< SAK=0x08  Mifare Classic 1K (S50)
    NFC_CARD_MIFARE_4K,         ///< SAK=0x18  Mifare Classic 4K (S70)
    NFC_CARD_ULTRALIGHT,        ///< SAK=0x00  Ultralight / NTAG（教育卡常用）
    NFC_CARD_MIFARE_PLUS,       ///< SAK=0x10/0x11
    NFC_CARD_ISO14443A4,        ///< SAK bit5=1（CPU 卡 / 银行卡 — 仅读 UID 不发指令）
    NFC_CARD_UNKNOWN,
} nfc_card_type_t;

/// NFC UID（4/7/10 字节，按 ISO14443-3 剥级联标签后的纯 UID）
typedef struct {
    uint8_t bytes[10];
    uint8_t length;             ///< 实际 UID 字节数（4/7/10）
} nfc_uid_t;

/// 卡片检测回调（在 detection task 上下文调用）
/// @param ctx 调用方注册时的 context 指针
typedef void (*nfc_card_cb_t)(nfc_card_type_t type, const nfc_uid_t *uid, void *ctx);

/* ========================================================================
 * 3 个 API
 * ====================================================================== */

/**
 * 一行初始化：底层 IIC + chip reset + verify version + 启动后台 detection task
 *
 * @param bus              I2C master bus handle
 * @param on_card          刷卡回调（必须非 NULL）
 * @param ctx              传给回调的 user context
 * @param poll_interval_ms 检测周期，0 = 默认 300 ms
 *
 * @return ESP_OK / ESP_ERR_INVALID_ARG / ESP_ERR_INVALID_STATE / ESP_ERR_NO_MEM /
 *         ESP_ERR_NOT_FOUND（chip 通信失败）
 */
esp_err_t mydazy_nfc_init(i2c_master_bus_handle_t bus,
                          nfc_card_cb_t on_card,
                          void *ctx,
                          uint32_t poll_interval_ms);

/**
 * 进深睡前停 detection task + 关天线
 * 天线关闭后整机降低 50 mA 静态电流。
 *
 * 同步阻塞最多 1 s 等 task 退出。
 */
esp_err_t mydazy_nfc_pause(void);

/**
 * 唤醒后重开天线 + 重启 detection task
 * 必须在 mydazy_nfc_init 已成功调用过的前提下使用。
 */
esp_err_t mydazy_nfc_resume(void);

/* ========================================================================
 * Helper（可选）
 * ====================================================================== */

/**
 * UID 转 16 进制冒号分隔字符串（如 "04:A1:23:..."）
 * @return 写入字节数（不含 \0）
 */
size_t mydazy_nfc_uid_to_str(const nfc_uid_t *uid, char *buf, size_t buf_size);

#ifdef __cplusplus
}
#endif
