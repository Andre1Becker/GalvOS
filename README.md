# GalvOS

**Open-source ESP32-S3 laser show controller — built to replace proprietary OEM controllers in RGB galvo laser projectors.**

GalvOS is a fully custom firmware and hardware project that replaces the original control board of a Mikoy 5W RGB laser projector with an ESP32-S3-based system featuring a 16-bit DAC, browser-based WebUI (PWA), DMX/Art-Net input, ILDA file playback, and hardware safety interlocks.

> Community contributions welcome :-)

---

## ⚠️ SAFETY WARNING — READ BEFORE PROCEEDING

**This project involves a  laser device.**

Lasers are capable of causing immediate and permanent eye injury and skin burns, and can ignite materials. This is the highest hazard class for laser products.

- **Never** look into the beam or at specular reflections.
- **Always** use appropriate laser safety eyewear (OD rating matched to wavelength and power).
- **Never** operate the laser without proper enclosure or beam stops in place.
- By modifying this device, the original CE marking and safety certification are **void**.
- **You are solely responsible** for the safe operation of any device built using this project.

**This project is intended for experienced makers and engineers who understand laser safety. It is NOT a beginner project.**

---

## Disclaimer

> THE HARDWARE DESIGNS, FIRMWARE, AND ALL OTHER MATERIALS IN THIS REPOSITORY ARE PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED.
>
> THE AUTHORS AND CONTRIBUTORS SHALL NOT BE LIABLE FOR ANY CLAIM, DAMAGES, OR OTHER LIABILITY — INCLUDING BUT NOT LIMITED TO PERSONAL INJURY, PROPERTY DAMAGE, OR LOSS OF DATA — ARISING FROM THE USE OR MISUSE OF THIS PROJECT.
>
> Modifying laser equipment may be illegal in your jurisdiction without appropriate licensing. Check your local laws before proceeding.
>
> This project is not affiliated with, endorsed by, or connected to Mikoy or any OEM laser manufacturer.

---

## Features

- **ESP32-S3** (WROOM-1 N16R8) — dual-core, Wi-Fi, BLE
- **16-bit DAC** (DAC8562) via SPI — high-resolution X/Y galvo control
- **Differential output stage** (OPA4134) for galvo drive (±5V)
- **DMX via MAX485
- **Art-Net node**
- **ILDA file playback** from SD card (FAT32, Class 10 UHS-I required)
- **Browser-based WebUI** (PWA, single-file LittleFS) — no app install needed
- **Hardware safety interlocks:**
  - NE555-based scan-fail detection
  - NE555-based hardware watchdog with SSR
  - Emergency stop (E-Stop) input
  - Optocoupler-isolated laser TTL (6N137)
  - Fail-safe pull-ups on laser GPIO pins
- **1-Wire temperature monitoring** (DS18B20)
- **Fan PWM control**
- **Incremental versioning** with changelog

---

## Hardware Overview

| Component | Part |
|---|---|
| MCU | ESP32-S3-WROOM-1 N16R8 (DevKitC-1) |
| DAC | DAC8562 (16-bit, dual, SPI) |
| Op-Amp | OPA4134UA (quad, SOIC14) |
| Galvo Set | Jolooyo JY-15K-BL (15 kpps, ±20° optical) |
| Laser Driver | MN-1M5AT (3-ch constant-current buck, 12V) |
| DMX | MAX485 module |
| Scan-Fail / Watchdog | NE555 × 2 |
| Temperature | DS18B20 (1-Wire) |
| Power | HW-613 buck converter (+5V); galvo PSU ±12.8V |
| Build | Perfboard 15×9 cm |

Full netlist and wiring diagrams are in the `/Hardware` directory.

---

## Repository Structure

```
GalvOS/
├── Firmware/          # PlatformIO project (ESP32-S3, Arduino framework)
├── Hardware/          # Netlists, KiCad schematics
├── Docs/              # Project documentation
├── README.md
├── LICENSE
└── CHANGELOG.md
```

---

## Getting Started

### Prerequisites
- [PlatformIO](https://platformio.org/) (VS Code extension recommended)
- ESP32-S3-DevKitC-1 board
- SD card: FAT32 formatted, **Class 10 UHS-I minimum** (Class 4 incompatible)

### Build & Flash
```bash
git clone https://github.com/YOUR_USERNAME/GalvOS.git
cd GalvOS/Firmware
pio run --target upload
pio run --target uploadfs   # upload WebUI to LittleFS
```

### WebUI
After flashing, connect to the ESP32 Wi-Fi AP or your local network and open the device IP in any browser. The WebUI is a PWA and can be installed on mobile home screens.

---

## Control Input

| Protocol | Details |
|---|---|
| DMX-512 | via XLR, MAX485, CH1 = Master Dimmer |
| Art-Net | UDP, universe configurable in WebUI |
| ILDA | .ild files from SD card, selectable in WebUI |

---

## License

- **Firmware & Software:** [GNU General Public License v3.0](LICENSE)
- **Hardware designs:** [CERN Open Hardware Licence v2 - Strongly Reciprocal (CERN-OHL-S)](https://ohwr.org/cern_ohl_s_v2.txt)

Contributions welcome under the same license terms.
