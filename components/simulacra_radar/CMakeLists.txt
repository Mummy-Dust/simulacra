idf_component_register(
    SRCS "radar_geom.c" "radar_ui.c" "radar_wire.c" "radar_gfx.c" "radar_render.c" "law3.c" "learn_wire.c" "learn_db.c" "sig_match.c" "sig_db.c" "sig_wire.c" "sig_seed.c" "config_wire.c" "enroll_wire.c"
    INCLUDE_DIRS "."
    REQUIRES mbedtls tweetnacl
)
