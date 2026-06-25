# M3 — Roster + Active-Set + Churn Engine (design spec)

- **Date:** 2026-06-24
- **Status:** Approved (brainstorm) — pending implementation plan
- **Target:** Seeed XIAO ESP32-C6, ESP-IDF v5.4.4, NimBLE
- **Builds on:** v1 (upstream Splinter retargeted to `esp32c6`, paced legacy advertiser, flashing + advertising on COM11)
- **Source of truth for requirements:** `the project requirements handoff`, milestone M3

## 1. Goal

Replace v1's single rotating legacy advertiser with a **synthetic population**: a handful of
plausible BLE devices that *persist for minutes each*, arrive and depart on a stagger, and
turn over slowly — driven by a hand-written (static) template set. No live capture yet (that
is M5/M6). Each milestone must leave a flashable, scanner-verifiable device.

## 2. Invariants that must survive (from the handoff)

1. **Aggregates only** — no per-device identifiers persisted. (No persistence at all in M3;
   relevant from M5.)
2. **No verbatim replay** — synthesize identities; never re-emit a captured real one. (M3
   identities are synthetic by construction.)
3. **Non-connectable advertising only**, and keep excluding the Apple / Microsoft / Google
   pairing-popup payload formats v1 already avoids.
4. **Population-match** — active set never far exceeds observed density. (M3: active set is a
   fixed tunable; live sizing is M6.)
5. **Raw-capture dumps behind a non-shipping debug flag.** (No captures in M3.)

## 3. Hardware constraint (confirmed)

The ESP32-C6 caps **concurrent extended-advertising instances at 4** — verified in the
installed IDF v5.4.4 tree:
- Controller: `BT_LE_MAX_EXT_ADV_INSTANCES` — `range 0 4`, default 1
  (`components/bt/controller/esp32c6/Kconfig.in`).
- NimBLE host: `BT_NIMBLE_MAX_EXT_ADV_INSTANCES` — `range 0 4`, default 1
  (`components/bt/host/nimble/Kconfig.in`).

So at most **4 identities are genuinely on-air simultaneously**. A logical active set larger
than 4 must **time-slice** across those 4 radios.

## 4. Decisions (brainstorm outcomes)

- **Active-set model:** time-slice a tunable active set (default 8) across the 4 hardware
  instances. Each identity keeps its MAC + payload for its full dwell; a scheduler rotates
  which 4 occupy the radios on a ~1 s slice. A scanner sampling over seconds sees ~8
  persistent devices. (N ≤ 4 ⇒ no slicing, 100 % duty; N = 8 ⇒ ~50 % duty per identity.)
- **Identity lifecycle:** persistent, **reappear-after-cooldown**. The roster holds
  pre-generated identities with **stable MACs** that cycle `IDLE → ACTIVE → COOLDOWN → IDLE`
  and reappear later with the *same* MAC. This makes "no MAC reappears within its cooldown
  window" a meaningful rule and models real devices that leave and return.
- **Dwell / cooldown:** uniform random, tunable. dwell ~ U(3–10 min), cooldown ~ U(30–60 min).
  Initial active set seeded with randomized remaining-dwell so departures don't clump.
- **Scheduler:** one FreeRTOS task, ~250 ms tick, table-driven state, all NimBLE/GAP calls
  from that single task (same context model v1 uses; no locking). Rejected: per-identity
  FreeRTOS software timers (callbacks cross task context into NimBLE — fiddly on single-core,
  no gain).

## 5. Architecture / module split

simulacra_main.c is already 331 lines; this work would exceed the 500-line guideline, so split
it — and isolate *generation* (what M6 later replaces) behind a clean seam:

| File | Responsibility |
|---|---|
| `main/identity.h` | `identity_t` struct + `id_state_t` enum |
| `main/roster.c` / `.h` | pre-generate + own the 256-entry pool; `roster_promote_candidate()` seam (M6 swaps the generation here, nothing else) |
| `main/churn.c` / `.h` | active-set / cooldown / time-slice engine + ext-adv instance mapping; the churn task |
| `main/simulacra_main.c` | slim: nvs + nimble init, `on_sync`, spawn the churn task |
| `main/decoy_vendors.h` | unchanged (vendor palette) |
| `main/CMakeLists.txt` | add `roster.c`, `churn.c` to SRCS |

