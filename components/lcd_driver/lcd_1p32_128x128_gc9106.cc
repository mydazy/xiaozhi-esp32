#include "lcd_driver_factory.h"

// 临时适配：GC9106 功能上与 ST7735P3 接口相近，可先复用 ST7735 创建流程点亮
// 后续可替换为专用 esp_lcd_new_panel_gc9106 + vendor init 表
extern "C" esp_err_t display_panel_create_gc9106(const display_panel_spi_params_t* p, display_panel_result_t* out);
extern "C" esp_err_t display_panel_create_st7735(const display_panel_spi_params_t* p, display_panel_result_t* out);

extern "C" esp_err_t display_panel_create_gc9106(const display_panel_spi_params_t* p, display_panel_result_t* out)
{
    return display_panel_create_st7735(p, out);
}
