idf_component_register(
    SRCS "simulacra_main.c" "churn_adv.c" "roster.c" "churn_selftest.c" "churn.c" "settings.c" "templates.c" "rf_model.c" "observe.c" "generate.c" "probe.c" "sniff.c" "espnow_sniff.c" "drift.c" "coexist.c" "detect.c" "webui.c" "esp_now_link.c" "learn.c" "sig_store.c" "fleet.c"
    INCLUDE_DIRS "."
    REQUIRES bt nvs_flash driver esp_wifi esp_netif esp_event esp_http_server simulacra_radar
    EMBED_TXTFILES "webui_index.html"
)

# Forward build-time gate flags passed via `idf.py -DFLAG=VALUE` to the C preprocessor,
# so e.g. `idf.py -DCHURN_SELFTEST=1` or `-DSIMULACRA_LEARN=0` take effect without editing
# source. A flag not passed on the command line is left undefined and the source `#ifndef`
# default applies.
foreach(flag
    CHURN_SELFTEST SIMULACRA_LEARN SIMULACRA_OBSERVE SIMULACRA_DETECT SIMULACRA_WEBUI
    SIMULACRA_ESPNOW SIMULACRA_PROBE SIMULACRA_ESPNOW_SNIFF SIMULACRA_SNIFF
    SIMULACRA_BOARD_XIAO_C6 SIMULACRA_TRACE)
    if(DEFINED ${flag})
        target_compile_definitions(${COMPONENT_LIB} PRIVATE ${flag}=${${flag}})
    endif()
endforeach()
