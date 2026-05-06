/*
 * SPDX-FileCopyrightText: 2026 mydazy
 * SPDX-License-Identifier: Apache-2.0
 *
 * SC7A20H 三轴加速度计驱动 — v4.0.0 极简版
 *
 * 设计哲学：判断越少问题越少，保留高频刚需，删除运行时切换。
 *
 * 仅 3 个 API 覆盖项目所有用例：
 *   1) sc7a20h_init        — 一行初始化（拿起阈值/时长 + worker 注入）
 *   2) sc7a20h_read_mg     — 读三轴加速度（mg），摇一摇/晃停共用
 *   3) sc7a20h_arm_wakeup  — 进深睡前一调（清 latch + 注册 EXT1）
 *
 * 项目级硬编码（编译期常量化，零运行时分支）：
 *   - I2C 地址  : 0x19
 *   - I2C 速率  : 400 kHz
 *   - 量程      : ±4g（HR 12-bit, 2 mg/LSB）
 *   - ODR       : 100 Hz
 *   - 中断路由  : INT1 + AOI1 + High Event OR (XYZ)
 *   - INT 极性  : active LOW (idle HIGH)
 *
 * 与项目高频场景的对应：
 *   - 拿起唤醒：sc7a20h_init() + sc7a20h_arm_wakeup() 配对，深睡 → INT1 LOW → wake
 *   - 摇一摇识别：100 ms 周期 sc7a20h_read_mg() + 模长平方比较
 *   - 闹钟摇停：与摇一摇共用 read_mg
 *
 * 工程细节：
 *   - 所有 I2C 走 i2c_bus_worker（防 4G RF 共线污染）
 *   - init 6+寄存器序列用 lock_session 包裹（防音频/触摸打断）
 *   - 不支持运行时切量程/ODR/power_down/del（程序生命周期内不释放）
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

/**
 * 一行初始化：注册到 worker + verify WHO_AM_I + 配置 motion detection
 *
 * @param worker             已创建的 i2c_bus_worker handle
 * @param pickup_thresh_mg   拿起唤醒阈值（mg）。项目推荐 768
 *                            （步长 32 mg/LSB @ ±4g，0x18 = 768 mg）
 * @param pickup_duration_ms 拿起持续时长（ms）。项目推荐 200
 *                            （步长 10 ms @ 100 Hz ODR，0x14 = 200 ms）
 *
 * @return  handle / NULL on error（自动清理）
 */
sc7a20h_handle_t sc7a20h_init(i2c_worker_handle_t worker,
                              uint16_t pickup_thresh_mg,
                              uint16_t pickup_duration_ms);

/**
 * 读三轴加速度（mg，int16 整数，无浮点）
 *
 * 输出范围：±4096 mg（量程 ±4g，量产实测 ±3500 内）
 * 调用周期：摇一摇 100 ms / 晃停 50 ms — CPU < 1%
 * 单次开销：1 次 I2C burst read（6 byte）+ 6 次移位乘法 ≈ 30 µs
 */
esp_err_t sc7a20h_read_mg(sc7a20h_handle_t h, int16_t *x, int16_t *y, int16_t *z);

/**
 * 进深睡前一调 — 清 INT1 latch + 注册 EXT1 wakeup + RTC GPIO 上拉
 *
 * @param int1_gpio  SC7A20H INT1 信号接的 RTC-capable GPIO
 *
 * 调用顺序约定：
 *   sc7a20h_arm_wakeup(handle, GPIO_NUM_3);
 *   esp_deep_sleep_start();   // 由调用方触发
 */
esp_err_t sc7a20h_arm_wakeup(sc7a20h_handle_t h, gpio_num_t int1_gpio);

#ifdef __cplusplus
}
#endif
