idf_component_register(
    SRCS "simulacra_main.c" "churn_adv.c" "roster.c" "churn_selftest.c" "churn.c" "templates.c" "rf_model.c" "observe.c" "generate.c" "probe.c" "sniff.c" "espnow_sniff.c" "drift.c" "coexist.c" "detect.c" "webui.c" "esp_now_link.c"
    INCLUDE_DIRS "."
    REQUIRES bt nvs_flash driver esp_wifi esp_netif esp_event esp_http_server simulacra_radar
    EMBED_TXTFILES "webui_index.html"
)
