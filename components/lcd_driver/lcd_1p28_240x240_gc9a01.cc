#include "lcd_driver_factory.h"
#include <esp_lcd_gc9a01.h>

static esp_err_t create_spi_io(const display_panel_spi_params_t* p, esp_lcd_panel_io_handle_t* io)
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

extern "C" esp_err_t display_panel_create_gc9a01(const display_panel_spi_params_t* p, display_panel_result_t* out)
{
    if (!p || !out) return ESP_ERR_INVALID_ARG;
    esp_lcd_panel_io_handle_t io = nullptr;
    esp_lcd_panel_handle_t panel = nullptr;
    esp_err_t _err = create_spi_io(p, &io);
    if (_err != ESP_OK) return _err;

    esp_lcd_panel_dev_config_t panel_config = {};
    panel_config.reset_gpio_num = p->reset_gpio_num;
    panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_config.bits_per_pixel = 16;

    esp_err_t err = esp_lcd_new_panel_gc9a01(io, &panel_config, &panel);
    if (err != ESP_OK) return err;

    esp_lcd_panel_reset(panel);
    esp_lcd_panel_init(panel);
    if (p->invert_color) esp_lcd_panel_invert_color(panel, true);
    if (p->swap_xy) esp_lcd_panel_swap_xy(panel, true);
    if (p->mirror_x || p->mirror_y) esp_lcd_panel_mirror(panel, p->mirror_x, p->mirror_y);
    esp_lcd_panel_disp_on_off(panel, true);

    out->io_handle = io;
    out->panel_handle = panel;
    return ESP_OK;
}
