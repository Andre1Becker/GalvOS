# Mikoy 5W RGB Laser -- ESP32-S3 Replacement (WebUI Edition)

Custom firmware for the ESP32-S3 as a parallel controller to the LF-092 mainboard.
This build focuses on a **complete WebUI** with live control, preview,
calibration, and password-protected actions.

## Features

- **Real dimming on DMX CH1** -- fix for the OEM bug
- **WebUI with five tabs:**
  - Dashboard -- safety status, telemetry, ARM/DISARM
  - DMX Live -- 16 sliders with override mode, immediate effect
  - Preview -- canvas render of the galvo output (30 fps via WebSocket)
  - Calibration -- XY offset, gain, swap/invert, color balance, test patterns
  - Configuration -- DMX address, hostname, WiFi, password
- **WebSocket streaming** for state (10 Hz) and preview (30 Hz binary)
- **SHA-256 auth** for safety-critical actions
- **Fully offline-capable** -- HTML/CSS/JS from LittleFS, no external resources
- **DMX, Art-Net, Ether Dream** in parallel as input sources
- **Hardware safety** with watchdog, interlock, key, E-stop, scan-fail

## Build & Flash

```bash
# 1. Build and flash firmware
pio run --target upload

# 2. Flash WebUI files (HTML/CSS/JS) to LittleFS
pio run --target uploadfs

# 3. Serial console
pio device monitor
```

**Important:** both steps are required -- firmware alone shows no WebUI,
because the HTML lives in LittleFS.

## First Commissioning

1. **Boot via USB:** serial console shows init logs
2. **WiFi setup:** if no WiFi credentials are stored, a SoftAP named
   `Laser-Setup` starts with password `laser1234`. Connect to the ESP and
   open `http://192.168.4.1/` in your browser
3. **Default password:** `laser` (enter in the modal when ARM/DISARM/override is requested)
4. **Change password immediately:** tab *Configuration* -> *WebUI Security*
5. **Configure WiFi:** tab *Configuration* -> *Network* -> enter SSID/pass and save
6. **Hostname (mDNS):** after reboot reachable at `http://laser-greven.local/`

## WebUI Operation

### Dashboard
All status LEDs must be green (interlock closed, key turned, E-stop not
pressed, scan-fail OK) before ARM works. If not, an italic error message
shows which condition is missing.

### DMX Live
The override toggle enables the mode. All 16 sliders are live -- movement
affects the laser at a max 30 Hz update rate. Quick presets below for fast
tests. Disabling override immediately returns to the external DMX/Art-Net input.

### Preview
Live display of the currently output points. Black = blanking, color = beam
on. The FPS indicator below shows the WebSocket transmission rate, not the
galvo sample rate. 30 fps is the target.

### Calibration
**Workflow for first commissioning:**
1. Click *Center Point* -- laser must appear in the center
2. If not, adjust X/Y offset until centered
3. Click *Square* -- should be right-angled, size similar to the enclosure aperture
4. If distorted: adjust X/Y gain
5. If wrong direction: toggle Invert X/Y or Swap XY
6. Click *ILDA Test Pattern* -- frame + circle must be concentric
7. Click **Save** -- otherwise everything is lost on restart

**Color balance:**
1. Set CH2 to 0 (white) via DMX override
2. Dim down the maximum-power diode (often blue)
3. Leave the weakest diode (often red) at 255
4. Adjust the middle diode (green) so white looks white (no tint)

### Configuration
The DMX address takes effect immediately without restart. Network changes
require a reboot, which is triggered automatically after saving.

## API Endpoints

See the `src/net/web_ui.h` header comment for all relevant endpoints.
Quick reference:

| Method | Path | Auth | Purpose |
|---|---|---|---|
| GET | `/` | -- | SPA |
| GET | `/api/config` | -- | read configuration |
| POST | `/api/config` | yes | partially update configuration |
| POST | `/api/arm` | yes | "1"/"0" body -- ARM/DISARM |
| POST | `/api/dmx-override` | yes | `{values:[16 bytes]}` |
| POST | `/api/override-mode` | yes | `{active:bool}` |
| POST | `/api/calib-live` | yes | live calibration (RAM only) |
| POST | `/api/calib-save` | yes | persist calibration to NVS |
| POST | `/api/test-pattern` | yes | `{pattern:"center\|cross\|square\|circle\|ilda"}` |
| POST | `/api/set-password` | yes | `{hash:"sha256hex"}` |
| GET | `/api/auth-check` | -- | checks `X-Auth` header |
| POST | `/api/reboot` | yes | restart |
| POST | `/api/factory-reset` | yes | erase NVS, reboot |
| WS | `/ws` | -- | state+preview streaming |

Auth via header `X-Auth: <sha256-hex-of-password>`.

## Architecture

```
                            +-----------------+
                            |   Browser (SPA) |
                            +--------+--------+
                                     | HTTPS/WS
                                     v
  +----------------------------------------------------------+
  |  ESP32-S3 -- Core 0                                       |
  |  +----------+  +----------+  +--------+  +------------+   |
  |  |  WebUI   |  |  Safety  |  | DMX-RX |  | Art-Net /  |   |
  |  | HTTP+WS  |  | Watchdog |  | MAX485 |  | Ether Dream|   |
  |  +----+-----+  +----+-----+  +---+----+  +-----+------+   |
  |       |             |            |             |         |
  |       +-------------+------------+-------------+         |
  |                       |                                  |
  |  Core 1 --------------v---------------                   |
  |  +--------------------------------+                      |
  |  | Pattern Engine                 |                      |
  |  |  - DMX/Override -> LaserPoints |                      |
  |  |  - apply calibration           |                      |
  |  |  - publishPreviewFrame()       |                      |
  |  +------------+-------------------+                       |
  |               v                                          |
  |  +--------------------------------+                      |
  |  | Galvo Timer ISR (50 kHz)       |                      |
  |  |  -> SPI to DAC8562 (XY)        |                      |
  |  |  -> GPIO TTL via 6N137 (RGB)   |                      |
  |  +--------------------------------+                      |
  +----------------------------------------------------------+
```

## Safety

This is a DIY project for a **Class-4 laser**. WebUI auth only protects
against accidental clicks on the local network -- real safety comes from the
hardware chain:

- `PIN_LASER_ENABLE` switches an SSR/relay that disconnects the laser power rail
- Software alone CANNOT turn on the laser if the interlock/key are not
  physically released
- Watchdog pulse on a dedicated pin -> external NE555 circuit kills power
  if the firmware hangs

Never aim toward people or an audience; wear protective goggles during adjustment.

## Roadmap

- [x] WebUI with DMX Live, preview, calibration, auth
- [x] Real dimming
- [x] DMX + Art-Net + Ether Dream in parallel
- [x] Hardware safety aggregation
- [ ] **Drive the 7-segment display of the original enclosure** (instead of OLED)
- [ ] Helios USB vendor class
- [ ] Play ILD files from LittleFS
- [ ] OTA updates via WebUI
- [ ] BLE Light-Elf compatibility

## License

MIT
