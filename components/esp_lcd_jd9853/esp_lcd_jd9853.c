/*
 * SPDX-FileCopyrightText: 2024-2025 ESP-IDF LCD Driver
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <sys/cdefs.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_commands.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_check.h"

#include "esp_lcd_jd9853.h"

static const char *TAG = "lcd_jd9853";

/* Forward declarations */
static esp_err_t panel_jd9853_del(esp_lcd_panel_t *panel);
static esp_err_t panel_jd9853_reset(esp_lcd_panel_t *panel);
static esp_err_t panel_jd9853_init(esp_lcd_panel_t *panel);
static esp_err_t panel_jd9853_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start, int x_end, int y_end, const void *color_data);
static esp_err_t panel_jd9853_invert_color(esp_lcd_panel_t *panel, bool invert_color_data);
static esp_err_t panel_jd9853_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y);
static esp_err_t panel_jd9853_swap_xy(esp_lcd_panel_t *panel, bool swap_axes);
static esp_err_t panel_jd9853_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap);
static esp_err_t panel_jd9853_disp_on_off(esp_lcd_panel_t *panel, bool on_off);

/**
 * @brief JD9853 panel structure
 */
typedef struct {
    esp_lcd_panel_t base;
    esp_lcd_panel_io_handle_t io;
    int reset_gpio_num;
    bool reset_level;
    int x_gap;
    int y_gap;
    uint8_t fb_bits_per_pixel;
    uint8_t madctl_val;
    uint8_t colmod_val;
    const jd9853_lcd_init_cmd_t *init_cmds;
    uint16_t init_cmds_size;
} jd9853_panel_t;

/**
 * @brief Default initialization sequence for BOE 1.83" 240x284 IPS panel
 */
static const jd9853_lcd_init_cmd_t vendor_specific_init_default[] = {
    /* Unlock chip registers */
    {0xDF, (uint8_t[]){0x98, 0x53}, 2, 0},
    {0xDE, (uint8_t[]){0x00}, 1, 0},

    /* Power settings */
    {0xB2, (uint8_t[]){0x25}, 1, 0},
    {0xB7, (uint8_t[]){0x00, 0x21, 0x00, 0x49}, 4, 0},
    {0xBB, (uint8_t[]){0x1F, 0x9A, 0x55, 0x73, 0x63, 0xF0}, 6, 0},

    /* Timing configuration */
    {0xC0, (uint8_t[]){0x22, 0x22}, 2, 0},
    {0xC1, (uint8_t[]){0x12}, 1, 0},
    {0xC3, (uint8_t[]){0x7D, 0x07, 0x14, 0x06, 0xC8, 0x6A, 0x6C, 0x77}, 8, 0},
    {0xC4, (uint8_t[]){0x00, 0x00, 0xA0, 0x6F, 0x1E, 0x1A, 0x16, 0x79, 0x1E, 0x1A, 0x16, 0x82}, 12, 0},

    /* Gamma correction (G2.2) */
    {0xC8, (uint8_t[]){
        0x3F, 0x2C, 0x26, 0x20, 0x25, 0x26, 0x21, 0x21,
        0x1F, 0x1F, 0x1F, 0x13, 0x11, 0x0B, 0x04, 0x00,
        0x3F, 0x2C, 0x26, 0x20, 0x25, 0x27, 0x21, 0x21,
        0x1F, 0x1F, 0x1F, 0x13, 0x11, 0x0B, 0x04, 0x00
    }, 32, 0},

    /* Power management */
    {0xD0, (uint8_t[]){0x04, 0x06, 0x62, 0x0F, 0x00}, 5, 0},
    {0xD7, (uint8_t[]){0x00, 0x30}, 2, 0},
    {0xE6, (uint8_t[]){0x14}, 1, 0},

    /* Page 1 settings */
    {0xDE, (uint8_t[]){0x01}, 1, 0},
    {0xB7, (uint8_t[]){0x03, 0x13, 0xEF, 0x35, 0x35}, 5, 0},
    {0xC1, (uint8_t[]){0x14, 0x15, 0xC0}, 3, 0},
    {0xC2, (uint8_t[]){0x06, 0x3A, 0xC7}, 3, 0},
    {0xC4, (uint8_t[]){0x72, 0x12}, 2, 0},
    {0xBE, (uint8_t[]){0x00}, 1, 0},

    /* Back to page 0 */
    {0xDE, (uint8_t[]){0x00}, 1, 0},

    /* Display settings */
    {0x35, (uint8_t[]){0x00}, 1, 0},      /* Tearing effect line on */
    {0x36, (uint8_t[]){0x00}, 1, 0},      /* Memory access control */
    {0x3A, (uint8_t[]){0x05}, 1, 0},      /* Pixel format: RGB565 */
    {0x2A, (uint8_t[]){0x00, 0x00, 0x00, 0xEF}, 4, 0},  /* Column: 0-239 */
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0x1B}, 4, 0},  /* Row: 0-283 */

    /* Exit sleep and display on */
    {0x11, NULL, 0, 120},
    {0x29, NULL, 0, 50},
};

