# Chapter 3 — Build & Configuration

## Table of Contents
- [Repository Setup](#repository-setup)
- [PlatformIO Build](#platformio-build)
- [Flash Instructions](#flash-instructions)
- [platformio.ini — Build Parameters](#platformioini--build-parameters)
- [Partition Table](#partition-table)
- [config.h — Compile-Time Constants](#configh--compile-time-constants)
- [RuntimeConfig — User Parameters](#runtimeconfig--user-parameters)
- [ProjectionConfig — Galvo & Laser Parameters](#projectionconfig--galvo--laser-parameters)
- [SafetyConfig — Temperature Thresholds](#safetyconfig--temperature-thresholds)
- [Optimizer Defaults](#optimizer-defaults)
- [pinmap.h — GPIO Assignments](#pinmaph--gpio-assignments)
- [NVS — Parameter Persistence](#nvs--parameter-persistence)
- [Resetting to Defaults](#resetting-to-defaults)

---

## Repository Setup

Clone the repository and open it in VS Code with the PlatformIO extension installed:

```bash
git clone https://github.com/Andre1Becker/GalvOS.git
cd GalvOS
code .
```

PlatformIO detects `platformio.ini` automatically. On the first build it will download the ESP32-S3 platform toolchain (~500 MB) and all library dependencies. This happens once — subsequent builds are fast.

**Library dependencies** are declared in `platformio.ini` and managed automatically. You do not need to install anything manually:

| Library | Version | Purpose |
|---------|---------|---------|
| `esp32async/AsyncTCP` | ^3.4.10 | Async TCP for ESPAsyncWebServer |
| `esp32async/ESPAsyncWebServer` | ^3.11.1 | WebUI HTTP server + WebSocket |
| `someweisguy/esp_dmx` | ^4.1.0 | DMX-512 receive via UART |
| `rstephan/ArtnetWifi` | git | Art-Net UDP receiver |
| `bblanchon/ArduinoJson` | ^7.0.4 | JSON serialization (PSRAM allocator) |
| `paulstoffregen/OneWire` | ^2.3.8 | 1-Wire bus |
| `milesburton/DallasTemperature` | ^3.11.0 | DS18B20 temperature sensor driver |

---

## PlatformIO Build

All build actions are run from the PlatformIO VS Code sidebar or the terminal:

```bash
# Build firmware (no flash)
pio run

# Flash firmware only (data/index.html not updated)
pio run --target upload

# Flash LittleFS only (updates WebUI, firmware not changed)
pio run --target uploadfs

# *** RECOMMENDED: build and flash everything in one step ***
pio run --target upload_all

# Open serial monitor (115200 baud)
pio device monitor
```

**Important:** Any change to files in the `data/` directory (the WebUI) requires `upload_all` or `uploadfs` — a firmware-only flash will not update the WebUI. The same applies in reverse: changes to firmware source files do not require an `uploadfs`.

### What `upload_all` Does

The `scripts/upload_all.py` pre-build script adds a custom target that:
1. Builds the LittleFS image from `data/`
2. Flashes the firmware
3. Flashes the LittleFS

The `scripts/gzip_assets.py` pre-build hook automatically gzip-compresses `index.html` (and any other HTML/CSS/JS/SVG files in `data/`) before the LittleFS image is assembled. This reduces the on-flash size from ~440 KB to ~105 KB and eliminates a heap spike that occurred when serving the uncompressed file over Wi-Fi. The uncompressed originals remain in `data/` as the editable source; only the `.gz` variants are served.

---

## Flash Instructions

1. Connect the ESP32-S3 DevKitC-1 to your PC via USB (the USB port labeled "UART", not "USB").
2. On the **first flash** of a new board, or if the partition table has changed, hold the BOOT button while pressing EN, then release both. This puts the board in download mode.
3. Run `pio run --target upload_all`.
4. After flashing, the ESP32 restarts automatically.

**Serial port selection:** PlatformIO usually detects the correct port automatically. If you have multiple serial devices, set `upload_port` in `platformio.ini`:
```ini
upload_port = /dev/ttyUSB0   ; Linux
upload_port = COM5            ; Windows
```

**Upload speed:** Set to 921600 baud in `platformio.ini`. This is stable on most systems. If you see flash errors, reduce to `460800`.

---

## platformio.ini — Build Parameters

These are the parameters in `platformio.ini` that you may want to adjust. Everything else should be left at its default unless you have a specific reason to change it.

### Parameters You Will Likely Touch

| Parameter | Default | Description |
|-----------|---------|-------------|
| `LASER_FW_VERSION` | `"5.77.4"` | Version string shown in the WebUI header and serial log. Increment this when you modify the firmware (see [Version Bumps](#version-bumps)). |
| `GALVO_SAMPLE_RATE_HZ` | `30000` | The ISR tick rate — how many DAC samples are written per second. This is **not** the same as `galvo_kpps` in the WebUI (which controls how many of those ticks contain new pattern points). Default 30,000 Hz = 30 kpps effective output at full density. |
| `DEFAULT_DMX_ADDRESS` | `1` | Default DMX start address on first boot (before any NVS config). |
| `DEFAULT_DMX_UNIVERSE` | `0` | Default Art-Net universe on first boot. |
| `upload_port` | *(auto)* | Serial port for flashing. Set explicitly if auto-detect picks the wrong port. |
| `upload_speed` | `921600` | Baud rate for flashing. Reduce to `460800` if you see flash errors. |

### Parameters You Should Not Change (and Why)

| Parameter | Value | Why Not |
|-----------|-------|---------|
| `board_build.flash_size` | `16MB` | Must match the N16R8 physical flash. Wrong value = flash corruption. |
| `board_build.psram_type` | `octal` | Required for the N16R8 OPI PSRAM. Other values break PSRAM. |
| `board_build.arduino.memory_type` | `qio_opi` | Required for N16R8 octal PSRAM. Do not change. |
| `board_build.partitions` | `partitions.csv` | Custom partition table — the default ESP32-S3 table is too small for the LittleFS image. |
| `BOARD_HAS_PSRAM` | defined | Enables PSRAM in the Arduino core. Without this, `ps_malloc()` does not work. |
| `ARDUINO_USB_MODE=1` | defined | Enables native USB CDC. Required for USB serial on the S3. |
| `CONFIG_ASYNC_TCP_RUNNING_CORE=0` | defined | Pins AsyncTCP to Core 0, away from the galvo ISR on Core 1. |

### Version Bumps

Version strings follow **Major.Minor.Patch**:
- **Patch** — single bug fix, one file changed
- **Minor** — new feature or refactor touching multiple call sites
- **Major** — broad architectural change

Update `LASER_FW_VERSION` in `platformio.ini` using Python string replacement (not sed — the escaped quotes make sed fragile):
```python
# In a patch script:
content = content.replace('-D LASER_FW_VERSION=\\"5.77.4\\"',
                          '-D LASER_FW_VERSION=\\"5.77.5\\"', 1)
```

---

## Partition Table

`partitions.csv` defines the flash memory layout:

| Partition | Type | Size | Purpose |
|-----------|------|------|---------|
| `nvs` | data/nvs | 20 KB | NVS key-value store — all runtime config |
| `otadata` | data/ota | 8 KB | OTA update bookkeeping |
| `app0` | app/ota_0 | 5 MB | Active firmware image |
| `app1` | app/ota_1 | 5 MB | OTA update staging slot |
| `spiffs` | data/spiffs | 5 MB | LittleFS — WebUI (`index.html.gz` + assets) |
| `coredump` | data/coredump | 64 KB | Core dump on crash (for post-mortem debugging) |

Total: 15.25 MB of the 16 MB flash used. The LittleFS partition is labeled `spiffs` for historical PlatformIO compatibility — it is formatted as LittleFS, not SPIFFS.

---

## config.h — Compile-Time Constants

These constants are defined in `include/config.h` and require a firmware rebuild if changed. They are not adjustable at runtime.

### Pattern Engine Limits

| Constant | Default | Description |
|----------|---------|-------------|
| `PATTERN_POINTS_MAX` | `2048` | Maximum number of `LaserPoint` entries in the pattern buffer. Increasing this uses more PSRAM. |
| `POINTS_MODE_MAX_DOTS` | `80` | UI slider ceiling for the Points-Only mode dot count. |
| `POINTS_MODE_MIN_DWELL` | `3` | Minimum dwell ticks per dot in Points-Only mode. Below this, the dot is invisible. |
| `POINTS_MODE_MAX_DWELL` | `30` | Maximum dwell ticks per dot — prevents a small number of dots from consuming the whole frame budget. |
| `RANDOM_PTS_MAX_COUNT` | `14` | UI slider ceiling for the Random Points preset "Amount" parameter. |
| `KALEIDO_SEGMENTS_MAX` | `16` | UI slider ceiling for the Kaleidoscope effect segment count. |

### Content Limits

| Constant | Default | Description |
|----------|---------|-------------|
| `PLAYLIST_MAX_ENTRIES` | `32` | Maximum entries in an ILDA playlist. |
| `PAINT_STROKES_MAX` | `12` | Maximum strokes/shapes on the Paint canvas. |
| `PAINT_VERTS_PER_STROKE` | `96` | Maximum vertices per stroke (simplified client-side before upload). |
| `ZONE_POINTS_MAX` | `16` | Maximum vertices in the projection zone polygon. |
| `ILDA_MAX_FILES` | `40` | Maximum `.ild` files indexed from the SD card. |

---

## RuntimeConfig — User Parameters

`RuntimeConfig` (defined in `include/config.h`, stored as `gConfig`) holds all parameters that can be changed at runtime via the WebUI or REST API. They are persisted to NVS and survive reboots.

### Galvo Geometry

| Field | Default | Range | Description |
|-------|---------|-------|-------------|
| `galvo_x_offset` | `0` | −32767..32767 | DC offset applied to the X galvo output (DAC units). Use to center the image horizontally. |
| `galvo_y_offset` | `0` | −32767..32767 | DC offset applied to the Y galvo output. Use to center the image vertically. |
| `galvo_x_gain` | `32767` | 0..32767 | Scaling factor for X. Full scale = 32767. Reduce to shrink the image horizontally. |
| `galvo_y_gain` | `32767` | 0..32767 | Scaling factor for Y. Reduce to shrink the image vertically. |
| `swap_xy` | `false` | bool | Swap X and Y galvo channels. Use if your galvo wiring has X and Y reversed. |
| `invert_x` | `false` | bool | Mirror the image horizontally. |
| `invert_y` | `false` | bool | Mirror the image vertically. |

### DAC Output Limiting

| Field | Default | Description |
|-------|---------|-------------|
| `dac_limit_min` | `0x0666` | Minimum DAC code (clips the lower end of travel). Default ≈ 2.5% from the bottom — keeps OPA4134 output within ±5.5V. |
| `dac_limit_max` | `0xF999` | Maximum DAC code (clips the upper end of travel). Default ≈ 2.5% from the top. |

These limits protect the galvo driver from being fed voltages outside its rated ±5V input range. The OPA4134 has a gain of 2.2× (R2/R4 = 22 kΩ instead of the theoretical 10 kΩ), so the actual output can slightly exceed ±5V at full DAC swing. The limits compensate for this. Adjust cautiously — reducing `dac_limit_min`/increasing `dac_limit_max` expands the scan angle but may stress the galvo driver.

### Color & Brightness

| Field | Default | Range | Description |
|-------|---------|-------|-------------|
| `gain_r` | `115` | 0..255 | White balance gain for the red channel. Calibrated for R=1W, 638 nm (V(λ)=0.235). |
| `gain_g` | `43` | 0..255 | White balance gain for the green channel. Calibrated for G=1W, 520 nm (V(λ)=0.710). |
| `gain_b` | `255` | 0..255 | White balance gain for the blue channel. Calibrated for B=3W, 445 nm (V(λ)=0.040). |
| `gamma_enable` | `true` | bool | Enable CIE 1931 perceptual brightness correction. When enabled, the 0–255 PWM range follows human perception rather than linear power. Strongly recommended — linear laser output looks "blown out" at mid-brightness. |
| `thresh_r` | `143` | 0..255 | Visibility threshold for red: the minimum PWM duty at which the red laser diode actually emits visible light. Below this value the beam is physically dark. Calibrated via the Calibration tab. |
| `thresh_g` | `144` | 0..255 | Same for green. |
| `thresh_b` | `169` | 0..255 | Same for blue. The blue channel typically has the highest threshold. |

**How thresholds work:** The `mapVisibleRange()` function remaps the logical 0–255 color range onto [thresh_x .. 255], so "0% brightness" in patterns always means the laser is off, and "100% brightness" always means full output — regardless of the threshold. Without calibrated thresholds, the bottom portion of the brightness range does nothing visible, making the projector appear to have lower dynamic range than it actually has.

### Network

| Field | Default | Description |
|-------|---------|-------------|
| `wifi_ssid` | `""` | Wi-Fi network name. Empty = start in AP mode (SSID: "galvOS", open). |
| `wifi_pass` | `""` | Wi-Fi password. |
| `hostname` | `"galvOS"` | mDNS hostname. Accessible as `http://galvOS.local` on networks with mDNS support. Auto-generated from MAC if empty. |
| `wifi_static` | `false` | Use static IP instead of DHCP. |
| `wifi_ip` / `wifi_gw` / `wifi_mask` / `wifi_dns` | `""` | Static IP configuration. Only used when `wifi_static = true`. |
| `ntp_server` | `"pool.ntp.org"` | NTP server for time synchronisation. |
| `ntp_tz` | `"UTC0"` | POSIX timezone string. Example for Central European Time: `"CET-1CEST,M3.5.0,M10.5.0/3"`. |

### DMX / Art-Net

| Field | Default | Range | Description |
|-------|---------|-------|-------------|
| `dmx_address` | `1` | 1..512 | DMX start channel. CH1 = Master Dimmer, CH2–25 as per the channel map in `config.h`. |
| `artnet_universe` | `0` | 0..32767 | Art-Net universe number. |

### Safety & Diagnostics

| Field | Default | Description |
|-------|---------|-------------|
| `scanfail_timeout_ms` | `50` | How long the NE555 scan-fail timer runs before declaring a fault (firmware side, for display only — the hardware NE555 has its own RC time constant). |
| `watchdog_period_ms` | `500` | Hardware watchdog heartbeat interval. Firmware pulses GPIO14 at this rate; the NE555 watchdog must be retriggered within this window or it cuts the laser rail. |
| `heap_critical_bytes` | `6144` | Minimum free internal DRAM block. If internal heap fragmentation causes the largest free block to fall below this, the firmware triggers `esp_restart()`. Calibrated to 6 KB — approximately 2× margin below the measured peak load. |
| `safety_override` | `false` | Bypass software safety checks. **Use only for development.** Never enable in production. |
| `auth_hash` | `""` | SHA-256 hex of the WebUI password. Empty = default password `"laser"`. Set via the Settings tab. |
| `dac_debug_log` | `false` | Log DAC8562 writes (hex) to serial and WebUI log, rate-limited. Useful for debugging DAC output; not for production use. |

---

## ProjectionConfig — Galvo & Laser Parameters

`ProjectionConfig` (stored as `gProjection`, NVS namespace `"projection"`) holds parameters related to the physical galvo and laser hardware.

| Field | Default | Description |
|-------|---------|-------------|
| `galvo_kpps` | `20` | **The most important runtime parameter.** Output rate in kilo-points-per-second. This directly controls the ISR period and how fast the galvo mirrors move. Range: 12–60 kpps. The Jolooyo JY-15K-BL is rated at 15 kpps — running above this causes missed steps and visible distortion. Start at 15 and only increase if your specific hardware handles it. |
| `galvo_rated_kpps` | `15` | The galvo's rated speed from its datasheet. Used as the basis for PPS scaling in the optimizer — do not confuse with `galvo_kpps`. If you use a different galvo set, set this to its rated speed. |
| `scan_angle_mech_deg` | `25.0°` | Galvo mechanical half-angle (±25° = 50° full sweep). Used for display and safety zone calculations. |
| `exit_angle_deg` | `20.0°` | Housing aperture half-angle — often smaller than the mechanical limit. |
| `ilda_test_angle_deg` | `8.0°` | ILDA standard rating angle (±8° optical). Used for PPS scaling calculations. |
| `power_r_mw` | `1000.0` | Red channel laser power in mW. Used for white balance auto-calculation and laser hazard display. |
| `power_g_mw` | `1000.0` | Green channel laser power in mW. |
| `power_b_mw` | `3000.0` | Blue channel laser power in mW. The blue diode in the Mikoy 5W is 3W — be aware that 445 nm carries an elevated photochemical retinal hazard (B(λ) = 0.22). |
| `distance_m` | `3.0` | Throw distance to the projection surface in metres. Used for spot size and safety calculations in the UI. |

---

## SafetyConfig — Temperature Thresholds

`SafetyConfig` (stored as `gSafety`, in the `"laser"` NVS namespace) controls the temperature-based safety responses.

| Field | Default | Description |
|-------|---------|-------------|
| `temp_warn_c` | `45°C` | Temperature at which fans switch to 100% duty. Normal operation: fans run at `fan_min_pct`. |
| `temp_reduce_c` | `55°C` | Temperature at which laser power is reduced to 50% (via `thermal_power_scale`). |
| `temp_shutdown_c` | `70°C` | Temperature at which an immediate shutdown is triggered. |
| `fan_min_pct` | `15%` | Minimum fan PWM percentage. Below ~15%, most 12V fans fail to start reliably. |
| `fan_auto` | `true` | Automatic fan speed based on temperature. If false, fans run at `fan_min_pct` always. |

---

## Optimizer Defaults

All optimizer defaults are defined as `OPT_DEFAULT_*` macros in `config.h`. These set the initial values for all six optimizer profiles on first boot (before NVS). Changing them requires a rebuild and an NVS reset to take effect (existing NVS values take priority over compile-time defaults).

For a full explanation of what each parameter does, see [Chapter 5 — The Optimizer](05-optimizer.md).

| Macro | Default | Description |
|-------|---------|-------------|
| `OPT_DEFAULT_CORNER_ANGLE_DEG` | `25.0°` | Minimum angle (at a vertex) that triggers corner dwell extra points. |
| `OPT_DEFAULT_MIN_CORNER_PTS` | `2` | Minimum extra points added at a corner. |
| `OPT_DEFAULT_MAX_CORNER_PTS` | `8` | Maximum extra points added at a corner. |
| `OPT_DEFAULT_PTS_PER_1000_UNITS` | `6.0` | Interior point density — points added per 1000 DAC units of segment length. |
| `OPT_DEFAULT_MIN_SEGMENT_PTS` | `2` | Minimum points per segment (excluding endpoints). |
| `OPT_DEFAULT_BLANK_SAMPLES` | `16` | Default blank jump sample count (without distance scaling). |
| `OPT_DEFAULT_MAX_PTS_PER_FRAME` | `1010` | Point budget per frame. **Known effective limit: 1300** — no optical improvement is observed above this value on the JY-15K-BL hardware. |
| `OPT_DEFAULT_MIN_BLANK_SAMPLES` | `6` | Minimum blank samples (floor for distance-scaled blanking). |
| `OPT_DEFAULT_BLANK_PTS_PER_1000_UNITS` | `8.0` | Blank sample count scales with jump distance at this rate. |
| `OPT_DEFAULT_MIN_INTERIOR_PTS_PER_SEG` | `8` | Minimum interior points for longer segments. |
| `OPT_DEFAULT_STAGE1_BLANK_TARGET` | `16` | Stage 1 blank target point count. |
| `OPT_DEFAULT_RESAMPLE_ENABLED` | `false` | Constant-spacing resample stage. Disabled by default — output is identical to pre-resample when off. |
| `OPT_DEFAULT_RESAMPLE_SPACING_UNITS` | `160.0` | Spacing between resampled points in DAC units. |
| `OPT_DEFAULT_RINGING_COMP_ENABLED` | `false` | ZV input-shaping ringing compensation. **Must be measured on your hardware before enabling** — wrong values make ringing worse. |
| `OPT_DEFAULT_RING_FREQ_HZ` | `200.0` | Resonant frequency of the galvo (Hz). Measure via scope step-response capture. |
| `OPT_DEFAULT_RING_DAMPING_RATIO` | `0.15` | Damping ratio of the galvo. Measure via scope. |
| `OPT_DEFAULT_VEL_CLAMP_ENABLED` | `false` | Velocity clamp. Disabled by default — tune `max_step_units` for your hardware first. |
| `OPT_DEFAULT_MAX_STEP_UNITS` | `200.0` | Maximum per-tick position change (DAC units/sample). Long lit moves above this are subdivided. |
| `OPT_DEFAULT_ACCEL_CLAMP_ENABLED` | `false` | Acceleration clamp. Disabled by default. |
| `OPT_DEFAULT_MAX_ACCEL_UNITS` | `800.0` | Maximum per-tick change in step magnitude (DAC units/sample²). |

---

## pinmap.h — GPIO Assignments

All GPIO assignments are defined in `include/pinmap.h`. The following table summarises the assignments. Pins marked **Do Not Use** are reserved by hardware and cannot be reassigned.

| GPIO | Assignment | Direction | Notes |
|------|-----------|-----------|-------|
| 1 | `PIN_SD_MISO` | Input | SD card MISO on SPI3 (independent from DAC's SPI2). Pull-up recommended. |
| 4 | `PIN_DMX_RX` | Input | DMX-512 receive from MAX485 RO |
| 5 | `PIN_SD_SCK` | Output | SD card SCK on SPI3 (independent from DAC's SPI2) |
| 6 | `PIN_SD_MOSI` | Output | SD card MOSI on SPI3 (independent from DAC's SPI2) |
| 7 | `PIN_LASER_R` | Output | Red laser TTL (via 6N137). Fail-safe pull-up R_FSR 10kΩ → +3.3V |
| 8 | `PIN_LASER_G` | Output | Green laser TTL (via 6N137). Fail-safe pull-up R_FSG 10kΩ → +3.3V |
| 10 | `PIN_GALVO_CS` | Output | DAC8562 /SYNC (chip select) on SPI2 |
| 11 | `PIN_GALVO_MOSI` | Output | SPI2 MOSI — DAC8562 only, no longer shared |
| 12 | `PIN_GALVO_SCK` | Output | SPI2 SCLK — DAC8562 only, no longer shared |
| 13 | `PIN_DAC_CLR_N` | Output | DAC8562 /CLR — pulsed LOW at init, then HIGH |
| 14 | `PIN_WATCHDOG_OUT` | Output | Hardware watchdog heartbeat to NE555 (U12) |
| 16 | `PIN_FAN1_PWM` | Output | Fan 1 PWM (25 kHz, 8-bit) |
| 17 | `PIN_FAN2_PWM` | Output | Fan 2 PWM (25 kHz, 8-bit) |
| 18 | `PIN_ONEWIRE` | Bidirectional | DS18B20 1-Wire data. 4.7 kΩ pull-up to +3.3V required. |
| 21 | `PIN_LASER_B` | Output | Blue laser TTL (via 6N137). Fail-safe pull-up R_FSB 10kΩ → +3.3V |
| 38 | `PIN_LASER_ENABLE` | Output | Central laser enable → SSR1. HIGH only when all safety checks pass. |
| 39 | `PIN_SCAN_FAIL_IN` | Input | NE555 scan-fail output (U11) |
| 42 | `PIN_SD_CS` | Output | SD card chip select on SPI3 (independent from DAC's SPI2) |
| 43 | `PIN_DEBUG_TX` | Output | UART0 TX / USB CDC TX |
| 44 | `PIN_DEBUG_RX` | Input | UART0 RX / USB CDC RX |
| 47 | `PIN_ESTOP` | Input | Emergency stop (active = pin HIGH via pull-up = E-stop not pressed) |
| 48 | `PIN_STATUS_LED` | Output | Onboard RGB LED |

**Reserved — Do Not Use:**

| GPIO | Reason |
|------|--------|
| 0, 3, 45, 46 | Strapping pins — state at boot determines boot mode |
| 19, 20 | USB D−/D+ — native USB CDC |
| 35, 36, 37 | OPI PSRAM internal connections on N16R8 — not accessible |

**Currently unassigned (free for expansion):**

GPIO 5, 6, 15, 40, 41, 42 are free. GPIO 15, 40, 41 were previously used for an encoder (removed in v3.2).

---

## NVS — Parameter Persistence

GalvOS uses the ESP32 NVS (Non-Volatile Storage) to persist configuration across reboots. Parameters are stored in two NVS namespaces:

| Namespace | Contents |
|-----------|---------|
| `"laser"` | `RuntimeConfig` fields, optimizer profiles, safety config, Wi-Fi credentials |
| `"projection"` | `ProjectionConfig` fields (galvo_kpps, laser power, angles, distance) |

NVS values take priority over compile-time defaults on every boot. This means:
- Changing an `OPT_DEFAULT_*` macro in `config.h` has **no effect** if that parameter is already stored in NVS.
- To apply new compile-time defaults, you must reset NVS (see below).

Optimizer profile parameters are keyed with a per-profile suffix (`_s`, `_c`, `_w`, `_3`, `_sol`, `_sc`) pinned to the profile index — not the profile name. This means profile parameters survive renames without resetting to defaults.

---

## Resetting to Defaults

**Via WebUI:** Settings tab → "Reset NVS" button. This clears all NVS keys in the `"laser"` and `"projection"` namespaces and restarts the ESP32. On reboot, all compile-time defaults are applied and a new Wi-Fi AP is started.

**Via serial monitor:** Send `nvs_reset` over the serial console (if implemented), or trigger a factory reset by holding a specific GPIO low during boot (check the current firmware for the exact method).

**Via esptool (nuclear option):** Erase the entire NVS partition:
```bash
esptool.py --port /dev/ttyUSB0 erase_region 0x9000 0x5000
```
This clears NVS completely. The firmware will apply all compile-time defaults on next boot.

> **Note:** Wi-Fi credentials are stored in NVS. After a reset, the ESP32 will return to AP mode (SSID: "galvOS") with no password until you reconfigure it via the WebUI Settings tab.
