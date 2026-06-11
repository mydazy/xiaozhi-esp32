/*
 * SPDX-FileCopyrightText: 2026 mydazy
 * SPDX-License-Identifier: Apache-2.0
 *
 * AXS5106L 电容触摸驱动 — v5.0.0
 *
 * 四 API 覆盖三板（命名规范 / 参数细节 / 调试方法详见 docs/p30-touch-flows.html）：
 * 项目硬编码（编译期常量化）：
 *   - I²C 地址 0x63 / 速率 400 kHz
 *   - swap_xy / mirror_x / mirror_y 全 false（V2907 firmware 已 rotation）
 *   - LVGL polling 30ms · INT 边沿计数 + storm 检测
 *
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

/* 编译期开关：1=显示触摸轨迹红点 + raw 坐标日志（调试期开启）· 0=量产关闭 */
#ifndef AXS5106L_TOUCH_DEBUG_OVERLAY
#define AXS5106L_TOUCH_DEBUG_OVERLAY 0
#endif

/* ========== Public types ========== */

typedef struct axs5106l_touch_t *axs5106l_touch_handle_t;

/**
 * RF 抗扰模式 · 按板级 i2c 干扰程度切档
 *   NORMAL：WiFi / 无 4G 干扰 · storm 20/s · debounce 5ms · guard 100ms
 *   STRICT：4G 共线 · storm 12/s · debounce 8ms · guard 200ms
 * 板级 i2c_master_bus_config_t::glitch_ignore_cnt 仍需板级分别配置（4G=15 / 其他=7）。
 */
typedef enum {
    AXS5106L_RF_NORMAL = 0,
    AXS5106L_RF_STRICT = 1,
} axs5106l_rf_mode_t;

/* 5 类事件回调 · 全部在 LVGL task 上下文调用（非 ISR · 可执行轻量同步动作） */
typedef void (*axs5106l_wake_cb_t)       (void *ctx);
typedef void (*axs5106l_click_cb_t)      (int16_t x, int16_t y, void *ctx);
typedef void (*axs5106l_swipe_cb_t)      (int16_t dx, int16_t dy, void *ctx);
typedef void (*axs5106l_long_press_cb_t) (bool is_release, int16_t x, int16_t y, void *ctx);

/// 配置体（init 时一次性传入，运行时不变）
typedef struct {
    /* 硬件 · 必填 */
    i2c_worker_handle_t        worker;
    gpio_num_t                 rst_gpio;     /* 与 LCD 共享 · 三板均 GPIO_4 */
    gpio_num_t                 int_gpio;     /* RTC-capable · 三板均 GPIO_5 */
    uint16_t                   width;        /* 三板 284 */
    uint16_t                   height;       /* 三板 240 */

    /* RF 抗扰 · 4G 共线传 RF_STRICT · 其他传 RF_NORMAL（或 0 等同 NORMAL）*/
    axs5106l_rf_mode_t         rf_mode;

    /* 回调 · 5 个独立 cb · 不需要的字段传 NULL */
    void                      *cb_ctx;
    axs5106l_wake_cb_t         on_wake;          /* 息屏首次触摸（唤醒屏幕）*/
    axs5106l_click_cb_t        on_click;         /* 单击（含坐标）*/
    axs5106l_click_cb_t        on_double_click;  /* 双击 */
    axs5106l_swipe_cb_t        on_swipe;         /* 滑动（dx/dy 给方向 · |dx|>|dy| 横滑）*/
    axs5106l_long_press_cb_t   on_long_press;    /* 长按（is_release: false=按下 · true=松开）*/
} axs5106l_touch_config_t;

/**
 * Phase 1 — LCD 启动**前**调用：
 *   配 RST + I²C device + firmware 升级 + chip_id 校验 + 应用 rf_mode
 *
 * @return ESP_OK / ESP_ERR_INVALID_ARG / ESP_ERR_NO_MEM / ESP_ERR_NOT_FOUND
 */
esp_err_t axs5106l_touch_init(const axs5106l_touch_config_t *cfg,
                              axs5106l_touch_handle_t *out);

/**
 * Phase 2 — LVGL 启动**后**调用：
 *   配 INT GPIO + 安装 IRAM ISR（仅计数）+ lv_indev_create + 30ms polling
 *
 * @return ESP_OK / ESP_ERR_INVALID_ARG
 */
esp_err_t axs5106l_touch_attach_lvgl(axs5106l_touch_handle_t h);


/// 深睡前调：关 INT ISR + 写 sleep 寄存器（0x19=0x03）· 必须先于 AUDIO_PWR_EN=0
/* 吞掉当前这次按压（直到抬手都不投递给 LVGL/手势）。
 * 用途：省电降亮时的"首触只点亮屏幕"——唤醒首帧坐标易被 RF 抖动污染，
 * 投递会误触按钮。须在 on_wake 回调内同步调用。*/
void axs5106l_touch_swallow_current_press(axs5106l_touch_handle_t h);

esp_err_t axs5106l_touch_sleep(axs5106l_touch_handle_t h);

/// 浅睡唤醒后调：软复位 + 重开 ISR + 重置 storm 检测（当前三板未用）
esp_err_t axs5106l_touch_resume(axs5106l_touch_handle_t h);

#ifdef __cplusplus
}
#endif