esp_err_t esp_lcd_new_panel_jd9853(const esp_lcd_panel_io_handle_t io,
                                   const esp_lcd_panel_dev_config_t *panel_dev_config,
                                   esp_lcd_panel_handle_t *ret_panel)
{
    esp_err_t ret = ESP_OK;
    jd9853_panel_t *jd9853 = NULL;

    ESP_GOTO_ON_FALSE(io && panel_dev_config && ret_panel, ESP_ERR_INVALID_ARG, err, TAG, "invalid argument");

    jd9853 = (jd9853_panel_t *)calloc(1, sizeof(jd9853_panel_t));
    ESP_GOTO_ON_FALSE(jd9853, ESP_ERR_NO_MEM, err, TAG, "no mem for jd9853 panel");

    if (panel_dev_config->reset_gpio_num >= 0) {
        gpio_config_t io_conf = {
            .mode = GPIO_MODE_OUTPUT,
            .pin_bit_mask = 1ULL << panel_dev_config->reset_gpio_num,
        };
        ESP_GOTO_ON_ERROR(gpio_config(&io_conf), err, TAG, "configure GPIO for RST line failed");
    }

    /* Configure color endian */
    switch (panel_dev_config->rgb_endian) {
    case LCD_RGB_ENDIAN_RGB:
        jd9853->madctl_val = 0;
        break;
    case LCD_RGB_ENDIAN_BGR:
        jd9853->madctl_val = LCD_CMD_BGR_BIT;
        break;
    default:
        ESP_GOTO_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, err, TAG, "unsupported rgb endian");
        break;
    }

    /* Configure pixel format */
    switch (panel_dev_config->bits_per_pixel) {
    case 16:
        jd9853->colmod_val = 0x55;
        jd9853->fb_bits_per_pixel = 16;
        break;
    case 18:
        jd9853->colmod_val = 0x66;
        jd9853->fb_bits_per_pixel = 24;
        break;
    default:
        ESP_GOTO_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, err, TAG, "unsupported pixel width");
        break;
    }

    jd9853->io = io;
    jd9853->reset_gpio_num = panel_dev_config->reset_gpio_num;
    jd9853->reset_level = panel_dev_config->flags.reset_active_high;

    if (panel_dev_config->vendor_config) {
        jd9853->init_cmds = ((jd9853_vendor_config_t *)panel_dev_config->vendor_config)->init_cmds;
        jd9853->init_cmds_size = ((jd9853_vendor_config_t *)panel_dev_config->vendor_config)->init_cmds_size;
    }

    /* Register panel operations */
    jd9853->base.del = panel_jd9853_del;
    jd9853->base.reset = panel_jd9853_reset;
    jd9853->base.init = panel_jd9853_init;
    jd9853->base.draw_bitmap = panel_jd9853_draw_bitmap;
    jd9853->base.invert_color = panel_jd9853_invert_color;
    jd9853->base.set_gap = panel_jd9853_set_gap;
    jd9853->base.mirror = panel_jd9853_mirror;
    jd9853->base.swap_xy = panel_jd9853_swap_xy;
    jd9853->base.disp_on_off = panel_jd9853_disp_on_off;

    *ret_panel = &(jd9853->base);
    ESP_LOGI(TAG, "LCD panel created, version: %d.%d.%d",
             ESP_LCD_JD9853_VER_MAJOR, ESP_LCD_JD9853_VER_MINOR, ESP_LCD_JD9853_VER_PATCH);

    return ESP_OK;

