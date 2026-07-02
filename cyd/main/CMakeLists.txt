idf_component_register(
    SRCS "cyd_main.c"
    INCLUDE_DIRS "."
    REQUIRES simulacra_radar esp_lcd esp_wifi nvs_flash mbedtls driver esp_timer
)
