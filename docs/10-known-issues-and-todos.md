# Chapter 10 — Known Issues & Todos

> This is an honest list. Every project has rough edges — GalvOS more than most, because it is a one-person hardware/firmware/UI project that started as a dimmer fix and grew considerably beyond that. Nothing here is hidden. If you run into one of these, you are not doing it wrong.

## Table of Contents

- [Critical Issues](#critical-issues)
- [Resolved Issues](#resolved-issues)
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

None currently open.

---

## Resolved Issues

### SD Card Caused Galvo Malfunction (resolved v6.09.0)

**Severity:** Was Critical  
**Status:** Resolved — perfboard rewired, SD/ILDA confirmed working (galvo output unaffected by card access)  
**Symptom:** If an SD card was inserted, the galvos behaved erratically — incorrect output, uncontrolled movement.  
**Root cause:** Not bus contention — the SD card was wired onto the DAC8562's SPI2 pins (SCK=GPIO12, MOSI=GPIO11, MISO=GPIO2, CS=GPIO9) under the assumption that Arduino's `SPIClass(HSPI)` attaches to SPI2_HOST on ESP32-S3. It does not: `HSPI` is bound to the independent SPI3 peripheral. Routing SPI3 onto SPI2's GPIOs meant two different peripherals both drove the same pins through the GPIO matrix, which only lets one peripheral own a pin's output at a time — `SPIClass::begin()` silently stole GPIO12/GPIO11 away from the DAC every time SD init ran, and real SD card traffic then appeared on the DAC's own clock/data lines, corrupting its output.  
**Fix:** SD moved to fully independent GPIOs (SCK=GPIO5, MOSI=GPIO6, MISO=GPIO1, CS=GPIO42) on SPI3, with zero pin overlap with the DAC's SPI2, in firmware v5.90.0 — see `include/pinmap.h` and `hardware/netlist.txt`. The 4 SD wires on the perfboard have since been physically moved to GPIO5/6/1/42; `sd_card::init()` now finds the card and the WebUI ILDA/SD tab lists files correctly.  
**Impact:** ILDA file playback from SD card works. Playlist tab is functional.

---

## Hardware Issues

### OPA4134 Gain Deviation

**Status:** Known, compensated in firmware  
**Detail:** The OPA4134 feedback resistors R2/R4 are 22 kΩ instead of the theoretical 10 kΩ, resulting in a gain of 2.2× rather than 2.0×. This means the full DAC swing would produce slightly more than ±5V at the galvo input. Compensated via `dac_limit_min`/`dac_limit_max` in `RuntimeConfig` (default: 0x0666..0xF999, ≈95% of full range). No action required unless you replace the resistors.

---

## Pattern Issues

Curves: if once active, need to manually turned of so other presets could start.

---

## Text Mode Issues

None known at least...

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

**Status:** Partially addressed  
**Detail:** Some subsystems (e.g. Art-Net receiver) are always active after boot, regardless of whether they are in use. The ability to enable/disable individual features from the WebUI is planned. This would reduce background CPU load and Wi-Fi channel congestion in setups that only use DMX.
OSC, sACN, and the Helios network DAC (Config tab → "Control Interfaces") already have this toggle: their sockets stay open at boot (same lifecycle as Art-Net/EtherDream), but a disabled feature ignores received data instead of acting on it. Art-Net/EtherDream/DMX still lack a toggle -- this issue stays open for those.

### Add a Restore button - not just text

**Status:** Planned

---

## Planned Features

These are features that are designed and intended, but not yet implemented.

### Control Interfaces

| Interface | Status | Notes |
|---|---|---|
| Ether Dream (TCP) | ✅ Complete | Compatible with QLC+, Pangolin, LaserBoy, Shownet |
| DMX512 (MAX485) | ✅ Complete | Hardware RX-only |
| Art-Net (UDP) | ✅ Complete | DMX over IP |
| Helios DAC (network) | ✅ Complete | `src/net/helios_net.cpp` — custom TCP framing (5-byte header + 7-byte points), not the official USB protocol; wired through the live optimizer transform + frame-budget clamp like the other render paths |
| Helios DAC (USB) | ⚠️ Stub | `src/net/helios_usb.cpp` — TinyUSB vendor-class not wired up; USB-CDC conflict (Serial debug vs. Vendor-Class on same OTG controller) must be resolved first |
| OSC (Open Sound Control) | ✅ Complete | `src/net/osc_in.cpp` — UDP port 9000, OSC 1.0 (no bundles); `/galvos/preset`, `/galvos/color`, `/galvos/speed`, `/galvos/brightness`, `/galvos/enable` |
| IDN (ILDA Digital Network) | ❌ Planned | UDP port 7255; discrete frame mode only (wave mode out of scope); requires IDN-Hello discovery + IDN-Stream frame parser |
| sACN / E1.31 | ✅ Complete | `src/net/sacn_in.cpp` — UDP multicast 239.255.0.1:5568, universe 1 only; lowest priority of the three DMX-shaped sources (Art-Net and DMX512 both win over it) |

### Multi-Controller

| Feature | Status | Notes |
|---|---|---|
| ESP-NOW Sync | ❌ Planned | Peer-to-peer frame sync over WiFi hardware; ~1–2 ms latency; no extra hardware required; Master broadcasts frame counter + preset ID |
| ArtNet Master Mode | ❌ Planned | GalvOS as ArtNet sender; enables sync via existing show software (Pangolin, QLC+) |

### Stand-alone & Integration

| Feature | Status | Notes |
|---|---|---|
| Autostart Preset | ❌ Planned | Persist last active preset in NVS; replay on boot without WebUI interaction |
| ILDA Output Header | ❌ Planned | Expose DAC X/Y + RGB TTL on standard ILDA 15-pin D-Sub; enables show software control without Art-Net |

---

## Contributing a Fix

If you fix one of the issues above, please see [Chapter 9 — Contributing](09-contributing.md) for the patch workflow and commit message format.

When submitting a fix for a known issue, reference the issue name from this chapter in your commit message body:

```text
Fix Typewriter animation looping

Typewriter now restarts after completing one pass.
Resolves: "Typewriter Animation — Runs Once Only" (docs/10-known-issues-and-todos.md)
```
