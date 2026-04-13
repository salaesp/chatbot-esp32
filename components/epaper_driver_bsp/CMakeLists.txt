idf_component_register(
    SRCS "epaper_driver_bsp.cpp"
    PRIV_REQUIRES
        esp_driver_gpio
        esp_driver_spi
    REQUIRES
        esp_driver_gpio
        esp_driver_spi
    INCLUDE_DIRS "./"
)
