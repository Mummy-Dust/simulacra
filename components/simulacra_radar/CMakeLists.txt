idf_component_register(
    SRCS "radar_geom.c" "radar_ui.c" "radar_wire.c" "radar_gfx.c" "radar_render.c"
    INCLUDE_DIRS "."
    REQUIRES mbedtls
)
