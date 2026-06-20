// splinter — BLE privacy / anti-tracking decoy for the ESP32 family (ESP-IDF + NimBLE)
//
// Continuously fabricates a churning crowd of plausible-but-fake BLE devices so
// that, in a space you control, a tracking/scanning system sees lots of ordinary
// traffic and your real device(s) do not stand out.
//
// Two build flavours, selected automatically by the target's BLE capability:
//   * Classic ESP32 (BT 4.2): ONE legacy advertiser, rotated at maximum rate —
//     a single MAC on air at a time that churns into a large crowd over time.
//   * BLE-5 chips, e.g. ESP32-C5 / C3 (CONFIG_BT_NIMBLE_EXT_ADV): up to
//     CONFIG_BT_NIMBLE_MAX_EXT_ADV_INSTANCES *concurrent* legacy-PDU advertising
//     sets, each with its own MAC, all rotated round-robin — genuinely
//     simultaneous decoys AND maximum churn.
//
// Decoy guardrails (see decoy_vendors.h): advertising is NON-CONNECTABLE and the
// payload is never shaped like Apple Continuity / Microsoft Swift Pair / Google
// Fast Pair, so it creates realistic presence without popping pairing dialogs on
// bystanders' devices.

#include <stdio.h>
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

#include "decoy_vendors.h"

static const char *TAG = "splinter";

// ---- tunables -------------------------------------------------------------
#define SIMULACRA_ROTATE_MS   250   // paced mode: delay between decoys (BENCHMARK=0)
#define SIMULACRA_NAME_PROB    60   // % chance a decoy advertises a device name
#define SIMULACRA_MFG_PROB     85   // % chance a decoy carries vendor mfg data
#define SIMULACRA_ADV_MS      100   // on-air advertising interval per decoy (ms)
#define SIMULACRA_EXT_REFRESH_MS 20 // C5 ext-adv: gap between per-instance identity
                                   // refreshes. The instances advertise concurrently
                                   // on their own; this just paces the churn AND
                                   // yields the single core (host/USB/idle/WDT).

// 1 = flood/benchmark (max rate, reports devices/sec). 0 = paced decoy.
#define SIMULACRA_BENCHMARK     1

// BLE advertising interval is expressed in 0.625 ms units.
#define ADV_ITVL_UNITS(ms)   ((uint16_t)(((ms) * 1000) / 625))

static volatile bool s_host_synced = false;

// Build a valid random-static address: 6 random bytes with the two most
// significant bits set (out[5] is the most-significant octet). Regenerates the
// astronomically rare all-zero / all-ones random part that NimBLE would reject.
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

// Shared: populate a non-connectable decoy's advertising fields. `mfg` must point
// to a >=10 byte scratch buffer that outlives the field's use/serialization. The
// manufacturer body is uniform random — intentionally NOT a pop-up-triggering
// Continuity / Swift Pair / Fast Pair shape.
static void build_decoy_fields(struct ble_hs_adv_fields *f, uint8_t *mfg)
{
    const vendor_t *v = &VENDORS[esp_random() % VENDOR_COUNT];

    memset(f, 0, sizeof(*f));
    f->flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;

    bool used_name = false;
    if (v->name != NULL && (esp_random() % 100) < SIMULACRA_NAME_PROB) {
        f->name = (uint8_t *)v->name;
        f->name_len = strlen(v->name);
        f->name_is_complete = 1;
        used_name = true;
    }

    if ((esp_random() % 100) < SIMULACRA_MFG_PROB) {
        size_t body = used_name ? 3 : (3 + (esp_random() % 5));
        mfg[0] = (uint8_t)(v->company_id & 0xff);
        mfg[1] = (uint8_t)((v->company_id >> 8) & 0xff);
        for (size_t i = 0; i < body; i++) {
            mfg[2 + i] = (uint8_t)(esp_random() & 0xff);
        }
        f->mfg_data = mfg;
        f->mfg_data_len = 2 + body;
    }

    f->tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    f->tx_pwr_lvl_is_present = 1;
}

