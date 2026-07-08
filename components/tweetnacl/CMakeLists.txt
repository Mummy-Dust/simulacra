idf_component_register(
    SRCS "tweetnacl.c" "randombytes.c"
    INCLUDE_DIRS "."
    REQUIRES esp_hw_support
)
