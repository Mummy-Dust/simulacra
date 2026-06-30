// splinter v2 — BLE privacy / anti-tracking decoy (ESP-IDF + NimBLE), ESP32-C6.
//
// v2 builds a synthetic *population*: plausible-but-fake BLE devices that persist
// and turn over like a real crowd, so a tracker in a space you control sees lots
// of ordinary traffic and your real device(s) don't stand out. The engine lives in
// roster.* (identity pool) and churn.* (active-set / cooldown / time-slice); this
// file is the slim entry point.
//
// Two build modes, selected by CHURN_SELFTEST below:
//   * CHURN_SELFTEST=0 (normal, shipped): drive the decoy population — churn_tick
//     advances the state machine every CHURN_TICK_MS and applies identity changes
//     to the 4 ext-adv radios via the NimBLE adapter (churn_adv_apply).
//   * CHURN_SELFTEST=1 (M3 dev): run the on-target logic self-test with the radio
//     idle, loop-printing PASS/FAIL over serial. No advertising.
//
// Decoy guardrails (see decoy_vendors.h): advertising is NON-CONNECTABLE and the
// payload is never shaped like Apple Continuity / Microsoft Swift Pair / Google
// Fast Pair, so it creates realistic presence without popping pairing dialogs.

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "nvs_flash.h"
#include "driver/gpio.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "services/gap/ble_svc_gap.h"

#include "identity.h"
#include "roster.h"
#include "churn.h"
#include "churn_adv.h"
#include "churn_selftest.h"
#include "observe.h"
#include "rf_model.h"
#include "generate.h"

#if !defined(CONFIG_BT_NIMBLE_EXT_ADV)
#error "Splinter v2 requires CONFIG_BT_NIMBLE_EXT_ADV (see sdkconfig.defaults.esp32c6)"
#endif

// Normal (shipped) mode. Set to 1 to build the on-target self-test instead.
#ifndef CHURN_SELFTEST
#define CHURN_SELFTEST 1
#endif

// Observe mode (M5): set to 1 to passively scan + model the ambient BLE environment
// (never advertises). Takes precedence over CHURN_SELFTEST when set.
#ifndef SIMULACRA_OBSERVE
#define SIMULACRA_OBSERVE 0
#endif

static const char *TAG = "splinter";
static volatile bool s_host_synced = false;

static void simulacra_task(void *arg)
{
    while (!s_host_synced) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
#if SIMULACRA_OBSERVE
    // M5 observe mode: passively scan + model the ambient BLE environment. The scan
    // callback (NimBLE host task) does all the modeling/persisting; this task just idles.
    observe_start(esp_random());
    for (;;) { observe_heartbeat(); vTaskDelay(pdMS_TO_TICKS(2000)); }
#elif CHURN_SELFTEST
    int fails = churn_selftest_run();
    for (;;) {  // loop-print so the USB-JTAG reader reliably catches it
        ESP_LOGW(TAG, "SELFTEST result: %s (fails=%d)", fails ? "FAIL" : "PASS", fails);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
#else
    // Drive the synthetic BLE population: each tick advances the active-set /
    // cooldown / time-slice state machine and reprograms the 4 ext-adv radios via
    // the NimBLE adapter. vTaskDelay yields every tick so the single-core task
    // watchdog never fires.
    //
    // roster_init() MUST run before churn_init(): churn pulls identities straight
    // from the roster pool, and an uninitialized pool yields adv_itvl_ms=0, which
    // the controller rejects (HCI 0x12). The self-tests init the roster themselves.
    roster_init();
    // M6 population-match: size the active set to the observed population (persona-tuned).
    {
        rf_model_t m;
        if (rf_model_load_nvs(&m) == 0 && m.total_obs >= GEN_MIN_OBS)
            churn_set_active_target(generate_active_target(&m));
    }
    churn_set_apply(churn_adv_apply);
    churn_init((uint32_t)(esp_timer_get_time() / 1000));
    for (;;) {
        churn_tick((uint32_t)(esp_timer_get_time() / 1000));
        vTaskDelay(pdMS_TO_TICKS(CHURN_TICK_MS));
    }
#endif
}

static void on_sync(void)
{
    s_host_synced = true;
    ESP_LOGI(TAG, "NimBLE host synced");
}

static void on_reset(int reason)
{
    s_host_synced = false;
    ESP_LOGW(TAG, "NimBLE host reset, reason=%d", reason);
}

static void nimble_host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

// Seeed XIAO ESP32-C6 antenna switch: GPIO3 LOW enables the RF switch; GPIO14 selects the
// antenna (LOW = onboard ceramic, HIGH = external U.FL). Must run before BLE start.
// IMPORTANT: selecting external (HIGH) with no U.FL antenna attached degrades RF. Set
// SIMULACRA_EXT_ANTENNA to 0 if running on the onboard antenna.
// C6-ONLY: GPIO3/GPIO14 are XIAO-C6-specific pins; other boards (e.g. the C5 DevKit, which
// selects its antenna with a hardware jumper) must not drive them, so guard on the target.
#if CONFIG_IDF_TARGET_ESP32C6
#ifndef SIMULACRA_EXT_ANTENNA
#define SIMULACRA_EXT_ANTENNA 1   // 1 = external U.FL (fitted on this build), 0 = onboard ceramic
#endif
static void xiao_c6_select_antenna(void)
{
    gpio_reset_pin(GPIO_NUM_3);
    gpio_set_direction(GPIO_NUM_3, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_NUM_3, 0);                       // enable the RF switch (active low)
    gpio_reset_pin(GPIO_NUM_14);
    gpio_set_direction(GPIO_NUM_14, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_NUM_14, SIMULACRA_EXT_ANTENNA ? 1 : 0);  // 1 = external U.FL, 0 = onboard
}
#endif

void app_main(void)
{
#if CONFIG_IDF_TARGET_ESP32C6
    xiao_c6_select_antenna();
#endif

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    // NimBLE logs every GAP procedure at INFO; keep only warnings+.
    esp_log_level_set("NimBLE", ESP_LOG_WARN);

    ESP_ERROR_CHECK(nimble_port_init());

    ble_hs_cfg.sync_cb  = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    ble_svc_gap_init();

    nimble_port_freertos_init(nimble_host_task);
    xTaskCreate(simulacra_task, "splinter", 4096, NULL, 5, NULL);
}