The v1 `#ifdef CONFIG_BT_NIMBLE_EXT_ADV` ext-adv path in simulacra_main.c is removed/superseded
by churn.c (its calls are reused there); the legacy `#else` path is retained only as the
non-EXT_ADV fallback for other targets, or removed — decided at plan time.

## 6. Data: `identity_t`

```c
typedef enum { ID_IDLE, ID_ACTIVE, ID_COOLDOWN } id_state_t;

typedef struct {
    uint8_t    addr[6];          // stable random-static MAC (top 2 bits set)
    uint16_t   company_id;       // vendor (from template)
    uint8_t    payload[31];      // frozen AD bytes, serialized once at generation
    uint8_t    payload_len;
    uint16_t   adv_itvl_ms;      // this identity's on-air interval
    id_state_t state;
    uint32_t   active_until_ms;  // ACTIVE: dwell deadline (absolute, ms)
    uint32_t   eligible_at_ms;   // COOLDOWN: earliest re-promotion time (absolute, ms)
} identity_t;
```
~46 B × 256 ≈ 12 KB, fully static (no heap). "now" = `esp_timer_get_time() / 1000` (ms);
dwell/cooldown are absolute deadlines, so no per-tick decrement loops.

## 7. Roster generation (`roster.c` — M3 static path)

`roster_init()` pre-generates all `CHURN_ROSTER_SIZE` identities. Each gets:
- a stable random-static MAC via the existing `make_random_static_addr()`,
- a vendor drawn from `VENDORS[]` (`decoy_vendors.h`),
- a **frozen** AD payload, built once via the existing `build_decoy_fields()` →
  `ble_hs_adv_set_fields()` serialization and stored in `payload[]`,
- an `adv_itvl_ms` (M3: a randomized value in a plausible range, e.g. 100–300 ms; per-vendor
  interval bands are M4),
- `state = ID_IDLE`.

Seam consumed by churn:
```c
identity_t *roster_promote_candidate(uint32_t now_ms);  // an eligible IDLE identity, or NULL
```
M6 replaces only the generation inside roster.c (model-driven sampling); churn.c is untouched.

## 8. Churn engine (`churn.c`)

One task, `CHURN_TICK_MS` (250 ms) tick:

```
tick(now_ms):
  # retire expired + promote replacements
  for slot in 0..N-1:
     id = active[slot]
     if id != NULL and now_ms >= id->active_until_ms:
        id->state = ID_COOLDOWN
        id->eligible_at_ms = now_ms + rand(CHURN_COOLDOWN_MIN_MS .. CHURN_COOLDOWN_MAX_MS)
        cand = roster_promote_candidate(now_ms)        # NULL tolerated, retried next tick
        active[slot] = cand
        if cand: cand->state = ID_ACTIVE
                 cand->active_until_ms = now_ms + rand(CHURN_DWELL_MIN_MS .. CHURN_DWELL_MAX_MS)

  # time-slice: map the N active identities onto the 4 radios
  if now_ms - last_slice_ms >= CHURN_SLICE_MS:
     last_slice_ms = now_ms; phase++
     for i in 0..CHURN_HW_INSTANCES-1:
        target = active[(phase*CHURN_HW_INSTANCES + i) % N]
        if instance_occupant[i] != target:
           ble_gap_ext_adv_stop(i)
           ble_gap_ext_adv_set_addr(i, target->addr)
           ble_gap_ext_adv_set_data(i, target->payload, target->payload_len)
           ble_gap_ext_adv_start(i, 0, 0)
           instance_occupant[i] = target
  vTaskDelay(CHURN_TICK_MS)   # single-core: yields → IDLE runs → no watchdog
```
- Cooldown→idle transition is implicit: `roster_promote_candidate` only returns identities
  whose `state==ID_IDLE`, or `state==ID_COOLDOWN && now >= eligible_at_ms` (flipping them to
  IDLE on selection).
