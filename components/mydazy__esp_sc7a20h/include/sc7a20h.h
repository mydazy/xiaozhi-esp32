/*
 * SPDX-FileCopyrightText: 2026 mydazy
 * SPDX-License-Identifier: Apache-2.0
 *
 * SC7A20H 3-axis accelerometer driver (Silan Microelectronics).
 * Register-compatible with ST LIS2DH12 / LIS3DH.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <driver/i2c_master.h>
#include <driver/gpio.h>
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
 * Public types
 * ====================================================================== */

/// Opaque handle to an SC7A20H driver instance.
typedef struct sc7a20h_dev_t *sc7a20h_handle_t;

/// Raw 3-axis sample (12-bit, sign-extended after right-shift).
typedef struct {
    int16_t x;
    int16_t y;
    int16_t z;
} sc7a20h_raw_acce_t;

/// Scaled 3-axis acceleration in milli-g (mg).
typedef struct {
    float x;
    float y;
    float z;
} sc7a20h_acce_t;

/// Full-scale range selection.
typedef enum {
    SC7A20H_RANGE_2G  = 0x00,  ///< +/- 2 g  (default at power-up)
    SC7A20H_RANGE_4G  = 0x10,  ///< +/- 4 g
    SC7A20H_RANGE_8G  = 0x20,  ///< +/- 8 g
    SC7A20H_RANGE_16G = 0x30,  ///< +/- 16 g
} sc7a20h_range_t;

/// Output data rate.
typedef enum {
    SC7A20H_ODR_POWER_DOWN = 0x00,
    SC7A20H_ODR_1HZ        = 0x10,  ///< Ultra-low power
    SC7A20H_ODR_10HZ       = 0x20,  ///< Low power
    SC7A20H_ODR_25HZ       = 0x30,
    SC7A20H_ODR_50HZ       = 0x40,
    SC7A20H_ODR_100HZ      = 0x50,  ///< Driver default
    SC7A20H_ODR_200HZ      = 0x60,
    SC7A20H_ODR_400HZ      = 0x70,
} sc7a20h_odr_t;

/// Constructor configuration.
typedef struct {
    uint8_t i2c_addr;       ///< 7-bit I2C address (typically 0x18 or 0x19)
    sc7a20h_range_t range;  ///< Initial full-scale range
    sc7a20h_odr_t   odr;    ///< Initial output data rate
} sc7a20h_config_t;

#define SC7A20H_DEFAULT_CONFIG()        \
    {                                   \
        .i2c_addr = 0x19,               \
        .range    = SC7A20H_RANGE_4G,   \
        .odr      = SC7A20H_ODR_100HZ,  \
    }

/// Motion-detection configuration.
typedef struct {
    uint8_t threshold;   ///< Threshold (step = full-scale/128, e.g. 0x08 ~= 250 mg @ 4 g)
    uint8_t duration;    ///< Duration  (step = 1/ODR samples)
    bool    enable_x;    ///< Detect motion on X axis
    bool    enable_y;    ///< Detect motion on Y axis
    bool    enable_z;    ///< Detect motion on Z axis
} sc7a20h_motion_config_t;

#define SC7A20H_DEFAULT_MOTION_CONFIG() \
    {                                   \
        .threshold = 0x08,              \
        .duration  = 0x02,              \
        .enable_x  = true,              \
        .enable_y  = true,              \
        .enable_z  = true,              \
    }

/// User-space callback fired when a motion event is observed (debounced).
/// Runs in caller context, NOT from ISR.
typedef void (*sc7a20h_wakeup_cb_t)(void *user_ctx);

/* ========================================================================
 * Lifecycle
 * ====================================================================== */

/**
 * @brief Create and initialise an SC7A20H driver instance.
 *
 * Verifies WHO_AM_I, configures range / ODR, returns a handle on success.
 *
 * @param[in]  i2c_bus  Existing I2C master bus handle.
 * @param[in]  cfg      Driver configuration. Use @c SC7A20H_DEFAULT_CONFIG().
 * @param[out] out      Receives the handle on success.
 *
 * @return ESP_OK                on success;
 *         ESP_ERR_INVALID_ARG   if a parameter is NULL;
 *         ESP_ERR_NOT_FOUND     if WHO_AM_I mismatch;
 *         ESP_ERR_NO_MEM        on allocation failure;
 *         underlying I2C error otherwise.
 */
