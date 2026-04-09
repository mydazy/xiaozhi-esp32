/*
 * SPDX-FileCopyrightText: 2021-2023 Espressif Systems (Shanghai) CO LTD
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

// 修改文件名和标识
static const char *TAG = "jd9853";

static esp_err_t panel_jd9853_del(esp_lcd_panel_t *panel);
static esp_err_t panel_jd9853_reset(esp_lcd_panel_t *panel);
static esp_err_t panel_jd9853_init(esp_lcd_panel_t *panel);
static esp_err_t panel_jd9853_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start, int x_end, int y_end, const void *color_data);
static esp_err_t panel_jd9853_invert_color(esp_lcd_panel_t *panel, bool invert_color_data);
static esp_err_t panel_jd9853_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y);
static esp_err_t panel_jd9853_swap_xy(esp_lcd_panel_t *panel, bool swap_axes);
static esp_err_t panel_jd9853_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap);
static esp_err_t panel_jd9853_disp_on_off(esp_lcd_panel_t *panel, bool off);

typedef struct {
    esp_lcd_panel_t base;
    esp_lcd_panel_io_handle_t io;
    int reset_gpio_num;
    bool reset_level;
    int x_gap;
    int y_gap;
    uint8_t fb_bits_per_pixel;
    uint8_t madctl_val; // save current value of LCD_CMD_MADCTL register
    uint8_t colmod_val; // save current value of LCD_CMD_COLMOD register
    const jd9853_lcd_init_cmd_t *init_cmds;
    uint16_t init_cmds_size;
} jd9853_panel_t;

esp_err_t esp_lcd_new_panel_jd9853(const esp_lcd_panel_io_handle_t io, const esp_lcd_panel_dev_config_t *panel_dev_config, esp_lcd_panel_handle_t *ret_panel)
{
    esp_err_t ret = ESP_OK;
    jd9853_panel_t *jd9853 = NULL;
    gpio_config_t io_conf = { 0 };

    ESP_GOTO_ON_FALSE(io && panel_dev_config && ret_panel, ESP_ERR_INVALID_ARG, err, TAG, "invalid argument");
    jd9853 = (jd9853_panel_t *)calloc(1, sizeof(jd9853_panel_t));
    ESP_GOTO_ON_FALSE(jd9853, ESP_ERR_NO_MEM, err, TAG, "no mem for jd9853 panel");

    if (panel_dev_config->reset_gpio_num >= 0) {
        io_conf.mode = GPIO_MODE_OUTPUT;
        io_conf.pin_bit_mask = 1ULL << panel_dev_config->reset_gpio_num;
        ESP_GOTO_ON_ERROR(gpio_config(&io_conf), err, TAG, "configure GPIO for RST line failed");
    }

#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 0, 0)
    switch (panel_dev_config->color_space) {
    case ESP_LCD_COLOR_SPACE_RGB:
        jd9853->madctl_val = 0;
        break;
    case ESP_LCD_COLOR_SPACE_BGR:
        jd9853->madctl_val |= LCD_CMD_BGR_BIT;
        break;
    default:
        ESP_GOTO_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, err, TAG, "unsupported color space");
        break;
    }
#else
    switch (panel_dev_config->rgb_endian) {
    case LCD_RGB_ENDIAN_RGB:
        jd9853->madctl_val = 0;
        break;
    case LCD_RGB_ENDIAN_BGR:
        jd9853->madctl_val |= LCD_CMD_BGR_BIT;
        break;
    default:
        ESP_GOTO_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, err, TAG, "unsupported rgb endian");
        break;
    }
#endif

    switch (panel_dev_config->bits_per_pixel) {
    case 16: // RGB565
        jd9853->colmod_val = 0x55;
        jd9853->fb_bits_per_pixel = 16;
        break;
    case 18: // RGB666
        jd9853->colmod_val = 0x66;
        // each color component (R/G/B) should occupy the 6 high bits of a byte, which means 3 full bytes are required for a pixel
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
    jd9853->base.del = panel_jd9853_del;
    jd9853->base.reset = panel_jd9853_reset;
    jd9853->base.init = panel_jd9853_init;
    jd9853->base.draw_bitmap = panel_jd9853_draw_bitmap;
    jd9853->base.invert_color = panel_jd9853_invert_color;
    jd9853->base.set_gap = panel_jd9853_set_gap;
    jd9853->base.mirror = panel_jd9853_mirror;
    jd9853->base.swap_xy = panel_jd9853_swap_xy;
#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 0, 0)
    jd9853->base.disp_off = panel_jd9853_disp_on_off;
#else
    jd9853->base.disp_on_off = panel_jd9853_disp_on_off;
#endif
    *ret_panel = &(jd9853->base);
    ESP_LOGD(TAG, "new jd9853 panel @%p", jd9853);

    // ESP_LOGI(TAG, "LCD panel create success, version: %d.%d.%d", ESP_LCD_jd9853_VER_MAJOR, ESP_LCD_jd9853_VER_MINOR, ESP_LCD_jd9853_VER_PATCH);

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

    // 硬件复位时序（文档2：10.3.6 要求>10μs）
    if (jd9853->reset_gpio_num >= 0) {
        gpio_set_level(jd9853->reset_gpio_num, jd9853->reset_level);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(jd9853->reset_gpio_num, !jd9853->reset_level);
        vTaskDelay(pdMS_TO_TICKS(120));  // 文档2要求120ms初始化等待
    } else { 
        // perform software reset
        vTaskDelay(pdMS_TO_TICKS(50));
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_SWRESET, NULL, 0), TAG, "send command failed");
        vTaskDelay(pdMS_TO_TICKS(20)); // spec, wait at least 5ms before sending new command
    }

    return ESP_OK;
}

static const jd9853_lcd_init_cmd_t vendor_specific_init_default[] = {
    // 芯片识别与基本配置
    {0xDF, (uint8_t[]){0x98, 0x53}, 2, 0},  // 芯片ID设置
    {0xDE, (uint8_t[]){0x00}, 1, 0},       // 配置寄存器初始化

    // 电源与驱动控制
    {0xB2, (uint8_t[]){0x25}, 1, 0},       // VGH/VGL电压设置
    {0xB7, (uint8_t[]){0x00, 0x21, 0x00, 0x49}, 4, 0}, // 源极驱动能力
    {0xBB, (uint8_t[]){0x1F, 0x9A, 0x55, 0x73, 0x63, 0xF0}, 6, 0}, // VCOM设置

    // 时序与接口配置
    {0xC0, (uint8_t[]){0x22, 0x22}, 2, 0}, // 门极时钟控制
    {0xC1, (uint8_t[]){0x12}, 1, 0},       // 帧率控制
    {0xC3, (uint8_t[]){0x7D, 0x07, 0x14, 0x06, 0xC8, 0x6A, 0x6C, 0x77}, 8, 0}, // 行序控制
    {0xC4, (uint8_t[]){0x00, 0x00, 0xA0, 0x6F, 0x1E, 0x1A, 0x16, 0x79, 0x1E, 0x1A, 0x16, 0x82}, 12, 0},

    // 伽马校正（关键优化点）
    {0xC8, (uint8_t[]){
        0x3F, 0x2C, 0x26, 0x20, 0x25, 0x26, 0x21, 0x21,
        0x1F, 0x1F, 0x1F, 0x13, 0x11, 0x0B, 0x04, 0x00,
        0x3F, 0x2C, 0x26, 0x20, 0x25, 0x27, 0x21, 0x21,
        0x1F, 0x1F, 0x1F, 0x13, 0x11, 0x0B, 0x04, 0x00
    }, 32, 0},  // 完整伽马曲线参数

    // 电源管理
    {0xD0, (uint8_t[]){0x04, 0x06, 0x62, 0x0F, 0x00}, 5, 0}, // DC/DC控制
    {0xD7, (uint8_t[]){0x00, 0x30}, 2, 0},             // 泵电路控制
    {0xE6, (uint8_t[]){0x14}, 1, 0},

    {0xDE, (uint8_t[]){0x01}, 1, 0},

    {0xB7, (uint8_t[]){0x03, 0x13, 0xEF, 0x35, 0x35}, 5, 0},

    {0xC1, (uint8_t[]){0x14, 0x15, 0xC0}, 3, 0},

    {0xC2, (uint8_t[]){0x06, 0x3A, 0xC7}, 3, 0},

    {0xC4, (uint8_t[]){0x72, 0x12}, 2, 0},
    {0xBE, (uint8_t[]){0x00}, 1, 0},
    {0xDE, (uint8_t[]){0x00}, 1, 0},
    {0x35, (uint8_t[]){0x00}, 1, 0},

    {0x36, (uint8_t[]){0x00}, 1, 0}, 
    {0x3A, (uint8_t[]){0x05}, 1, 0},        // 像素格式设置 (RGB666)
    {0x2A, (uint8_t[]){0x00, 0x00, 0x00, 0xEF}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0x1B}, 4, 0},


    {0x11, NULL, 0, 120},                  // 退出睡眠模式 (120ms延迟)
    {0x29, NULL, 0, 50}                    // 开启显示 (50ms延迟)
};

static esp_err_t panel_jd9853_init(esp_lcd_panel_t *panel)
{
    jd9853_panel_t *jd9853 = __containerof(panel, jd9853_panel_t, base);
    esp_lcd_panel_io_handle_t io = jd9853->io;

    // 退出睡眠模式 (文档2：9.2.12 SLPOUT=11h)
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_SLPOUT, NULL, 0), TAG, "SLPOUT failed");
    vTaskDelay(pdMS_TO_TICKS(120));

    // 设置内存访问控制 (文档2：9.2.26 MADCTL=36h)
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_MADCTL, (uint8_t[]) {
        jd9853->madctl_val,
    }, 1), TAG, "MADCTL failed");

    // 设置像素格式 (文档2：9.2.30 COLMOD=3Ah)
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_COLMOD, (uint8_t[]) {
        jd9853->colmod_val,
    }, 1), TAG, "COLMOD failed");

    // JD9853特定初始化序列
    const jd9853_lcd_init_cmd_t *init_cmds = NULL;
    uint16_t init_cmds_size = 0;
    bool using_external_init = false;
    if (jd9853->init_cmds) {
        init_cmds = jd9853->init_cmds;
        init_cmds_size = jd9853->init_cmds_size;
        using_external_init = true;
    } else {
        init_cmds = vendor_specific_init_default;
        init_cmds_size = sizeof(vendor_specific_init_default) / sizeof(jd9853_lcd_init_cmd_t);
        using_external_init = false;
    }

    bool is_cmd_overwritten = false;
    for (int i = 0; i < init_cmds_size; i++) {
        // Check if the command would overwrite internally configured values
        switch (init_cmds[i].cmd) {
        case LCD_CMD_MADCTL:
            {
                uint8_t new_val = ((uint8_t *)init_cmds[i].data)[0];
                is_cmd_overwritten = using_external_init && (new_val != jd9853->madctl_val);
                jd9853->madctl_val = new_val;
            }
            break;
        case LCD_CMD_COLMOD:
            {
                uint8_t new_val = ((uint8_t *)init_cmds[i].data)[0];
                is_cmd_overwritten = using_external_init && (new_val != jd9853->colmod_val);
                jd9853->colmod_val = new_val;
            }
            break;
        default:
            is_cmd_overwritten = false;
            break;
        }

        if (is_cmd_overwritten) {
            ESP_LOGW(TAG, "The %02Xh command has been used and will be overwritten by external initialization sequence", init_cmds[i].cmd);
        }

        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, init_cmds[i].cmd, init_cmds[i].data, init_cmds[i].data_bytes), TAG, "send command failed");
        vTaskDelay(pdMS_TO_TICKS(init_cmds[i].delay_ms));
    }
    ESP_LOGD(TAG, "send init commands success");

    return ESP_OK;
}

static esp_err_t panel_jd9853_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start, int x_end, int y_end, const void *color_data)
{
    jd9853_panel_t *jd9853 = __containerof(panel, jd9853_panel_t, base);
    assert((x_start < x_end) && (y_start < y_end) && "start position must be smaller than end position");
    esp_lcd_panel_io_handle_t io = jd9853->io;

    x_start += jd9853->x_gap;
    x_end += jd9853->x_gap;
    y_start += jd9853->y_gap;
    y_end += jd9853->y_gap;

    // define an area of frame memory where MCU can access
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_CASET, (uint8_t[]) {
        (x_start >> 8) & 0xFF,
        x_start & 0xFF,
        ((x_end - 1) >> 8) & 0xFF,
        (x_end - 1) & 0xFF,
    }, 4), TAG, "send command failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_RASET, (uint8_t[]) {
        (y_start >> 8) & 0xFF,
        y_start & 0xFF,
        ((y_end - 1) >> 8) & 0xFF,
        (y_end - 1) & 0xFF,
    }, 4), TAG, "send command failed");
    // transfer frame buffer
    size_t len = (x_end - x_start) * (y_end - y_start) * jd9853->fb_bits_per_pixel / 8;
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_color(io, LCD_CMD_RAMWR, color_data, len), TAG, "send color failed");

    return ESP_OK;
}

static esp_err_t panel_jd9853_invert_color(esp_lcd_panel_t *panel, bool invert_color_data)
{
    jd9853_panel_t *jd9853 = __containerof(panel, jd9853_panel_t, base);
    esp_lcd_panel_io_handle_t io = jd9853->io;
    int command = 0;
    if (invert_color_data) {
        command = LCD_CMD_INVON;
    } else {
        command = LCD_CMD_INVOFF;
    }
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, command, NULL, 0), TAG, "send command failed");
    return ESP_OK;
}

static esp_err_t panel_jd9853_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y)
{
    jd9853_panel_t *jd9853 = __containerof(panel, jd9853_panel_t, base);
    esp_lcd_panel_io_handle_t io = jd9853->io;
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
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_MADCTL, (uint8_t[]) {
        jd9853->madctl_val
    }, 1), TAG, "send command failed");
    return ESP_OK;
}

static esp_err_t panel_jd9853_swap_xy(esp_lcd_panel_t *panel, bool swap_axes)
{
    jd9853_panel_t *jd9853 = __containerof(panel, jd9853_panel_t, base);
    esp_lcd_panel_io_handle_t io = jd9853->io;
    if (swap_axes) {
        jd9853->madctl_val |= LCD_CMD_MV_BIT;
    } else {
        jd9853->madctl_val &= ~LCD_CMD_MV_BIT;
    }
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, LCD_CMD_MADCTL, (uint8_t[]) {
        jd9853->madctl_val
    }, 1), TAG, "send command failed");
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
    esp_lcd_panel_io_handle_t io = jd9853->io;
    int command = 0;

#if ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(5, 0, 0)
    on_off = !on_off;
#endif

    if (on_off) {
        command = LCD_CMD_DISPON;
    } else {
        command = LCD_CMD_DISPOFF;
    }
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, command, NULL, 0), TAG, "send command failed");
    return ESP_OK;
}
