#pragma once

#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_io.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct display_panel_result_t {
    esp_lcd_panel_io_handle_t io_handle;
    esp_lcd_panel_handle_t panel_handle;
} display_panel_result_t;

// 通用 SPI 面板创建参数
typedef struct display_panel_spi_params_t {
    int host;                 // spi_host_device_t
    int cs_gpio_num;
    int dc_gpio_num;
    int reset_gpio_num;       // -1 表示不使用
    int pclk_hz;              // 示例: 40*1000*1000
    int trans_queue_depth;    // 示例: 10
    int lcd_cmd_bits;         // 示例: 8
    int lcd_param_bits;       // 示例: 8
    int width;                // 分辨率宽
    int height;               // 分辨率高
    int offset_x;             // 显示偏移
    int offset_y;
    bool swap_xy;
    bool mirror_x;
    bool mirror_y;
    bool invert_color;
} display_panel_spi_params_t;

// ILI9341 面板创建
esp_err_t display_panel_create_ili9341(const display_panel_spi_params_t* params, display_panel_result_t* out);

// JD9853 面板创建
esp_err_t display_panel_create_jd9853(const display_panel_spi_params_t* params, display_panel_result_t* out);

// GC9A01 面板创建（1.28"）
esp_err_t display_panel_create_gc9a01(const display_panel_spi_params_t* params, display_panel_result_t* out);

// ST7735P3 面板创建（0.9"）
esp_err_t display_panel_create_st7735(const display_panel_spi_params_t* params, display_panel_result_t* out);

// GC9106 面板创建（1.32" 128x128）
esp_err_t display_panel_create_gc9106(const display_panel_spi_params_t* params, display_panel_result_t* out);

// ST77916 / ST77912 面板创建（可选 vendor init 表）
typedef struct display_panel_st779x_vendor_t {
    const void* init_cmds;     // 指向 st77916_lcd_init_cmd_t 或兼容表
    int init_cmds_size;        // 表项数量
} display_panel_st779x_vendor_t;

esp_err_t display_panel_create_st77916(const display_panel_spi_params_t* params,
                                       const display_panel_st779x_vendor_t* vendor,
                                       display_panel_result_t* out);

esp_err_t display_panel_create_st77912(const display_panel_spi_params_t* params,
                                       const display_panel_st779x_vendor_t* vendor,
                                       display_panel_result_t* out);

#ifdef __cplusplus
}
#endif
