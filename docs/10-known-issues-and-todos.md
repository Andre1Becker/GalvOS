# Chapter 10 — Known Issues & Todos

> This is an honest list. Every project has rough edges — GalvOS more than most, because it is a one-person hardware/firmware/UI project that started as a dimmer fix and grew considerably beyond that. Nothing here is hidden. If you run into one of these, you are not doing it wrong.

## Table of Contents

- [Critical Issues](#critical-issues)
- [Hardware Issues](#hardware-issues)
- [Pattern Issues](#pattern-issues)
- [Text Mode Issues](#text-mode-issues)
- [Calibration Issues](#calibration-issues)
- [UI Issues](#ui-issues)
- [Planned Features](#planned-features)
- [Contributing a Fix](#contributing-a-fix)

---

## Critical Issues

These affect core functionality and should be resolved before relying on those features in production.

### SD Card Causes Galvo Malfunction

**Severity:** Critical  
**Status:** Fixed in firmware v5.90.0 — **hardware rewire still pending**  
**Symptom:** If an SD card is inserted, the galvos behave erratically — incorrect output, uncontrolled movement.  
**Root cause:** Found. Not bus contention — the SD card was wired onto the DAC8562's SPI2 pins (SCK=GPIO12, MOSI=GPIO11, MISO=GPIO2, CS=GPIO9) under the assumption that Arduino's `SPIClass(HSPI)` attaches to SPI2_HOST on ESP32-S3. It does not: `HSPI` is bound to the independent SPI3 peripheral. Routing SPI3 onto SPI2's GPIOs meant two different peripherals both drove the same pins through the GPIO matrix, which only lets one peripheral own a pin's output at a time — `SPIClass::begin()` silently stole GPIO12/GPIO11 away from the DAC every time SD init ran, and real SD card traffic then appeared on the DAC's own clock/data lines, corrupting its output.  
**Fix:** SD moved to fully independent GPIOs (SCK=GPIO5, MOSI=GPIO6, MISO=GPIO1, CS=GPIO42) on SPI3, with zero pin overlap with the DAC's SPI2 — see `include/pinmap.h` and `hardware/netlist.txt`. **The perfboard has not been rewired yet** — until the 4 SD wires are physically moved to GPIO5/6/1/42, `sd_card::init()` will simply fail to find a card (safe, but SD stays non-functional).  
**Impact:** ILDA file playback from SD card is non-functional until the rewire is done. ILDA files loaded via other means (Art-Net, Ether Dream) are unaffected.  
**Workaround (until rewired):** Leave the SD card slot empty. All other features (presets, WebUI, DMX, Art-Net) work normally.

---

## Hardware Issues

### OPA4134 Gain Deviation

**Status:** Known, compensated in firmware  
**Detail:** The OPA4134 feedback resistors R2/R4 are 22 kΩ instead of the theoretical 10 kΩ, resulting in a gain of 2.2× rather than 2.0×. This means the full DAC swing would produce slightly more than ±5V at the galvo input. Compensated via `dac_limit_min`/`dac_limit_max` in `RuntimeConfig` (default: 0x0666..0xF999, ≈95% of full range). No action required unless you replace the resistors.

---

## Pattern Issues

None atm :-)

---

## Text Mode Issues

All five bugs below were fixed in **v6.05.7** ("Text mode gets a proper wardrobe, not just fig leaves"). Kept here for the curious and for anyone still running an older build.

### ~~Bounce Animation — No Effect~~ — Fixed in v6.05.7

~~Selecting the "Bounce" animation in Text mode has no visible effect. The text renders statically.~~

Root cause: the animation's edge margin (18000 DAC units) was smaller than the half-width most normal-length strings already occupy, so the computed bounce range clamped to zero — the toggle did precisely nothing. Margin is now derived from the real DAC edge (32767 units).

### ~~Typewriter Animation — Runs Once Only~~ — Fixed in v6.05.7

~~The Typewriter animation plays through once and stops. It does not loop.~~

Root cause: the pattern engine's `n==0` guard — hit on Typewriter's own blank beat between passes — skipped advancing the animation phase, freezing its clock on the exact tick that produced nothing. Phase now advances on blank frames too.

### ~~Orbit Animation — Rotation Direction~~ — Fixed in v6.05.7

~~The Orbit animation's rotation direction is fixed and cannot be switched. A control to reverse the direction is planned.~~

Added a dedicated `orbit_reverse` toggle (WebUI: Text tab → "🔄 Reverse Spin (Orbit)", API: `orbit_reverse` field on `POST /api/text`) — `flip_x`/`flip_y` only reorder letters along the arc, they never touched the actual spin rate, so this needed a field of its own.

### ~~Star Wars Scroll — Two Issues~~ — Fixed in v6.05.7

~~Symptom 1: The scroll direction is incorrect (text moves in the wrong direction). Symptom 2: The text renders as dots only instead of the expected vector strokes.~~

The dots issue was a point-budget bug: a hardcoded 80-points-per-glyph ceiling was tighter than the Text profile's own blank-jump overhead for multi-stroke letters (B/E/K...), so most glyphs collapsed to isolated dots. Removed the artificial ceiling in favor of the real per-letter buffer share. Scroll direction (bottom-large → top-small, vanishing point) is now confirmed correct against the Corner Color Map calibration pattern.

### ~~Scroll Left / Right — Cut Off Mid-Crawl~~ — Fixed in v6.05.7

~~Text never leaves the projection area fully but is cut off and immediately starts over. Bad looking effect :-)~~

Root cause: the scroll period used an 18000-unit edge margin instead of the real DAC edge (32767 units), so the string doubled back before its trailing edge ever cleared the screen. Same fix applied to Bounce (above).

*Bonus, not previously tracked here:* text also used to truncate after ~8 characters (a point-density constant put ~300 points on an average glyph against the 2048-point frame buffer) — turned down from 8000 to 1400 in the same release, alongside the empty-text-field now defaulting to `GALVOS` instead of showing nothing.

---

## Calibration Issues

### Several Calibration Patterns Need Fixing

**Status:** Open  
**Detail:** A subset of calibration patterns produce incorrect or unexpected output. Specific patterns affected are under investigation.

### Channel Parameter Not Working

**Status:** Open  
**Detail:** The per-channel selector in the calibration flow does not correctly isolate individual channels in all cases.

### ILDA Standard Test Pattern — Incorrect Output

**Status:** Open  
**Symptom:** The ILDA standard test pattern (used to verify galvo linearity and speed) does not render correctly. This makes it unsuitable as a calibration reference until resolved.
**Solution:** Remove completely as not needed

---

## UI Issues

### Features Not Toggleable via UI

**Status:** Planned  
**Detail:** Some subsystems (e.g. Art-Net receiver) are always active after boot, regardless of whether they are in use. The ability to enable/disable individual features from the WebUI is planned. This would reduce background CPU load and Wi-Fi channel congestion in setups that only use DMX.

---

## Planned Features

These are features that are designed and intended, but not yet implemented.

### ~~Auto-Tuning via Global Shutter Camera~~ — Implemented in v6.03.0–v6.05.0

~~The plan is to add an auto-tuning mode that uses a global-shutter camera input to capture the projected image and automatically calibrate galvo linearity, offset, gain, and potentially optimizer parameters. This would replace the current manual calibration workflow.~~

Done. Firmware v6.03.0 added the `/api/calib-cam/*` camera-in-the-loop calibration API, and the companion `scripts/optimizeGalvo/optimizeGalvo.py` tool (added alongside, now at v2.5.0) drives it end-to-end: pixel↔DAC homography from a 4-dot reference pattern, Optuna-based auto-tuning of the Vector/Smooth/Waves/MultiObject optimizer profiles against camera-measured beam quality, a `diagnose` mode that separates geometry drift from optimizer-setting problems, and an `autotune-camera` mode that tunes the camera's own exposure/gain/threshold instead of firmware parameters. v6.04.1 added per-channel pattern color (patterns default to blue now, to avoid RGB boresight smear on a mono camera) and v6.05.0 fixed a frame-budget edge case where heavily-tuned-down profiles could leave closed shapes not reconnecting.

This does **not** replace the manual galvo geometry calibration (offset/gain/swap/invert) in the Calibration tab — it only auto-tunes optimizer scan/dwell parameters. See the new [Chapter 6 — Camera-in-the-Loop Auto-Tuning](06-camera-autotuning.md) for the full workflow.

**Optimize Heap Usage even more:** ~~117KBs to 121 KBs Untracked Heap Memory usage~~
Fixed in v6.04.0: ~120 KB of `static` scratch buffers (preset/paint/calib pattern
verts, EtherDream RX, SD file table, gPaint canvas, optimizer transform scratch)
moved from DRAM .bss to lazily allocated PSRAM (`src/util/ps_scratch.h`).
Static RAM 180,728 B -> 57,872 B; the "untracked" remainder is the WiFi/lwIP/
AsyncTCP stack plus FreeRTOS task stacks, which cannot leave internal RAM.

---

## Contributing a Fix

If you fix one of the issues above, please see [Chapter 9 — Contributing](09-contributing.md) for the patch workflow and commit message format.

When submitting a fix for a known issue, reference the issue name from this chapter in your commit message body:

```text
Fix Typewriter animation looping

Typewriter now restarts after completing one pass.
Resolves: "Typewriter Animation — Runs Once Only" (docs/10-known-issues-and-todos.md)
```
