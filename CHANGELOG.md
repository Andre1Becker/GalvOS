# Changelog

All notable changes to GalvOS, newest first. Versions are `MAJOR.MINOR.PATCH`
(see [Chapter 3 — Version Bumps](docs/03-build-and-config.md#version-bumps)) and are
tagged in Git as `vX.Y.Z`. Patch releases are folded into their minor line.

The WebUI carries its own independent `UI_VERSION`; it is not tracked separately here.

---

## 6.7x — Output rate autotune (2026-08-19)

- **6.79.1** — `points_per_sec` returned a wrapped garbage value after long polling gaps.
  `pointsPerSec()` did `(delta_points * 1000) / elapsed` in 32-bit, and its averaging
  window is not 1 s — nothing recomputes the counter on a timer, so the window is however
  long it has been since something last *read* it. With no poller (WebUI closed) that
  window grows without bound, and at ~45 kpps a gap over ~95 s overflows the multiply.
  Measured on this board: first reads after 100 s and 140 s of silence returned 2457 and
  14694 pps against a true 45355 — matching the wrapped arithmetic to within 0.1%, while
  30 s and 60 s gaps read clean. Not cosmetic: `optimizeGalvo` derives the camera shutter
  from this field, and a 4.8 kpps reading made it compute a 251.6 ms frame period for a
  pattern that takes 26.8 ms. Both this and `fps()` (same shape, overflow past ~40 h
  rather than 95 s — latent, but no reason for the two to differ) now use the
  `(uint64_t) … * 1000ULL` idiom `autotuneTrial()` already uses in the same file. This
  also restores the "`points_per_sec` vs `projection.kpps` is the one-line health check"
  note, which was unreliable whenever the WebUI had not been open.

- **6.79** — `galvo_rated_kpps` migrated 45 → the datasheet 15. Not cosmetic: community
  presets ship raw optimizer values with no anchor of their own and the firmware default
  anchor is 15, so every import was read with `r` 3× too large — density 3× low, accel
  ceiling 9× too permissive. The rescale factor has the output rate cancel out, making it
  an exact change of units (`raw_new = raw_old × k^e`, k=3) rather than a re-tune: zero
  values saturate their bounds and effective values are preserved to 1.25e-06 across all
  8 profiles. Snapshots and derivation in `docs/migrations/`. Two fixes shipped alongside,
  each broken alone: `text_renderer.cpp`'s per-glyph density constant was tied to the
  anchor via `1/ppsRatio()` (would have tripled text density into the "only ~8 chars
  render" bug — now anchored to a local `TEXT_DENSITY_REF_KPPS`, verified 763→714→585 lit
  points as output drops 45→30→20 kpps), and the exit-angle warning compared the sample
  clock against the mechanical ILDA derating and would have been pinned on permanently.
  Separately: **`/api/restore` never worked** — the handler ignored `index`/`total` and
  parsed each ~1.4 KB TCP chunk as a whole document, so every ~6 KB backup failed with
  "bad json", WebUI Restore button included. Now reassembles the body first. Found by
  trying to use it for this migration.

- **6.78** — Two defects behind "autotune finds no limit". (1) `galvoTask`'s tick pacer
  accumulated an unbounded deficit: when `period_us` is shorter than the loop body
  (~21.3 µs here, a ~46.9 kpps ceiling) the busy-wait never binds and `next_tick` falls
  behind wall-clock every iteration, so lowering the rate afterwards did nothing for
  60–90 s while the optimizer, producer pacing and inverse filter all assumed the new
  rate. Five copies replaced by one `paceTick()` clamping the deficit to a single
  period. (2) The autotune's overflow criterion could not see this at all —
  `paceProducer()` throttles the producer to ~80% of the *commanded* rate, so a short
  consumer and a short producer stay balanced (`buffer_fill` 14% at 60 kpps, same as at
  43). It now measures achieved output ticks against commanded and requires ≥98%.
  Hardware-verified: the sweep returns 45 kpps with `achieved_pps` 46954, matching an
  independent manual measurement. The device's existing 43 kpps setting delivers 100.0%
  of commanded and was correct all along.

- **6.77** — The Projection tab's sample-rate autotune stopped inventing its own answer.
  It seeded its binary-search ceiling from `galvo_rated_kpps` — the galvo's *mechanical*
  ILDA spec, unrelated to the MCU sample throughput it actually measures — so with a
  rated value raised above the datasheet figure it returned `rated - margin` without
  ever observing a single overflow, and with an honest one it could never propose
  anything above it. It now searches the full 12–60 kpps range independently and reports
  `ceiling_not_reached` when the range runs clean end to end, instead of dressing the
  range top up as a measurement. It also refuses to run at all (`no_load`) when the ring
  is not being fed — with the master dimmer at 0 the producer delivers 25 pts/s into a
  ring draining at tens of thousands, so overflow is structurally impossible and every
  trial passes at any rate. The inverse filter is now refreshed per trial rather than
  only at the end, so a sweep is no longer measured through a filter still configured
  for the pre-sweep rate. WebUI: the Output Sample Rate slider is no longer capped at
  the rated speed (oversampling above the rated point rate is normal — the mirror
  low-passes what it cannot follow), and the Projection tab spells out that the two
  numbers are independent.

---

## 6.6x — Correction stages, cleanup (2026-08-12 → 2026-08-14)

- **6.67** — Triage pass over `bugs01.md`'s backlog. Safety/UX: idle output (no Preset/Text/
  Paint/ILDA/live DMX active) now blanks the beam instead of drawing a full-brightness legacy
  circle; ILDA `/api/ilda/play` no longer blocks the async_tcp task for the whole SD read/parse
  (was timing out `/api/ilda/status` polls and hanging the UI), running the load on its own
  task instead. Correctness: animation phase now advances by wall-clock time instead of once
  per loop iteration (H-Line's Speed slider ran up to 2x its intended rate on sparse frames);
  Autoscale Mode's WebUI labels matched a never-implemented "Off/Fit/Fill" instead of the
  firmware's actual Pulse/Grow/Shrink enum; Wave-preset harmonics fed a post-wrap multiplied
  phase into `aang()` instead of their own call, snapping once per loop instead of looping
  cleanly; Starburst Party's 24 spokes went through 24 separate per-spoke budget checks instead
  of one batched call, silently dropping the back half of the burst once the frame budget ran
  out. UI: Mirror H/V checkboxes were permanently disabled in Kaleidoscope mode, the one mode
  their own click handler exists to switch out of; Auto-Rotation Speed range widened back to
  the shipped default; quick-angle buttons added for Rotation; H-Line gained an optional bounce
  mode (eases back down instead of snapping to the bottom every sweep); glyph outline text
  (Paint's Text tool) now clamps to the DAC range like the main Text tab already did, instead of
  rail-clamping into a smear at the canvas edge. Also: `scripts/optimizeGalvo/optimizeGalvo.py`
  maintenance pass (host-side tooling, no firmware dependency) — `measure-resonance` retries
  with a wider fine-sweep span instead of giving up on Q/`ring_damping_ratio`; `searchSpace.json`
  regenerates with sensible defaults when missing instead of erroring about a "shipped example"
  that never existed; `docs/06-camera-autotuning.md` gained the `calibrate-warp`/
  `measure-resonance`/`tune-dac-range` coverage it never had.
- **6.66** — Blank-jump emission de-duplicated into one shared helper; `/api/status` and
  `/api/state` now build their common fields from a single source so they cannot drift apart.
- **6.65** — Fixes: Particles/Starfield blank-jump window widened (dots stopped drawing
  connect-the-dots streaks), ILDA loader frame counting made consistent across both passes,
  dimmer pipeline no longer applies its scaling more than once, arming refuses while an OTA
  is in flight, Core 1 no longer blocks on SPI2, duplicate watchdog route removed.
- **6.64** — DMX BPM mapped 1:1 with **Beat-Stop** on channel value 0; Tap Tempo no longer
  expires mid-set; Points-Only fade defaults fixed; `/api/paint/set` no longer leaks state
  between clients.
- **6.62** — **Model-based inverse filter**: per-axis regularized deconvolution of the galvo's
  measured resonance, applied to every emitted point (`/api/inverse-filter/*`).
- **6.60** — Clip diagnostic: the firmware reports when output is being clipped rather than
  silently flattening corners.

## 6.5x — Warp, brightness, welding, SVG (2026-08-12)

- **6.59** — Per-segment colors for polygons and stars (`/api/seg_colors`).
- **6.58** — Curvature-adaptive resampling: point spacing follows the curve instead of
  distributing evenly along it.
- **6.57** — 2-opt refinement on top of the nearest-neighbour jump order.
- **6.56** — **Brightness compensation** grid: per-region RGB gain against throw-distance and
  angle falloff (`/api/brightness/*`).
- **6.55** — **Warp** grid (up to 5×5 keystone/surface correction, `/api/warp/*`) plus
  `outputScale`, a proportional pre-scale that keeps corners inside the galvo's linear range.
- **6.54** — **SVG import**: browser-side parsing and simplification into the Paint canvas,
  with SVG file storage on SD (`/api/svg/*`).
- **6.53** — **Laser Welding** effect: travelling torch head, fading afterglow, ballistic sparks,
  rendered from the Paint canvas (`/api/weld*`).
- **6.52** — Mirror mode no longer reflects into itself.
- **6.51** — Kaleidoscope reworked to a true dihedral fold-then-stamp.

## 6.4x — Optimizer refactor wave (2026-07-31 → 2026-08-12)

A systematic pass over `point_optimizer.cpp`, tracked as prompts P1–P23.

- **6.50** — ILDA frames get smooth landing behavior; optimizer comments brought in line with
  what the code actually does.
- **6.49** — Segment reorder (nearest-neighbour jump ordering); Pillar 2 stops treating every
  jump as the same length.
- **6.48** — Five copies of the blank-jump logic consolidated; straight lines no longer dim in
  the middle (endpoint dwell and edge shaping decoupled).
- **6.47** — Frame budget measured instead of estimated.
- **6.46** — Plan and emit stages share one description of the frame.
- **6.45** — Dead knobs removed: `min_segment_pts`, `speedT`, `lift` recoloring; corner severity
  computed once instead of six times.
- **6.44** — Per-frame context tracking; min/max pairs corrected; scratch buffer enlarged;
  PPS scaling extended to every density parameter; acceleration clamp works in both directions.
- **6.43** — Blank jumps land on target: ZV shaper tail extended, budget over-reservation fixed,
  Pillar 3 status published, zero-length jumps skipped.
- **6.42** — Optimizer telemetry (`/api/optimizer-stats`, Live Telemetry card).
- **6.41** — `/update` page rebuilt: firmware **and** filesystem OTA, real error reporting,
  progress bar, no auto-reboot, backup shortcut.

## 6.3x — WebUI rewrite and reorganization (2026-07-29 → 2026-07-31)

- **6.40** — ILDA file delete/rename/download from the browser.
- **6.39** — ILDA eviction and renaming; Helios USB stub removed for good.
- **6.38** — Color animations and Points-Only mode sync to the BPM clock.
- **6.37** — Temperature display unit (°C/°F/K), per-sensor names and offsets.
- **6.36** — Preset defaults and slider ranges brought in line with firmware behavior;
  per-category hover glyphs on preset tiles.
- **6.35** — Independent `UI_VERSION`; Dashboard field-grid layout; fixed a chart error that
  blanked the Dashboard for the first minutes after boot.
- **6.34** — Mobile safety controls: ARM and Master Dimmer always visible, 44 px touch targets,
  accessible status dots.
- **6.33** — Dashboard becomes status-only, Presets tab becomes the performance surface;
  pinned top bar; preset switching no longer flashes a streak.
- **6.32** — Full WebUI rewrite: three themes on a CSS token engine, responsive shell, 13 tabs.
- **6.30–6.31** — Spatial Noise (2D value-noise modulator type), Dotter (dot scatter), and
  optimizer jitter.
- **6.28–6.29** — Camera module (3D view targets) and Duplicator (grid/radial/spiral cloning);
  binding writes debounced off the network path.
- **6.27** — Modulator registry: modules self-register types and targets, UI discovers them.
- **6.26** — Layer engine removed (superseded by the modulators); sidebar reorganization.

## 6.2x — Beat, sequencer, modulation (2026-07-28)

- **6.25** — Presets tab consolidation; BPM Tap route fixed.
- **6.24** — Sequencer transitions no longer freeze modulator state.
- **6.23** — **Modulation engine**: 8 slots × 16 bindings (LFO, noise, envelope, step sequencer).
- **6.22** — **Preset Sequencer**: BPM-quantized set list with blank transitions; never
  auto-starts on boot.
- **6.21** — **Global BPM clock** (Manual / Tap / DMX).
- **6.20** — Network streams no longer contend with DMX for the ring buffer; Ether Dream
  buffer handling and client diagnostics fixed.
- **6.17** — First switchable WebUI themes.
- **6.16** — Per-protocol debug logging for every network input.
- **6.15** — Ether Dream no longer reports a permanent warm-up state.

## 6.1x — ILDA and SD (2026-07-27)

- **6.14** — Live ILDA speed/size parameters; standing SD auto-mount retry.
- **6.13** — ILDA playback passes through the affine transform and velocity clamp only.
- **6.12** — Oversized files greyed out against a PSRAM estimate; upload filenames sanitized.
- **6.11** — Playlist tab folded into ILDA/SD; per-file loading indicator.
- **6.10** — SD subfolder scanning; ILDA player master switch; load-loop yielding and real
  SD mutex enforcement.

## 6.0x — Network protocols, backup, camera tuning (2026-07-21 → 2026-07-26)

- **6.09** — SD tab play controls.
- **6.08** — **sACN/E1.31**, **OSC**, and **Helios network DAC** receivers, each individually
  enable-able.
- **6.07** — **Community Presets**: GitHub browser, on-device storage (20 presets), strict
  schema validation, versioned backup filenames.
- **6.06** — **Backup & Restore** of the full configuration as one JSON document.
- **6.05** — Corner dwell scales down to fit the frame budget (closed shapes always reconnect);
  text mode overhaul.
- **6.04** — ~120 KB of static scratch buffers moved from DRAM `.bss` to lazily-allocated PSRAM.
- **6.03** — **Camera-in-the-loop calibration API** (`/api/calib-cam/*`) and the host-side
  `optimizeGalvo.py` tool.
- **6.02** — DAC8562 SPI clock raised to 40 MHz.
- **6.00–6.01** — NVS partition grown to 256 KB; Wi-Fi scan rewritten against the IDF API;
  orientation/mirroring bugs fixed across text, fireworks, tree, and paint.

## 5.9x — SD card on its own bus (2026-07-21)

- **5.90** — **SD moved to an independent SPI3 bus** (GPIO5/6/1/42). Root cause of "the galvos
  go crazy when a card is inserted": Arduino's `SPIClass(HSPI)` binds to SPI3, so wiring SD onto
  SPI2's pins let it steal the DAC's clock and data lines through the GPIO matrix.

## 5.8x–5.89 — Optimizer, memory, WebUI groundwork (2026-07-18 → 2026-07-20)

- Memory owner registry and the WebUI Memory Viewer (`/api/meminfo`).
- Text optimizer profile; ring-buffer fill reporting fixed.
- Laser-on hold to suppress blank-jump entry strokes.
- Paint canvas scaled to the configured projection zone.
- Pinned header/safety banner; charts gained hover crosshairs and a time axis.

## 5.1x–5.8x — Feature build-out (2026-07-02 → 2026-07-17)

- Point optimizer stages: resample, velocity/acceleration clamps, PPS scaling, static-pattern
  cache, per-preset-class profiles, Smart Defaults, sample-rate autotune.
- Calibration suite: per-channel visibility thresholds, three-circle white balance,
  CIE 1931 perceptual LUT, calibration pattern library, projection zone editor.
- Paint by Finger, Points-Only mode, auto-scale, kaleidoscope, dynamic preset list,
  text renderer overhaul.
- Art-Net input with source-priority arbitration; thermal power reduction wired into fan control.

## 5.0x — The optimizer arrives (2026-06-20 → 2026-07-01)

- **Pillar 1** — adaptive, corner-aware point density.
- **Pillar 2** — distance-proportional, smoothstep-eased blank jumps.
- Color animation engine moved into firmware (DMX CH23–25); segment color mode.
- Raw SPI2 register access for DAC writes (~22 → 30 kpps).

## 4.5.9 — Initial public commit (2026-06-16)

First tracked version: ESP32-S3 firmware replacing the OEM controller, with DAC8562 galvo
output, DMX-512 input, the WebUI, and the hardware safety chain.
