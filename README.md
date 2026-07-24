<img width="1855" height="430" alt="image" src="https://github.com/user-attachments/assets/27f2adcc-0e56-4d4b-a446-699b2150028c" />

# GalvOS

> **Open-source ESP32-S3 laser show controller — built to replace proprietary OEM controllers in RGB galvo laser projectors.**

[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](LICENSE)
[![Hardware License: CERN-OHL-S](https://img.shields.io/badge/HW_License-CERN--OHL--S-orange.svg)](https://ohwr.org/cern_ohl_s_v2.txt)
[![Platform: ESP32-S3](https://img.shields.io/badge/Platform-ESP32--S3-red.svg)](https://www.espressif.com/en/products/socs/esp32-s3)
[![Framework: Arduino](https://img.shields.io/badge/Framework-Arduino-teal.svg)](https://www.arduino.cc/)

---

## ⚠️ SAFETY WARNING — READ BEFORE PROCEEDING

(I know - BORING! But hey - at least you read this far. So keep going!)

**This project involves CLASS IIIB or CLASS IV laser devices.**

Lasers at these power levels are capable of causing **immediate and permanent eye injury** and skin burns, and can ignite materials at close range.

- **Never** look into the beam or at specular (mirror-like) reflections.
- **Always** use appropriate laser safety eyewear — OD rating matched to your specific wavelengths and power level.
- **Never** operate the laser without proper enclosure or beam stops in place.
- By modifying this device, the original CE marking and safety certification are **void**.
- **You are solely responsible** for the safe operation of any device built using this project.
- Check your local laws — modifying laser equipment may require licensing in your jurisdiction.

> 🔴 **This project is intended for experienced makers and engineers who understand laser safety. Treat every beam as dangerous until proven otherwise.**

---

## What Is GalvOS?

GalvOS replaces the original control board of an RGB galvo laser projector (specifically the Mikoy 5W) with an ESP32-S3-based system. The result is a firmware platform that is more capable, fully open, and frankly more fun to work with than whatever the OEM shipped.

The origin story: the stock firmware couldn't dim the laser — it was full-brightness or nothing. What started as a quick bug investigation turned into a complete hardware and firmware replacement. Classic "Well, that escalated quickly..." moment.

**Key capabilities:**

- 16-bit galvo DAC (vs. OEM 12-bit) via SPI DAC8562
- Browser-based WebUI — no app install, works from any phone or desktop
- DMX-512 and Art-Net input
- ILDA file playback from SD card
- Per-point laser modulation with full RGB PWM control
- Hardware safety interlocks (scan-fail detection, watchdog, E-Stop, optoisolated TTL)
- Point optimizer pipeline with adaptive density, S-curve blanking, and ringing compensation
- Camera-in-the-loop auto-tuning of optimizer parameters via a companion Python tool (see [Chapter 6](docs/06-camera-autotuning.md))
- Temperature monitoring (up to 5× DS18B20 sensors)

---

## Full Feature List

Yes, all of this is real, and yes, it's all running on a $6 microcontroller.

### Output & Rendering

| Feature | What it does |
| --- | --- |
| 16-bit galvo DAC (DAC8562) | 4× the resolution of the OEM's 12-bit — lines that were jagged are now just... lines. |
| 9-stage point optimizer pipeline | Turns "draw a hexagon" into a stream the mirrors can physically survive — corner dwell, blanking, velocity/acceleration clamp, ZV ringing compensation. |
| 6 optimizer profiles (Vector, Smooth, Waves, Wireframe, MultiObject, Particles) | One-size-fits-all optimizer settings don't exist, so pick per preset class instead. Auto-switches with the active preset. |
| Smart Defaults button | Computes sane optimizer parameters from your kpps and frame budget. For when guessing sliders gets old. |
| Adjustable galvo sample rate (12–60 kpps) with Autotune | Binary-searches the highest rate your hardware handles before it starts buffering complaints. |
| CIE 1931 gamma correction | γ≈2.2 so "50% brightness" actually looks like 50% brightness to your eyeballs, not to a linear sensor. |
| Projection zone clipping | Draw a polygon, laser respects it. Points outside get blanked instead of redecorating your neighbor's wall. |

### Patterns & Effects

| Feature | What it does |
| --- | --- |
| Built-in preset library (Geometry, Waves, 3D, Scenes, math curves) | A whole gallery of shapes so you don't have to parametrize a Lissajous curve at 2 AM. |
| Mathematical curve engine | Lissajous, spirographs, epicycloids — up to 5 live parameter sliders each. |
| Auto-rotation (independent X/Y/Z) + static rotation offset | Spin any pattern on any axis, at its own speed, because static geometry is for cowards. |
| 7 color animation modes (Gradient, Chase, Strobe, Pulse, Twinkle, Flip, Seg) | Layer a light show on top of any preset's own color without touching its code. |
| Points-Only Mode | Turns any pattern into a dot cloud with configurable fade-in/out and fade direction — instant particle show. |
| Kaleidoscope (2–16-fold) & Mirror | Symmetric multiplication of whatever's on screen, because one hexagon is never enough. |
| Freehand Paint tab | Draw with your finger or mouse, project it as vectors. Shape tools included for people who can't draw circles. |
| Laser Text mode | 3 fonts, 10 animations (scroll, bounce, typewriter, Star Wars crawl, ...), up to 127 characters. |
| Countdown timer with laser payoff | Set a timer, and when it hits zero: show text or fire off an ILDA file. Genuinely useful for events. |

### Control Inputs

| Feature | What it does |
| --- | --- |
| DMX-512 input (MAX485, 25 channels) | Talks to any real lighting desk like a grown-up fixture. |
| Art-Net input | DMX over Ethernet/Wi-Fi for the desk-less crowd. |
| Live software DMX console (WebUI) | 25 sliders, no physical desk required, with instant test patterns (red circle, rainbow). |
| WebUI override priority | WebUI wins over DMX on demand — for when you need to grab manual control mid-show. |
| Master dimmer (WebUI + DMX CH1 combined) | One dial to rule the overall brightness, regardless of source. |

### Storage & Playback

| Feature | What it does |
| --- | --- |
| ILDA file playback from SD card | Plays the industry-standard laser show format straight off a memory card. |
| Playlist manager | Queue multiple ILDA files with per-entry loop count and pause duration. |
| Independent SPI3 bus for SD | SD reads no longer corrupt the DAC output mid-frame (see [Known Issues](docs/10-known-issues-and-todos.md) for the physical rewire still pending). |

### Calibration & Tuning

| Feature | What it does |
| --- | --- |
| Galvo geometry calibration | Offset, gain (linked X/Y), swap, invert — dial in a perfectly centered, correctly proportioned image. |
| Color/gamma calibration patterns | White fill, per-channel fill, three-circle white balance, crosshair, grid, DAC range box, official ILDA test pattern. |
| Visibility threshold calibration | Finds each laser diode's "dead zone" so 0–100% brightness maps to what's actually visible instead of a chunk of invisible PWM range. |
| Auto White Balance | Calculates per-channel gain from configured laser power so R/G/B actually look equally bright. |
| Camera-in-the-loop auto-tuning | A USB camera + Optuna search auto-tunes optimizer profiles against measured beam quality — no more "nudge a slider, squint at the wall" loop. See [Chapter 6](docs/06-camera-autotuning.md). |

### Safety & Reliability

| Feature | What it does |
| --- | --- |
| Hardware E-Stop input | A physical kill switch the firmware cannot override. |
| NE555 scan-fail detection | If the galvo stops moving (or firmware hangs), the laser cuts — hardware-level, no software in the loop. |
| NE555 hardware watchdog | Independent of the ESP32's own watchdog; catches the case where the ESP32 itself locks up. |
| Fail-safe optoisolated RGB TTL | 10kΩ pull-ups keep every laser channel OFF by default on boot, reset, panic, or brownout. |
| OTA update lockout while armed | Can't push new firmware to a live, armed laser. On purpose. |
| Safety Assessment card | Live laser-class and audience-distance estimate from your configured power and beam angles. |
| Thermal protection | Up to 5× DS18B20 sensors, configurable warn/reduce/shutdown thresholds, auto or manual fan control. |

### Connectivity & UI

| Feature | What it does |
| --- | --- |
| Browser-based WebUI, installable as a PWA | No app store, no native install — works full-screen from any phone, tablet, or desktop. |
| Live Dashboard | Safety status, telemetry, CPU load, temperature history, DAC output rate, and frame composition — all scrolling in real time. |
| Live log console + memory viewer | Streamed over WebSocket, colour-coded by severity, plus a heap/PSRAM breakdown by subsystem for hunting leaks. |
| Wi-Fi AP + STA mode, mDNS | Boots as its own access point out of the box; joins your network and answers at `galvOS.local` once configured. |
| Static IP / DHCP configuration | For the network nerds who don't trust DHCP leases. |
| REST API with token auth | Full external control surface — see [API Reference](docs/08-api-reference.md). |
| Factory reset | Nukes all NVS config back to defaults when you've fat-fingered one setting too many. |

---

## Quick Start

Feeling yolo? Take this route then:

1. **Read the [Safety section](docs/01-introduction.md#safety)** — seriously.
2. **Check [Prerequisites](docs/02-prerequisites.md)** — tools and hardware needed.
3. **[Build & Flash](docs/03-build-and-config.md)** — PlatformIO build, firmware + WebUI upload.
4. **[Connect to WebUI](docs/04-ui-guide.md)** — Wi-Fi, browser, done.

---

## Documentation

Need a good sleep aid? This is the one for you here:

| Chapter | Description |
| --- | --- |
| [01 — Introduction](docs/01-introduction.md) | Project background, safety, disclaimer, hardware overview |
| [02 — Prerequisites](docs/02-prerequisites.md) | Tools, hardware, software, and skills needed |
| [03 — Build & Configuration](docs/03-build-and-config.md) | PlatformIO setup, all configurable parameters, flash instructions |
| [04 — UI Guide](docs/04-ui-guide.md) | Complete walkthrough of all WebUI tabs and controls |
| [05 — The Optimizer](docs/05-optimizer.md) | Deep-dive into the point optimizer pipeline |
| [06 — Auto-Tuning (CAM-based)](docs/06-camera-autotuning.md) | Automated optimizer tuning via `scripts/optimizeGalvo/optimizeGalvo.py` and a USB camera |
| [07 — Troubleshooting](docs/07-troubleshooting.md) | Common problems and how to solve them |
| [08 — API Reference](docs/08-api-reference.md) | REST API endpoints for integration and automation |
| [09 — Contributing](docs/09-contributing.md) | How to contribute, code style, patch workflow |
| [10 — Known Issues & Todos](docs/10-known-issues-and-todos.md) | Open bugs, missing features, planned work |
| [11 — Glossary & Terminology](docs/11-glossary.md) | All abbreviations and technical terms explained |

---

## Hardware at a Glance

| Component | Part |
| --- | --- |
| MCU | ESP32-S3-WROOM-1 N16R8 (16 MB Flash, 8 MB OPI PSRAM) |
| DAC | DAC8562 (16-bit, dual-channel, SPI) |
| Output Stage | OPA4134UA quad op-amp — differential ±5V galvo drive |
| Galvo Set | Jolooyo JY-15K-BL (15 kpps, ±20° optical) |
| Laser Driver | MN-1M5AT (3-channel constant-current buck, active-HIGH) |
| DMX | MAX485 module (receive-only, XLR) |
| Safety | NE555 scan-fail + NE555 hardware watchdog, 6N137 optocouplers |
| Temperature | DS18B20 × up to 5, 1-Wire bus |
| Power | HW-613 buck converter (+5V rail); external ±15V galvo PSU |
| Construction | Perfboard 15 × 9 cm; Gerber File for custom PCB is in progress |

Full netlist: [`hardware/`](hardware/)

---

## Repository Structure

```text
GalvOS/
├── src/                    # Firmware source (C++, Arduino/PlatformIO)
│   ├── control/            # DMX, Art-Net, safety control
│   ├── ilda/               # ILDA file parser
│   ├── net/                # WebUI server, REST API
│   ├── output/             # Galvo ISR, DAC output, RGB PWM
│   ├── patterns/           # Pattern engine, presets, optimizer
│   ├── safety/             # E-Stop, watchdog, scan-fail
│   ├── sensors/            # DS18B20, temperature monitoring
│   ├── storage/            # NVS config persistence
│   └── util/               # Shared utilities
├── include/
│   ├── config.h            # Runtime config struct, optimizer defaults
│   └── pinmap.h            # All GPIO assignments
├── data/
│   └── index.html          # WebUI (single-file PWA, served via LittleFS)
├── hardware/               # Netlist, wiring diagrams
├── scripts/
│   ├── upload_all.py       # PlatformIO target: flash firmware + LittleFS
│   ├── gzip_assets.py      # Pre-build hook: gzip data/ assets
│   └── optimizeGalvo/      # Camera-in-the-loop auto-tuning tool (see docs/06)
├── docs/                   # Full documentation (you are here)
├── assets/                 # Screenshots, diagrams
├── platformio.ini          # Build configuration
└── partitions.csv          # Flash partition table
```

---

## License

- **Firmware & Software:** [GNU General Public License v3.0](LICENSE)
- **Hardware Designs:** [CERN Open Hardware Licence v2 — Strongly Reciprocal (CERN-OHL-S)](https://ohwr.org/cern_ohl_s_v2.txt)

Contributions welcome under the same license terms. See [Contributing](docs/09-contributing.md).
