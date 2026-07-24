# Chapter 7 — Troubleshooting

> Things go wrong. This chapter documents what they look like, why they happen, and what to do about them. The tables are kept terse on purpose — if you are here, you probably want the answer, not a lecture.

## Table of Contents

- [Before You Debug](#before-you-debug)
- [Reading the Serial Log](#reading-the-serial-log)
- [Boot & Connectivity Issues](#boot--connectivity-issues)
- [Galvo Issues](#galvo-issues)
- [Laser Issues](#laser-issues)
- [Color & Calibration Issues](#color--calibration-issues)
- [Optimizer & Pattern Quality Issues](#optimizer--pattern-quality-issues)
- [WebUI Issues](#webui-issues)
- [ILDA & SD Card Issues](#ilda--sd-card-issues)
- [Safety System Issues](#safety-system-issues)
- [Memory & Stability Issues](#memory--stability-issues)
- [Known Bugs & Limitations](#known-bugs--limitations)

---

## Before You Debug

A serial monitor attached to the ESP32 is your best friend. Open it before you power anything up:

```bash
pio device monitor
```

Default baud rate: **115200**. The monitor uses the `esp32_exception_decoder` filter, which decodes Guru Meditation addresses into file names and line numbers automatically.

The boot log tells you nearly everything: firmware version, chip ID, PSRAM size, free heap, Wi-Fi connection result, task start confirmations, and — if anything goes wrong — what failed and why. Read it before trying anything else.

**Key log messages to know:**

| Message | Meaning |
| --- | --- |
| `=== Mikoy Laser FW x.x.x ===` | Firmware started successfully |
| `Config loaded. DMX=x Hostname=x` | NVS config loaded OK |
| `WiFi connected: x.x.x.x (RSSI -xx dBm)` | STA mode connected |
| `AP started: galvOS` | AP mode started (no STA credentials) |
| `Ring buffer overflow #N` | Pattern engine is generating frames faster than the ISR drains them — see [Optimizer Issues](#optimizer--pattern-quality-issues) |
| `TASK FAILED: name (Heap=N)` | A FreeRTOS task could not be created — internal heap exhausted at boot |
| `last_failsafe: reason` | Why the safety system refused to arm last time |
| `[heap] after patterns::init: N B free` | Internal DRAM available after startup; should be ≥30 KB |

---

## Reading the Serial Log

Log levels in GalvOS:

- `I` (INFO) — normal operation, printed in white/default
- `W` (WARN) — something unexpected but recoverable
- `E` (ERROR) — something failed; operation may be degraded

The WebUI Log tab (streaming over WebSocket) shows the same output. It auto-refreshes only when the Log tab is active.

If the ESP32 crashes with a Guru Meditation error, the serial monitor prints a backtrace. With the `esp32_exception_decoder` filter active in PlatformIO, the addresses are decoded into source file names and line numbers. Copy the full crash output — it is the fastest path to finding the bug.

---

## Boot & Connectivity Issues

### No serial output at all

**Cause:** Wrong serial port, wrong baud rate, or USB cable is charge-only (no data lines).  
**Fix:** Verify you are using the UART USB port on the DevKitC-1 (labeled "UART", not "USB"). Try a different cable. Confirm `monitor_speed = 115200` in `platformio.ini`.

### Firmware boots but immediately restarts in a loop

**Cause A:** Heap exhausted during task creation.  
**Diagnosis:** Serial log shows `TASK FAILED: name (Heap=N)` with N near zero.  
**Fix:** Something is allocating on internal DRAM that should be in PSRAM. Check that `BOARD_HAS_PSRAM` is defined and that the N16R8 OPI PSRAM is detected (`PSRAM: 8388608` in boot log).

**Cause B:** Panic handler fired — laser turned off, `esp_restart()` called.  
**Diagnosis:** Serial log shows `Guru Meditation` or `abort()` before the restart.  
**Fix:** Decode the backtrace with `esp32_exception_decoder`. The `last_failsafe` field in `/api/state` stores the reason in RTC memory across restarts.

### No Wi-Fi AP appears after first flash

**Cause:** LittleFS not flashed — WebUI missing, but this should not prevent AP from starting.  
**Fix:** Use `pio run --target upload_all`, not just `upload`. Confirm AP starts in serial log: `AP started: galvOS`.

### WebUI loads but shows "-- none --" everywhere

**Cause:** JavaScript running but API calls failing — server not yet ready, or a route is returning errors.  
**Diagnosis:** Open browser DevTools → Network tab. Look for failed `/api/state` calls.  
**Fix:** Wait 5–10 seconds after boot for all tasks to initialise. If still failing, check serial log for errors.

### mDNS hostname (`galvOS.local`) not resolving

**Cause:** mDNS not supported on some networks (corporate Wi-Fi, VPNs) or Windows without Bonjour.  
**Fix:** Use the IP address directly. Find it in the Dashboard → System card, or in the serial log: `WiFi connected: x.x.x.x`.

### Wi-Fi scan (Configuration tab) returns "error" or hangs

**Cause:** `WiFi.scanNetworks()` on the Arduino core has several failure modes on the ESP32-S3 — a busy-bit that stays stuck after a mode switch, `WIFI_SCAN_FAILED` while STA is mid-(re)connect, or an async completion event that doesn't reliably fire under load. All were seen in the field between v6.00.3 and v6.00.7.  
**Fix:** As of v6.00.7, the scan handler calls `esp_wifi_scan_start()`/`esp_wifi_scan_get_ap_records()` directly instead of going through `WiFiScanClass`, retries up to 3× on any negative result code, and reports a distinct `"error"` status to the UI instead of silently looping. If a scan still fails repeatedly, check the serial log for `esp_err_to_name()` plus internal-heap headroom — `esp_wifi_scan_start()` needs a contiguous internal-DRAM block, so heap pressure (see [Memory & Stability Issues](#memory--stability-issues)) is a plausible root cause of a stubborn failure.  
**Note:** An AP-mode (SoftAP) scan returning 0 networks is a separate, unfixable ESP-IDF channel-lock limitation — enter the SSID manually instead.

### Wi-Fi drops and does not reconnect

**Cause:** The Wi-Fi watchdog task detects the drop and sets `s_wifi_services_started = false`. `setAutoReconnect(true)` handles reconnection automatically.  
**Diagnosis:** Serial log shows `WiFi dropped, will auto-reconnect`, followed by a reconnection or continued attempts.  
**Fix:** Usually self-resolves. If it does not, check signal strength (RSSI). Values worse than −75 dBm cause intermittent drops.

---

## Galvo Issues

### Galvos go crazy when SD card is inserted

**Cause:** SD card wired onto the DAC8562's SPI2 pins; the GPIO matrix let SD's real SPI3 traffic overwrite the DAC's clock/data lines. Fixed in firmware v5.90.0, but requires a physical rewire (SD → GPIO5/6/1/42) — see [Known Issues](10-known-issues-and-todos.md).  
**Fix (until rewired):** Do not insert the SD card.

### Output is mirrored horizontally

**Fix:** Calibration tab → Invert X checkbox → Save calibration.

### Output is mirrored vertically

**Fix:** Calibration tab → Invert Y checkbox → Save calibration.

### Output is rotated 90°

**Cause:** X and Y galvo channels are wired in reverse.  
**Fix:** Calibration tab → Swap X/Y checkbox → Save calibration.

### Image is off-center

**Fix:** Calibration tab → X Offset / Y Offset sliders. Use the Crosshair calibration pattern as a reference. Save calibration.

### Image is too large or too small

**Fix:** Calibration tab → X Gain / Y Gain sliders (use "Linked" to keep aspect ratio). Or adjust the Size slider in Global Controls (Presets tab) per-session.

### Visible ringing / oscillation on blank jumps

**Cause:** The galvo mirror is oscillating at its natural mechanical resonance frequency after arriving at a new position.  
**Fix — Quick:** Increase `blank_samples` in the Optimizer tab. More settle ticks give the mirror more time to damp out.  
**Fix — Proper:** Measure `ring_freq_hz` and `ring_damping_ratio` with an oscilloscope (step response on the galvo feedback signal), then enable `ringing_comp_enabled`. See [Chapter 5 — ZV Ringing Compensation](05-optimizer.md#stage-8--zv-ringing-compensation-pillar-3).

### Corners look rounded or don't meet

**Cause A:** Not enough corner dwell points.  
**Fix:** Increase `max_corner_pts` in the Optimizer tab.

**Cause B:** `galvo_kpps` set too high for the scan angle. The galvo cannot settle at the corner before the next edge starts.  
**Fix:** Reduce `galvo_kpps` (Projection tab). Run the Autotune to find the safe maximum.

### Closed shape has a visible gap instead of reconnecting

**Cause (fixed in v6.05.0):** At a heavily tuned-down `max_pts_per_frame`, corner dwell was treated as fixed overhead — only interior density was ever scaled back to fit the frame budget. On a many-vertex closed shape (Octagon and up, dense Lissajous/rose/trochoid curves, Concentric Rings), corner dwell alone could exceed what was left of the budget, and the point emitter silently stopped mid-shape — always cutting off the final edge and closing dwell first, which reads as "the shape doesn't close."  
**Fix:** The optimizer now scales `min_corner_pts`/`max_corner_pts` down together (floor: 1 point per vertex) whenever corner dwell alone doesn't fit the budget, trading corner sharpness for a guaranteed closed loop. This is automatic — no configuration needed. If you still see a gap on current firmware, `max_pts_per_frame` is too low even for 1 point per vertex; raise it in the Optimizer tab.

### Straight lines look curved

**Cause:** The galvo mirror is still settling from a blank jump when the first interior points of the edge are being drawn.  
**Fix:** Increase `blank_samples` and `min_blank_samples`. Ensure the smoothstep blanking is active (it always is — but increasing the sample count gives it more travel time).

### Image flickers or strobes at high frame rates

**Cause:** Ring buffer overflow — the pattern engine is pushing frames faster than the ISR is draining them. Serial log: `Ring buffer overflow #N`.  
**Fix:** Reduce `opt_max_pts_per_frame` in the Optimizer tab (fewer points per frame = higher frame rate). The Autotune function in the Projection tab finds the highest safe kpps for the current pattern.

### DAC output stuck at 0x8000 (center)

**Cause A:** `gDebugNoHW = true` — No-HW mode is active, DAC writes are skipped.  
**Fix:** Configuration tab → Debug → No-HW Mode → disable → reboot.

**Cause B:** DAC8562 /CLR line held low.  
**Fix:** Verify `PIN_DAC_CLR_N` (GPIO13) pull-up to +5V (R_CLR 10 kΩ). Measure GPIO13 with a multimeter — it should read ~3.3V after boot.

---

## Laser Issues

### Laser fires at boot before ARM

**What is happening:** The fail-safe pull-ups (R_FSR/G/B, 10 kΩ → +3.3V) on GPIO7/8/21 hold the GPIOs HIGH during boot. This makes the 6N137 LEDs dark, which pulls the optocoupler outputs to +1.65V (HIGH) — which is "laser ON" for the MN-1M5AT active-HIGH driver.  
**Why this is expected:** This is the correct fail-safe behaviour for the TTL signals. However, the laser power rail is controlled by PIN_LASER_ENABLE (GPIO38 → SSR1), which the safety system holds LOW until all conditions are satisfied. The laser cannot actually fire until `safety::allOk()` returns true and the user presses ARM.  
**If the laser fires anyway:** The SSR1 is being energised before ARM. Check R_PD_EN (10 kΩ pull-down on GPIO38 → GND) is fitted. Measure GPIO38 at boot — it must be LOW before ARM.

### Laser does not turn on after ARM

**Cause A:** `master_dimmer = 0`.  
**Diagnosis:** Dashboard → Telemetry → Master Dimmer shows 0.  
**Fix:** Set Master Dimmer > 0 via the WebUI DMX override or DMX CH1. The guard `max(master_dimmer, ui_master_dimmer)` means both must be zero to produce no output.

**Cause B:** SSR1 not wired correctly, or relay contact open.  
**Diagnosis:** GPIO38 goes HIGH on ARM (measure with multimeter). If GPIO38 is HIGH but the laser power rail is still dead, the SSR or relay is the fault.  
**Fix:** Verify SSR1 wiring (IN+ from R_EN 1 kΩ to GPIO38, IN− to GND). Check SSR1 output contacts are in the laser power rail.

**Cause C:** `safety_override = false` and a safety condition is not met.  
**Diagnosis:** Dashboard → Safety Status shows a red LED. The `fault_reason` field shows which condition failed.  
**Fix:** Resolve the indicated condition (E-Stop wiring, scan-fail NE555 timing, watchdog heartbeat).

### One color channel missing

**Cause A:** 6N137 for that channel not powered or failed.  
**Diagnosis:** Measure +5V at that 6N137's VCC pin (Pin8) and VE pin (Pin7).  
**Fix:** Check power wiring to U_6N_R/G/B Pin8 and Pin7 → +5V_BUCK.

**Cause B:** Open circuit in the series resistor (R_R/G/B 220 Ω) or the GPIO → resistor trace.  
**Diagnosis:** Measure voltage across the 220 Ω resistor while that channel should be active.  
**Fix:** Re-solder or replace R_R/G/B.

**Cause C:** Firmware LEDC channel detached.  
**Diagnosis:** Check serial log for LEDC errors at boot.  
**Note:** `ledcAttachPin()` is called once at `setup()` — never per blank/unblank cycle. If you see this error, something is calling it in a loop.

### Colors washed out or wrong after a color animation

**Cause:** The color animation left `col_override = true` with an unintended color. The `_stopGradient()` function must send `col_override: false` to clear it.  
**Fix:** Presets tab → Global Controls → **↺ Reset Colors** button. This calls `resetColorOverride()` which explicitly sends `col_override: false` to the firmware.

### Master Dimmer set to 100% but laser is very dim

**Cause:** `thresh_r/g/b` thresholds are set too high — the `mapVisibleRange()` function is compressing the effective range.  
**Fix:** Run the visibility threshold calibration (Calibration tab → Start test beam → adjust Base R/G/B sliders). Alternatively, reduce the threshold values manually if you know your diodes' actual dead zone.

---

## Color & Calibration Issues

### Base color sliders in Calibration have no effect

**Cause:** The test beam bypass is not active. `mapVisibleRange(255, any_threshold)` always returns 255 — a full-brightness signal bypasses the threshold mapping.  
**Fix:** Press **▶ Start test beam** in the Calibration tab before adjusting threshold sliders. The test beam sends a logical level of ~1 (not 255), which goes through `mapVisibleRange()` and is therefore controlled by the threshold value.

### Colors look blown out at mid-brightness (gamma issue)

**Cause A:** Gamma enabled on a laser driver that already applies its own gamma correction — double-gamma.  
**Fix:** Calibration tab → CIE 1931 Gamma → toggle off. Save calibration.

**Cause B:** `calib_patterns.cpp:colorOut()` applies `applyGamma()` before storing into `LaserPoint.r/g/b`, but `galvo_out.cpp:rgbWrite()` applies it again.  
**Diagnosis:** This is a firmware bug pattern to be aware of when modifying calibration patterns. Each color value should go through gamma exactly once.

### Three-Circle Pattern colors do not match visually

**Cause:** White balance gains (gain_r/g/b) not calibrated for your laser module's actual power.  
**Fix A — Manual:** Calibration tab → Start Three Circle Pattern → adjust Gain R/G/B until all circles look equally bright.  
**Fix B — Auto:** Press **🪄 Auto White Balance** — computes gains from the power values in the Projection tab using V(λ) weighting. Only accurate if the power values (`power_r_mw`, `power_g_mw`, `power_b_mw`) are correct for your module.

### Calibration pattern looks wrong or produces incorrect output

**Cause:** Known issue with some calibration patterns. See [Known Issues](10-known-issues-and-todos.md).

---

## Optimizer & Pattern Quality Issues

### Pattern flickers

**Cause A:** Frame rate too low — `opt_max_pts_per_frame` too high.  
**Diagnosis:** Dashboard → Telemetry → Galvo Rate shows pps lower than expected. Serial log shows `Ring buffer overflow`.  
**Fix:** Reduce `opt_max_pts_per_frame` in the Optimizer tab until the overflow stops.

**Cause B:** Ring buffer underrun — pattern engine is generating frames too slowly (complex patterns at low kpps).  
**Diagnosis:** Serial log shows underrun messages.  
**Fix:** Reduce pattern complexity or increase `galvo_kpps`.

### No improvement in image quality above 1300 points per frame

**This is expected.** The known effective limit on the JY-15K-BL at 30 kpps is 1300 points per frame. Above this, the per-point time budget (33 µs) is the limiting factor, not the number of points. Setting `opt_max_pts_per_frame` above 1300 wastes frame budget without improving optical quality.

### Wireframe corners look bad (rounded or missing)

**Cause:** `buildWfChains()` not called for this wireframe pattern. Isolated 2-vertex `PathSegment` entries prevent `has_incoming && has_outgoing` from ever being true, so corner dwell never fires.  
**Fix:** This is a firmware-side issue. When writing new 3D patterns, ensure `buildWfChains()` is called to group edge pairs into proper chains before passing to the optimizer.

### Optimizer parameter changes have no effect

**Cause:** The pattern cache is serving a stale cached result. `gPatternCacheGen` was not incremented after the optimizer parameter change.  
**Diagnosis:** Change a non-optimizer parameter (e.g. speed), then change back — this forces a cache invalidation.  
**Expected behaviour:** Any optimizer parameter change via the WebUI automatically bumps `gPatternCacheGen`. If you are changing parameters directly in NVS without going through the WebUI, you may need to restart.

### PPS scaling produces unexpected densities

**Cause:** `galvo_rated_kpps` not set correctly for your galvo.  
**Fix:** Projection tab → set Galvo Rated Speed to your galvo's datasheet kpps. The optimizer uses the ratio `rated_kpps / galvo_kpps` to scale density. If rated_kpps is wrong, all scaled values will be wrong.

---

## WebUI Issues

### WebUI does not load (connection refused or timeout)

**Cause A:** LittleFS not flashed — `data/index.html` missing.  
**Fix:** Run `pio run --target upload_all`.

**Cause B:** ESP32 in AP mode and you are not connected to the `galvOS` AP.  
**Fix:** Connect your device to the `galvOS` Wi-Fi network, then open `http://192.168.4.1`.

**Cause C:** mDNS not resolving `galvOS.local`.  
**Fix:** Use IP address directly (see Dashboard → System, or serial log).

### Preset grid is empty

**Cause:** `/api/presets` request failed — server busy or route error.  
**Fix:** Reload the tab. If persistently empty, check the serial log for errors on the `/api/presets` handler. The WebUI falls back to the static `STATIC_PRESET_DEFS` SVG lookup for thumbnails, but the grid itself requires the API.

### API returns 404 for `/api/calib-pattern/stop` or `/api/text/vertices`

**Cause:** Route registration order issue in ESPAsyncWebServer. These specific routes must be registered before any prefix-matching wildcard routes (`/api/calib-pattern/*`). This is a known architectural constraint — if you add new routes, keep this rule in mind.  
**Fix:** In `web_ui.cpp`, verify `/api/calib-pattern/stop` is registered before any prefix handler that would match `/api/calib-pattern/...`.

### Controls in the WebUI lag or don't respond

**Cause A:** Core 0 CPU load too high — WebSocket messages are being dropped.  
**Diagnosis:** Dashboard → CPU Load graph shows Core 0 near or above 90%.  
**Fix:** Reduce pattern complexity or close extra browser tabs connected to the device.

**Cause B:** Wi-Fi interference or weak signal causing packet loss.  
**Diagnosis:** Dashboard → System → WiFi Signal weaker than −70 dBm.  
**Fix:** Move ESP32 or access point closer together, or switch Wi-Fi channels.

---

## ILDA & SD Card Issues

### SD card causes galvo malfunction

**Cause:** SD card wired onto the DAC8562's SPI2 pins; the GPIO matrix let SD's real SPI3 traffic overwrite the DAC's clock/data lines. Fixed in firmware v5.90.0, but requires a physical rewire (SD → GPIO5/6/1/42) — see [Known Issues](10-known-issues-and-todos.md).  
**Fix (until rewired):** Remove the SD card. All other features work normally without it.

### ILDA files not listed in the ILDA tab

**Cause A:** SD card not formatted as FAT32.  
**Fix:** Format the card as FAT32 (not exFAT, not NTFS). Maximum 40 `.ild` files indexed.

**Cause B:** SD card speed class insufficient.  
**Fix:** Use Class 10 UHS-I minimum. Class 4 cards are incompatible with reliable SPI access at DAC-sharing speeds.

---

## Safety System Issues

### System refuses to ARM — no obvious reason

**Diagnosis:** Dashboard → Safety Status card → `fault_reason` text line. Also check `last_failsafe` in `/api/state` JSON, which survives restarts and shows the last hardware-level shutdown reason.

**Common conditions and fixes:**

| Fault reason | Cause | Fix |
| --- | --- | --- | --- |
| `estop` | E-Stop circuit open | Check J_ESTOP wiring. Pin1 must be pulled to +3.3V via R_ESTOP (10 kΩ). Shorting Pin2 to Pin1 disables E-Stop. |
| `scanfail` | NE555 scan-fail timer timed out | DAC must be producing output on VOUTA for the scan-fail NE555 (U11) to be triggered. Starts a preset before arming. Also check C_T/R_T values on U11. |
| `watchdog` | NE555 hardware watchdog timeout | GPIO14 heartbeat pulse from firmware not arriving at U12 TRIG. Check R_HB (1 kΩ) and NE555 U12 wiring. |
| `thermal` | Temperature above shutdown threshold | Check temperature sensors in Thermal tab. Default shutdown at 70°C. |
| `user` | User has not pressed ARM | Press the ARM button on the Dashboard. |

### Laser cuts out mid-operation

**Cause A:** Hardware watchdog timeout — firmware loop exceeded `watchdog_period_ms`.  
**Diagnosis:** Serial log shows watchdog-related messages. `last_failsafe` in RTC memory.  
**Fix:** Usually caused by a hang on Core 0 (Wi-Fi, heavy JSON serialisation). Reduce WebUI polling frequency if you have custom integrations. Check for memory allocation failures.

**Cause B:** Temperature exceeded `temp_shutdown_c` (default 70°C).  
**Diagnosis:** Dashboard → Temperature History chart.  
**Fix:** Check fan operation. Verify thermal paste on laser diode module. Reduce `temp_shutdown_c` threshold if your setup runs hotter than expected for a softer warning margin.

**Cause C:** E-Stop button physically pressed or J_ESTOP connector loose.  
**Fix:** Verify E-Stop connector is seated. Check R_ESTOP (10 kΩ) pull-up is present.

### Scan-fail fires spuriously

**Cause:** NE555 U11 timing too aggressive for the configured kpps. At very low kpps, DAC updates are infrequent and the AC coupling (C_AC 100 nF) may not trigger the NE555 TRIG pin reliably.  
**Fix:** Increase `scanfail_timeout_ms` in the Configuration tab. Or increase `galvo_kpps` so DAC output is more frequent.

---

## Memory & Stability Issues

### Crash on boot — `Guru Meditation Error: Core 0 panic'ed`

**Most common cause:** Internal DRAM heap exhaustion during task creation or large allocation.

**Diagnosis:** Boot log line `[heap] after patterns::init: N B free`. Should be ≥ 30,000 bytes. If N < 10,000, something is allocating on DRAM that should be in PSRAM.

**Allocation rules:**

- Buffers > 16 KB → `ps_malloc()` or `heap_caps_malloc(MALLOC_CAP_SPIRAM)`
- All `JsonDocument` instances → `JsonDocument doc(&jsonAllocator())` (uses `SpiRamAllocator`)
- API responses → `sendJsonPsram()` (chunked, PSRAM buffer)

**Fix:** If you have added code that allocates large buffers, move them to PSRAM.

### `heap_critical_bytes` threshold triggered restart

**What happens:** If the largest free block of internal DRAM falls below `heap_critical_bytes` (default 6144 bytes), the firmware calls `esp_restart()`. This prevents the system from reaching a state where even small allocations fail and cause undefined behaviour.

**Diagnosis:** Serial log shows a restart with no Guru Meditation. Check `last_failsafe`.

**Fix A:** If this is triggered by normal operation, the `heap_critical_bytes` threshold may be set too conservatively for your configuration. Increase it slightly with caution.  
**Fix B:** If this is triggered by a specific action (opening a tab, loading a preset), that action is causing an unexpected internal DRAM allocation. Profile with the Log tab open.

### PSRAM not detected

**Symptom:** `PSRAM: 0` in the boot log. All PSRAM allocations fail silently.  
**Cause A:** Wrong board variant — must be N16R8, not N8R8 or no-PSRAM variant.  
**Cause B:** `board_build.psram_type = octal` or `board_build.arduino.memory_type = qio_opi` missing from `platformio.ini`.  
**Fix:** Verify `platformio.ini` settings. Verify the physical chip marking on the ESP32 module reads `N16R8`.

---

## Known Bugs & Limitations

A quick-reference index of open issues documented in [Chapter 10](10-known-issues-and-todos.md):

| Issue | Impact | Workaround |
| --- | --- | --- |
| SD card causes galvo malfunction | ILDA playback non-functional | Remove SD card |
| Text: Bounce has no effect | Minor | Use Scroll or Static |
| Text: Typewriter runs once only | Minor | — |
| Text: Star Wars Scroll direction wrong | Minor | Use Scroll Left/Right |
| Text: Star Wars renders as dots | Minor | — |
| Calibration channel selector not working | Minor | Calibrate with RGB combined |
| ILDA Standard Test Pattern bad output | Minor | Use Crosshair/Grid patterns instead |
