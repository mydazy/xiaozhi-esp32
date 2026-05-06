idf_component_register(
    SRCS "sc7a20h.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_i2c esp_driver_gpio
    PRIV_REQUIRES esp_hw_support esp_timer esp_common
)
