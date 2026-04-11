# ESP LCD JD9853

ESP-IDF LCD driver component for JD9853 (Jadard) TFT LCD controller.

## Specifications

| Parameter | Value |
|-----------|-------|
| Driver IC | JD9853 (Jadard) |
| Resolution | 240 x 284 |
| Display Size | 1.83 inch |
| Panel Type | IPS |
| Interface | SPI (4-wire) |
| Color Depth | RGB565 (16-bit) / RGB666 (18-bit) |

## Supported Panels

- BOE WV018LZQ-N80-3QP1 (1.83" 240x284 IPS)

## Usage

```c
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_jd9853.h"

// 1. Configure SPI bus
spi_bus_config_t bus_cfg = JD9853_PANEL_BUS_SPI_CONFIG(
    LCD_SCLK_PIN,   // SCLK
    LCD_MOSI_PIN,   // MOSI
    JD9853_LCD_H_RES * JD9853_LCD_V_RES * sizeof(uint16_t)
);
spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO);

// 2. Configure panel IO
esp_lcd_panel_io_handle_t io_handle;
esp_lcd_panel_io_spi_config_t io_cfg = JD9853_PANEL_IO_SPI_CONFIG(
    LCD_CS_PIN,     // CS
    LCD_DC_PIN,     // DC
    NULL,           // Callback
    NULL            // Callback context
);
esp_lcd_new_panel_io_spi(SPI2_HOST, &io_cfg, &io_handle);

// 3. Create panel
esp_lcd_panel_handle_t panel_handle;
esp_lcd_panel_dev_config_t panel_cfg = {
    .reset_gpio_num = LCD_RST_PIN,
    .rgb_endian = LCD_RGB_ENDIAN_BGR,
    .bits_per_pixel = 16,
};
esp_lcd_new_panel_jd9853(io_handle, &panel_cfg, &panel_handle);

// 4. Initialize
esp_lcd_panel_reset(panel_handle);
esp_lcd_panel_init(panel_handle);
esp_lcd_panel_disp_on_off(panel_handle, true);
```

## Custom Initialization

To use a custom initialization sequence:

```c
static const jd9853_lcd_init_cmd_t custom_init_cmds[] = {
    {0xDF, (uint8_t[]){0x98, 0x53}, 2, 0},
    // ... more commands
    {0x11, NULL, 0, 120},
    {0x29, NULL, 0, 50},
};

jd9853_vendor_config_t vendor_cfg = {
    .init_cmds = custom_init_cmds,
    .init_cmds_size = sizeof(custom_init_cmds) / sizeof(custom_init_cmds[0]),
};

esp_lcd_panel_dev_config_t panel_cfg = {
    .reset_gpio_num = LCD_RST_PIN,
    .rgb_endian = LCD_RGB_ENDIAN_BGR,
    .bits_per_pixel = 16,
    .vendor_config = &vendor_cfg,
};
```

## Datasheet

JD9853 datasheet can be found at [Jadard Technology](http://www.jadard.com/).

## License

Apache-2.0