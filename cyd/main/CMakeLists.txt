idf_component_register(
    SRCS "cyd_main.c"
    INCLUDE_DIRS "."
    REQUIRES simulacra_radar esp_lcd esp_wifi esp_netif esp_event nvs_flash mbedtls driver esp_timer fatfs sdmmc
)

# Forward build-time gate flags passed via `idf.py -DFLAG=VALUE` to the C preprocessor,
# mirroring main/CMakeLists.txt. SIMULACRA_CONFIG_CTRL enables the Vigil CONTROL page + SEND.
foreach(flag SIMULACRA_CONFIG_CTRL)
    if(DEFINED ${flag})
        target_compile_definitions(${COMPONENT_LIB} PRIVATE ${flag}=${${flag}})
    endif()
endforeach()
