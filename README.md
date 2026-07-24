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
| [06 — Camera-in-the-Loop Auto-Tuning](docs/06-camera-autotuning.md) | Automated optimizer tuning via `scripts/optimizeGalvo/optimizeGalvo.py` and a USB camera |
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
