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

None currently open.

---

## Hardware Issues

### Fan Tacho Inputs Not Read

**Status:** Open
**Detail:** `PIN_FAN1_TACH` (GPIO2) and `PIN_FAN2_TACH` (GPIO9) are wired through 4.7 kΩ pull-ups on the board, but no firmware reads them. There is no RPM readout and no stalled-fan detection — thermal safety relies entirely on the DS18B20 sensors.

### OPA4134 Gain Deviation

**Status:** Known, compensated in firmware  
**Detail:** The OPA4134 feedback resistors R2/R4 are 22 kΩ instead of the theoretical 10 kΩ, resulting in a gain of 2.2× rather than 2.0×. This means the full DAC swing would produce slightly more than ±5V at the galvo input. Compensated via `dac_limit_min`/`dac_limit_max` in `RuntimeConfig` (default: 0x0666..0xF999, ≈95% of full range). No action required unless you replace the resistors.

---

## Pattern Issues

### Curves backend is now dead code (WebUI card removed)

**Status:** Open — cleanup TODO  
**Detail:** The "∿ Curves" WebUI card (live parametric curve generator, distinct from the
"Curves" preset category which stays) was removed from `data/index.html` — no UI can reach it
anymore. `src/patterns/curve_patterns.cpp/.h`, the `pattern_engine.cpp` integration, and the
`/api/curves` GET/POST routes in `src/net/web_ui.cpp` were intentionally left in place for this
pass and are now unreferenced from the frontend. Remove them in a follow-up firmware cleanup.

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
**Symptom:** The ILDA standard test pattern (ITC Rev.002 1995, used to verify galvo linearity and speed) does not render correctly, which makes it unusable as a calibration reference. Use the Crosshair and Grid patterns from the Pattern Library instead until this is fixed.

---

## UI Issues

### Disabled interfaces still hold their socket

**Status:** By design, documented here so it isn't reported as a bug
**Detail:** OSC, sACN, Helios network DAC, Art-Net and Ether Dream each have an enable/disable checkbox in Config tab → "Control Interfaces", plus a per-protocol Debug Log toggle (DMX included). A disabled interface ignores received data instead of acting on it, but its listening socket stays open until the next reboot. Only DMX has no enable toggle — it is a hardware UART receiver with no radio/CPU cost worth toggling.

### Telemetry "Source" label mismatch

**Status:** Open  
**Detail:** The Dashboard Telemetry card maps `state.source` through the array `['DMX','ArtNet','EtherDream','Helios','OSC','sACN','Internal']`, but the firmware's `ControlSource` enum is `0=none, 1=DMX, 2=ArtNet, 3=EtherDream, 4=Helios, 5=Internal, 6=WebUI, 7=sACN, 8=OSC` — so e.g. an active DMX source (1) displays as "ArtNet" and WebUI (6) displays as "Internal". One-line fix in `data/index.html` (`dash-source` handler).

### Add a Restore button - not just text

**Status:** Planned

### Inverse filter has no UI

**Status:** Open
**Detail:** The per-axis galvo deconvolution (`/api/inverse-filter/*`) is fully implemented in firmware and persisted to NVS, but there is no card for it anywhere in the WebUI — it can currently only be measured and configured over the REST API. A panel next to Ringing Compensation on the Optimizer tab is the obvious home.

### Hosted UI simulator drifts from the device UI

**Status:** Open
**Detail:** `docs/index.html` (served at [www.galvos.de](https://www.galvos.de)) is a standalone browser simulation of the WebUI, maintained by hand. It is not built from `data/index.html`, so it lags behind whenever the real UI changes.

---

## Planned Features

These are features that are designed and intended, but not yet implemented.

### Control Interfaces

| Interface | Status | Notes |
| --- | --- | --- |
| Ether Dream (TCP) | ✅ Complete | Compatible with QLC+, Pangolin, LaserBoy, Shownet |
| DMX512 (MAX485) | ✅ Complete | Hardware RX-only |
| Art-Net (UDP) | ✅ Complete | DMX over IP |
| Helios DAC (network) | ✅ Complete | `src/net/helios_net.cpp` — custom TCP framing (5-byte header + 7-byte points), not the official USB protocol; wired through the live optimizer transform + frame-budget clamp like the other render paths |
| Helios DAC (USB) | ❌ Dropped | Not planned — TinyUSB vendor-class stub removed; USB-CDC conflict (Serial debug vs. Vendor-Class on same OTG controller) made it not worth pursuing |
| OSC (Open Sound Control) | ✅ Complete | `src/net/osc_in.cpp` — UDP port 9000, OSC 1.0 (no bundles); `/galvos/preset`, `/galvos/color`, `/galvos/speed`, `/galvos/brightness`, `/galvos/enable` |
| IDN (ILDA Digital Network) | ❌ Planned | UDP port 7255; discrete frame mode only (wave mode out of scope); requires IDN-Hello discovery + IDN-Stream frame parser |
| sACN / E1.31 | ✅ Complete | `src/net/sacn_in.cpp` — UDP multicast 239.255.0.1:5568, universe 1 only; lowest priority of the three DMX-shaped sources (Art-Net and DMX512 both win over it) |

### Multi-Controller

| Feature | Status | Notes |
| --- | --- | --- |
| ESP-NOW Sync | ❌ Planned | Peer-to-peer frame sync over WiFi hardware; ~1–2 ms latency; no extra hardware required; Master broadcasts frame counter + preset ID |
| ArtNet Master Mode | ❌ Planned | GalvOS as ArtNet sender; enables sync via existing show software (Pangolin, QLC+) |

### Stand-alone & Integration

| Feature | Status | Notes |
| --- | --- | --- |
| Autostart Preset | ❌ Planned | Persist last active preset in NVS; replay on boot without WebUI interaction |
| ILDA Output Header | ❌ Planned | Expose DAC X/Y + RGB TTL on standard ILDA 15-pin D-Sub; enables show software control without Art-Net |

---

## Contributing a Fix

If you fix one of the issues above, please see [Chapter 9 — Contributing](09-contributing.md) for the patch workflow and commit message format.

When submitting a fix for a known issue, reference the issue name from this chapter in your commit message body:

```text
fix: map Telemetry source labels to the real ControlSource enum

The Dashboard label array skipped SRC_NONE and SRC_WEBUI, so DMX
displayed as ArtNet and WebUI as Internal.
Resolves: "Telemetry \"Source\" label mismatch" (docs/10-known-issues-and-todos.md)
```
