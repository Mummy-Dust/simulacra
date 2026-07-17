# Persona BLE-Shape Realism Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make cross-protocol personas advertise a Law-3-safe phone BLE shape (flags-only / 16-bit service-UUID list, widened interval band) instead of a vendor-matched accessory payload, closing the device-class tell and the interval/vendor monoculture.

**Architecture:** A new `template_build_phone` builder produces the persona AD directly (reusing the existing safe encoders); `ble_device_sync` calls it instead of drawing an accessory shape from the roster (`roster_pick_company`, now removed). `phantom_sync_ble` passes an Apple flag so iPhone personas stay flags-only (no Continuity). Behaviour is verified through the existing `synth_dump --personas` / `--persona-pop` host dumps and their Python tests.

**Tech Stack:** C (firmware + MSVC host build via `tools/decoy_audit/run.ps1`), Python 3.12 pytest integration tests over the built `synth_dump.exe`.

## Global Constraints

- **Public repo `Mummy-Dust/simulacra`.** Commit identity = `Mummy-Dust <152051018+Mummy-Dust@users.noreply.github.com>` (already the repo's local git config). Never commit the OS username, absolute `C:\Users\<name>` paths, real MACs/SSIDs, or `private/` / `.superpowers/` scratch.
- **Commit trailers required on every commit:**
  ```
  Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_01TgsxaF69foVD8qLkeULeJy
  ```
- **Law-3:** never emit a frame that can trigger a real-device UI (Apple Continuity 0x004C mfg, Fast-Pair service-data with a model id). Personas emit only flags-only or a 16-bit service-UUID *list* — no mfg, no service-data. `company_id` is always 0.
- **Uniqueness:** every address stays routed through `uniq_try` (unchanged — `make_random_addr` already does this).
- Branch `feat/persona-ble-shape` (already created off `main`). Merge to `main` locally with `--no-ff`; **no push**.
- Host build: `tools/decoy_audit/run.ps1 -Rebuild` (the `cl` line is source of truth). Tests: `"C:/Program Files/Python312/python.exe" -m pytest`.

---

### Task 1: Persona phone-BLE shape

**Files:**
- Modify: `main/templates.h` (declare `template_build_phone`; add `#include <stdbool.h>`)
- Modify: `main/templates.c` (add `template_build_phone` + interval band constants)
- Modify: `main/ble_devices.h:45-46` (change `ble_device_sync` signature: `uint16_t company` → `bool apple`; update the doc comment)
- Modify: `main/ble_devices.c` (add `#include "templates.h"`; rewrite `ble_device_sync` body)
- Modify: `main/phantom.c:88` (pass `ph->family == PF_APPLE` instead of `phantom_company(...)`)
- Modify: `main/roster.h:21` and `main/roster.c:110-119` (remove `roster_pick_company`)
- Modify: `tools/probe_audit/host_stubs/ble_devices_stub.c:14-20` (update stub signature)
- Modify: `tools/decoy_audit/README.md:224-226` (replace the vendor-match / reservoir-sampling description)
- Test: `tools/decoy_audit/tests/test_personas.py` (replace two vendor-match tests with a realistic-shape test)

**Interfaces:**
- Consumes: `enc_svc_uuid16(struct ble_hs_adv_fields *)` and `rnd_range(uint16_t, uint16_t)` (both static in `templates.c`); `make_random_addr`, `top2_for`, `rotate_base` (already in `ble_devices.c`); `phantom_family_t`/`PF_APPLE` (`phantom.h`).
- Produces: `int template_build_phone(bool apple, uint8_t out_payload[31], uint8_t *out_len, uint16_t *out_itvl_ms)`; `int ble_device_sync(int slot, int persona_idx, bool apple, uint32_t born_ms, uint32_t life_ms, uint32_t generation)`.

**Note on TDD shape:** this codebase has no per-function C harness; persona behaviour is asserted by Python integration tests over the built `synth_dump.exe`. So the failing test is the rewritten `test_personas.py`, run against the **current** (still vendor-matched) binary — it fails because the current binary emits `company != 0`. After the code change and a rebuild, it passes.

- [ ] **Step 1: Rewrite the persona-shape tests (RED)**

In `tools/decoy_audit/tests/test_personas.py`, **delete** `test_samsung_google_families_are_vendor_matched` and `test_same_vendor_personas_get_diverse_payloads`, and add this test in their place (keep `test_ble_members_are_rpa_and_law3_safe` as-is):

```python
    def test_persona_ble_is_realistic_phone_shape(self):
        ble, _ = personas(4)
        self.assertTrue(ble, "no persona BLE events")
        # A persona presents a terse PHONE shape: no manufacturer data at all, so the on-air
        # company id is always 0 (never earbuds vendor mfg, never Apple Continuity 0x004C).
        for _t, _i, _addr, atype, comp, _g, _itvl in ble:
            self.assertEqual(atype, "rpa", "persona BLE member is not RPA")
            self.assertEqual(comp, 0, f"persona emitted manufacturer data (company {comp:#06x})")
        # Widened phone interval band -> personas spread across intervals instead of clustering on
        # the single 120-180 ms accessory band (the measured interval monoculture tell).
        itvls = {itvl for _t, _i, _a, _at, _c, _g, itvl in ble}
        self.assertGreater(len(itvls), 1, "persona intervals collapsed to one value (monoculture)")
```

The `defaultdict` import is now unused — change the header import line `from collections import defaultdict` to remove it (the file has no other `defaultdict` use after the deletion).

- [ ] **Step 2: Run the rewritten test against the current binary — verify it fails (RED)**

Run: `"C:/Program Files/Python312/python.exe" -m pytest tools/decoy_audit/tests/test_personas.py::Personas::test_persona_ble_is_realistic_phone_shape -v`
Expected: **FAIL** — the current `synth_dump.exe` emits vendor-matched `company` (0x0075/0x00E0), so `assertEqual(comp, 0)` fails.

(If `synth_dump.exe` does not exist yet, first build it once with `tools/decoy_audit/run.ps1 -Rebuild` so there is a current-behaviour binary to fail against.)

- [ ] **Step 3: Add `template_build_phone` to templates.c/.h**

In `main/templates.h`, add `#include <stdbool.h>` near the top (after `#include <stdint.h>`), and declare after `template_build_vendor_mfg`:

```c
// Build a phone-plausible BLE advertisement for a cross-protocol persona. Law-3 safe by
// construction: emits only flags-only, or flags + a 16-bit service-UUID LIST (no manufacturer
// data, no service-data), so it can never trigger a Continuity / Fast-Pair pairing pop-up.
// company_id is implicitly 0. apple=true -> flags-only (the iPhone floor; no Continuity available);
// apple=false -> a 16-bit service-UUID list part of the time, else flags-only. Returns 0 on success.
int template_build_phone(bool apple, uint8_t out_payload[31], uint8_t *out_len, uint16_t *out_itvl_ms);
```

In `main/templates.c`, add the interval-band constants just below the `TEMPLATES[]` array (after line 24) — or anywhere above the new function — and append the function at the end of the file (after `template_build_vendor_mfg`, so the static `enc_svc_uuid16`/`rnd_range` above are in scope):

```c
// Real phones present on BLE as a terse advertiser on a wide, jittered interval -- not the tight
// 120-180 ms accessory band. This band spreads N personas across the interval histogram.
#define PHONE_ITVL_MIN_MS  180
#define PHONE_ITVL_MAX_MS 1000

int template_build_phone(bool apple, uint8_t out_payload[31], uint8_t *out_len, uint16_t *out_itvl_ms)
{
    struct ble_hs_adv_fields f;
    memset(&f, 0, sizeof(f));
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;

    // Apple: flags-only (Continuity is off-limits -> the honest iPhone floor). Android: ~40% carry
    // a 16-bit service-UUID list (battery / device-info / HID / Google svc), else flags-only.
    if (!apple && (esp_random() % 100) < 40) enc_svc_uuid16(&f);

    uint8_t buf[BLE_HS_ADV_MAX_SZ], len = 0;
    if (ble_hs_adv_set_fields(&f, buf, &len, sizeof(buf)) != 0) { *out_len = 0; return 1; }
    memcpy(out_payload, buf, len);
    *out_len = len;
    *out_itvl_ms = rnd_range(PHONE_ITVL_MIN_MS, PHONE_ITVL_MAX_MS);
    return 0;
}
```

- [ ] **Step 4: Rewire `ble_device_sync` to build the phone shape**

In `main/ble_devices.c`, add `#include "templates.h"` under the existing includes (after `#include "roster.h"`). Replace the whole `ble_device_sync` function (lines 130-150) with:

```c
int ble_device_sync(int slot, int persona_idx, bool apple,
                    uint32_t born_ms, uint32_t life_ms, uint32_t generation)
{
    if (slot < 0 || slot >= s_n) return 0;
    ble_device_t *d = &s_dev[slot];
    if (d->persona_idx == persona_idx && d->persona_gen == generation && d->alive) return 0;
    // A phone presents on BLE as a terse phone shape (flags-only / svc-uuid16), never accessory
    // manufacturer data. Build it directly (no roster draw); company id stays 0.
    d->id.company_id    = 0;
    d->id.tx_power      = 0;
    d->id.archetype_idx = 0;
    if (template_build_phone(apple, d->id.payload, &d->id.payload_len, &d->id.adv_itvl_ms) != 0)
        d->id.payload_len = 0;                          // serialization guard (self-test catches)
    d->atype = BLE_ATYPE_RPA;                           // phones present on BLE as RPA
    make_random_addr(d->id.addr, top2_for(BLE_ATYPE_RPA));   // fresh unique RPA address
    d->role   = BLE_ROLE_TRANSIENT;                     // lifetime is the phantom's, not a band
    d->born_ms = born_ms;
    d->life_ms = life_ms;
    d->alive = true;
    d->next_rotate_ms = born_ms + rotate_base(BLE_ATYPE_RPA);
    d->persona_idx = (int8_t)persona_idx;
    d->persona_gen = generation;
    return 1;
}
```

Update the declaration + doc comment in `main/ble_devices.h` (lines 41-46) to:

```c
// Bind BLE slot `slot` to phantom `persona_idx` (see phantom.h): when the phantom's generation
// advances, reincarnate the slot as an RPA device carrying a Law-3-safe phone advertisement
// (flags-only / 16-bit service-UUID list, no manufacturer data), the phantom's shared born/life,
// and a fresh unique address. `apple` selects the iPhone floor (flags-only, no Continuity).
// Returns 1 if reincarnated. Bound slots do NOT expire via ble_devices_tick; the phantom owns them.
int ble_device_sync(int slot, int persona_idx, bool apple,
                    uint32_t born_ms, uint32_t life_ms, uint32_t generation);
```

- [ ] **Step 5: Pass the Apple flag from `phantom_sync_ble`**

In `main/phantom.c`, line 88, change:

```c
        ble_device_sync(i, i, phantom_company(ph->family), ph->born_ms, ph->life_ms, ph->generation);
```
to:
```c
        ble_device_sync(i, i, ph->family == PF_APPLE, ph->born_ms, ph->life_ms, ph->generation);
```

Leave `phantom_company` in place — `probe_dump.c` and the persona dumps still use it as a family *label*.

- [ ] **Step 6: Remove the now-unused `roster_pick_company`**

In `main/roster.c`, delete the whole `roster_pick_company` function (lines 110-119). In `main/roster.h`, delete its declaration (line 21).

- [ ] **Step 7: Update the probe_audit link stub**

In `tools/probe_audit/host_stubs/ble_devices_stub.c`, replace the stub definition (lines 14-20) with the new signature:

```c
int ble_device_sync(int slot, int persona_idx, bool apple,
                     uint32_t born_ms, uint32_t life_ms, uint32_t generation)
{
    (void)slot; (void)persona_idx; (void)apple;
    (void)born_ms; (void)life_ms; (void)generation;
    return 0;
}
```

(`ble_devices.h` already includes `<stdbool.h>`, so `bool` is in scope here.)

- [ ] **Step 8: Update the decoy_audit README**

In `tools/decoy_audit/README.md`, replace lines 224-226:

```
device: **dual-radio coverage** (every Wi-Fi identity has a co-present BLE twin), co-appearance on
the same tick, that persona BLE members are RPA and Law-3-safe (never Apple mfg data),
Samsung/Google vendor-matching, cross-radio address uniqueness, and **same-vendor payload diversity**
(reservoir-sampled `roster_pick_company` — several Samsung "phones" must not be byte-identical).
```
with:
```
device: **dual-radio coverage** (every Wi-Fi identity has a co-present BLE twin), co-appearance on
the same tick, that persona BLE members are RPA and Law-3-safe (never Apple mfg data), a **realistic
phone shape** (terse flags-only / 16-bit service-UUID list, no accessory manufacturer data),
cross-radio address uniqueness, and a **widened phone interval band** (personas spread across
intervals rather than clustering on one accessory value).
```

- [ ] **Step 9: Rebuild both host tools and run the full pytest suites (GREEN)**

Rebuild the decoy_audit binary (picks up the changed C):
Run: `pwsh tools/decoy_audit/run.ps1 -Rebuild` (or `powershell -File tools/decoy_audit/run.ps1 -Rebuild`)
Expected: `[build] compiling synth_dump.exe ...` then a clean run (the scorecard prints; a non-clean build fails at `build failed`).

Rebuild the probe_audit binary (stub signature changed):
Run: `pwsh tools/probe_audit/run.ps1 -Rebuild` (if that flag exists; otherwise delete `tools/probe_audit/probe_dump.exe` and run `tools/probe_audit/run.ps1`)
Expected: clean build + run.

Run both test suites:
Run: `"C:/Program Files/Python312/python.exe" -m pytest tools/decoy_audit/tests tools/probe_audit/tests -q`
Expected: **all pass**, including `test_personas.py::Personas::test_persona_ble_is_realistic_phone_shape`.

- [ ] **Step 10: Commit**

```bash
git add main/templates.c main/templates.h main/ble_devices.c main/ble_devices.h main/phantom.c \
        main/roster.c main/roster.h tools/probe_audit/host_stubs/ble_devices_stub.c \
        tools/decoy_audit/README.md tools/decoy_audit/tests/test_personas.py
git commit -F - <<'EOF'
feat(persona-ble-shape): Law-3-safe phone BLE shape for personas

Personas now advertise a terse phone shape (flags-only / 16-bit service-UUID
list) on a widened 180-1000 ms interval band, built by template_build_phone,
instead of a vendor-matched accessory payload from roster_pick_company (removed).
Closes the device-class tell (phone twin no longer advertises as earbuds/Tile)
and the interval/vendor monoculture. company_id is always 0; iPhone personas
stay flags-only (no Continuity). Vendor-match tests replaced with a
realistic-phone-shape test.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01TgsxaF69foVD8qLkeULeJy
EOF
```

---

### Task 2: Firmware compile-check + persona-active measurement

**Files:** none changed (verification + a short results note appended to the spec).

**Interfaces:** none.

- [ ] **Step 1: Firmware compile-verify (both targets)**

The persona binding is firmware code; confirm it still builds on both chips. From the repo root, with the IDF export sourced (`~/esp/v5.5/esp-idf/export.ps1`):
Run: `idf.py -B build_c5 set-target esp32c5 build` then `idf.py -B build_c6 set-target esp32c6 build`
Expected: both `Project build complete`. (If the toolchain is unavailable in this environment, record that and defer to a bench build — do not block the measurement on it.)

- [ ] **Step 2: Run the persona-active scorecard**

Run: `pwsh tools/decoy_audit/run.ps1` (uses the default `private/long.pcap`)
Capture the `[persona] address-type mix with personas active ...` block and the headline scorecard.

- [ ] **Step 3: Record the measured deltas**

Append a short "Results (2026-07-17)" section to `docs/superpowers/specs/2026-07-17-persona-ble-shape-design.md` with the persona-active `interval`, `ad_structure`, and `address_type_mix` numbers vs `long.pcap`, and the delta from the prior baseline (interval was +0.12; expect it lower). State plainly if any number did not move as expected. Commit:

```bash
git add docs/superpowers/specs/2026-07-17-persona-ble-shape-design.md
git commit -F - <<'EOF'
docs(persona-ble-shape): record persona-active measurement after the shape fix

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01TgsxaF69foVD8qLkeULeJy
EOF
```

---

## Self-Review

- **Spec coverage:** template_build_phone (Task 1 S3), ble_device_sync rewire (S4), phantom flag (S5), roster_pick_company removal (S6), stub (S7), README (S8), test rewrite (S1), measurement (Task 2). All spec sections covered.
- **Type consistency:** `template_build_phone(bool, uint8_t[31], uint8_t*, uint16_t*)` and `ble_device_sync(int, int, bool, uint32_t, uint32_t, uint32_t)` are used identically in every reference (templates.h decl, ble_devices.c call, phantom.c call, stub). `enc_svc_uuid16`/`rnd_range` signatures match their `templates.c` definitions.
- **Placeholder scan:** none — all code shown in full.
