idf_component_register(
    SRCS "radar_geom.c" "radar_ui.c" "radar_wire.c" "radar_gfx.c" "radar_render.c" "law3.c" "learn_wire.c" "learn_db.c"
    INCLUDE_DIRS "."
    REQUIRES mbedtls
)
