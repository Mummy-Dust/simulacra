# CYD Necromancer Dashboard — Design

**Date:** 2026-07-11
**Status:** Approved — decisions locked (§10); implementation plan to follow
**Design reference:** the rendered mockup (home + board-detail at true 240×320), necromancer-hacker skin.
**Builds on:** the existing CYD UI framework — `radar_ui_t` (view state machine) + `radar_render_view` (banded full-frame renderer) + the `cyd_main.c` touch loop. This is a **reskin + restructure**, not a rewrite.

## 1. Problem / goal

The CYD (Vigil control node) currently boots into a **threat radar** and cycles through flat views (RADAR → DETAIL → STATS → LIBRARY → CONTROL) on tap. It works, but (a) it's utilitarian, not something to show off, and (b) after Milestone A the CONTROL presets are mostly **inert** (only Dormant/Wake still does anything), so the UI misrepresents what it controls.

Rebuild the CYD front-end as a **polished, on-theme dashboard**: an icon-sigil home that surfaces fleet health at a glance and navigates into detail views — legible to a DEFCON passer-by, still a real instrument. Reframe the data in the project's necromancer vernacular (decoys = **shades**, ambient devices = **the living**, detections = **hunters**), drop the dead controls, and make per-node status first-class.

**DEFCON-ready v1 scope** (this spec): home + board-detail + hunters(radar) + a trimmed control page + per-node status + the theme. Explicitly *not* a from-scratch UI toolkit.

## 2. Honest constraints (named, not a footnote)

- **Renderer:** classic ESP32, no full framebuffer — `radar_render_view` draws top-to-bottom in a reused **band buffer**. So the design is **flat fills + hairline rules + simple sigils** (all cheap). The mockup's soft glows/scanlines are *target vibe only*; on-panel they become subtle dithered halos or are dropped. **No LVGL** (RAM cost) — we extend the existing banded renderer.
- **Type:** the panel renders a **bitmap font**, not the mockup's system serif/mono. The serif small-caps grimoire titles map to the device's display weight; data stays in the fixed font. Legibility at 240×320 wins over fidelity.
- **Touch:** resistive XPT2046, bit-banged, single-touch, no gestures — tap targets must be finger-sized (≥ ~40px) and navigation must work with taps + a back target only.
- **Color:** 16-bit RGB565. The palette (§5) is chosen to survive 565 quantization (no subtle gradients relied on for meaning).

## 3. Architecture

Extend, don't replace, the three existing pieces:

- **`radar_ui_t` view state machine (`radar_ui.{c,h}`)** — add a `RADAR_VIEW_HOME` view and make it the **idle/home** (replacing "idle returns to RADAR"). Change navigation from "tap advances to next view" to **"tap a home sigil → that view; back-target → home."** Keep the idle-return-to-home and backlight-timeout behavior. The threat-wake behavior (a new follower forces the screen on) is preserved and wakes to **Hunters** — a live follower is an alert, so the screen jumps straight to the detection view.
- **`radar_render_view` banded renderer (`radar_render.{c,h}`)** — add a `render_home` path (fleet strip + sigil grid + ticker) and reskin the existing view renderers (DETAIL/STATS/LIBRARY/CONTROL) to the theme. Factor shared primitives (sigil blitter, stat block, pulse dot, chip, hairline) into small helpers so every view shares one visual language.
- **`cyd_main.c` touch loop** — map taps to the new home-grid hit regions + back-target; dispatch to `radar_ui_on_input` with the selected view. Reskin the fleet modal to match.

**Per-node status (new).** Today the CYD holds a single `s_status` (last received). The fleet strip needs **per-node** status: extend the CYD to keep a small table of `radar_wire_status_t` keyed by fleet-member identity (via `fleet_db`), updated as each node's status arrives, with a liveness timestamp (stale → "silent"/greyed). This is the one genuinely new data-plane piece; it reuses the existing signed status wire unchanged.

## 4. Views (v1)

Mapping the mockup's sigil grid onto the existing view enum + the new home:

| Home sigil | View | Content | Source |
|---|---|---|---|
| **The Circle** | HOME→node detail (reuse DETAIL) | per-node hero: shades bound, form breakdown, echoes, the living, hunters, Dormant/Wake | per-node `radar_wire_status_t` |
| **Hunters** | RADAR (reskinned) | the detection radar/list — followers & fingerprint matches | `threats[]` |
| **The Living** | STATS (reskinned) | ambient crowd model — population, arrivals, observation depth | `pop_ewma`, `total_obs`, `epoch` |
| **Rites** | CONTROL (trimmed) | **Dormant/Wake** (the surviving control) + auto-density indicator; dead presets removed | `flags`, settings |
| **Wards** | LIBRARY / fleet modal | fleet keys / enrolment / the signature library | `fleet_db`, librarian snapshot |
| **Grimoire** | INFO (new, minimal) | firmware version, fleet epoch, node roster/liveness — a small "about" stub | build info, `fleet_db`, per-node table |