- Starvation impossible: 256 idle ≫ 8 active + cooldown pool.
- `N ≤ CHURN_HW_INSTANCES` ⇒ occupancy never rotates (100 % duty).

## 9. Ext-adv layer / NimBLE

Reuse v1's already-working calls and the `configure_instance()` parameters verbatim:
legacy-PDU (`legacy_pdu=1`), `connectable=0`, `scannable=0`, `own_addr_type=BLE_OWN_ADDR_RANDOM`,
1M PHY. Instances are configured once at startup; the churn loop only does
stop/set_addr/set_data/start to swap occupants. This settles the handoff's "verify NimBLE
signatures against the installed version" gotcha — that path already compiles against this
exact IDF/NimBLE. Non-connectable + excluded pairing-popup formats preserved (payloads still
come from the unchanged `build_decoy_fields`).

## 10. sdkconfig — new `main/../sdkconfig.defaults.esp32c6`

```
CONFIG_BT_NIMBLE_EXT_ADV=y
CONFIG_BT_NIMBLE_MAX_EXT_ADV_INSTANCES=4
CONFIG_BT_LE_EXT_ADV=y
CONFIG_BT_LE_MAX_EXT_ADV_INSTANCES=4
```
Mirrors the existing `sdkconfig.defaults.esp32c5`; flips the C6 onto the EXT_ADV path. Console
stays secondary USB-JTAG (unchanged). A clean re-target (`rm -rf build sdkconfig; idf.py
set-target esp32c6`) regenerates config so these defaults apply.

## 11. Tunables (top of `churn.c`)

| Macro | Default | Meaning |
|---|---|---|
| `CHURN_ROSTER_SIZE` | 256 | pre-generated pool size |
| `CHURN_ACTIVE_SET` | 8 | logical concurrent identities (acceptance: tunable constant) |
| `CHURN_HW_INSTANCES` | 4 | hardware radios (= sdkconfig cap) |
| `CHURN_TICK_MS` | 250 | scheduler tick (yields → no watchdog) |
| `CHURN_SLICE_MS` | 1000 | time-slice rotation period |
| `CHURN_DWELL_MIN_MS` / `_MAX_MS` | 180000 / 600000 | dwell 3–10 min |
| `CHURN_COOLDOWN_MIN_MS` / `_MAX_MS` | 1800000 / 3600000 | cooldown 30–60 min |

## 12. Robustness

- **Single-core safe:** the 250 ms `vTaskDelay` each tick lets IDLE run → no task watchdog
  (v1's flood-loop lesson, designed out here).
- **No heap:** roster + active set + occupant table are fixed arrays.
- **Failure handling:** every `ble_gap_ext_adv_*` rc is checked; WARN-and-continue, never abort.
- **NimBLE context:** all GAP calls from the one churn task (after host sync), as in v1.

## 13. Verification (M3 acceptance)

- A longer `bleak` scan (a few minutes) recording per-MAC first/last-seen:
  - ~`CHURN_ACTIVE_SET` distinct decoy MACs present at any sampling,
  - each persisting minutes (not one-shot flicker),
  - staggered arrivals/departures and gradual turnover,
  - **no MAC reappearing within its cooldown window.**
- Set `CHURN_ACTIVE_SET` to a different value, rebuild, confirm the observed count tracks it.
- No task-watchdog over a sustained run (serial read, as in v1 verification).

## 14. Out of scope (deferred)

- Per-vendor structural payload templates + joint vendor/interval/payload sampling — **M4**.
- Live BLE/Wi-Fi observation → distributions, persistence — **M5**.
- Model-driven generation + population-match sizing — **M6** (drops into `roster.c` only).
- Probe-request injection, coexistence/duty-cycle — **M7/M8**.

## 15. Open risks

- Per-slice stop/start of 4 instances (~4/s) is far gentler than v1's 20 ms refresh, but
  confirm no controller complaints (rc logging will surface them).
- `ble_gap_ext_adv_set_data` consumes an mbuf in v1's pattern; in M3 the payload is a frozen
  flat buffer, so wrap it per call (`ble_hs_mbuf_from_flat`) — verify no mbuf exhaustion at
  the slice rate (pool is replenished; rate is low).
