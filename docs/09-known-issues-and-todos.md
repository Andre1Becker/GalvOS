# Chapter 9 — Known Issues & Todos

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
**Status:** Open  
**Symptom:** If an SD card is inserted, the galvos behave erratically — incorrect output, uncontrolled movement.  
**Root cause:** Under investigation. The SD card shares SPI2 with the DAC8562 (SCK on GPIO12, MOSI on GPIO11). A timing or bus contention issue during SD card initialisation appears to corrupt DAC output. The current workaround is to not populate the SD card until this is resolved.  
**Impact:** ILDA file playback from SD card is currently non-functional. ILDA files loaded via other means (Art-Net, Ether Dream) are unaffected.  
**Workaround:** Leave the SD card slot empty. All other features (presets, WebUI, DMX, Art-Net) work normally.

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

### Bounce Animation — No Effect

**Status:** Open  
**Symptom:** Selecting the "Bounce" animation in Text mode has no visible effect. The text renders statically.

### Typewriter Animation — Runs Once Only

**Status:** Open  
**Symptom:** The Typewriter animation plays through once and stops. It does not loop.

### Orbit Animation — Rotation Direction

**Status:** Open  
**Detail:** The Orbit animation's rotation direction is fixed and cannot be switched. A control to reverse the direction is planned.

### Star Wars Scroll — Two Issues

**Status:** Open  
**Symptom 1:** The scroll direction is incorrect (text moves in the wrong direction).  
**Symptom 2:** The text renders as dots only instead of the expected vector strokes.

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

---

## UI Issues

### Features Not Toggleable via UI

**Status:** Planned  
**Detail:** Some subsystems (e.g. Art-Net receiver) are always active after boot, regardless of whether they are in use. The ability to enable/disable individual features from the WebUI is planned. This would reduce background CPU load and Wi-Fi channel congestion in setups that only use DMX.

---

## Planned Features

These are features that are designed and intended, but not yet implemented.

### Auto-Tuning via Global Shutter Camera

The plan is to add an auto-tuning mode that uses a global-shutter camera input to capture the projected image and automatically calibrate galvo linearity, offset, gain, and potentially optimizer parameters. This would replace the current manual calibration workflow.

---

## Contributing a Fix

If you fix one of the issues above, please see [Chapter 8 — Contributing](08-contributing.md) for the patch workflow and commit message format.

When submitting a fix for a known issue, reference the issue name from this chapter in your commit message body:

```
Fix Typewriter animation looping

Typewriter now restarts after completing one pass.
Resolves: "Typewriter Animation — Runs Once Only" (docs/09-known-issues-and-todos.md)
```
