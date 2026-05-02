/*
 * SPDX-FileCopyrightText: 2026 mydazy
 * SPDX-License-Identifier: Apache-2.0
 *
 * AXS5106L capacitive touch driver — public C API.
 *
 * Two-phase init pattern (required when the touch IC shares its reset line
 * with the LCD):
 *
 *   axs5106l_touch_new(&cfg, &tp);    // before LVGL: configures GPIO/I2C, runs FW upgrade
 *   ... initialise LCD + start LVGL ...
 *   axs5106l_touch_attach_lvgl(tp);   // after LVGL: registers as lv_indev_t
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <driver/i2c_master.h>
#include <driver/gpio.h>
#include <esp_err.h>
#include <esp_lvgl_port.h>     /* lv_indev_t */

#ifdef __cplusplus
extern "C" {
#endif

/* Set to 1 at compile time to enable a debug overlay: red tracking dot
 * + periodic [calib] log of the chip's raw coordinate range. */
#ifndef AXS5106L_TOUCH_DEBUG_OVERLAY
#define AXS5106L_TOUCH_DEBUG_OVERLAY 0
#endif

/* ========================================================================
 * Public types
 * ====================================================================== */

/// Opaque driver handle.
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

/// Callback fired on the first touch press (use for screen wake-up).
typedef void (*axs5106l_wake_cb_t)(void *user_ctx);

/// Callback fired when a gesture is recognized.
typedef void (*axs5106l_gesture_cb_t)(axs5106l_gesture_t g,
                                      int16_t x, int16_t y,
                                      void *user_ctx);

/// Constructor configuration.
typedef struct {
    i2c_master_bus_handle_t i2c_bus;   ///< Existing I2C master bus
    gpio_num_t              rst_gpio;  ///< Reset GPIO (may be shared with LCD)
    gpio_num_t              int_gpio;  ///< INT GPIO (active low)
    uint16_t                width;     ///< Logical screen width  (px)
    uint16_t                height;    ///< Logical screen height (px)
    bool                    swap_xy;   ///< Swap X and Y axes after reading
    bool                    mirror_x;  ///< Mirror X
    bool                    mirror_y;  ///< Mirror Y
} axs5106l_touch_config_t;

#define AXS5106L_TOUCH_DEFAULT_CONFIG(bus, rst, irq, w, h)  \
    {                                                       \
        .i2c_bus  = (bus),                                  \
        .rst_gpio = (rst),                                  \
        .int_gpio = (irq),                                  \
        .width    = (w),                                    \
        .height   = (h),                                    \
        .swap_xy  = false,                                  \
        .mirror_x = false,                                  \
        .mirror_y = false,                                  \
    }

/* ========================================================================
 * Lifecycle
 * ====================================================================== */

/**
 * @brief Phase 1 — pull RST, run firmware upgrade if needed, verify chip ID.
 *
 * Call before LVGL is started.
 *
 * @param[in]  cfg Driver configuration.
 * @param[out] out Receives the handle on success.
 */
esp_err_t axs5106l_touch_new(const axs5106l_touch_config_t *cfg,
                             axs5106l_touch_handle_t *out);

/**
 * @brief Phase 2 — register as an LVGL pointer input device.
 *
 * Call after LVGL is initialised. Idempotent.
 */
esp_err_t axs5106l_touch_attach_lvgl(axs5106l_touch_handle_t h);

/**
 * @brief Tear down: unregister LVGL device, release I2C, hold RST high.
 */
esp_err_t axs5106l_touch_del(axs5106l_touch_handle_t h);

/* ========================================================================
 * Power
 * ====================================================================== */

esp_err_t axs5106l_touch_sleep(axs5106l_touch_handle_t h);
esp_err_t axs5106l_touch_resume(axs5106l_touch_handle_t h);

/* ========================================================================
 * Callbacks
 * ====================================================================== */

void axs5106l_touch_set_wake_callback(axs5106l_touch_handle_t h,
                                      axs5106l_wake_cb_t cb,
                                      void *user_ctx);

void axs5106l_touch_set_gesture_callback(axs5106l_touch_handle_t h,
                                         axs5106l_gesture_cb_t cb,
                                         void *user_ctx);

/* ========================================================================
 * Introspection
 * ====================================================================== */

/// Get the underlying lv_indev_t (only valid after attach_lvgl).
lv_indev_t *axs5106l_touch_get_lvgl_device(axs5106l_touch_handle_t h);

#ifdef __cplusplus
}  /* extern "C" */
#endif
