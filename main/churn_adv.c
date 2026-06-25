#include <string.h>
#include "churn_adv.h"
#include "esp_log.h"
#include "nimble/nimble_port.h"
#include "nimble/hci_common.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"

static const char *TAG = "churn_adv";
#define ADV_ITVL_UNITS(ms) ((uint16_t)(((ms) * 1000) / 625))

int churn_adv_apply(uint8_t instance, const identity_t *id)
{
    int rc;
    ble_gap_ext_adv_stop(instance);  // ok if not running

    struct ble_gap_ext_adv_params p;
    memset(&p, 0, sizeof(p));
    p.legacy_pdu    = 1;             // legacy PDU -> visible to all scanners
    p.connectable   = 0;
    p.scannable     = 0;
    p.own_addr_type = BLE_OWN_ADDR_RANDOM;
    p.primary_phy   = BLE_HCI_LE_PHY_1M;
    p.secondary_phy = BLE_HCI_LE_PHY_1M;
    p.sid           = instance;
    p.itvl_min      = ADV_ITVL_UNITS(id->adv_itvl_ms);
    p.itvl_max      = ADV_ITVL_UNITS(id->adv_itvl_ms + 30);
    p.tx_power      = 127;
    rc = ble_gap_ext_adv_configure(instance, &p, NULL, NULL, NULL);
    if (rc) { ESP_LOGW(TAG, "configure inst %u rc=%d", instance, rc); return rc; }

    ble_addr_t a;
    a.type = BLE_ADDR_RANDOM;
    memcpy(a.val, id->addr, 6);
    rc = ble_gap_ext_adv_set_addr(instance, &a);
    if (rc) { ESP_LOGW(TAG, "set_addr inst %u rc=%d", instance, rc); return rc; }

    struct os_mbuf *om = ble_hs_mbuf_from_flat(id->payload, id->payload_len);
    if (!om) { ESP_LOGW(TAG, "mbuf NULL inst %u", instance); return BLE_HS_ENOMEM; }
    rc = ble_gap_ext_adv_set_data(instance, om);  // consumes om
    if (rc) { ESP_LOGW(TAG, "set_data inst %u rc=%d", instance, rc); return rc; }

    rc = ble_gap_ext_adv_start(instance, 0, 0);   // duration 0 = forever
    if (rc) ESP_LOGW(TAG, "start inst %u rc=%d", instance, rc);
    return rc;
}
