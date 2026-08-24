# Chapter 10 — Known Issues & Todos

*Previous: [Chapter 9 — Contributing](09-contributing.md)*

> This is an honest list. Every project has rough edges — GalvOS more than most, because it is a one-person hardware/firmware/UI project that started as a dimmer fix and grew considerably beyond that. Nothing here is hidden. If you run into one of these, you are not doing it wrong.

## Table of Contents

- [Critical Issues](#critical-issues)
- [Hardware Issues](#hardware-issues)
- [Pattern Issues](#pattern-issues)
- [Text Mode Issues](#text-mode-issues)
- [Calibration Issues](#calibration-issues)
- [UI Issues](#ui-issues)
- [Build & Tooling Issues](#build--tooling-issues)
- [Planned Features](#planned-features)
- [Contributing a Fix](#contributing-a-fix)

---

## Critical Issues

These affect core functionality and should be resolved before relying on those features in production.

### `safety_override` bypasses E-Stop and watchdog, not just scanfail

**Status:** Open — confirmed live, 2026-08-24
**Detail:** `.claude/CLAUDE.md`'s safety rules state `safety_override` "should NOT bypass
E-Stop, watchdog, or arming — only scanfail". The actual gate in `safety.cpp`'s `allOk()`
does not match that:

```cpp
bool allOk() {
    if (gConfig.safety_override) {
        return s_user_arm_request;  // bypass HW/watchdog/subsystem checks
    }
    return gState.estop_ok.load() &&
           gState.scanfail_ok.load() &&
           watchdogOk() &&
           subsystemsOk() &&
           s_user_arm_request;
}
```

With `safety_override` on, `estop_ok`, `scanfail_ok`, `watchdogOk()`, and `subsystemsOk()` are
all skipped — the comment even says so. Confirmed live: with the HW watchdog heartbeat
(GPIO14) not satisfied (`watchdog_ok:false`), arming failed normally, but flipping
`safety_override` armed the laser anyway (`laser_armed:true`) while `watchdog_ok` stayed
`false`. Whether E-Stop is also bypassable this way was not independently tested live (no
reason to trigger E-Stop just to check), but the code path is unconditional — it reads
`estop_ok` the same way as the other three, so there is no reason to expect it survives when
the others don't. Reconcile the code with the documented intent (scanfail-only bypass), or
update the documented intent if the wider bypass is actually deliberate.

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

### Raising the dimmer with no preset renders the legacy DMX pattern

**Status:** Open  
**Detail:** `pattern_engine.cpp`'s `legacyDmxActive` counts `gState.ui_override` as
"a controller is driving the DMX channels", so turning the master dimmer up from the
WebUI with **no preset selected** runs the legacy DMX emulation — `genPattern()` plus
the full transform/calibration/push path — every frame. Measured at 16–24% of Core 0
for a mode the user has switched off. But `ui_override` means "WebUI takes priority
over DMX", not "render the legacy DMX emulation"; `gOverride.active` and the real
DMX/Art-Net/sACN receivers already cover the cases where legacy rendering is wanted.
Dropping `ui_override` from that condition looks correct but is a user-visible
behaviour change, so it is filed rather than taken.

### Optimizer dominates the per-frame cost of any uncached preset

**Status:** Open — the next place Core-0 work belongs  
**Detail:** With per-stage render timing now available
([`/api/tasks`](08-api-reference.md#get-apitasks) → `render`), roughly three quarters
of every frame that misses the pipeline output cache sits inside `generate_us`, i.e.
in `optimizer::optimize()` — 5942 µs of a 7936 µs frame for Circle, measured at
44 kpps. The stages around it barely move between presets (`pipeline_us` ~1.9 ms,
`push_us` ~0.3 ms). The whole-frame cache only covers the six presets on
`presets::isStaticPreset()`'s explicit allow-list, so everything else pays this every
frame.

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

### Disabled interfaces still hold their listening socket

**Status:** Partly fixed in 6.83.0; the remaining half is by design
**Detail:** OSC, sACN, Helios network DAC, Art-Net and Ether Dream each have an
enable/disable checkbox in Config tab → "Control Interfaces", plus a per-protocol Debug
Log toggle (DMX included). A disabled interface ignores received data instead of acting
on it, and its listening socket stays open until the next reboot. Only DMX has no enable
toggle — it is a hardware UART receiver with no radio/CPU cost worth toggling.

Until 6.83.0 those checkboxes gated only the *data path*: the receiver tasks were
created unconditionally and kept polling regardless — Helios and Ether Dream at 500 Hz
each, Art-Net at 200 Hz — while Ether Dream also went on broadcasting a discovery beacon
every second, advertising itself on the network and accepting TCP clients whose frames
were then silently dropped. `helios_net_enabled` was not read anywhere at all. Each task
now re-checks its flag on a 250 ms tick and skips the socket work entirely, hanging up
any attached client; the flag still takes effect without a reboot. What remains is only
the open listen socket itself, which costs nothing to hold.

### Telemetry "Source" label mismatch

**Status:** Open  
**Detail:** The Dashboard Telemetry card maps `state.source` through the array `['DMX','ArtNet','EtherDream','Helios','OSC','sACN','Internal']`, but the firmware's `ControlSource` enum is `0=none, 1=DMX, 2=ArtNet, 3=EtherDream, 4=Helios, 5=Internal, 6=WebUI, 7=sACN, 8=OSC` — so e.g. an active DMX source (1) displays as "ArtNet" and WebUI (6) displays as "Internal". One-line fix in `data/index.html` (`dash-source` handler).

### Inverse filter has no UI

**Status:** Open
**Detail:** The per-axis galvo deconvolution (`/api/inverse-filter/*`) is fully implemented in firmware and persisted to NVS, but there is no card for it anywhere in the WebUI — it can currently only be measured and configured over the REST API. A panel next to Ringing Compensation on the Optimizer tab is the obvious home.

### Hosted UI simulator drifts from the device UI

**Status:** Open
**Detail:** `docs/index.html` (served at [www.galvos.de](https://www.galvos.de)) is a standalone browser simulation of the WebUI, maintained by hand. It is not built from `data/index.html`, so it lags behind whenever the real UI changes.

### WebUI screenshots in Chapter 4 are stale

**Status:** Open
**Detail:** Every screenshot in `docs/assets/screenshots/` was captured against UI v1.5.3 (2026-07-30); the WebUI is currently at v1.25.0. The overall Dashboard layout (Safety & Arm / System / Telemetry top row) still matches, but a fair amount has moved since — mobile-nav fixes, the Optimizer/Projection tab reworks, and the v6.80.0 Dashboard chart grid are all unshown. Concretely: `tab_dashboard.png`, `card_system.png`, and `card_cpu.png` no longer match the current layout, and there is no screenshot yet for the new Buffer Fill / WiFi RSSI charts. Screenshots are captured live from a running device via `scripts/capture_screenshots.py` (see the screenshot note near the top of [Chapter 4](04-ui-guide.md)) — needs a real board on hand to redo, which wasn't available when this was logged.

---

## Build & Tooling Issues

### Host regression suite cannot be run

**Status:** Open  
**Detail:** `platformio.ini` defines only the `esp32-s3-devkitc-1` environment, so
`pio test -e native` — the host-side optimizer contract tests under
`test/test_optimizer/` — has no environment to run in. The tests are still in the tree.
Restore a `native` env before the next optimizer change, which is exactly the kind of
work they exist to guard.

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

---

*Next: [Chapter 11 — Glossary & Terminology](11-glossary.md)*
