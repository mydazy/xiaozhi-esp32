# lcd_driver 组件使用说明

本组件为多屏幕驱动的统一封装，按“芯片_尺寸_分辨率.cc”命名，便于识别与维护。

## 已适配芯片与命名
- 0.71" GC9D01 → 走 GC9 系列接口（兼容初始化表）
- 0.9"  ST7735P3 → `st7735p3_0p9_160x80.cc`
- 1.28" GC9A01 → `gc9a01_1p28_240x240.cc`
- 1.83" JD9853 → `jd9853_1p83_240x284.cc`
- 兼容：ST77916 → `st77916_1p28_240x240.cc`，ST77912 → `st77912_0p71_160x160.cc`

## 依赖
- 组件依赖已在 `components/lcd_driver/CMakeLists.txt` 中声明：
  - 基础：`esp_lcd`, `esp_lcd_panel_io`
  - 芯片：`esp_lcd_gc9a01`, `esp_lcd_st7735`
- 主工程 `main/CMakeLists.txt` 中需确保：
```
idf_component_register(... REQUIRES lcd_driver ...)
```

## MyDazy-E20 屏幕选择（烧录前）
- 执行 `idf.py menuconfig`
- Board Type 选择 `MyDazy-E20`
- 进入 `MYDAZY_E20 LCD Type`：
  - 0.71" GC9D01
  - 0.9"  ST7735P3
  - 1.28" GC9A01

## 代码示例（板级初始化片段）
```c
#include "lcd_driver_factory.h"

static void create_display(spi_host_device_t host, int cs, int dc, int rst,
                           int w, int h, bool swap_xy, bool mx, bool my,
                           display_panel_result_t* out) {
  display_panel_spi_params_t p = {
    .host = host, .cs_gpio_num = cs, .dc_gpio_num = dc, .reset_gpio_num = rst,
    .pclk_hz = 40*1000*1000, .trans_queue_depth = 10,
    .lcd_cmd_bits = 8, .lcd_param_bits = 8,
    .width = w, .height = h, .offset_x = 0, .offset_y = 0,
    .swap_xy = swap_xy, .mirror_x = mx, .mirror_y = my, .invert_color = false,
  };
#if CONFIG_MYDAZY_LCD_GC9A01
  ESP_ERROR_CHECK(display_panel_create_gc9a01(&p, out));
#elif CONFIG_MYDAZY_LCD_ST7735P3
  ESP_ERROR_CHECK(display_panel_create_st7735(&p, out));
#elif CONFIG_MYDAZY_LCD_GC9D01
  // GC9D01 复用 GC9 系列接口（需或可传 vendor 表的版本可后续扩展）
  ESP_ERROR_CHECK(display_panel_create_gc9a01(&p, out));
#endif
}
```

## 双目（双屏）接入建议
- 复用同一 `DC/BL`，区分两路 `CS`（如 `CS1/CS2`），分别调用一次创建函数获取两个 `panel_handle`。
- LVGL 侧分别注册成两个 `lv_display_t*`，用现有 `LcdDoubleDisplay` 或自定义 UI 进行组合。

## 备注
- GC9D01 需要供应商初始化表以获得最佳显示效果。当前可先用兼容表点亮，后续替换为官方参数即可。
