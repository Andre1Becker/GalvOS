# Chapter 4 — UI Guide

The GalvOS WebUI is a single-page application served directly from the ESP32's LittleFS flash. No internet connection required, no app to install — open a browser and go.

## Table of Contents
- [Accessing the WebUI](#accessing-the-webui)
- [Installing as a PWA](#installing-as-a-pwa)
- [General Layout](#general-layout)
- [Tab: Dashboard](#tab-dashboard)
- [Tab: Presets](#tab-presets)
- [Tab: DMX Live](#tab-dmx-live)
- [Tab: Text](#tab-text)
- [Tab: Paint](#tab-paint)
- [Tab: ILDA / SD](#tab-ilda--sd)
- [Tab: Calibration](#tab-calibration)
- [Tab: Optimizer](#tab-optimizer)
- [Tab: Projection](#tab-projection)
- [Tab: Playlist](#tab-playlist)
- [Tab: Thermal](#tab-thermal)
- [Tab: Log](#tab-log)
- [Tab: Configuration](#tab-configuration)

---

## Accessing the WebUI

On first boot, GalvOS starts in Wi-Fi Access Point mode:

- **SSID:** `galvOS`
- **Password:** none (open network)
- **IP address:** `192.168.4.1`

Open `http://192.168.4.1` in any browser. Once you configure a Wi-Fi network in the Configuration tab and restart, GalvOS connects to your network and is available at the assigned DHCP address — or at `http://galvOS.local` on networks that support mDNS.

> **Tip:** The IP address is always shown on the Dashboard tab under System → IP Address.

---

## Installing as a PWA

GalvOS ships as a Progressive Web App (PWA). This means you can install it on your device's home screen and launch it like a native app — no browser chrome, full screen, offline-capable UI.

**On Android (Chrome):**
1. Open the WebUI in Chrome.
2. Tap the three-dot menu → "Add to Home screen".
3. Confirm. The app icon appears on your home screen.

**On iOS (Safari):**
1. Open the WebUI in Safari.
2. Tap the Share button (box with arrow) → "Add to Home Screen".
3. Confirm.

**On Desktop (Chrome/Edge):**
1. Look for the install icon in the address bar (a small computer with a download arrow).
2. Click it and confirm.

---

## General Layout

The UI is divided into a **tab bar** at the top and a **content area** below. All tabs are accessible at any time — switching tabs does not stop the laser or change the active pattern.

<img width="1563" height="52" alt="image" src="https://github.com/user-attachments/assets/17b74a56-2283-45db-89fb-828af40485f2" />


---

## Tab: Dashboard

The Dashboard is the home screen and the first thing you see on load. It gives you a live status overview of the entire system.

<img width="1304" height="1118" alt="image" src="https://github.com/user-attachments/assets/5e84f797-35c8-4a98-b1e0-97ddb4a8adcc" />


### Safety Status Card

Shows the state of the hardware safety interlocks:

- **E-Stop** — green LED: E-Stop circuit is closed (not pressed), system can arm. Red: E-Stop is active, laser cannot arm.
- **Scan-Fail HW** — green LED: the NE555 scan-fail circuit is detecting DAC activity. Red: scan-fail triggered (galvo has stopped or firmware hung).
- **Fault reason** — if the system refused to arm, a text line appears here explaining which condition failed. This reads from the RTC memory value that survives restarts.
- **ARM / DISARM buttons** — ARM requests the safety system to enable the laser power rail (all hardware conditions must also be satisfied). DISARM immediately cuts the laser rail regardless of pattern state.

### Telemetry Card

Live readouts updated every second:

- **Source** — which control input is currently driving the output: `WEBUI`, `DMX`, `ARTNET`, `ILDA`, or `INTERNAL` (preset).
- **Master Dimmer** — effective master brightness (0–255), combining DMX CH1 and the WebUI override.
- **DMX Frames** — running count of DMX frames received. Useful to confirm DMX signal is arriving.
- **Galvo Rate** — current output rate in points-per-second with a visual bar. The bar fills relative to the configured `galvo_kpps` maximum.
- **Buffer fill level** — how full the DAC output ring buffer is. Sustained overflows cause flicker and are visible as "Ring buffer overflow" in the log.
- **Last DMX activity** — time since the last DMX frame arrived. Goes red if DMX signal is lost.

### CPU Load Graph

A scrolling 60-second graph of both core loads:

- **Core 0 (cyan)** — handles Wi-Fi, WebUI HTTP, Art-Net, DMX, safety. Typically 10–40% under normal use.
- **Core 1 (orange)** — runs the galvo ISR and pattern engine. Typically ~100% — this is normal and expected. The ISR is time-sliced.
- Warning lines at 70% (yellow dashed) and 90% (red dashed) mark potential overload on Core 0.

<img width="872" height="271" alt="image" src="https://github.com/user-attachments/assets/988a1e92-5a48-4759-8e04-07ae93b75604" />


### Temperature History Chart

A colour-coded scrolling chart of all DS18B20 sensor readings:

- 🔴 Laser diode module
- 🟠 Driver board
- 🟡 Galvo board
- 🟢 PSU
- 🔵 Ambient / chassis

Current temperatures are shown as a row of badges below the chart.

### System Card

Static system information: firmware version, hostname, IP address, Wi-Fi signal strength (RSSI), uptime, free heap (internal DRAM), free PSRAM, NTP time, DAC/galvo status, SD card status, and the current API auth token (click to copy).

---

## Tab: Presets

The Presets tab is the main performance control surface. It is split into two areas: **Global Controls** (always visible at the top) and the **Preset Grid** below.

<img width="1301" height="1160" alt="image" src="https://github.com/user-attachments/assets/dc8df1f4-79b4-4d41-8c01-b3532cc1a67c" />

### Global Controls

A 5-column card that applies to every active preset in real time. Changes take effect immediately without reloading the pattern.

<img width="1310" height="730" alt="image" src="https://github.com/user-attachments/assets/2659232d-327a-49f4-aaca-9511a75bcc4f" />

**Column 1 — Speed / Size / Rotation:**
- **Speed** — pattern animation speed (0–255). Meaning varies by preset: step increment, phase advance, or oscillation rate.
- **Speed Multiplier** — shown for some presets that support a secondary speed factor.
- **Size** — scales the pattern output (10–255). 255 = full scan range. Reduce to shrink the image.
- **Auto-Scaling speed** — oscillates size between 0 and the Size value at the set rate. Three modes: Small→Big→Small, Small→Big, Big→Small.
- **Rotation (Z)** — static Z-axis rotation offset (−180° to +180°).

**Column 2 — Auto-Rotation:**
- **Continuous rotation toggle** — enables continuously spinning rotation on any or all axes.
- **Z / Y / X axis speed** — independent speed for each rotation axis (0–100).
- **Master speed** — global multiplier applied on top of per-axis speeds.

**Column 3 — Color & Color Animations:**
- **Color Override toggle** — when on, the color picker overrides the preset's built-in color.
- **Color wheel** — click or drag to select hue and saturation. A vertical brightness slider is on the right.
- **Hex input** — type a hex color code directly (`ffc96e` etc.).
- **Quick color buttons** — one-tap access to R, G, B, Magenta, Yellow, Cyan, White.
- **Color Animations** — seven animation modes applied on top of any color override:
  - **Gradient** — smooth color cycle through a selected sequence. Choose a sequence (0–9) and set direction and speed.
  - **Chase** — one color at a time, cycling through a sequence.
  - **Strobe** — rapid on/off at the set speed in the selected color.
  - **Pulse** — sine-wave brightness oscillation in the selected color.
  - **Twinkle** — random brightness spikes; simulates a glitter/spark effect.
  - **Flip** — hard cuts between R, G, B, W at the set speed.
  - **Seg** — divides the pattern's points into segments, each painted a different color from the selected palette. Segment count and direction are adjustable.
- **⏹ Stop Animation** — stops any running color animation and returns to the last static color.
- **↺ Reset Colors** — clears any color override and returns to the preset's built-in color. Use this if colors appear washed out after a color animation.

**Column 4 — Points-Only Mode:**
Converts any preset into a dot-cloud: instead of drawing connected lines, the optimizer samples points from the pattern and dwells on each one as a lit dot.
- **Points-Only Mode toggle** — on/off.
- **Point count** — number of dots (2–80).
- **Fade-in / Fade-out** — enable smooth brightness ramp at each dot, with configurable duration (0–5000 ms).
- **Fade direction** — controls the order in which points fade: Inside→Outside, Outside→Inside, Left→Right, Right→Left, Top→Bottom, Bottom→Top.
- **Static Mode** — disables fading entirely; all dots at full brightness.

**Column 5 — Kaleidoscope & Mirror:**
- **Kaleidoscope** — replicates the pattern into N rotationally symmetric segments (2–16). Mirror H and Mirror V options alternate between original and mirrored copies of each segment.
- **Mirror** — simpler reflection: Off, ↔ X (horizontal flip), ↕ Y (vertical flip), ✳ Radial4 (4-fold copy without reflection).

### Preset Grid

The main preset library, fetched from `/api/presets` on tab load. Each preset is shown as a tile with an SVG thumbnail and name.

<img width="1304" height="258" alt="image" src="https://github.com/user-attachments/assets/4afebe03-7820-4669-a687-2322651f0d84" />


- **Category filters** — buttons above the grid filter by category (Geometry, Waves, 3D, Scenes, etc.). Click to toggle. Multiple categories can be active simultaneously.
- **Click a preset** — activates it immediately. The active preset name is shown in the Global Controls header.
- **⏹ Off** — stops the current preset (laser off).
- **↺ Reset all** — resets all Global Controls sliders to their defaults without changing the active preset.

### Waves Sub-Grid

A separate section for wave presets, with two additional parameters:
- **Amplitude** (0.1–2.0×) — scales the wave height.
- **Frequency ×** (0.25–4.0×) — scales the wave frequency.

### 3D Presets Sub-Grid

3D presets use the Auto-Rotation controls in Column 2 for movement. Rotation is not built into each preset — enable Auto-Rotation in Global Controls to spin them.

### Mathematical Curves

A panel for parametric mathematical curves (Lissajous, spirographs, epicycloids, etc.). Each curve exposes its own parameter sliders (up to 5 per curve), a color picker, and a reset button. Select a curve by clicking it; click **⏹ Off** to return to preset mode.

### Countdown Timer

A standalone utility embedded in the Presets tab. Set hours/minutes/seconds, then Start/Pause/Stop. On expiry: do nothing, show a text message (Text mode), or play an ILDA file.

---

## Tab: DMX Live

Provides a software DMX console — 25 sliders corresponding to GalvOS's 25 DMX channels.

<img width="1313" height="1016" alt="image" src="https://github.com/user-attachments/assets/8ca62261-cdfc-4d50-8f34-71b45346b862" />


- **WebUI override toggle** — when enabled, the slider values are sent directly to the pattern engine, overriding any incoming hardware DMX signal. When disabled, the sliders display the last received DMX values (read-only view).
- **Reset all channels** — returns all sliders to their off/default state.
- **Test: Red circle** / **Test: Rainbow** — quick preset buttons that set a combination of channels to show a test pattern.

Full channel map: see [Chapter 3 — Build & Configuration → RuntimeConfig → DMX/Art-Net](03-build-and-config.md).

---

## Tab: Text

Projects laser text. Text mode overrides any active preset and DMX input while active.

<img width="1304" height="481" alt="image" src="https://github.com/user-attachments/assets/f97eaf84-a390-4475-aa8a-0ad86cde2ad2" />

- **Text input** — supports uppercase A–Z, digits 0–9, and `.,:!?-+`. Maximum 127 characters. Up to 16 characters display statically; longer text scrolls automatically.
- **Font** — Simple (thin strokes, fastest), Bold (thick strokes), Outline (double-line).
- **Animation** — Static, Scroll Left/Right, Bounce, Typewriter, Wave, Pulse, Rotate, Zoom, Orbit, Star Wars Scroll. See [Known Issues](09-known-issues-and-todos.md) for animation bugs.
- **Live toggle** — when on, text updates are sent to the laser as you type. Turn off if you want to compose text before displaying it.
- **Speed / Size** — animation speed and text size.
- **Color / Rainbow** — fixed color via color picker, or rainbow cycling across characters.
- **Flip X / Flip Y** — mirror the text output horizontally or vertically.
- **▶ Show text / ⏹ Stop** — activate or deactivate text mode.

---

## Tab: Paint

A freehand drawing canvas that projects directly onto the laser.

<img width="1316" height="1022" alt="image" src="https://github.com/user-attachments/assets/dd4a19df-7960-4434-afc9-cc2dbc7c8945" />

- **Draw mode** — finger or mouse draws freehand strokes on the canvas.
- **Shape tools** — add rectangles, triangles, or circles as closed polygons.
- **Mirror brush** — mirror the current stroke across X, Y, or both axes while drawing.
- **Color picker** — set the color for the next stroke.
- **Clear** — erase all strokes.
- **Project** — sends the current canvas to the laser. The projector renders the strokes as a vector point cloud.

**Limitations:** The canvas is smaller than the full projection area (see [Known Issues](09-known-issues-and-todos.md)). Maximum 12 strokes, 96 vertices per stroke.

---

## Tab: ILDA / SD

ILDA file playback from an SD card.

<img width="1055" height="524" alt="image" src="https://github.com/user-attachments/assets/5fa75f64-acd0-4362-b1ea-95e8ef679064" />


> ⚠️ **Known issue:** SD card insertion currently causes galvo malfunction. See [Known Issues](09-known-issues-and-todos.md). This tab is non-functional until that issue is resolved.

- **File list** — lists `.ild` files found on the SD card (up to 40 files).
- **Playback controls** — select a file, set loop mode, and play/stop.
- **ILDA Speed / Size / Brightness** — override the ILDA file's built-in parameters.
- **SD card status** — shows card type, total size, free space, and file count.
- **⟳ Mount / ⏏ Eject** — remount or safely eject the SD card (Dashboard tab, System card).

---

## Tab: Calibration

The calibration workflow covers four areas: color/gamma calibration, galvo geometry calibration, projection zone setup, and the ILDA standard test pattern.

<img width="2504" height="1074" alt="image" src="https://github.com/user-attachments/assets/368000d7-7b14-4474-a666-080c26126583" />


### Color & Gamma Calibration (left card)

A list of calibration patterns to project while adjusting parameters. Select a pattern to activate it; press ⏹ Stop to return to normal operation. Pattern descriptions:

| Pattern | Purpose |
|---------|---------|
| White fill | Full-white output — for overall brightness assessment |
| Red / Green / Blue fill | Single-channel output — for per-channel threshold calibration |
| Three Circles | One solid R, G, B circle side by side — for white balance matching |
| Crosshair | Geometric reference for offset and gain calibration |
| Grid | Linearity check across the full scan range |
| DAC Range Box | Corner-to-corner rectangle at full DAC range — for DAC limit calibration |
| ILDA Test Pattern | Official ILDA 1995 test pattern — for galvo driver tuning |

### Galvo Calibration (right card)

Live geometry adjustments, sent to the hardware on every slider change:

- **X/Y Offset** — center the output image. Adjust until the crosshair pattern is centered on the projection surface.
- **X/Y Gain** — scale the image. The "Linked" button keeps X and Y gain equal (maintains aspect ratio).
- **Swap X/Y** — swap the X and Y galvo channels if they are wired in reverse.
- **Invert X / Invert Y** — mirror the output along either axis.
- **DAC limit min/max** — restrict the DAC output range to keep the OPA4134 output within the galvo's ±5V input rating. Default: 0x0666..0xF999.

Press **💾 Save calibration** to persist all values to NVS.

### Parameter Card (visibility threshold & white balance)

**Visibility threshold (Basiswert):**

Each laser diode has a minimum PWM duty below which it emits no visible light — the dead zone. The threshold calibration finds this minimum so GalvOS can map 0–100% logical brightness onto the actually visible range.

Procedure:
1. Press **▶ Start test beam** — a static low-level beam activates on each channel, bypassing gain, gamma, and master dimmer so only the threshold sliders control it.
2. For each color channel, lower the slider until that color just goes dark. The value at which it disappears is the threshold.
3. Press **💾 Save thresholds**.

**Color channel brightness matching (White Balance):**

1. Press **▶ Start the Three Circle Pattern** — three solid circles (R, G, B) appear side by side.
2. Adjust Gain R / G / B until all three circles appear equally bright to the eye.
3. Alternatively, press **🪄 Auto White Balance** — the firmware calculates gains from the configured laser power values and applies them automatically.
4. Press **💾 Save calibration**.

**CIE 1931 Gamma:**
Toggles perceptual brightness correction. When enabled (default), the firmware applies a γ≈2.2 transfer curve so equal numerical steps in brightness look equal to human perception. Disable only if you need linear 0–255 output for a specific application.

### Projection Zone

An interactive canvas for defining a clipping polygon — the area the laser is allowed to scan. Lit points outside the polygon are blanked (laser off, mirror position retained).

<img width="620" height="977" alt="image" src="https://github.com/user-attachments/assets/940cb0d2-f7c0-4804-afe7-5ef76690e92a" />


- **Drag vertices** to shape the zone.
- **Tap an edge** to add a new vertex.
- **Double-tap a vertex** to remove it (minimum 3 vertices).
- **Project Outline** — projects the zone boundary onto the screen so you can verify it matches your safe scan area.
- **⬛ Enable Clipping** — activates zone clipping. Only enable after verifying the outline is correct.
- **Save Zone / Reset to Rectangle** — save or discard changes.

### ILDA Standard Test Pattern

The official 1995 ILDA Technical Committee test pattern for galvo driver calibration.

<img width="1257" height="273" alt="image" src="https://github.com/user-attachments/assets/551db37a-f992-46a3-aec2-193603163664" />


Use this pattern to tune the galvo driver board's damping and servo gain trim pots. The tab includes a step-by-step tuning procedure:

1. Set scan size — reduce until the circle begins to distort, then back 10%.
2. Verify axis polarity — "Y" label at top, "X" label at right. If reversed, use Invert X/Y.
3. Adjust Y damping until a small overshoot appears at corners (≈2/3 of reference marker height), then minimize it with high-frequency damping.
4. Adjust Y servo gain until the circle's top/bottom touches the inner square edges.
5. Repeat for X axis.
6. Verify DC offset — the pattern elements should be symmetric around the center.
7. Verify blanking — dashed lines in the pattern should be clean and dark.

> **Known issue:** The ILDA test pattern currently has incorrect output. See [Known Issues](09-known-issues-and-todos.md).

---

## Tab: Optimizer

Per-preset-class optimizer profile management. See [Chapter 5 — The Optimizer](05-optimizer.md) for a full explanation of what each parameter does.

<img width="2508" height="1084" alt="image" src="https://github.com/user-attachments/assets/534e0bcc-c2ad-4c37-aa22-41e773d20d50" />


### Profile Selector

Six optimizer profiles, one per preset class:

| Profile | Preset class | Scanner workload |
|---------|-------------|-----------------|
| Vector | Closed polygons, straight runs | Corner dwell |
| Smooth | Continuous closed curves | Interior density |
| Waves | Open polylines, high frequency | Velocity clamp |
| Wireframe | 3D edge chains | Corner dwell + short jumps |
| MultiObject | Several closed objects | Long blank jumps |
| Particles | Isolated dots | Blank jumps only |

The active profile switches automatically when a preset is activated. You can also select a profile manually to edit it.

### Smart Defaults Button

**⚙ Smart defaults** — computes recommended parameter values from `opt_max_pts_per_frame` and `galvo_kpps` and applies them to the current profile. A good starting point after changing galvo hardware or kpps setting.

### Parameter Sliders

All sliders update the active profile live and show their **effective values** (`opt_eff_*`) after PPS scaling. The effective values are what the optimizer actually uses — they may differ from the raw slider values when `galvo_kpps` differs from `galvo_rated_kpps`.

See [Chapter 5 — The Optimizer → Parameter Reference](05-optimizer.md#parameter-reference) for a full table of all parameters.

---

## Tab: Projection

Hardware configuration for the galvo scanner and laser module.

<img width="1311" height="1109" alt="image" src="https://github.com/user-attachments/assets/396dcec9-74e1-4c56-8e8f-acdc333a6e62" />


### Galvo Sample Rate Card

- **Galvo rated speed** — set this to your galvo's datasheet kpps rating (Jolooyo JY-15K-BL = 15 kpps). This is the reference for PPS scaling in the optimizer.
- **Output Sample Rate slider** — `galvo_kpps`: the actual ISR tick rate (12–60 kpps). **This is the most important hardware parameter.** Start at your galvo's rated speed and only increase if the hardware handles it without distortion.
- Warning box — appears if the selected rate exceeds what is safe for the configured scan angle.
- **Autotune** — binary search for the highest kpps that avoids ring buffer overflow. Start a real pattern before running this for a meaningful result.
- **Period readout** — shows the calculated µs per DAC sample at the current rate.
- **💾 Apply & Save** — writes kpps and rated_kpps to NVS.

### Angular Configuration Card

Physical angles of the galvo and housing setup — used for the projection geometry calculator and safety assessment:
- **Mechanical Half-Angle** — galvo mirror maximum deflection (±°).
- **Housing Exit Half-Angle** — actual beam exit limit (typically smaller than galvo limit).
- **ILDA Rating Half-Angle** — standard ±8°; only change if your galvo's datasheet specifies a different rating angle.

### Laser Module Power Card

Laser power in mW per channel. Used for white balance auto-calculation and the safety assessment display. Totals shown:
- **Total power** (all channels)
- **Visible power V(λ)** — weighted by eye sensitivity
- **Blue-light hazard B(λ)** — the blue channel (445 nm) has the highest photochemical retinal risk

### Projection Geometry Card

Enter throw distance (0.5–30 m) to calculate projected image dimensions, area, peak irradiance, and points-per-frame budget at 30 fps.

### Safety Assessment Card

A simplified laser hazard summary based on configured power and angles: laser class, total/visible/BLH power, estimated minimum audience distance, NE555 scan-fail status, and rate-vs-angle adequacy.

---

## Tab: Playlist

Build and manage playlists of ILDA files for automated sequential playback.

> ⚠️ Requires SD card — see [Known Issues](09-known-issues-and-todos.md) for the SD card / galvo bug.

- Add ILDA files to the playlist, set loop count and pause duration per entry.
- Loop All toggle — loops the entire playlist indefinitely.
- Play / Stop controls.

---

## Tab: Thermal

Fan and temperature management.

[SCREENSHOT: Thermal tab]

- **Temperature thresholds** — configure warn, reduce, and shutdown temperatures (`temp_warn_c`, `temp_reduce_c`, `temp_shutdown_c`).
- **Fan control** — Auto mode (temperature-driven) or manual PWM override per fan.
- **Minimum fan speed** — prevents fans from stalling at low PWM (`fan_min_pct`, default 15%).
- Current sensor readings are also visible in the Dashboard temperature chart.

---

## Tab: Log

Live firmware log output, streamed from the ESP32 over the WebSocket. Auto-refreshes only when this tab is active.

[SCREENSHOT: Log tab]

- Log entries are colour-coded by severity: INFO (dim), WARN (orange), ERROR (red).
- Use this tab to diagnose startup issues, track DMX frame counts, or watch for ring buffer overflow warnings.
- The log buffer is limited in size — older entries are overwritten.

---

## Tab: Configuration

Network, DMX, safety, IP, and debug settings.

[SCREENSHOT: Configuration tab]

### DMX and Art-Net
- **DMX Start Address** (1–512) — first DMX channel GalvOS responds to.
- **Art-Net Universe** (0–32767) — Art-Net universe number.

### WiFi Connection
- Scan for available networks or enter SSID manually.
- Enter password. Hostname (default `galvOS`) sets the mDNS name.
- **NTP Server / Timezone** — time synchronisation settings. Use POSIX TZ string format.
- **Connect** — connects without restarting. **Save & Restart** — saves and reboots.

### IP Configuration
- Toggle between DHCP and static IP.
- Enter static IP, gateway, subnet mask, and DNS when static mode is enabled.

### Safety Configuration
- **Safety Override** — bypasses E-Stop and scan-fail checks. Only use with the laser disarmed and no audience. Red warning state clearly shown.
- **⚠ Factory Reset** — clears all NVS config and restarts. Wi-Fi credentials are lost; AP mode restarts.

### Debug
- **No-HW Mode** — skips SPI/DAC init at boot. Use only for firmware development without hardware connected. Disable before normal operation.
- **DAC Debug Log** — logs DAC8562 register writes to serial and the Log tab (rate-limited). For low-level DAC debugging only.
- **OTA Update** — firmware update via HTTP at `http://laser/update` (admin / your password).
