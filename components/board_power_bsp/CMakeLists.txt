idf_component_register(
    SRCS "board_power_bsp.cpp"
    PRIV_REQUIRES esp_driver_gpio
    INCLUDE_DIRS "./"
)
