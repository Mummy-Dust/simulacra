idf_component_register(
    SRCS "simulacra_main.c" "churn_adv.c" "roster.c" "churn_selftest.c" "churn.c" "templates.c" "rf_model.c" "observe.c" "generate.c" "probe.c" "sniff.c" "drift.c"
    INCLUDE_DIRS "."
    REQUIRES bt nvs_flash driver esp_wifi esp_netif esp_event
)
