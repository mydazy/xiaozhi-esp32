idf_component_register(
    SRCS "i2c_bus_worker.c"
    INCLUDE_DIRS "include"
    REQUIRES driver freertos esp_timer
)
