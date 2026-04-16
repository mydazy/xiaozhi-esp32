idf_component_register(
    SRCS "axs5106l_touch.cc" "axs5106l_upgrade.cc"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_driver_i2c esp_driver_gpio esp_lvgl_port esp_timer
)