The **HOME** screen itself: top bar (wordmark + channeling pulse + uptime), the **live fleet strip** (one card per node: name, shade count, activity sparkline, state chip), the **sigil grid**, and an aggregate ticker.

**Shade-form breakdown (Milestone-A showcase).** The board-detail's *restless / wandering / bound* (RPA / NRPA / static) counts are the headline of the new work — but `radar_wire_status_t` carries only the `active_devices` total today. v1 adds **one small wire field** (LOCKED): three `uint8_t` subtype counts (restless/wandering/bound) on the decoy's status, sourced from `ble_devices`, surfaced in DETAIL. This is the only wire change. The decoy computes the counts from its live device population; the CYD renders them as the form breakdown.

## 5. Theme tokens (RGB565-safe)

From the approved mockup. Named constants in the renderer (no magic 565 literals):

| Token | Hex | Role |
|---|---|---|
| `void` | `#09060F` | ground |
| `crypt` | `#160E24` | panels / tiles |
| `edge` | `#2C1E45` | hairline rules / rune borders |
| `bone` | `#ECE5D4` | primary text |
| `ash` | `#8579A0` | muted labels (violet-grey) |
| `arcane` | `#A45CF5` | accent — sigils, selection, summoning |
| `channel` | `#4FE0B0` | semantic: alive / channeling |
| `ward` | `#E6A64F` | semantic: warning / dormant-pending |
| `hunter` | `#F0555F` | semantic: detection / threat |

Semantic colors are separate from the accent. Sigils are simple flat/stroked glyphs (pentacle-circle, watching eye, heartbeat, star, key-eye, tome) drawn by a small sigil blitter — no bitmaps to store if drawn as primitives, or 1-bit masks if simpler.

## 6. Vernacular (fixed, with meaning)

`shades` = live BLE decoys (`active_devices`) · `restless/wandering/bound` = RPA/NRPA/static forms · `echoes` = Wi-Fi probes (`probes_sent`) · `the living` = ambient real devices (`pop_ewma`/`total_obs`) · `hunters` = followers + fingerprint matches (`threats[]`) · `channeling/dormant` = advertising vs paused (`flags` bit0) · `the circle` = the enrolled fleet.

## 7. Testing strategy

The render + UI logic is already partly host-testable (the project has `radar_geom`/`radar_ui` unit tests in `churn_selftest`). Extend that discipline:
- **Pure UI-state tests** (host, in the self-test harness): home-is-idle-default; tap-sigil selects the mapped view; back-target returns home; idle-return-to-home; wake-on-follower; per-node status table updates + stale-out.
- **Pure render smoke** where feasible: `radar_render_view(HOME, …)` fills the band buffer without overflow for representative status (0 nodes, N nodes, a hunter present) — a golden-ish size/no-crash check, not pixel diffing.
- **On-target:** flash the CYD, verify home renders, taps navigate, fleet strip shows per-node live data from real decoys, Dormant/Wake still works, idle/backlight behave. This is the DEFCON acceptance pass.

## 8. Non-goals (YAGNI)

- **No LVGL / no new UI toolkit** — extend the banded renderer.
- **No new controls** beyond Dormant/Wake + an auto-density indicator. The dead density/timing presets are **removed**, not re-wired (see the BLE milestone's follow-up rationale: churn-speed knobs are anti-realistic).
- **No new telemetry** except the optional 3-byte shade-form field (§4).
- **No animation beyond the existing sweep + a channeling pulse** — extra motion reads as noise on a device screen.
- **No touch gestures** — taps + back-target only.
- **Grimoire (logs/settings) is a stub or fold** in v1 — full logs viewer is later.

## 9. Global constraints

- **Public repo** — no absolute local paths, OS usernames, real MACs, or real SSIDs in committed files.
- **Target** — the CYD firmware (`cyd/`), classic ESP32 + ILI9341 240×320 + XPT2046. Decoy firmware (`main/`) changes limited to the optional shade-form status field.
- **Reuse** — build on `radar_ui` / `radar_render` / `fleet_db` / the signed status wire; do not fork them.
- **Honest ceiling carried over** — the dashboard reports what the fleet does; it does not change the anti-fingerprinting defense or its co-location ceiling. No UI copy overstates protection ("hidden" is a vibe, not a guarantee).

## 10. Decisions (locked) + known limits

- **Shade-form wire field — LOCKED IN.** Add the 3-byte subtype counts (restless/wandering/bound) to the decoy status; DETAIL renders the form breakdown (the Milestone-A showcase).
- **Follower-wake target — LOCKED: HUNTERS.** A new hunter wakes the screen straight to the detection view.
- **Grimoire — LOCKED: minimal INFO stub.** Firmware version, fleet epoch, node roster/liveness in v1; full logs viewer is later. Keeps the 6-tile grid complete.
- **Known limit — fleet size on the strip.** 3 nodes fit cleanly; past 3, the strip needs paging/scroll (deferred).
