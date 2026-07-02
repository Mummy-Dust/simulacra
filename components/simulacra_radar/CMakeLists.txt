idf_component_register(
    SRCS "radar_geom.c" "radar_ui.c" "radar_wire.c"
    INCLUDE_DIRS "."
    REQUIRES mbedtls
)