#ifdef CONFIG_BT_NIMBLE_EXT_ADV
// =========================================================================
// BLE 5 path: several CONCURRENT extended-advertising instances (e.g. C5/C3)
// =========================================================================
#ifndef CONFIG_BT_NIMBLE_MAX_EXT_ADV_INSTANCES
#define CONFIG_BT_NIMBLE_MAX_EXT_ADV_INSTANCES 1
#endif
#define SIMULACRA_INSTANCES   CONFIG_BT_NIMBLE_MAX_EXT_ADV_INSTANCES

// Configure one instance as a legacy-PDU, non-connectable advertiser so it is
// visible to ordinary (legacy) scanners. Done once per instance at startup.
static int configure_instance(uint8_t instance)
{
    struct ble_gap_ext_adv_params p;
    memset(&p, 0, sizeof(p));
    p.legacy_pdu    = 1;                 // legacy PDUs -> seen by all scanners
    p.connectable   = 0;
    p.scannable     = 0;                 // NONCONN legacy = legacy_pdu only
    p.own_addr_type = BLE_OWN_ADDR_RANDOM;
    p.primary_phy   = BLE_HCI_LE_PHY_1M;
    p.secondary_phy = BLE_HCI_LE_PHY_1M;
    p.sid           = instance;
    p.itvl_min      = ADV_ITVL_UNITS(SIMULACRA_ADV_MS);
    p.itvl_max      = ADV_ITVL_UNITS(SIMULACRA_ADV_MS + 30);
    p.tx_power      = 127;               // controller clamps to its max
    return ble_gap_ext_adv_configure(instance, &p, NULL, NULL, NULL);
}

// Give one instance a fresh identity (MAC + payload) and (re)start it. The other
// instances keep advertising throughout, so ~SIMULACRA_INSTANCES are always live.
static int refresh_instance(uint8_t instance)
{
    int rc;

    ble_gap_ext_adv_stop(instance);      // ok if it wasn't running

    uint8_t raw[6];
    make_random_static_addr(raw);
    ble_addr_t addr;
    addr.type = BLE_ADDR_RANDOM;
    memcpy(addr.val, raw, sizeof(raw));
    rc = ble_gap_ext_adv_set_addr(instance, &addr);
    if (rc != 0) return rc;

    struct ble_hs_adv_fields fields;
    uint8_t mfg[10];
    build_decoy_fields(&fields, mfg);

    uint8_t buf[BLE_HS_ADV_MAX_SZ];
    uint8_t buf_len = 0;
    rc = ble_hs_adv_set_fields(&fields, buf, &buf_len, sizeof(buf));
    if (rc != 0) return rc;

    struct os_mbuf *om = ble_hs_mbuf_from_flat(buf, buf_len);
    if (om == NULL) return BLE_HS_ENOMEM;
    rc = ble_gap_ext_adv_set_data(instance, om);   // consumes om
    if (rc != 0) return rc;

    return ble_gap_ext_adv_start(instance, 0, 0);  // duration 0 = forever
}

