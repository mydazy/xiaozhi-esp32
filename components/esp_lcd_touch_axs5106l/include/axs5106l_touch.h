/*
 * SPDX-FileCopyrightText: 2026 mydazy
 * SPDX-License-Identifier: Apache-2.0
 *
 * AXS5106L 电容触摸驱动 — v4.0.0 极简版（公开 C API）
 *
 * 设计哲学：判断越少问题越少，保留高频刚需。
 *
 * 仅 4 个 API 覆盖项目所有用例：
 *   1) axs5106l_touch_new          — Phase 1（LCD 启动前）：GPIO + 升级 + chip_id 校验
 *   2) axs5106l_touch_attach_lvgl  — Phase 2（LVGL 启动后）：注册 lv_indev_t
 *   3) axs5106l_touch_sleep        — 进深睡前关 INT ISR + 写 sleep 寄存器
 *   4) axs5106l_touch_resume       — 唤醒后软复位 + 重开 INT ISR
 *
 * 两阶段 init 的硬件原因：rst_gpio 与 LCD 共享 → 必须在 LCD 之前 reset；
 * attach_lvgl 必须在 LVGL 启动后才能调 lv_indev_create。
 *
 * 删除的旧 API（v3.0 vs v4.0）：
 *   - axs5106l_touch_del            → 量产生命周期内不释放，N/A
 *   - axs5106l_touch_set_wake_callback     → 移入 cfg.wake_cb
 *   - axs5106l_touch_set_gesture_callback  → 移入 cfg.gesture_cb
 *   - axs5106l_touch_get_lvgl_device       → 外部不需要
 *
 * 项目硬编码（编译期常量化）：
 *   - I2C 地址 0x63，速率 400 kHz
 *   - swap_xy=false, mirror_x=false, mirror_y=false（V2907 firmware 已内部 rotation）
 *   - LVGL polling 30ms，硬件 INT 边沿计数 + storm 检测
 *
 * 所有 I2C 走 i2c_bus_worker（防 4G RF 共线污染）。
 *
 * 高频使用场景：
 *   - 屏幕唤醒：手指触屏 → INT 下降沿 → wake_cb（亮屏 + 退出省电）
 *   - 单击/双击/长按/松开：gesture_cb（500ms 长按门槛 + 60ms 短触）
 *   - 滑动：gesture_cb 上/下/左/右（量产可丢弃）
 *   - 深睡：进 sleep + 关 INT ISR（防 RF 误唤）
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <driver/gpio.h>
#include <esp_err.h>
#include <esp_lvgl_port.h>
#include "i2c_bus_worker.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 编译期开关：调试期可开 1 显示触摸轨迹红点 + raw 坐标日志 */
#ifndef AXS5106L_TOUCH_DEBUG_OVERLAY
#define AXS5106L_TOUCH_DEBUG_OVERLAY 0
#endif

/* ========================================================================
 * Public types
 * ====================================================================== */

typedef struct axs5106l_touch_t *axs5106l_touch_handle_t;

/// Gesture identifiers reported by the recognizer.
typedef enum {
    AXS5106L_GESTURE_NONE = 0,
    AXS5106L_GESTURE_SINGLE_CLICK,
    AXS5106L_GESTURE_DOUBLE_CLICK,
    AXS5106L_GESTURE_LONG_PRESS,
    AXS5106L_GESTURE_LONG_PRESS_RELEASE,
    AXS5106L_GESTURE_SWIPE_UP,
    AXS5106L_GESTURE_SWIPE_DOWN,
    AXS5106L_GESTURE_SWIPE_LEFT,
    AXS5106L_GESTURE_SWIPE_RIGHT,
} axs5106l_gesture_t;

/// 屏唤醒回调（首次触摸）。在 LVGL task 上下文调用。
typedef void (*axs5106l_wake_cb_t)(void *user_ctx);

/// 手势回调（识别完成）。在 LVGL task 上下文调用。
typedef void (*axs5106l_gesture_cb_t)(axs5106l_gesture_t g,
                                      int16_t x, int16_t y,
                                      void *user_ctx);

/// 配置（init 时一次性传入，运行时不变）
typedef struct {
    i2c_worker_handle_t     worker;     ///< 已创建的 i2c_bus_worker
    gpio_num_t              rst_gpio;   ///< Reset GPIO（与 LCD 可共享）
    gpio_num_t              int_gpio;   ///< INT GPIO（active LOW）
    uint16_t                width;      ///< 屏幕逻辑宽（px）
    uint16_t                height;     ///< 屏幕逻辑高（px）
    axs5106l_wake_cb_t      wake_cb;    ///< 首次触摸回调（可 NULL）
    axs5106l_gesture_cb_t   gesture_cb; ///< 手势识别回调（可 NULL）
    void                   *cb_ctx;     ///< 两个回调共用的 user context
} axs5106l_touch_config_t;

/* ========================================================================
 * Lifecycle (Phase 1 / 2)
 * ====================================================================== */

/**
 * Phase 1 — LCD 启动**前**调用：
 *   - 配 RST GPIO + I2C device
 *   - 检查并升级固件
 *   - 验证 chip_id
 *
 * @return ESP_OK / ESP_ERR_INVALID_ARG / ESP_ERR_NO_MEM / ESP_ERR_NOT_FOUND
 */
esp_err_t axs5106l_touch_new(const axs5106l_touch_config_t *cfg,
                             axs5106l_touch_handle_t *out);

/**
 * Phase 2 — LVGL 启动**后**调用：
 *   - 配 INT GPIO + 安装 ISR（边沿计数器，不读 I2C）
 *   - 注册 lv_indev_t（30ms polling read 30ms LVGL）
 *
 * @return ESP_OK / ESP_ERR_INVALID_ARG
 */
esp_err_t axs5106l_touch_attach_lvgl(axs5106l_touch_handle_t h);

/* ========================================================================
 * Power
 * ====================================================================== */

/// 进深睡前调：关 INT ISR + 写 sleep 寄存器（0x19=0x03）
esp_err_t axs5106l_touch_sleep(axs5106l_touch_handle_t h);

/// 唤醒后调：软复位芯片 + 重开 INT ISR + 重置 storm 检测窗口
esp_err_t axs5106l_touch_resume(axs5106l_touch_handle_t h);

#ifdef __cplusplus
}
#endif