err:
    if (jd9853) {
        if (panel_dev_config->reset_gpio_num >= 0) {
            gpio_reset_pin(panel_dev_config->reset_gpio_num);
        }
        free(jd9853);
    }
    return ret;
}

static esp_err_t panel_jd9853_del(esp_lcd_panel_t *panel)
{
    jd9853_panel_t *jd9853 = __containerof(panel, jd9853_panel_t, base);

    if (jd9853->reset_gpio_num >= 0) {
        gpio_reset_pin(jd9853->reset_gpio_num);
    }
    ESP_LOGD(TAG, "del jd9853 panel @%p", jd9853);
    free(jd9853);
    return ESP_OK;
}

static esp_err_t panel_jd9853_reset(esp_lcd_panel_t *panel)
{
    jd9853_panel_t *jd9853 = __containerof(panel, jd9853_panel_t, base);
    esp_lcd_panel_io_handle_t io = jd9853->io;

    if (jd9853->reset_gpio_num >= 0) {
        /* Hardware reset */
        gpio_set_level(jd9853->reset_gpio_num, jd9853->reset_level);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(jd9853->reset_gpio_num, !jd9853->reset_level);
        vTaskDelay(pdMS_TO_TICKS(120));
    } else {
        /* Software reset */
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_SWRESET, NULL, 0), TAG, "send command failed");
        vTaskDelay(pdMS_TO_TICKS(120));
    }

    return ESP_OK;
}

static esp_err_t panel_jd9853_init(esp_lcd_panel_t *panel)
{
    jd9853_panel_t *jd9853 = __containerof(panel, jd9853_panel_t, base);
    esp_lcd_panel_io_handle_t io = jd9853->io;

    /* 【优化】直接应用厂商初始化序列，序列中已包含 SLPOUT (0x11)
     * 移除单独的 SLPOUT/MADCTL/COLMOD，避免重复延迟和命令 */
    const jd9853_lcd_init_cmd_t *init_cmds;
    uint16_t init_cmds_size;

    if (jd9853->init_cmds) {
        init_cmds = jd9853->init_cmds;
        init_cmds_size = jd9853->init_cmds_size;
    } else {
        init_cmds = vendor_specific_init_default;
        init_cmds_size = sizeof(vendor_specific_init_default) / sizeof(jd9853_lcd_init_cmd_t);
    }

    for (int i = 0; i < init_cmds_size; i++) {
        /* 跟踪 MADCTL/COLMOD 值用于后续 mirror/swap 操作 */
        if (init_cmds[i].cmd == LCD_CMD_MADCTL && init_cmds[i].data) {
            jd9853->madctl_val = ((uint8_t *)init_cmds[i].data)[0];
        } else if (init_cmds[i].cmd == LCD_CMD_COLMOD && init_cmds[i].data) {
            jd9853->colmod_val = ((uint8_t *)init_cmds[i].data)[0];
        }

        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, init_cmds[i].cmd,
            init_cmds[i].data, init_cmds[i].data_bytes), TAG, "send command failed");

        if (init_cmds[i].delay_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(init_cmds[i].delay_ms));
        }
    }

    ESP_LOGD(TAG, "initialization complete");
    return ESP_OK;
}

static esp_err_t panel_jd9853_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start,
                                          int x_end, int y_end, const void *color_data)
{
    jd9853_panel_t *jd9853 = __containerof(panel, jd9853_panel_t, base);
    esp_lcd_panel_io_handle_t io = jd9853->io;

    assert((x_start < x_end) && (y_start < y_end) && "invalid coordinates");

    x_start += jd9853->x_gap;
    x_end += jd9853->x_gap;
    y_start += jd9853->y_gap;
    y_end += jd9853->y_gap;

    /* Set column address */
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_CASET, (uint8_t[]){
        (x_start >> 8) & 0xFF, x_start & 0xFF,
        ((x_end - 1) >> 8) & 0xFF, (x_end - 1) & 0xFF,
    }, 4), TAG, "CASET failed");

    /* Set row address */
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_RASET, (uint8_t[]){
        (y_start >> 8) & 0xFF, y_start & 0xFF,
        ((y_end - 1) >> 8) & 0xFF, (y_end - 1) & 0xFF,
    }, 4), TAG, "RASET failed");

    /* Transfer frame buffer */
    size_t len = (x_end - x_start) * (y_end - y_start) * jd9853->fb_bits_per_pixel / 8;
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_color(io, LCD_CMD_RAMWR, color_data, len),
        TAG, "RAMWR failed");

    return ESP_OK;
}

