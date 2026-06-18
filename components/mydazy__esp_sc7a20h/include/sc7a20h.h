/*
 * SPDX-FileCopyrightText: 2026 mydazy
 * SPDX-License-Identifier: Apache-2.0
 *
 * SC7A20H 三轴加速度计驱动 — v5.0.0
 *
 * 三 API · 两场景（命名 / 寄存器 / 调试方法详见 docs/p30-sc7a20h-flows.html）：
 *   sc7a20h_init    — 基础初始化 + 拿起灵敏度
 *   sc7a20h_wakeup  — ① 深睡前 arm EXT1 唤醒
 *   sc7a20h_shake   — ② 摇一摇检测 + 回调
 *
 * 所有灵敏度参数在调用时直接传入 · 不暴露 struct · 不支持运行时切换。
 */

#pragma once

#include <stdint.h>
#include <driver/gpio.h>
#include <esp_err.h>
#include "i2c_bus_worker.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sc7a20h_dev_t *sc7a20h_handle_t;

/* 回调签名（motion_task 上下文 · 非 ISR · 不可阻塞 · 必须 Schedule 切主线程） */
typedef void (*sc7a20h_shake_cb_t)(void *ctx);

/**
 * 基础初始化 + 配置拿起唤醒灵敏度
 *
 * 做了：worker 注册 · WHO_AM_I 校验 · 锁定 ±4g HR / ODR 100Hz / INT1 active LOW
 *       LIR=0 · AOI1 → INT1 · 写入拿起阈值 + 持续时间
 *
 * 示例：sc7a20h_init(i2c_worker_, 320 / mg /, 100 / ms /);
 *
 * @param worker             已创建的 i2c_bus_worker 句柄
 * @param pickup_thresh_mg   拿起阈值 mg · 步长 32 · 三板项目值 320
 * @param pickup_duration_ms 拿起持续 ms · 步长 10 · 三板项目值 100
 * @return  handle / NULL（失败自动清理）
 */
sc7a20h_handle_t sc7a20h_init(i2c_worker_handle_t worker,
                              uint16_t pickup_thresh_mg,
                              uint16_t pickup_duration_ms);

/**
 * ① 拿起唤醒 — 深睡前 arm · 兜底清 INT1 latch + enable_ext1 ANY_LOW + RTC pullup
 *
 * 示例：sc7a20h_wakeup(handle, GPIO_NUM_3);
 *
 */
esp_err_t sc7a20h_wakeup(sc7a20h_handle_t h, gpio_num_t int1_gpio);

/**
 * ② 摇一摇检测 · 首次调用启动共享 motion_task（栈 2560 · P1 · Core 1）
 *
 * 示例：sc7a20h_shake(handle, 1500, 600, 3, 1500, &OnShake, this);
 *
 * @param deviation_mg   偏离 1g 阈值 mg · 项目默认 1500（儿童力气小 → 1200）
 * @param window_ms      滑窗时长 ms · 须 100 整数倍 · 默认 600
 * @param target_frames  窗内强动帧触发阈值 · 默认 3
 * @param cooldown_ms    触发后冷却 ms · 默认 1500
 * @param cb             触发回调（不可为 NULL）
 * @return ESP_OK / INVALID_ARG / INVALID_STATE(重复启动) / NO_MEM
 */
esp_err_t sc7a20h_shake(sc7a20h_handle_t h,
                        uint16_t deviation_mg,
                        uint16_t window_ms,
                        uint8_t  target_frames,
                        uint16_t cooldown_ms,
                        sc7a20h_shake_cb_t cb,
                        void *ctx);

#ifdef __cplusplus
}
#endif