static void simulacra_run(void)
{
    for (uint8_t i = 0; i < SIMULACRA_INSTANCES; i++) {
        int rc = configure_instance(i);
        if (rc != 0) {
            ESP_LOGW(TAG, "configure instance %u failed rc=%d", i, rc);
        }
    }
    ESP_LOGW(TAG, "BLE5 flood: %d concurrent ext-adv instances, round-robin refresh",
             SIMULACRA_INSTANCES);

    uint8_t inst = 0;
    uint32_t t0 = esp_log_timestamp(), ok = 0, fail = 0;
    for (;;) {
        if (refresh_instance(inst) == 0) {
            ok++;
        } else {
            fail++;
        }
        inst = (inst + 1) % SIMULACRA_INSTANCES;

        uint32_t now = esp_log_timestamp();
        if (now - t0 >= 1000) {
            ESP_LOGW(TAG, "rate: %lu refreshes/sec, %d live instances (fail=%lu)",
                     (unsigned long)ok, SIMULACRA_INSTANCES, (unsigned long)fail);
            ok = 0;
            fail = 0;
            t0 = now;
        }

        // Always yield: on the single-core C5 a tight loop starves the BLE host,
        // the USB-Serial-JTAG console, and the idle/watchdog tasks. The 4
        // instances keep advertising concurrently between refreshes regardless.
        vTaskDelay(pdMS_TO_TICKS(SIMULACRA_EXT_REFRESH_MS));
    }
}

#else
// =========================================================================
// Classic ESP32 (BT 4.2) path: one legacy advertiser, rotated fast
// =========================================================================

// Quiet (no per-cycle logging) so it can be driven at the controller's maximum
// identity-switch rate without the UART becoming the bottleneck.
static int start_one_decoy(void)
{
    int rc;

    uint8_t addr[6];
    make_random_static_addr(addr);
    rc = ble_hs_id_set_rnd(addr);
    if (rc != 0) return rc;

    struct ble_hs_adv_fields fields;
    uint8_t mfg[10];
    build_decoy_fields(&fields, mfg);

    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) return rc;

    struct ble_gap_adv_params advp;
    memset(&advp, 0, sizeof(advp));
    advp.conn_mode = BLE_GAP_CONN_MODE_NON;   // non-connectable: pure presence
    advp.disc_mode = BLE_GAP_DISC_MODE_GEN;
    advp.itvl_min  = ADV_ITVL_UNITS(SIMULACRA_ADV_MS);
    advp.itvl_max  = ADV_ITVL_UNITS(SIMULACRA_ADV_MS + 30);

    return ble_gap_adv_start(BLE_OWN_ADDR_RANDOM, NULL, BLE_HS_FOREVER,
                             &advp, NULL, NULL);
}

static void simulacra_run(void)
{
#if SIMULACRA_BENCHMARK
    ESP_LOGW(TAG, "BENCHMARK/flood: rotating one legacy advertiser at max rate");
    uint32_t t0 = esp_log_timestamp(), ok = 0, fail = 0;
    for (;;) {
        ble_gap_adv_stop();
        if (start_one_decoy() == 0) {
            ok++;
        } else {
            fail++;
        }
        uint32_t now = esp_log_timestamp();
        if (now - t0 >= 1000) {
            ESP_LOGW(TAG, "rate: %lu devices/sec (fail=%lu)",
                     (unsigned long)ok, (unsigned long)fail);
            ok = 0;
            fail = 0;
            t0 = now;
        }
    }
#else
    ESP_LOGI(TAG, "splinter active — rotating a decoy every %d ms", SIMULACRA_ROTATE_MS);
    for (;;) {
        ble_gap_adv_stop();
        if (start_one_decoy() != 0) {
            ESP_LOGW(TAG, "decoy cycle failed");
        }
        vTaskDelay(pdMS_TO_TICKS(SIMULACRA_ROTATE_MS));
    }
#endif
}
#endif // CONFIG_BT_NIMBLE_EXT_ADV

static void simulacra_task(void *arg)
{
    while (!s_host_synced) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    simulacra_run();
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

    // NimBLE logs every GAP procedure at INFO; at high rotation rates that UART
    // spam would dominate and throttle the loop. Keep only warnings+.
    esp_log_level_set("NimBLE", ESP_LOG_WARN);

    ESP_ERROR_CHECK(nimble_port_init());

    ble_hs_cfg.sync_cb  = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    ble_svc_gap_init();

    nimble_port_freertos_init(nimble_host_task);
    xTaskCreate(simulacra_task, "splinter", 4096, NULL, 5, NULL);
}
