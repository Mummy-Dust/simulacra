# Fleet Key Provisioning (ECDH Enrollment) — Design

**Date:** 2026-07-08
**Status:** Design — approved for planning
**Branch:** `feat/fleet-key-provisioning` (off `origin/main` `c688d70`)

## Goal

Stop baking the shared ESP-NOW transport key into firmware and git. Instead, a
decoy joins the fleet by an **on-air, mutually-authenticated ECDH enrollment**
with Vigil, receives the fleet transport key over that channel, and stores it in
**NVS**. This gets the symmetric secret out of the repo, solves the key-bootstrap
problem, and gives us **rotation and per-node revocation**.

## Context: what exists today

Three secrets currently live as compile-time headers baked into every firmware
image (and committed to git as placeholders):

| Secret | File | Role | Symmetry |
| --- | --- | --- | --- |
| `SIMULACRA_ESPNOW_KEY[32]` | `components/simulacra_radar/radar_key.h` | AES-GCM transport key for the ESP-NOW link (`radar_wire_seal/open`) | **symmetric — same 32 bytes on every board** |
| `SIMULACRA_CTRL_PK[32]` | `components/simulacra_radar/sim_ctrl_key.h` | Decoys verify signed CONFIG presets | asymmetric (public) |
| `SIMULACRA_CTRL_SK[64]` | `cyd/main/sim_ctrl_sk.h` | Vigil signs CONFIG presets | asymmetric (secret, Vigil-only) |

The **control** keypair is already asymmetric and satisfies the "capturing a
decoy ≠ fleet control" guarantee ([[vigil-remote-control]]). The weak spot is the
**symmetric transport key**: it is identical on every node and shipped in the
image, so capturing any one node yields the whole fleet's transport key, and it
cannot be rotated without reflashing every board.

Key transport facts (verified in code):

- `radar_wire_seal(frame, len, type, payload, plen, key[32], salt[4], counter)`
  and `radar_wire_open(...)` already take the **key as a parameter** — the 32-byte
  key is not hardcoded inside the wire layer. Making it a runtime NVS value is a
  call-site change, not a crypto rewrite. (`components/simulacra_radar/radar_wire.h`)
- **Every** frame type (STATUS, REQUEST, LEARN_SYNC/OFFER, SIG_SYNC, FLEET_MACS,
  CONFIG) is wrapped by this AES-GCM PSK layer in `main/esp_now_link.c`. An
  un-enrolled decoy has no PSK, so **enrollment frames cannot use this layer** —
  they ride raw ESP-NOW and are secured entirely by the ECDH/Ed25519 layer we add.
- TweetNaCl is already vendored (`components/tweetnacl/`) and provides everything
  needed: `crypto_sign` (Ed25519) and `crypto_box` (Curve25519 ECDH + XSalsa20-
  Poly1305 authenticated encryption). **No new crypto dependency.**
- The main-task stack was already bumped to 12288 for TweetNaCl signing
  ([[vigil-remote-control]]); the enrollment crypto reuses that budget.

## Trust model

