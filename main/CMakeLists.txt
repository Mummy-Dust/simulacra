idf_component_register(
    SRCS "simulacra_main.c" "churn_adv.c" "roster.c" "churn_selftest.c" "churn.c" "templates.c" "rf_model.c" "observe.c"
    INCLUDE_DIRS "."
    REQUIRES bt nvs_flash
)
