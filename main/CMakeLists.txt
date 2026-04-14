idf_component_register(
    SRCS "main.cpp"
    PRIV_REQUIRES
        board_power_bsp
        epaper_driver_bsp
        audio_bsp
        esp_driver_gpio
        esp_wifi
        esp_event
        esp_netif
        nvs_flash
        esp_http_client
        esp-tls
        mbedtls
        lwip
        esp_timer
        espressif__cjson
        lvgl
    EMBED_TXTFILES "ca.pem"
    INCLUDE_DIRS "./"
)