esp_err_t sc7a20h_create(i2c_master_bus_handle_t i2c_bus,
                         const sc7a20h_config_t *cfg,
                         sc7a20h_handle_t *out);

/**
 * @brief Power down the device and release all resources.
 */
esp_err_t sc7a20h_del(sc7a20h_handle_t h);

/* ========================================================================
 * Data readout
 * ====================================================================== */

/// Read raw acceleration sample (12-bit signed, already right-shifted).
esp_err_t sc7a20h_get_raw_acce(sc7a20h_handle_t h, sc7a20h_raw_acce_t *raw);

/// Read acceleration scaled to milli-g.
esp_err_t sc7a20h_get_acce(sc7a20h_handle_t h, sc7a20h_acce_t *acce);

/* ========================================================================
 * Motion detection (INT1)
 * ====================================================================== */

/**
 * @brief Enable or disable the motion-detect interrupt on INT1.
 * @param h       Driver handle.
 * @param enable  true to enable, false to disable.
 * @param mcfg    Detection parameters; pass NULL to use defaults.
 */
esp_err_t sc7a20h_set_motion_detection(sc7a20h_handle_t h,
                                       bool enable,
                                       const sc7a20h_motion_config_t *mcfg);

/// Install a user-space callback for motion events.
/// Pass cb=NULL to clear.
esp_err_t sc7a20h_set_wakeup_callback(sc7a20h_handle_t h,
                                      sc7a20h_wakeup_cb_t cb,
                                      void *user_ctx);

/* ========================================================================
 * Power management / runtime tuning
 * ====================================================================== */

esp_err_t sc7a20h_enter_power_down(sc7a20h_handle_t h);
esp_err_t sc7a20h_exit_power_down(sc7a20h_handle_t h);

esp_err_t sc7a20h_set_range(sc7a20h_handle_t h, sc7a20h_range_t range);
esp_err_t sc7a20h_set_odr(sc7a20h_handle_t h, sc7a20h_odr_t odr);

/* ========================================================================
 * Deep-sleep wakeup
 * ====================================================================== */

/**
 * @brief One-call deep-sleep wakeup setup using EXT1.
 *
 * After this, calling @c esp_deep_sleep_start() will let the MCU wake when
 * the SC7A20H INT1 line goes low. Internally configures RTC GPIO pull-up
 * and registers the EXT1 wakeup source.
 *
 * @param int1_gpio GPIO connected to SC7A20H INT1 (must be RTC-capable).
 */
esp_err_t sc7a20h_config_deep_sleep_wakeup(sc7a20h_handle_t h, gpio_num_t int1_gpio);

/**
 * @brief Read INT1_SRC (0x31) to clear a latched motion interrupt.
 *
 * When CTRL_REG5 latch (bit3 LIR_INT1) is enabled, INT1 stays asserted until
 * INT1_SRC is read. Call this whenever you need to "consume" a pending
 * motion event without taking the wakeup path — e.g. before re-arming
 * deep-sleep, or after handling a motion ISR in active mode.
 *
 * @param h        Driver handle.
 * @param[out] src Optional. If non-NULL, receives the INT1_SRC byte (IA / Z* /
 *                 Y* / X* flags). Pass NULL to discard.
 */
esp_err_t sc7a20h_clear_motion_latch(sc7a20h_handle_t h, uint8_t *src);

/* ========================================================================
 * Convenience
 * ====================================================================== */

/**
 * @brief Create + initialise + enable motion detection in one call.
 *
 * Typical "pick-up to wake" use case. Installs a built-in 500 ms debounce
 * log callback if you don't pass your own afterwards.
 */
esp_err_t sc7a20h_create_with_motion_detection(i2c_master_bus_handle_t i2c_bus,
                                               const sc7a20h_config_t *cfg,
                                               const sc7a20h_motion_config_t *mcfg,
                                               sc7a20h_handle_t *out);

#ifdef __cplusplus
}  /* extern "C" */
#endif
