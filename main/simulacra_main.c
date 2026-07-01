// Simulacra — BLE privacy / anti-tracking decoy (ESP-IDF + NimBLE), ESP32-C5/C6.
// Fork of Splinter by 0xXyc (Jacob Swiz).
//
// v2 builds a synthetic *population*: plausible-but-fake BLE devices that persist
// and turn over like a real crowd, so a tracker in a space you control sees lots
// of ordinary traffic and your real device(s) don't stand out. The engine lives in
// roster.* (identity pool) and churn.* (active-set / cooldown / time-slice); this
// file is the slim entry point.
//
// DEFAULT build (all flags below = 0): combined BLE + Wi-Fi coexist decoy (M8).
//   BLE ext-adv and Wi-Fi synthetic probe-request injection run concurrently via
//   ESP-IDF SW coexistence. The coexist coordinator (coexist.c) live-re-profiles the
//   room every ~10 min (Ward/C5) or ~5 min (Shade/C6): it observes the ambient BLE
//   environment, updates the rf_model, and reshapes the synthetic population without
//   a reflash. On Shade (C6), a high drift score triggers anti-entourage: accelerated
//   churn (3× speed) that decays linearly back to normal over ~2 min.
//   BLE-only fallback if Wi-Fi init fails.
//
// Dev / verification flags (set exactly one to 1 to override the default):
//   SIMULACRA_PROBE=1   Wi-Fi-only probe injector (NimBLE not started)
//   SIMULACRA_SNIFF=1   Wi-Fi probe sniffer — promiscuous capture, log counts
//                       (verification tool / M9 observe seed)
//   SIMULACRA_OBSERVE=1 BLE-only ambient observe + model (never advertises)
//   CHURN_SELFTEST=1    On-target host-logic self-test; radio idle, PASS/FAIL serial
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
#include "probe.h"
#include "sniff.h"
#include "coexist.h"
#include "detect.h"
#include "webui.h"

#if !defined(CONFIG_BT_NIMBLE_EXT_ADV)
#error "Simulacra requires CONFIG_BT_NIMBLE_EXT_ADV (see sdkconfig.defaults.esp32c6)"
#endif

// Normal (shipped) mode. Set to 1 to build the on-target self-test instead.
#ifndef CHURN_SELFTEST
#define CHURN_SELFTEST 0
#endif

// Observe mode (M5): set to 1 to passively scan + model the ambient BLE environment
// (never advertises). Takes precedence over CHURN_SELFTEST when set.
#ifndef SIMULACRA_OBSERVE
#define SIMULACRA_OBSERVE 0
#endif

// Threat Radar (M9): passive follower detection alongside the decoy. Default ON.
#ifndef SIMULACRA_DETECT
#define SIMULACRA_DETECT 1
#endif

// Web UI: on-demand open config AP for ~2 min at boot, then hand Wi-Fi to the decoy.
// Local MVP (open AP, no auth) -- default ON for now.
#ifndef SIMULACRA_WEBUI
#define SIMULACRA_WEBUI 1
#endif

// Probe mode (M7): set to 1 for Wi-Fi-only synthetic probe-request injection (NimBLE not started).
#ifndef SIMULACRA_PROBE
#define SIMULACRA_PROBE 0
#endif

// Sniff mode (verification / Wi-Fi-observe seed): promiscuous-capture probe requests, log counts.
#ifndef SIMULACRA_SNIFF
#define SIMULACRA_SNIFF 0
#endif

static const char *TAG = "simulacra";
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
    // Combined coexist decoy (default): set up the BLE population, then hand the tick loop
    // to the coordinator (it owns churn_tick + Wi-Fi bursts + re-profile). roster_init()
    // MUST precede churn_init(): churn pulls identities straight from the roster pool.
    roster_init();
    {
        rf_model_t m;
        if (rf_model_load_nvs(&m) == 0 && m.total_obs >= GEN_MIN_OBS) {
            uint8_t at = generate_active_target(&m);
            churn_set_active_target(at);
            ESP_LOGW(TAG, "population-match: pop=%u active_target=%u",
                     (unsigned)(m.pop_ewma + 0.5f), (unsigned)at);
        }
    }
    churn_set_apply(churn_adv_apply);
    churn_init((uint32_t)(esp_timer_get_time() / 1000));
    detect_reset();
    detect_set_enabled(SIMULACRA_DETECT);   // M9 master enable (default on); coexist wires the rest
#if SIMULACRA_WEBUI
    coexist_set_wifi_enabled(false);   // keep Wi-Fi free for the config AP
    coexist_start();                    // BLE churn + detection start now
    webui_run_config_window(120000);    // open AP + dashboard for 2 min (BLE keeps churning)
    coexist_set_wifi_enabled(true);     // AP down -> Wi-Fi STA up, probe injection resumes
#else
    coexist_start();
#endif
    for (;;) {                                          // this task idles; coexist runs the show
        ESP_LOGW(TAG, "decoy alive active=%u", (unsigned)churn_active_count());
        vTaskDelay(pdMS_TO_TICKS(5000));
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
// BOARD-SPECIFIC: GPIO3/GPIO14 are the XIAO C6's antenna-switch pins. Other ESP32-C6 boards
// (e.g. the SparkFun Thing Plus C6, which selects its antenna with a hardware jumper) must NOT
// drive them, so this is gated on SIMULACRA_BOARD_XIAO_C6 (default 0 = don't touch the pins).
// Build for the XIAO C6 with -DSIMULACRA_BOARD_XIAO_C6=1 (and SIMULACRA_EXT_ANTENNA as needed).
#ifndef SIMULACRA_BOARD_XIAO_C6
#define SIMULACRA_BOARD_XIAO_C6 0
#endif
#if SIMULACRA_BOARD_XIAO_C6
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
#if SIMULACRA_BOARD_XIAO_C6
    xiao_c6_select_antenna();
#endif

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

#if SIMULACRA_PROBE
    // Wi-Fi-only mode: init Wi-Fi + start the probe injector; NimBLE is never initialized.
    probe_start();
    return;
#endif
#if SIMULACRA_SNIFF
    // Wi-Fi-only verification mode: promiscuous-capture probe requests, log counts. NimBLE idle.
    sniff_start();
    return;
#endif

    // NimBLE logs every GAP procedure at INFO; keep only warnings+.
    esp_log_level_set("NimBLE", ESP_LOG_WARN);

    ESP_ERROR_CHECK(nimble_port_init());

    ble_hs_cfg.sync_cb  = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    ble_svc_gap_init();

    nimble_port_freertos_init(nimble_host_task);
    xTaskCreate(simulacra_task, "simulacra", 4096, NULL, 5, NULL);
}
