/*
 * SPDX-FileCopyrightText: 2024-2025 ESP-IDF LCD Driver
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file esp_lcd_jd9853.h
 * @brief ESP-IDF LCD driver for JD9853 (Jadard)
 *
 * JD9853 is a TFT LCD driver IC from Jadard Technology.
 * Default configuration is for BOE 1.83" IPS panel (WV018LZQ-N80-3QP1).
 */

#pragma once

#include "esp_lcd_panel_vendor.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief JD9853 LCD driver version
 */
#define ESP_LCD_JD9853_VER_MAJOR    1
#define ESP_LCD_JD9853_VER_MINOR    0
#define ESP_LCD_JD9853_VER_PATCH    0

/**
 * @brief JD9853 LCD panel specifications (default: BOE 1.83" IPS)
 */
#define JD9853_LCD_H_RES            240     /*!< Horizontal resolution */
#define JD9853_LCD_V_RES            284     /*!< Vertical resolution */
#define JD9853_LCD_COLOR_BITS       16      /*!< Color depth (RGB565) */

/**
 * @brief LCD panel initialization command structure
 */
typedef struct {
    int cmd;                /*!< The specific LCD command */
    const void *data;       /*!< Buffer that holds the command specific data */
    size_t data_bytes;      /*!< Size of `data` in memory, in bytes */
    unsigned int delay_ms;  /*!< Delay in milliseconds after this command */
} jd9853_lcd_init_cmd_t;

/**
 * @brief LCD panel vendor configuration
 *
 * @note This structure needs to be passed to the `vendor_config` field in `esp_lcd_panel_dev_config_t`.
 */
typedef struct {
    const jd9853_lcd_init_cmd_t *init_cmds;  /*!< Pointer to initialization commands array.
                                                  Set to NULL to use default commands. */
    uint16_t init_cmds_size;                 /*!< Number of commands in above array */
} jd9853_vendor_config_t;

/**
 * @brief Create LCD panel for JD9853
 *
 * @param[in]  io LCD panel IO handle
 * @param[in]  panel_dev_config General panel device configuration
 * @param[out] ret_panel Returned LCD panel handle
 * @return
 *      - ESP_ERR_INVALID_ARG   if parameter is invalid
 *      - ESP_ERR_NO_MEM        if out of memory
 *      - ESP_OK                on success
 */
esp_err_t esp_lcd_new_panel_jd9853(const esp_lcd_panel_io_handle_t io,
                                   const esp_lcd_panel_dev_config_t *panel_dev_config,
                                   esp_lcd_panel_handle_t *ret_panel);

/**
 * @brief One-shot panel creation: SPI IO + JD9853 panel + init + display on
 *
 * @param spi_host     SPI host (e.g., SPI2_HOST)
 * @param cs           SPI CS GPIO
 * @param dc           SPI DC GPIO
 * @param reset        Reset GPIO (-1 if not used)
 * @param mirror_x     Horizontal mirror
 * @param mirror_y     Vertical mirror
 * @param swap_xy      Swap X/Y axes
 * @param invert_color Invert color
 * @param[out] ret_io    Returned panel IO handle
 * @param[out] ret_panel Returned panel handle
 */
esp_err_t esp_lcd_jd9853_create_panel(int spi_host, int cs, int dc, int reset,
                                      bool mirror_x, bool mirror_y, bool swap_xy, bool invert_color,
                                      esp_lcd_panel_io_handle_t *ret_io,
                                      esp_lcd_panel_handle_t *ret_panel);

/**
 * @brief SPI bus configuration helper macro
 *
 * @param sclk SPI clock pin number
 * @param mosi SPI MOSI pin number
 * @param max_trans_sz Maximum transfer size in bytes
 */
#define JD9853_PANEL_BUS_SPI_CONFIG(sclk, mosi, max_trans_sz)   \
    {                                                           \
        .sclk_io_num = sclk,                                    \
        .mosi_io_num = mosi,                                    \
        .miso_io_num = -1,                                      \
        .quadwp_io_num = -1,                                    \
        .quadhd_io_num = -1,                                    \
        .max_transfer_sz = max_trans_sz,                        \
    }

/**
 * @brief SPI panel IO configuration helper macro
 *
 * @param cs SPI chip select pin number
 * @param dc SPI data/command pin number
 * @param callback Callback function when SPI transfer is done
 * @param callback_ctx Callback function context
 */
#define JD9853_PANEL_IO_SPI_CONFIG(cs, dc, callback, callback_ctx)  \
    {                                                               \
        .cs_gpio_num = cs,                                          \
        .dc_gpio_num = dc,                                          \
        .spi_mode = 0,                                              \
        .pclk_hz = 80 * 1000 * 1000,                                \
        .trans_queue_depth = 10,                                    \
        .on_color_trans_done = callback,                            \
        .user_ctx = callback_ctx,                                   \
        .lcd_cmd_bits = 8,                                          \
        .lcd_param_bits = 8,                                        \
    }

#ifdef __cplusplus
}
#endif