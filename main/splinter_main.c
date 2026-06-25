// splinter v2 — BLE privacy / anti-tracking decoy (ESP-IDF + NimBLE), ESP32-C6.
//
// v2 builds a synthetic *population*: plausible-but-fake BLE devices that persist
// and turn over like a real crowd, so a tracker in a space you control sees lots
// of ordinary traffic and your real device(s) don't stand out. The engine lives in
// roster.* (identity pool) and churn.* (active-set / cooldown / time-slice); this
// file is the slim entry point.
//
// Two build modes, selected by CHURN_SELFTEST below:
//   * CHURN_SELFTEST=1 (M3 Tasks 2-5): run the on-target logic self-test with the
//     radio idle, loop-printing PASS/FAIL over serial. No advertising.
//   * CHURN_SELFTEST=0 (normal): drive the decoy population on the radios. Until
//     the churn engine is wired in (Task 6), normal mode runs simulacra_run()'s
//     4-instance smoke driver proving concurrent ext-adv.
//
// Decoy guardrails (see decoy_vendors.h): advertising is NON-CONNECTABLE and the
// payload is never shaped like Apple Continuity / Microsoft Swift Pair / Google
// Fast Pair, so it creates realistic presence without popping pairing dialogs.

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_random.h"
#include "nvs_flash.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nimble/hci_common.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"

#include "identity.h"
#include "churn_adv.h"
#include "churn_selftest.h"

#if !defined(CONFIG_BT_NIMBLE_EXT_ADV)
#error "Splinter v2 requires CONFIG_BT_NIMBLE_EXT_ADV (see sdkconfig.defaults.esp32c6)"
#endif

// M3 Tasks 2-5: build the on-target self-test (radio idle). Task 6 sets this to 0.
#ifndef CHURN_SELFTEST
#define CHURN_SELFTEST 1
#endif

static const char *TAG = "splinter";
static volatile bool s_host_synced = false;

#if !CHURN_SELFTEST
// ---- M3 Task 1 smoke driver (normal-mode only; replaced by churn in Task 6) ----

// Build a valid random-static address: 6 random bytes with the two most
// significant bits set. Regenerates the astronomically rare all-zero / all-ones
// random part that NimBLE would reject.
static void make_random_static_addr(uint8_t out[6])
{
    for (;;) {
        for (int i = 0; i < 6; i++) {
            out[i] = (uint8_t)(esp_random() & 0xff);
        }
        out[5] |= 0xc0;
        int ones = __builtin_popcount(out[5] & 0x3f);
        for (int i = 0; i < 5; i++) {
            ones += __builtin_popcount(out[i]);
        }
        if (ones != 0 && ones != 46) {
            return;
        }
    }
}

// M3 Task 1 smoke: build one DETERMINISTIC identifiable identity (instance i) —
// random-static MAC + a guaranteed manufacturer payload carrying a distinct decoy
// company id, so a scanner can attribute every instance unambiguously.
static void fill_identity(identity_t *id, uint8_t i)
{
    static const uint16_t cids[CHURN_HW_INSTANCES] = { 0x0075, 0x00E0, 0x009E, 0x0087 };
    make_random_static_addr(id->addr);

    struct ble_hs_adv_fields f;
    memset(&f, 0, sizeof(f));
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    uint8_t mfg[4];
    mfg[0] = (uint8_t)(cids[i] & 0xff);
    mfg[1] = (uint8_t)((cids[i] >> 8) & 0xff);
    mfg[2] = 0xA0;
    mfg[3] = i;
    f.mfg_data = mfg;
    f.mfg_data_len = sizeof(mfg);
    f.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    f.tx_pwr_lvl_is_present = 1;

    uint8_t buf[BLE_HS_ADV_MAX_SZ];
    uint8_t len = 0;
    if (ble_hs_adv_set_fields(&f, buf, &len, sizeof(buf)) != 0) {
        len = 0;
    }
    memcpy(id->payload, buf, len);
    id->payload_len = len;
    id->company_id = cids[i];
    id->adv_itvl_ms = 120;
}

static void simulacra_run(void)
{
    static identity_t ids[CHURN_HW_INSTANCES];
    for (uint8_t i = 0; i < CHURN_HW_INSTANCES; i++) {
        fill_identity(&ids[i], i);
        int rc = churn_adv_apply(i, &ids[i]);
        ESP_LOGW(TAG, "smoke inst %u start rc=%d (cid=0x%04x)", i, rc, ids[i].company_id);
    }
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
#endif  // !CHURN_SELFTEST

static void simulacra_task(void *arg)
{
    while (!s_host_synced) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
#if CHURN_SELFTEST
    int fails = churn_selftest_run();
    for (;;) {  // loop-print so the USB-JTAG reader reliably catches it
        ESP_LOGW(TAG, "SELFTEST result: %s (fails=%d)", fails ? "FAIL" : "PASS", fails);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
#else
    simulacra_run();
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

void app_main(void)
{
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
