#include "lcd_driver_factory.h"
#include <esp_check.h>
#include <esp_lcd_jd9853.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static esp_err_t create_spi_io(const display_panel_spi_params_t* p, esp_lcd_panel_io_handle_t* io)
{
    esp_lcd_panel_io_spi_config_t io_config = {};
    io_config.cs_gpio_num = p->cs_gpio_num;
    io_config.dc_gpio_num = p->dc_gpio_num;
    io_config.spi_mode = 0;
    // 降低SPI时钟频率以减少条纹问题，但保持足够的速度
    io_config.pclk_hz = (p->pclk_hz > 20 * 1000 * 1000) ? 20 * 1000 * 1000 : p->pclk_hz;
    io_config.trans_queue_depth = p->trans_queue_depth;
    io_config.lcd_cmd_bits = p->lcd_cmd_bits;
    io_config.lcd_param_bits = p->lcd_param_bits;
    return esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)p->host, &io_config, io);
}

esp_err_t display_panel_create_jd9853(const display_panel_spi_params_t* p, display_panel_result_t* out)
{
    if (!p || !out) return ESP_ERR_INVALID_ARG;
    esp_lcd_panel_io_handle_t io = nullptr;
    esp_lcd_panel_handle_t panel = nullptr;
    esp_err_t _err = create_spi_io(p, &io);
    if (_err != ESP_OK) return _err;

    esp_lcd_panel_dev_config_t panel_config = {};
    panel_config.reset_gpio_num = p->reset_gpio_num;
    panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;  // 修复：使用RGB顺序
    panel_config.bits_per_pixel = 16;

    esp_err_t err = esp_lcd_new_panel_jd9853(io, &panel_config, &panel);
    if (err != ESP_OK) return err;

    esp_lcd_panel_reset(panel);
    esp_lcd_panel_init(panel);
    
    // 添加延时确保显示稳定
    vTaskDelay(pdMS_TO_TICKS(100));
    
    if (p->invert_color) esp_lcd_panel_invert_color(panel, true);
    if (p->swap_xy) esp_lcd_panel_swap_xy(panel, true);
    if (p->mirror_x || p->mirror_y) esp_lcd_panel_mirror(panel, p->mirror_x, p->mirror_y);
    
    // 再次延时确保配置生效
    vTaskDelay(pdMS_TO_TICKS(50));
    
    esp_lcd_panel_disp_on_off(panel, true);

    out->io_handle = io;
    out->panel_handle = panel;
    return ESP_OK;
}
