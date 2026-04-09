# Conditional driver selection via Kconfig
# 仅编译被选择的屏幕驱动，避免无关文件进入固件

set(DRIVER_SRCS "lcd_driver_factory.cc")
set(DRIVER_REQS esp_lcd)
set(DRIVER_PRIV_REQS)

# MyDazy-E20 屏幕选择（menuconfig）
if(CONFIG_MYDAZY_LCD_GC9A01)
    list(APPEND DRIVER_SRCS lcd_1p28_240x240_gc9a01.cc)
    list(APPEND DRIVER_PRIV_REQS espressif__esp_lcd_gc9a01)
    list(APPEND DRIVER_PRIV_REQS esp_lcd_gc9a01)
endif()

if(CONFIG_MYDAZY_LCD_ST7735P3)
    list(APPEND DRIVER_SRCS lcd_0p9_160x80_st7735p3.cc)
    list(APPEND DRIVER_PRIV_REQS esp_lcd_st7735)
endif()

if(CONFIG_MYDAZY_LCD_GC9D01)
    # GC9D01 暂复用 0.71 160x160 适配，后续替换为专用驱动
    list(APPEND DRIVER_SRCS lcd_0p71_160x160_gc9d01.cc)
    list(APPEND DRIVER_PRIV_REQS espressif__esp_lcd_st77916)
endif()

# 其他板型（如 MyDazy-P30）常用驱动：保持可用（若无引用，链接器会剔除）
list(APPEND DRIVER_SRCS lcd_1p83_240x284_jd9853.cc)
list(APPEND DRIVER_SRCS lcd_1p28_240x240_st77916.cc)
list(APPEND DRIVER_SRCS lcd_1p32_128x128_gc9106.cc)

# 芯片专用头文件依赖（本组件内部使用，放 PRIV_REQUIRES）
list(APPEND DRIVER_PRIV_REQS ckv__esp_lcd_jd9853)
list(APPEND DRIVER_PRIV_REQS espressif__esp_lcd_st77916)
list(APPEND DRIVER_PRIV_REQS espressif__esp_lcd_gc9a01)
list(APPEND DRIVER_PRIV_REQS esp_lcd_gc9a01)

idf_component_register(
    SRCS ${DRIVER_SRCS}
    INCLUDE_DIRS "include"
    REQUIRES ${DRIVER_REQS}
    PRIV_REQUIRES ${DRIVER_PRIV_REQS}
)
