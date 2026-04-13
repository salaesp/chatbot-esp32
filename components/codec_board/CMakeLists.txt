# codec_board — CMakeLists.txt fixed for ESP-IDF v6.0
# 'driver' removed; replaced with explicit sub-components

set(COMPONENT_PRIV_REQUIRE
    esp_driver_i2c
    esp_driver_i2s
    espressif__esp_codec_dev
    fatfs
    esp_lcd
)

idf_component_register(
    SRCS
        "cfg_parse.c"
        "codec_board.c"
        "codec_init.c"
        "dummy_codec.c"
        "lcd_init.c"
        "drv/tca9554.c"
    INCLUDE_DIRS ./include
    PRIV_INCLUDE_DIRS drv
    REQUIRES "${COMPONENT_PRIV_REQUIRE}"
    EMBED_TXTFILES "board_cfg.txt"
)
