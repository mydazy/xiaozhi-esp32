idf_component_register(
    SRCS "mydazy_codec_ctrl_i2c.c"
    INCLUDE_DIRS "include"
    REQUIRES driver
    PRIV_REQUIRES espressif__esp_codec_dev mydazy__i2c_bus_worker
)