**Root of trust (present before any enrollment):** the decoy ships with
`SIMULACRA_CTRL_PK` (Vigil's Ed25519 public key) baked in. This is non-secret and
stays baked — it is precisely how a decoy authenticates Vigil during enrollment.
Nothing else about the fleet key is baked in.

- **Decoy authenticates Vigil** via an Ed25519 signature over the enrollment
  OFFER, verified with `SIMULACRA_CTRL_PK`.
- **Vigil authenticates the decoy** via an **identity allowlist (TOFU)**: each
  decoy has its own X25519 identity keypair; Vigil only completes enrollment for
  identity public keys on its allowlist.

Capturing a decoy still yields **only that decoy's** identity key and the current
fleet transport key — both revocable by dropping the identity from the allowlist
and rotating the fleet key. It yields **no** signing authority (no `CTRL_SK`).

## Identities and storage

**Decoy (Ward C5 / Shade C6):**
- On first boot, generate an X25519 **identity keypair** (`crypto_box_keypair`).
  Store the 32-byte secret in **NVS** (namespace `simfleet`, key `id_sk`); derive
  and cache the public key.
- Print the identity **fingerprint** (first 8 bytes of SHA-256 of the pubkey, hex,
  grouped) over serial at boot, e.g. `fleet id: 3f2a-91c4-be07-15dd`. The operator
  reads this off the USB console at flash time.
- Store the fleet transport key + epoch in NVS (`k_fleet`, `k_epoch`) once
  enrolled. Keep the **previous** key (`k_prev`) across a rotation for a grace
  window.

**Vigil (CYD):**
- Holds the Ed25519 control keypair as today.
- Owns the fleet transport key `K_fleet` (+ `epoch`) and an **allowlist** of
  trusted decoy identity pubkeys. Both are **sealed on SD** reusing the existing
  `learn_db` AES-256-GCM / HKDF-SHA256 sealing pattern
  (`components/simulacra_radar/learn_db.{h,c}`), at
  `/sdcard/simulacra/fleet.db`. Generates a random `K_fleet` on first run if absent.

## Enrollment handshake

Three messages over **raw ESP-NOW broadcast** (new frame types, not PSK-wrapped).
The operator opens a bounded **~30 s pairing window** by tapping **Enroll** on
Vigil. Notation: `‖` = concatenation; `Sign`/`Verify` = Ed25519; `Box`/`Unbox` =
`crypto_box`/`crypto_box_open`; `tag_*` = distinct 4-byte domain-separation
constants; nonces are fresh random.

```
                    Vigil (has CTRL_SK, K_fleet, allowlist)         Decoy (has CTRL_PK, id keypair)

 window opens  ──▶  M1 OFFER  (broadcast, signed)
                    { vigil_eph_pk, nonce_v, epoch,
                      sig = Sign(CTRL_SK, tag_OFFER‖vigil_eph_pk‖nonce_v‖epoch) }
                                                             ──▶  Verify sig with CTRL_PK.
                                                                  If unenrolled/seeking:
                    M2 REQUEST (unicast to Vigil)           ◀──
                    { id_pk, nonce_d,
                      box_req = Box(tag_REQ‖nonce_v, to=vigil_eph_pk, from=id_sk) }
   check id_pk ∈ allowlist  (TOFU: if new, show
   fingerprint on screen, operator Accept/Reject);
   Unbox(box_req) with id_pk proves possession of id_sk.
                    M3 GRANT   (unicast to decoy)           ──▶
                    { nonce_g,
                      box_grant = Box(tag_GRANT‖K_fleet‖epoch‖nonce_d,
                                      to=id_pk, from=vigil_eph_sk) }
                                                                  Unbox(box_grant) with
                                                                  vigil_eph_pk + id_sk;
                                                                  check nonce_d fresh; store
                                                                  K_fleet+epoch in NVS. Enrolled.
```

Why this is sound:

- **Vigil authenticity:** `vigil_eph_pk` is bound to Vigil by the Ed25519
  signature in M1. The decoy trusts `vigil_eph_pk` only because `CTRL_PK` verified
  it. `box_grant` is authenticated *from* `vigil_eph_sk`, so opening it with the
  signed `vigil_eph_pk` confirms the grant came from the same Vigil.
- **Decoy authenticity:** `box_req` is authenticated *from* `id_sk`; Vigil opens
  it with `id_pk`, proving the requester holds the private half of the identity it
  claims. Enrollment only proceeds if `id_pk` is (or is accepted onto) the allowlist.
- **Freshness / anti-replay:** `nonce_v` ties M2 to this OFFER; `nonce_d` ties M3
  to this REQUEST. A replayed OFFER carries a stale `nonce_v`; a replayed GRANT
  carries a stale `nonce_d`. Confidentiality of `K_fleet` comes from `crypto_box`
  (only `id_sk` opens M3).
- **MITM:** an attacker cannot forge M1 (no `CTRL_SK`) and cannot open M3 (no
  `id_sk`). The only residual risk is a rogue device presenting its own `id_pk`
  during the pairing window — defeated by the TOFU accept step: Vigil shows the
  requester fingerprint and the operator eyeball-matches it to the fingerprint the
  genuine decoy printed over serial before tapping Accept.

## TOFU accept flow (Vigil screen)

During the pairing window, when a REQUEST arrives from an `id_pk` **not** yet on
the allowlist, Vigil renders the requester's fingerprint on-screen and waits for a
touch **Accept / Reject**:

- **Accept** → add `id_pk` to the allowlist (sealed to SD) and send M3 GRANT.
- **Reject** or window timeout → drop; no grant.

An `id_pk` **already** on the allowlist (a re-enroll / rotation) is granted
automatically within the window without re-prompting. This is standard TOFU: trust
is established interactively once, verified out-of-band against the serial
fingerprint, then persisted.

## Rotation and revocation

- **Rotate:** Vigil generates a new `K_fleet` with a higher `epoch`, then
  **re-enrolls each allowlisted node** (unicast GRANT with the new key). Enrolled
  decoys keep `{current, previous}` keys during a grace window.
- **Revoke:** remove the target `id_pk` from the allowlist, then rotate. The
  revoked node is simply not re-enrolled, so it never receives the new key and
  goes dark on the next rotation.
- **Why not broadcast re-key:** a broadcast carrying the new key would also reach
  a to-be-revoked node (it still holds the old key and could read a PSK-wrapped
  broadcast), defeating revocation. Per-node unicast re-enrollment is therefore
  the authoritative path. (A broadcast fast-rekey for the no-revocation case is
  explicitly **out of scope** — YAGNI.)

**Grace window without a wire change:** during rotation a node holds both the new
and previous fleet keys in RAM. `radar_wire_open` **tries the current key, then
the previous key**; success with either decrypts the frame. This avoids adding a
key-epoch byte to the wire header (which would touch every node's frame format).
The `epoch` is tracked in NVS/enrollment for bookkeeping and to order rotations.

## Boot behavior

On boot the decoy loads `k_fleet` from NVS:

- **Present:** normal operation — full decoy churn + ESP-NOW telemetry/sync using
  the loaded key.
- **Absent:** **seek-enrollment mode** — full decoy churn (BLE/Wi-Fi) still runs
  (it does not need the fleet key), but instead of ESP-NOW telemetry the node
  listens for signed OFFER frames and participates in the handshake. On a
  successful GRANT it stores the key and resumes full ESP-NOW participation
  without a reboot.

## New frame types and files

**Wire (shared component `simulacra_radar`):**
- `RADAR_TYPE_ENROLL_OFFER = 8`, `RADAR_TYPE_ENROLL_REQUEST = 9`,
  `RADAR_TYPE_ENROLL_GRANT = 10` (raw ESP-NOW, not `radar_wire_seal`-wrapped).
- New `components/simulacra_radar/enroll_wire.{h,c}`: pack/parse + the three
  crypto operations (`enroll_offer_sign/verify`, `enroll_request_build/open`,
  `enroll_grant_seal/open`). Pure, host/target-testable, depends only on
  `tweetnacl`.

**Decoy (`main`):**
- New `main/fleet_key.{h,c}`: NVS-backed identity keypair + fleet key store
  (`fleet_key_init`, `fleet_key_get`/`fleet_key_prev`, `fleet_key_have`,
  `fleet_key_set`, `fleet_id_pk`, `fleet_id_fingerprint`).
- New enrollment responder wired into `main/esp_now_link.c`: handle OFFER →
  emit REQUEST → handle GRANT; seek-enrollment when unkeyed; try current-then-prev
  key in `on_recv`.
- Build gate `SIMULACRA_FLEET_PROVISION` added to the `foreach` forwarder in
  `main/CMakeLists.txt`. `radar_key.h` keeps a **fallback** compile-time key only
  when the gate is **off**, so existing builds/tests are unaffected.

**Vigil (`cyd`):**
- Fleet DB (allowlist + `K_fleet`/epoch) sealed on SD via `learn_db` pattern.
- Enrollment authority: pairing-window state, OFFER broadcaster, REQUEST handler
  with allowlist check, TOFU accept UI on the touchscreen, GRANT sender, and a
  rotate/revoke action.

**Tooling/docs:**
- `tools/gen_ctrl_key.py` unchanged (control keypair is the root of trust).
- README/provisioning note: read a decoy's serial fingerprint → confirm on Vigil.

## Testing

**On-target selftests** (`-DCHURN_SELFTEST=1`, `ST_CHECK`, serial `selftest: N/N pass`):
- OFFER sign → verify with `CTRL_PK` passes; tampered OFFER fails.
- REQUEST `box_req` round-trips; wrong `id_pk` fails.
- GRANT `box_grant` round-trips; recovered `K_fleet`/`epoch`/`nonce_d` match; a
  decoy without the right `id_sk` cannot open it.
- Allowlist accept vs. reject decision.
- Replay: stale `nonce_v` (OFFER) and stale `nonce_d` (GRANT) rejected.
- Key-epoch: `radar_wire_open` decrypts a frame sealed under the previous key
  during grace, and rejects a frame under an unknown key.
- NVS: identity keypair persists across reboot; fleet key store/reload.

**2-board bring-up** (Vigil + one decoy, on target): flash a decoy with no fleet
key → it seeks enrollment → tap Enroll on Vigil → fingerprint appears → Accept →
telemetry/radar flows → rotate → still flows → revoke that decoy + rotate → decoy
goes dark (no telemetry accepted).

## Scope

**In:** on-air ECDH enrollment (3-message handshake), decoy X25519 identity in
NVS, Vigil allowlist + fleet key sealed on SD, TOFU accept on Vigil screen,
rotation + revocation via re-enrollment, grace-window try-two-keys, seek-enrollment
boot mode, selftests + 2-board bring-up.

**Out (YAGNI):** per-node pairwise session keys for normal traffic; broadcast
fast-rekey; moving the control keypair off firmware (PK must stay baked as the
root of trust; SK is already operator-generated locally, not a real git secret);
Ward's screen for enrollment UX (Ward's display is a separate parked project —
[[ward-display-console]]); over-the-air firmware update of keys beyond the fleet
transport key.

## Open questions

None outstanding — framing (on-air enrollment, full feature in one spec), decoy
authentication (identity allowlist / TOFU), and the accept UX (on Vigil's screen
with serial cross-check) are all resolved.

## Related

[[vigil-remote-control]] · [[modular-multiboard-roles]] · [[self-learning-templates]]
(the `learn_db` sealing pattern reused here) · [[feature-backlog]]
(this closes the "fleet key provisioning (ECDH)" backlog item) ·
[[ward-display-console]] (parked; not a dependency).
