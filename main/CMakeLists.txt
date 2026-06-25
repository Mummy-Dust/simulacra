idf_component_register(
    SRCS "simulacra_main.c" "churn_adv.c"
    INCLUDE_DIRS "."
    REQUIRES bt nvs_flash
)
