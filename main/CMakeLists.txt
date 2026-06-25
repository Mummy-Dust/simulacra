idf_component_register(
    SRCS "simulacra_main.c" "churn_adv.c" "roster.c" "churn_selftest.c"
    INCLUDE_DIRS "."
    REQUIRES bt nvs_flash
)
