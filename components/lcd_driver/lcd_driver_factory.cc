#include "lcd_driver_factory.h"
#include <string.h>

__attribute__((unused)) static esp_err_t create_spi_io(const display_panel_spi_params_t* p,
                               esp_lcd_panel_io_handle_t* io)
{
    esp_lcd_panel_io_spi_config_t io_config = {};
    io_config.cs_gpio_num = p->cs_gpio_num;
    io_config.dc_gpio_num = p->dc_gpio_num;
    io_config.spi_mode = 0;
    io_config.pclk_hz = p->pclk_hz;
    io_config.trans_queue_depth = p->trans_queue_depth;
    io_config.lcd_cmd_bits = p->lcd_cmd_bits;
    io_config.lcd_param_bits = p->lcd_param_bits;
    return esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)p->host, &io_config, io);
}

// 让工厂头文件只提供声明；真正实现由各芯片文件提供，避免重复/未定义
extern "C" esp_err_t display_panel_create_ili9341(const display_panel_spi_params_t* p, display_panel_result_t* out);
extern "C" esp_err_t display_panel_create_jd9853(const display_panel_spi_params_t* p, display_panel_result_t* out);
extern "C" esp_err_t display_panel_create_st77916(const display_panel_spi_params_t* p,
                                       const display_panel_st779x_vendor_t* vendor,
                                       display_panel_result_t* out);
