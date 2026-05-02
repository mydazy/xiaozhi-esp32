/*
 * SPDX-FileCopyrightText: 2026 mydazy
 * SPDX-License-Identifier: Apache-2.0
 *
 * AXS5106L touch-controller firmware upgrade helper — C API.
 *
 * Reads the running firmware version from the chip and, when it differs from
 * the version embedded in @c axs5106l_firmware.h, performs a full MTP reflash
 * via the chip's debug-mode command sequence.
 *
 * Typical usage:
 *   axs5106l_upgrade_handle_t up;
 *   axs5106l_upgrade_init(i2c_handle, rst_gpio, &up);
 *   if (axs5106l_upgrade_run(up) == AXS5106L_UPGRADE_SUCCESS) { ... }
 *   axs5106l_upgrade_del(up);
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

/// Opaque upgrader handle.
typedef struct axs5106l_upgrade_t *axs5106l_upgrade_handle_t;

/// Result of a firmware upgrade attempt.
typedef enum {
    AXS5106L_UPGRADE_SUCCESS    =  0,  ///< Upgrade completed successfully.
    AXS5106L_UPGRADE_NOT_NEEDED = -1,  ///< Chip firmware already matches the embedded image.
    AXS5106L_UPGRADE_FAILED     = -2,  ///< Upgrade flow failed (erase/write/verify error).
    AXS5106L_UPGRADE_I2C_ERROR  = -3,  ///< I2C communication error.
} axs5106l_upgrade_result_t;

/**
 * @brief Allocate an upgrader bound to an I2C device and reset GPIO.
 *
 * @param[in]  i2c_handle I2C device handle (already added to a master bus).
 * @param[in]  rst_gpio   GPIO connected to the chip reset line.
 * @param[out] out        Receives the handle on success.
 */
esp_err_t axs5106l_upgrade_init(i2c_master_dev_handle_t i2c_handle,
                                gpio_num_t rst_gpio,
                                axs5106l_upgrade_handle_t *out);

/// Free the upgrader.
void axs5106l_upgrade_del(axs5106l_upgrade_handle_t h);

/**
 * @brief Compare versions and reflash if necessary.
 * @return Result code; see @ref axs5106l_upgrade_result_t.
 */
axs5106l_upgrade_result_t axs5106l_upgrade_run(axs5106l_upgrade_handle_t h);

/**
 * @brief Read the firmware version currently running on the chip.
 * @param[out] version Firmware version reported by the chip.
 * @return true on success.
 */
bool axs5106l_upgrade_get_chip_version(axs5106l_upgrade_handle_t h,
                                       uint16_t *version);

/// Version of the firmware image embedded in this build.
uint16_t axs5106l_upgrade_get_embedded_version(void);

#ifdef __cplusplus
}  /* extern "C" */
#endif
