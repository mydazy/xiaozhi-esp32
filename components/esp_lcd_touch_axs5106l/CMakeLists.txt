idf_component_register(
    SRCS "axs5106l_touch.c" "axs5106l_upgrade.c"
    INCLUDE_DIRS "include"
    REQUIRES mydazy__i2c_bus_worker esp_lvgl_port
    PRIV_REQUIRES esp_driver_i2c esp_driver_gpio esp_timer
)
