/*
 * SPDX-FileCopyrightText: 2026 MyDazy
 * SPDX-License-Identifier: Apache-2.0
 *
 * MyDazy 版 esp_codec_dev I2C 控制接口
 * ====================================
 * 上游 espressif/esp_codec_dev 的 `audio_codec_new_i2c_ctrl()` 内部直接
 * 调用 `i2c_master_transmit/receive`，与本项目的自有 I2C driver
 * （触摸/sensor/NFC）共享 I2C 总线，4G RF 共线场景下会发生协议层污染。
 *
 * 本组件提供 `mydazy_codec_new_i2c_ctrl()`，行为与上游 1:1 等价，
 * 但内部把所有 I2C 访问转发到 i2c_bus_worker 单线程串行执行。
 *
 * BoxAudioCodec 等使用方只需把 `audio_codec_new_i2c_ctrl()` 替换为
 * `mydazy_codec_new_i2c_ctrl()`，所有上游 codec driver（ES8311/ES7210/...
 * 源码不动）的 I2C 操作自动汇入 worker。
 */

#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "audio_codec_ctrl_if.h"        /* 来自 espressif/esp_codec_dev */
#include "i2c_bus_worker.h"

#ifdef __cplusplus
extern "C" {
#endif

/** MyDazy 版 codec I2C 配置 */
typedef struct {
    i2c_worker_handle_t worker;     /**< 已创建的 i2c_bus_worker */
    uint8_t             addr;       /**< 8-bit 地址（与上游 audio_codec_i2c_cfg_t.addr 一致：高 7 bit + R/W bit=0） */
    uint32_t            scl_speed_hz; /**< 默认 100000 与上游一致 */
} mydazy_codec_i2c_cfg_t;

/**
 * 创建 MyDazy 版 codec I2C ctrl_if 实例
 *
 * 与 espressif/esp_codec_dev 的 `audio_codec_new_i2c_ctrl()` 行为完全一致，
 * 区别：内部 I2C 访问通过 i2c_bus_worker 串行化。
 *
 * @param cfg 配置（必须包含已 i2c_worker_create 的 worker）
 * @return ctrl_if / NULL on error
 */
const audio_codec_ctrl_if_t *mydazy_codec_new_i2c_ctrl(const mydazy_codec_i2c_cfg_t *cfg);

#ifdef __cplusplus
}
#endif
