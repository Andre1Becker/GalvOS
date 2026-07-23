# Chapter 7 — API Reference

The GalvOS REST API is served by the ESP32 WebUI server (ESPAsyncWebServer) at `http://<device-ip>/api/`. The WebUI itself uses this API exclusively — everything the browser can do, an external system can do too.

## Table of Contents
- [Base URL & Conventions](#base-url--conventions)
- [Authentication](#authentication)
- [Route Registration Order](#route-registration-order)
- [System & Status](#system--status)
- [Configuration](#configuration)
- [Safety & ARM](#safety--arm)
- [Presets & Live Controls](#presets--live-controls)
- [Color Animations & Curves](#color-animations--curves)
- [Calibration](#calibration)
- [Optimizer](#optimizer)
- [Projection](#projection)
- [Text Mode](#text-mode)
- [Paint Mode](#paint-mode)
- [DMX & Art-Net](#dmx--art-net)
- [ILDA & SD Card](#ilda--sd-card)
- [Playlist](#playlist)
- [Zone (Projection Clipping)](#zone-projection-clipping)
- [Thermal & Fan](#thermal--fan)
- [Timer](#timer)
- [Wi-Fi](#wi-fi)
- [Log](#log)
- [Debug & Diagnostics](#debug--diagnostics)

---

## Base URL & Conventions

```
Base URL:  http://<device-ip>/api/
Format:    JSON (application/json) for all requests and most responses
Encoding:  UTF-8
```

**Request bodies** are JSON unless noted otherwise. Send `Content-Type: application/json`.

**Response bodies** are JSON or plain text (`text/plain`). Successful write operations typically respond with `200 OK` and body `"OK"` or a small JSON object. Errors respond with `400 Bad Request` and body `"bad json"` or a JSON error object.

**Partial updates:** Most `POST /api/config` and similar write endpoints apply only the fields present in the request body. Fields not included are not changed. There is no need to send the full configuration on every write.

---

## Authentication

Write endpoints (all `POST`) require an `X-Auth` header with the current session token:

```
X-Auth: <token>
```

The token is a session-scoped random value generated at boot. It is displayed in the Dashboard → System card ("API Token") and returned in the `/api/state` response as `auth_token`. The token changes on every reboot.

`GET` endpoints are unauthenticated (read-only).

Default WebUI password for OTA updates: the first 8 hex digits of the chip MAC (shown as "HTTP-OTA Pass" on the Dashboard). Username: `admin`.

---

## Route Registration Order

ESPAsyncWebServer matches routes in registration order. Two endpoints are order-sensitive and **must** be registered before any wildcard prefix handler that would otherwise capture them:

- `POST /api/calib-pattern/stop` — registered before `/api/calib-pattern`
- `GET /api/text/vertices` — registered before `/api/text`
- `/api/calib-cam/{start,params,stop,status}` — registered before `/api/calib-pattern/...` and well before the LittleFS catch-all

This is handled correctly in the shipped firmware. If you add new routes that share a prefix with an existing endpoint, register the more specific route first.

---

## System & Status

### `GET /api/state`

Full system state. Polled by the WebUI every second.

**Response fields:**

| Field | Type | Description |
|-------|------|-------------|
| `estop_ok` | bool | E-Stop circuit closed (not pressed) |
| `scanfail_ok` | bool | NE555 scan-fail hardware OK |
| `laser_armed` | bool | Laser power rail enabled |
| `watchdog_ok` | bool | Hardware watchdog heartbeat OK |
| `subsystems_ok` | bool | All firmware subsystems healthy |
| `last_failsafe` | string | Reason for last safety shutdown (survives restart) |
| `arm_requested` | bool | User has pressed ARM |
| `calib_active` | bool | Calibration pattern currently active |
| `ilda_active` | bool | ILDA player currently active |
| `playlist_active` | bool | Playlist currently running |
| `safety_override` | bool | Safety override enabled |
| `source` | int | Active control source: 0=none, 1=DMX, 2=ArtNet, 3=EtherDream, 4=Helios, 5=Internal, 6=WebUI |
| `master_dimmer` | int | Effective master brightness (0–255) |
| `dmx_frame_count` | int | Running count of received DMX frames |
| `points_per_sec` | int | Current galvo output rate (pps) |
| `fps` | int | Drawn frames per second |
| `buffer_fill` | int | Ring buffer fill level (%) |
| `last_dmx_age_ms` | int | ms since last DMX frame (−1 if never received) |
| `preset_idx` | int | Active preset index (−1 if none) |
| `starfield_stars` | int | Actual rendered star count for the Starfield preset (since v6.02.4) — the requested Size (0–255) can be capped lower by the Particles optimizer profile's `max_pts_per_frame` budget; the WebUI shows this value instead of echoing the raw slider |
| `dac_ok` | bool | DAC8562 initialised and responding |
| `no_hw_mode` | bool | No-HW debug mode active |
| `heap` | int | Free internal heap (bytes) |
| `psram` | int | Free PSRAM (bytes) |
| `cpu0` | int | Core 0 CPU load (%) |
| `cpu1` | int | Core 1 CPU load (%) |
| `ip` | string | Current IP address |
| `rssi` | int | Wi-Fi signal strength (dBm) |
| `uptime_s` | int | Uptime in seconds |
| `hostname` | string | mDNS hostname |
| `fw_version` | string | Firmware version string |
| `ota_pass` | string | OTA HTTP password (chip-ID based) |
| `auth_token` | string | Current API write token |
| `temps` | array | Temperature readings per sensor (°C, null if sensor absent) |
| `names` | array | Sensor names |
| `ok` | array | Sensor OK flags |
| `fan1_duty` / `fan2_duty` | int | Current fan PWM duty (0–255) |
| `temp_alert` | bool | Any sensor above warn threshold |
| `temp_crit` | bool | Any sensor above shutdown threshold |
| `sd_ready` | bool | SD card mounted and accessible |
| `sd_free_kb` / `sd_total_kb` | int | SD card capacity |
| `sd_fs_type` | string | Filesystem type (e.g. "FAT32") |
| `sd_file_count` | int | Number of `.ild` files indexed |
| `ntp_synced` | bool | NTP time synchronised |
| `found` | int | Number of DS18B20 sensors found on 1-Wire bus |

---

### `GET /api/status`

Lightweight status response. Lower overhead than `/api/state` — uses direct `snprintf` instead of JSON serialisation. Suitable for high-frequency polling.

**Response fields:** `estop_ok`, `scanfail_ok`, `laser_armed`, `source`, `master_dimmer`, `points_per_sec`, `buffer_fill`, `debug_mode`, `ui_override`, `ui_master_dimmer`, `fw_version`, `ota_pass`, `free_heap`, `free_psram`, `hostname`, `ip`, `rssi`, `uptime_s`, `last_dmx_age_ms`.

---

## Configuration

### `GET /api/config`

Returns the full `RuntimeConfig` plus all optimizer profiles.

Key response fields: all `RuntimeConfig` fields (see [Chapter 3](03-build-and-config.md#runtimeconfig--user-parameters)), plus:

| Field | Description |
|-------|-------------|
| `opt_active_profile` | Index of the currently active optimizer profile (0–5) |
| `opt_profiles` | Array of 6 profile objects, each containing all optimizer parameters and their effective values (`opt_eff_*`) |
| `opt_profile_members` | Array of 6 arrays, each listing the preset names that belong to that profile |
| `opt_*` (top-level) | Active profile values — provided for backwards compatibility |

---

### `POST /api/config`

Write one or more `RuntimeConfig` fields. Only fields present in the body are updated. Takes effect immediately; changes are not automatically persisted to NVS (use the Calibration save button or `/api/calib-save`).

**Example — change DMX address:**
```json
{"dmx_address": 17}
```

**Example — update white balance gains:**
```json
{"gain_r": 115, "gain_g": 43, "gain_b": 255}
```

**Example — update visibility thresholds:**
```json
{"thresh_r": 143, "thresh_g": 144, "thresh_b": 169}
```

---

## Safety & ARM

### `POST /api/arm`

Arm or disarm the laser. Body is a plain `1` (arm) or `0` (disarm) — not JSON.

```
POST /api/arm
Body: 1
```

Response: `"ARMED"` or `"DISARMED"` (plain text).

Arming only succeeds if all hardware safety conditions are met (E-Stop OK, scan-fail OK, watchdog OK). The response reflects the request, not whether arming actually succeeded — check `/api/state` → `laser_armed` to confirm.

---

### `POST /api/safety-override`

Enable or disable the software safety override. **Development use only.**

```json
{"enabled": true}
```

Response: `{"ok": true, "enabled": true}`

---

### `GET /api/safety/config`

Returns current safety thresholds.

---

### `POST /api/safety/config`

Update safety thresholds.

```json
{
  "temp_warn_c": 45,
  "temp_reduce_c": 55,
  "temp_shutdown_c": 70,
  "fan_min_pct": 15,
  "fan_auto": true
}
```

---

## Presets & Live Controls

### `GET /api/presets`

Returns the full preset list. Used by the Presets tab grid.

**Response:** Array of preset objects:
```json
[
  {"idx": 0, "name": "Circle", "class": 0, "svg": "<svg>...</svg>"},
  ...
]
```

`class` maps to the optimizer profile index (0=Vector, 1=Smooth, 2=Waves, 3=Wireframe, 4=MultiObject, 5=Particles).

---

### `POST /api/preset`

Activate a preset by index, or deactivate.

```json
{"idx": 5}
```

Deactivate (laser off):
```json
{"idx": -1}
```

---

### `POST /api/preset-live`

Update live preset controls without changing the active preset. All fields are optional — only those present are applied. Takes effect on the next rendered frame.

**Available fields:**

| Field | Type | Range | Description |
|-------|------|-------|-------------|
| `speed` | int | 0–255 | Animation speed |
| `size` | int | 0–255 | Pattern scale |
| `autoscaleSpeed` | int | 0–100 | Auto-scaling speed (0 = off) |
| `autoscaleMode` | int | 0–2 | 0=Small→Big→Small, 1=Small→Big, 2=Big→Small |
| `col_r/g/b` | int | 0–255 | Color override channels |
| `col_override` | bool | — | Enable color override |
| `col_anim_type` | int | 0–7 | Color animation type (0=off) |
| `col_anim_seq` | int | 0–9 | Color sequence index |
| `col_anim_speed` | int | 0–255 | Color animation speed |
| `col_seg_count` | int | 1–10 | Segment color count |
| `col_seg_dir` | int | −1, +1 | Segment animation direction |
| `rotation` | int | −180–180 | Static Z rotation (degrees) |
| `rot_x/y/z` | bool | — | Enable continuous auto-rotation axis |
| `rot_speed` | float | 0–1 | Auto-rotation master speed |
| `wave_amp` | float | 0.1–2.0 | Wave amplitude multiplier |
| `wave_freq` | float | 0.25–4.0 | Wave frequency multiplier |
| `kaleido_enabled` | bool | — | Kaleidoscope effect |
| `kaleido_segments` | int | 2–16 | Number of segments |
| `kaleido_mirror_h/v` | bool | — | Mirror alternate segments |
| `mirror_mode` | int | 0–3 | Mirror: 0=off, 1=X, 2=Y, 3=Radial4 |
| `points_mode_enabled` | bool | — | Points-Only mode |
| `points_count` | int | 2–80 | Number of dots |
| `points_fade_in_on/out_on` | bool | — | Enable fade in/out |
| `points_fade_in_ms/out_ms` | int | 0–10000 | Fade duration (ms) |
| `points_fade_dir` | int | 0–5 | Fade direction |
| `points_static_on` | bool | — | Static mode (no fade) |
| `bp_trail_len` | int | 0–12 | Bouncing Points trail length |
| `bp_endless` | bool | — | Bouncing Points loop forever |
| `bp_duration_sec` | int | 1–90 | Duration when not endless |

---

## Color Animations & Curves

Color animations are applied via `POST /api/preset-live` using `col_anim_type` and related fields (see above).

To stop an animation and clear the color override:
```json
{"col_override": false, "col_anim_type": 0}
```

### `GET /api/curves`

Returns the mathematical curve definitions and current parameter values.

---

### `POST /api/curves`

Activate a curve and/or set its parameters.

```json
{
  "active_curve": 0,
  "params": [[1.0, 2.0, 0.0, 0.0, 0.0], ...],
  "colors": [{"r": 255, "g": 0, "b": 0}, ...]
}
```

Set `"active_curve": -1` to deactivate curve mode.

---

## Calibration

### `GET /api/calib-pattern/list`

Returns the list of available calibration patterns with index, name, and description.

---

### `POST /api/calib-pattern`

Activate a calibration pattern.

```json
{"idx": 0, "channel": 0}
```

`channel`: 0 = RGB, 1 = R only, 2 = G only, 3 = B only. Not all patterns respect the channel parameter.

---

### `POST /api/calib-pattern/stop`

Stop the active calibration pattern and return to normal operation.

> ⚠️ This route must be registered **before** any prefix-matching handler for `/api/calib-pattern`. See [Route Registration Order](#route-registration-order).

Body: empty or `{}`.

---

### Camera-in-the-Loop Calibration (`/api/calib-cam/*`)

Added in v6.03.0 — a session-based API for the host-side camera auto-tuning tool (`scripts/optimizeGalvo/optimizeGalvo.py`, see [Chapter 11](11-camera-autotuning.md)). It projects one of 6 dedicated camera-reference patterns and lets the host apply optimizer overrides live, RAM-only, without touching NVS. There is no dedicated WebUI panel for this — it exists purely for the host tool to drive.

All four routes are registered before `/api/calib-pattern/...` for the same route-ordering reason as `/api/calib-pattern/stop` — see [Route Registration Order](#route-registration-order).

#### `POST /api/calib-cam/start`

Starts a session and activates one of the camera-reference patterns.

```json
{"pattern": "square", "channel": 3}
```

`pattern`: one of `corners4`, `square`, `star`, `segments`, `circle`, `spiral`. `corners4` is the 4-dot homography reference used by the tool's `calibrate` command; the rest are used for measurement and tuning.

`channel` (optional, since v6.04.1): `0` = white (R+G+B), `1` = R, `2` = G, `3` = B (**default**). Patterns default to blue rather than white because a mono/global-shutter camera can see the R/G/B beams smear apart or offset if the laser diodes aren't perfectly co-boresighted — a single channel avoids that entirely.

Starting a session snapshots the current values of whichever optimizer profile the pattern belongs to, and switches the active profile to it if it isn't already active. Any previous session that was never cleanly `/stop`-ped (client crash, page reload mid-run) is force-restored first, so overrides can never leak across sessions.

---

#### `POST /api/calib-cam/params`

Applies optimizer parameter overrides to the session's profile, live, without persisting to NVS. Requires an active session (`/start` first).

```json
{
  "corner_angle_deg": 25.0,
  "max_corner_pts": 8,
  "blank_samples": 16,
  "profile": 0
}
```

All fields are optional; unrecognized keys are echoed back in the response's `ignored` array instead of erroring. **Note the field names here have no `opt_` prefix**, unlike `/api/optimizer-live` — `corner_angle_deg` here is `opt_corner_angle_deg` there. Both endpoints clamp to the same bounds by hand-kept convention. `profile`, if present, must match the profile the active pattern already belongs to (it exists only so the caller can double-check, not to switch profiles mid-session).

Response:

```json
{"ok": true, "applied": {"corner_angle_deg": 25.0, "max_corner_pts": 8}, "ignored": []}
```

`applied` echoes the effective (post-clamp) value of every field that was recognized and set.

---

#### `POST /api/calib-cam/stop`

Ends the session and restores the profile's pre-session snapshot (if any override was ever applied). Body: empty or `{}`.

> Tuned values do **not** persist by themselves — stopping the session always reverts to the snapshot. To keep a tuned result, the host tool must call `/api/optimizer-live` (with the winning values, `opt_`-prefixed) and `/api/optimizer-save` *before* calling `/stop`, or the values vanish when the session ends. This is also invoked automatically the instant E-Stop trips, so an aborted tuning run can never leave a preset's optimizer profile silently altered.

---

#### `GET /api/calib-cam/status`

```json
{
  "active": true,
  "pattern": "square",
  "overrides": {"corner_angle_deg": 25.0, "max_corner_pts": 8}
}
```

`overrides` lists only the fields that differ from the session's original snapshot — i.e. what has actually been changed so far.

---

### `POST /api/calib-live`

Apply galvo calibration values live (without saving to NVS). Fields optional.

```json
{
  "galvo_x_offset": 0,
  "galvo_y_offset": 0,
  "galvo_x_gain": 32767,
  "galvo_y_gain": 32767,
  "swap_xy": false,
  "invert_x": false,
  "invert_y": false,
  "dac_limit_min": 1638,
  "dac_limit_max": 63897
}
```

---

### `POST /api/calib-save`

Persist the current calibration values (gain, threshold, gamma, offsets, DAC limits) to NVS.

Body: empty or `{}`.

---

### `POST /api/calib-thresh-test`

Start or stop the visibility threshold test beam.

```json
{"active": true, "channel": 0}
```

`channel`: 0 = RGB, 1 = R, 2 = G, 3 = B.

The test beam bypasses gain, gamma, and master dimmer — only the threshold sliders control it.

---

### `POST /api/test-pattern`

Activate the ILDA standard test pattern.

```json
{"active": true, "size": 128}
```

---

## Optimizer

### `POST /api/optimizer-profile-switch`

Switch the active optimizer profile.

```json
{"profile": 0}
```

Profile indices: 0=Vector, 1=Smooth, 2=Waves, 3=Wireframe, 4=MultiObject, 5=Particles.

---

### `POST /api/optimizer-live`

Apply optimizer parameters to the active profile immediately (no NVS persist). All fields optional.

```json
{
  "opt_corner_angle_deg": 25.0,
  "opt_min_corner_pts": 2,
  "opt_max_corner_pts": 8,
  "opt_pts_per_1000_units": 6.0,
  "opt_min_segment_pts": 2,
  "opt_blank_samples": 16,
  "opt_max_pts_per_frame": 1010,
  "opt_min_blank_samples": 6,
  "opt_blank_pts_per_1000_units": 8.0,
  "opt_min_interior_pts_per_segment": 8,
  "opt_stage1_blank_target": 16,
  "opt_resample_enabled": false,
  "opt_resample_spacing_units": 160.0,
  "opt_ringing_comp_enabled": false,
  "opt_ring_freq_hz": 200.0,
  "opt_ring_damping_ratio": 0.15,
  "opt_vel_clamp_enabled": false,
  "opt_max_step_units": 200.0,
  "opt_accel_clamp_enabled": false,
  "opt_max_accel_units": 800.0
}
```

---

### `POST /api/optimizer-save`

Persist the current optimizer profile values to NVS.

Body: empty or `{}`.

---

## Projection

### `GET /api/projection`

Returns projection configuration and all derived safety calculations.

**Key response fields:**

| Field | Description |
|-------|-------------|
| `kpps` | Current output rate |
| `rated_kpps` | Galvo rated speed |
| `exit_angle` | Housing aperture half-angle (°) |
| `max_safe_kpps` | Maximum safe kpps for current angle |
| `total_mw` / `vis_mw` / `blhaz_mw` | Power totals |
| `awb_r/g/b` | Auto white balance gains |
| `img_w_m` / `img_h_m` | Projected image dimensions at `distance_m` |
| `irr_mw_cm2` | Peak irradiance (mW/cm²) |
| `min_dist_m` | Estimated minimum audience distance |
| `ne555_ok` | Whether kpps is high enough to trigger scan-fail NE555 |

---

### `POST /api/projection`

Update projection configuration and save to NVS.

```json
{
  "kpps": 20,
  "rated_kpps": 15,
  "scan_angle_mech_deg": 25.0,
  "exit_angle_deg": 20.0,
  "ilda_test_angle_deg": 8.0,
  "power_r_mw": 1000.0,
  "power_g_mw": 1000.0,
  "power_b_mw": 3000.0,
  "distance_m": 3.0
}
```

---

### `POST /api/projection/awb`

Compute and apply auto white balance gains from the current power values. Does not require a request body.

Response: `{"gain_r": N, "gain_g": N, "gain_b": N}`

---

### `GET /api/galvo/autotune`

Returns the current state of the kpps autotune sweep.

```json
{
  "running": false,
  "done": true,
  "floor_unstable": false,
  "candidate_kpps": 30,
  "result_kpps": 28,
  "step": 8,
  "step_total": 8
}
```

---

### `POST /api/galvo/autotune`

Start or abort the autotune sweep.

```json
{"action": "start"}
```

```json
{"action": "abort"}
```

The autotune binary-searches for the highest kpps that produces zero ring buffer overflows over a 1500 ms measurement window. Run with an active pattern for a meaningful result.

---

## Text Mode

### `POST /api/text`

Activate text mode and set content. Text mode overrides presets and DMX while active.

```json
{
  "text": "HELLO WORLD",
  "font": 0,
  "anim": 1,
  "speed": 80,
  "size": 128,
  "col_r": 255,
  "col_g": 255,
  "col_b": 255,
  "rainbow": false,
  "flip_x": false,
  "flip_y": false,
  "active": true
}
```

| Field | Values |
|-------|--------|
| `font` | 0=Simple, 1=Bold, 2=Outline |
| `anim` | 0=Static, 1=Scroll Left, 2=Scroll Right, 3=Bounce, 4=Typewriter, 5=Wave, 6=Pulse, 7=Rotate, 8=Zoom, 10=Orbit, 11=Star Wars |

Characters supported: A–Z, 0–9, `.,:!?-+`

---

### `GET /api/text`

Returns current text configuration.

---

### `POST /api/text/off`

Deactivate text mode. Body: empty.

---

### `GET /api/text/vertices`

Returns the pre-computed vertex list for the current text. Used by the WebUI for preview rendering.

> ⚠️ Must be registered **before** the general `/api/text` handler. See [Route Registration Order](#route-registration-order).

---

## Paint Mode

### `GET /api/paint`

Returns current paint canvas state (strokes, vertex counts, colors).

---

### `POST /api/paint/set`

Upload canvas strokes and activate paint mode. Paint mode overrides presets, curves, and DMX.

```json
{
  "active": true,
  "strokes": [
    {
      "closed": false,
      "r": 255, "g": 0, "b": 0,
      "x": [0.1, 0.5, 0.9],
      "y": [0.5, 0.2, 0.8]
    }
  ]
}
```

Coordinates are normalized [0.0, 1.0] and mapped to the galvo coordinate space by the firmware. Maximum 12 strokes, 96 vertices per stroke.

---

### `POST /api/paint/clear`

Clear all strokes from the canvas. Body: empty.

---

### `POST /api/paint/off`

Deactivate paint mode. Body: empty.

---

## DMX & Art-Net

### `GET /api/dmx/channels`

Returns the current DMX channel values (from hardware input or override) and the channel name map.

---

### `GET /api/dmx/address`

Returns `{"dmx_address": N, "artnet_universe": N}`.

---

### `POST /api/dmx/address`

Update DMX start address and/or Art-Net universe.

```json
{"dmx_address": 1, "artnet_universe": 0}
```

---

### `POST /api/dmx-override`

Set all 25 DMX override channel values at once.

```json
{"values": [255, 0, 0, 0, 0, 0, 0, 128, 0, 0, 0, 0, 128, 128, 0, 0, 0, 0, 128, 0, 255, 0, 0, 0, 0]}
```

Array must be exactly 25 elements (DMX_CHANNELS_USED).

---

### `POST /api/override-mode`

Enable or disable the WebUI DMX override.

```json
{"active": true}
```

When active, the slider values from `/api/dmx-override` drive the pattern engine instead of the hardware DMX input.

---

### `GET /api/artnet/status`

Returns Art-Net and Ether Dream connection status.

```json
{
  "enabled": true,
  "universe": 0,
  "dmx_address": 1,
  "etherdream_connected": false,
  "etherdream_playing": false
}
```

---

## ILDA & SD Card

### `GET /api/sd`

Returns the list of `.ild` files on the SD card.

---

### `GET /api/sd/info`

Returns SD card status (ready, type, total KB, free KB, file count, error message).

---

### `POST /api/sd/scan`

Re-scan the SD card for `.ild` files. Body: empty.

---

### `POST /api/sd/remount`

Unmount and remount the SD card. Body: empty.

---

### `POST /api/sd/eject`

Safely unmount the SD card. Body: empty.

---

### `POST /api/ilda/play`

Start ILDA playback.

```json
{
  "file_idx": 0,
  "loop": true,
  "speed": 128,
  "size": 128,
  "brightness": 255
}
```

---

### `POST /api/ilda/stop`

Stop ILDA playback. Body: empty.

---

### `POST /api/ilda/pause`

Pause/resume ILDA playback. Body: empty.

---

### `GET /api/ilda/status`

Returns current ILDA player state (active, file index, frame count, loop mode).

---

### `POST /api/ilda/upload`

Upload a `.ild` file directly to the SD card via HTTP multipart. Used for OTA ILDA file transfer.

---

## Playlist

### `GET /api/playlist`

Returns the current playlist configuration.

---

### `POST /api/playlist`

Set the playlist contents.

```json
{
  "loop_all": true,
  "entries": [
    {"file_idx": 0, "loop_count": 3, "pause_ms": 500},
    {"file_idx": 1, "loop_count": 0, "pause_ms": 1000}
  ]
}
```

`loop_count = 0` means infinite loop for that entry.

---

### `POST /api/playlist/start`

Start playlist playback. Body: empty.

---

### `POST /api/playlist/stop`

Stop playlist playback. Body: empty.

---

### `POST /api/playlist/reload`

Reload playlist from the current configuration. Body: empty.

---

## Zone (Projection Clipping)

### `GET /api/zone`

Returns the current zone polygon and clipping state.

```json
{
  "enabled": false,
  "count": 4,
  "x": [-24000, 24000, 24000, -24000],
  "y": [-24000, -24000, 24000, 24000]
}
```

Coordinates are in galvo DAC units (−32768 to +32767).

---

### `POST /api/zone`

Set the zone polygon and persist to NVS.

```json
{
  "count": 4,
  "x": [-20000, 20000, 20000, -20000],
  "y": [-20000, -20000, 20000, 20000]
}
```

---

### `POST /api/zone/enable`

Enable or disable zone clipping without changing the polygon.

```json
{"enabled": true}
```

---

### `POST /api/zone/preview`

Project the zone boundary as a calibration pattern (outline only, laser draws the polygon edge).

Body: empty.

---

## Thermal & Fan

### `POST /api/fan-override`

Override fan duty cycles manually.

```json
{"fan1": 128, "fan2": 200}
```

Set `fan_auto: true` in `/api/safety/config` to return to automatic control.

---

### `POST /api/temp-thresholds`

Update temperature thresholds (same as `POST /api/safety/config`).

---

### `POST /api/temp/offset`

Set a calibration offset for a specific temperature sensor.

```json
{"sensor": 0, "offset_c": -1.5}
```

---

### `POST /api/temp/name`

Set a display name for a temperature sensor.

```json
{"sensor": 0, "name": "Laser Diode"}
```

---

## Timer

### `POST /api/timer/set`

Configure the countdown timer.

```json
{"hours": 0, "minutes": 5, "seconds": 0, "expire": "text", "text": "Time's up!"}
```

`expire`: `"none"`, `"text"`, or `"ilda"`. If `"ilda"`, add `"ilda_file_idx": N`.

---

### `POST /api/timer/start`

Start the countdown. Body: empty.

---

### `POST /api/timer/pause`

Pause or resume the countdown. Body: empty.

---

### `POST /api/timer/stop`

Stop and reset the countdown. Body: empty.

---

### `POST /api/timer/reset`

Reset the countdown to the configured time. Body: empty.

---

### `GET /api/timer/state`

Returns current timer state.

```json
{
  "running": false,
  "remaining_s": 300,
  "total_s": 300,
  "expired": false
}
```

---

## Wi-Fi

### `GET /api/wifi-scan`

Trigger a Wi-Fi network scan (background, ~3 seconds). Poll the same endpoint to get results.

---

### `GET /api/wifi-status`

Returns current Wi-Fi connection state, SSID, IP, RSSI.

---

### `POST /api/wifi-connect`

Connect to a Wi-Fi network. Does not restart; use `POST /api/config` + reboot for persistent changes.

```json
{"ssid": "MyNetwork", "password": "secret"}
```

---

## Log

### `GET /api/log`

Returns recent firmware log entries as a JSON array of `{ts, level, cat, msg}` objects. The log buffer is limited in size; oldest entries are discarded.

---

### `POST /api/log/clear`

Clear the log buffer. Body: empty.

---

### `GET /api/log/stats`

Returns log buffer statistics (total entries, dropped count, categories).

---

## Debug & Diagnostics

### `GET /api/debug/hw`

Returns hardware debug state (DAC gain snapshot values for debugging the gain live-update path).

---

### `POST /api/debug/hw`

Write DAC debug configuration. **Development use only.**

---

### `POST /api/debug/dac-cmd`

Send a raw low-level DAC8562 command register write. **Hardware-level access — use with caution.**

```json
{"cmd": 3, "channel": 0, "value": 32767}
```

---

### `POST /api/debug-mode`

Enable or disable No-HW mode (skips DAC/SPI at next boot).

```json
{"enabled": false}
```

---

### `POST /api/factory-reset`

Clear all NVS configuration and restart. **Irreversible.** Wi-Fi credentials are lost; device returns to AP mode.

Body: empty.

---

### `POST /api/ui-control`

Generic UI control endpoint used by the WebUI for actions not covered by other endpoints (e.g. smart defaults computation for the Optimizer tab).

```json
{"action": "smart_defaults", "profile": 0}
```