static esp_err_t panel_jd9853_invert_color(esp_lcd_panel_t *panel, bool invert_color_data)
{
    jd9853_panel_t *jd9853 = __containerof(panel, jd9853_panel_t, base);
    int command = invert_color_data ? LCD_CMD_INVON : LCD_CMD_INVOFF;
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(jd9853->io, command, NULL, 0),
        TAG, "send command failed");
    return ESP_OK;
}

static esp_err_t panel_jd9853_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y)
{
    jd9853_panel_t *jd9853 = __containerof(panel, jd9853_panel_t, base);

    if (mirror_x) {
        jd9853->madctl_val |= LCD_CMD_MX_BIT;
    } else {
        jd9853->madctl_val &= ~LCD_CMD_MX_BIT;
    }

    if (mirror_y) {
        jd9853->madctl_val |= LCD_CMD_MY_BIT;
    } else {
        jd9853->madctl_val &= ~LCD_CMD_MY_BIT;
    }

    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(jd9853->io, LCD_CMD_MADCTL,
        (uint8_t[]){jd9853->madctl_val}, 1), TAG, "send command failed");
    return ESP_OK;
}

static esp_err_t panel_jd9853_swap_xy(esp_lcd_panel_t *panel, bool swap_axes)
{
    jd9853_panel_t *jd9853 = __containerof(panel, jd9853_panel_t, base);

    if (swap_axes) {
        jd9853->madctl_val |= LCD_CMD_MV_BIT;
    } else {
        jd9853->madctl_val &= ~LCD_CMD_MV_BIT;
    }

    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(jd9853->io, LCD_CMD_MADCTL,
        (uint8_t[]){jd9853->madctl_val}, 1), TAG, "send command failed");
    return ESP_OK;
}

static esp_err_t panel_jd9853_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap)
{
    jd9853_panel_t *jd9853 = __containerof(panel, jd9853_panel_t, base);
    jd9853->x_gap = x_gap;
    jd9853->y_gap = y_gap;
    return ESP_OK;
}

static esp_err_t panel_jd9853_disp_on_off(esp_lcd_panel_t *panel, bool on_off)
{
    jd9853_panel_t *jd9853 = __containerof(panel, jd9853_panel_t, base);
    int command = on_off ? LCD_CMD_DISPON : LCD_CMD_DISPOFF;
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(jd9853->io, command, NULL, 0),
        TAG, "send command failed");
    return ESP_OK;
}

esp_err_t esp_lcd_jd9853_create_panel(int spi_host, int cs, int dc, int reset,
                                      bool mirror_x, bool mirror_y, bool swap_xy, bool invert_color,
                                      esp_lcd_panel_io_handle_t *ret_io,
                                      esp_lcd_panel_handle_t *ret_panel)
{
    ESP_RETURN_ON_FALSE(ret_io && ret_panel, ESP_ERR_INVALID_ARG, TAG, "invalid argument");

    esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = cs,
        .dc_gpio_num = dc,
        .spi_mode = 0,
        .pclk_hz = 80 * 1000 * 1000,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)spi_host, &io_config, ret_io),
        TAG, "create SPI IO failed");

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = reset,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_jd9853(*ret_io, &panel_config, ret_panel),
        TAG, "create panel failed");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(*ret_panel), TAG, "reset failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(*ret_panel), TAG, "init failed");
    if (invert_color) esp_lcd_panel_invert_color(*ret_panel, true);
    if (swap_xy) esp_lcd_panel_swap_xy(*ret_panel, true);
    if (mirror_x || mirror_y) esp_lcd_panel_mirror(*ret_panel, mirror_x, mirror_y);
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(*ret_panel, true), TAG, "display on failed");

    return ESP_OK;
}