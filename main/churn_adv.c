#include <string.h>
#include "churn_adv.h"
#include "esp_log.h"
#include "nimble/nimble_port.h"
#include "nimble/hci_common.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"

static const char *TAG = "churn_adv";
#define ADV_ITVL_UNITS(ms) ((uint16_t)(((ms) * 1000) / 625))

// ble_hs_hci_cmd_tx lives in a private NimBLE header (host/src/ble_hs_hci_priv.h) but is a
// linkable (non-static) symbol; forward-declare it so we can issue the raw "LE Set Advertising
// Set Random Address" HCI command directly. ble_gap_ext_adv_set_addr() rejects RPA-shaped
// addresses (top-2-bits 01 / 0x40) with EINVAL — but real privacy phones advertise with exactly
// those. The controller transmits whatever 6 bytes we give it, so we set a validation-passing
// static stub via the host API (which flips rnd_addr_set=1 so ext_adv_start won't overwrite the
// address on enable) and then override the controller's adv-set random address with the real
// bytes via this raw command, before enabling.
int ble_hs_hci_cmd_tx(uint16_t opcode, const void *cmd, uint8_t cmd_len, void *rsp, uint8_t rsp_len);

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
    p.tx_power      = (id->tx_power != 0) ? id->tx_power : 127;   // per-identity dither; 0 -> max/default
    rc = ble_gap_ext_adv_configure(instance, &p, NULL, NULL, NULL);
    if (rc) { ESP_LOGW(TAG, "configure inst %u rc=%d", instance, rc); return rc; }

    ble_addr_t a;
    a.type = BLE_ADDR_RANDOM;
    memcpy(a.val, id->addr, 6);
    a.val[5] = (uint8_t)((a.val[5] & 0x3F) | 0xC0);   // force static top-2-bits so set_addr() validates
    rc = ble_gap_ext_adv_set_addr(instance, &a);       // sets rnd_addr_set=1 (+ stub addr at controller)
    if (rc) { ESP_LOGW(TAG, "set_addr inst %u rc=%d", instance, rc); return rc; }

    // Override the controller's adv-set random address with the REAL bytes (which may be an
    // RPA 0x40 address the host API rejects). Safe here because the set is not yet enabled and
    // rnd_addr_set is now true, so ble_gap_ext_adv_start() below will NOT re-send the stub.
    struct ble_hci_le_set_adv_set_rnd_addr_cp rac;
    rac.adv_handle = instance;
    memcpy(rac.addr, id->addr, 6);
    rc = ble_hs_hci_cmd_tx(BLE_HCI_OP(BLE_HCI_OGF_LE, BLE_HCI_OCF_LE_SET_ADV_SET_RND_ADDR),
                           &rac, sizeof(rac), NULL, 0);
    if (rc) { ESP_LOGW(TAG, "rnd_addr inst %u rc=%d", instance, rc); return rc; }

    struct os_mbuf *om = ble_hs_mbuf_from_flat(id->payload, id->payload_len);
    if (!om) { ESP_LOGW(TAG, "mbuf NULL inst %u", instance); return BLE_HS_ENOMEM; }
    rc = ble_gap_ext_adv_set_data(instance, om);  // consumes om
    if (rc) { ESP_LOGW(TAG, "set_data inst %u rc=%d", instance, rc); return rc; }

    rc = ble_gap_ext_adv_start(instance, 0, 0);   // duration 0 = forever
    if (rc) ESP_LOGW(TAG, "start inst %u rc=%d", instance, rc);
    return rc;
}
