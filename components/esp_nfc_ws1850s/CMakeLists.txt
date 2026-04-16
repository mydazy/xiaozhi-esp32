idf_component_register(
    SRCS "src/esp_nfc_ws1850s.cc"
         "src/nfc.c"
         "src/ws1850_iic.c"
         "src/iso14443b.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    PRIV_REQUIRES esp_driver_i2c esp_driver_gpio esp_timer freertos
)
