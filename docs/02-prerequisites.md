# Chapter 2 — Prerequisites

## Table of Contents
- [Who Is This For?](#who-is-this-for)
- [Knowledge Requirements](#knowledge-requirements)
- [Required Hardware](#required-hardware)
- [Required Software & Tools](#required-software--tools)
- [The Build Environment](#the-build-environment)
- [Before You Begin Checklist](#before-you-begin-checklist)

---

## Who Is This For?

GalvOS is aimed at experienced makers who are comfortable with electronics and embedded systems. That said, the documentation is written to be accessible — if you understand how a GPIO works and you're not afraid of a soldering iron, you have a reasonable shot at building this.

The one area where there is **no substitute for experience** is laser safety. Working with Class IIIB/IV lasers requires genuine understanding of the hazards, not just a reading of the safety section. If this is your first encounter with high-power lasers, spend time with the relevant safety standards (IEC 60825-1, or your national equivalent) before you start.

---

## Knowledge Requirements

You will have a much better time if you come in with:

**Essential:**
- Basic electronics: reading schematics, soldering, understanding voltage dividers and resistor networks
- Microcontroller basics: you have flashed firmware to an ESP32 or similar before
- Comfort with a terminal / command line

**Very helpful:**
- C++/Arduino framework — you don't need to rewrite the firmware, but being able to read and modify it is valuable
- PlatformIO basics — the build system used for this project
- Some familiarity with the ESP32 ecosystem (Wi-Fi setup, NVS, FreeRTOS at a surface level)

**Nice to have:**
- Basic understanding of galvo laser systems — how galvo mirrors work, what "kpps" means, why blanking matters
- DMX-512 protocol fundamentals, if you intend to use DMX control
- HTTP/REST API basics, if you want to integrate GalvOS with other systems

If you're missing some of the "helpful" items, the documentation will explain what you need to know in context. The "essential" items are assumed throughout.

---

## Required Hardware

### The Laser Projector

GalvOS was developed for and tested on the **Mikoy 5W RGB laser projector**. The key components extracted from it (or sourced separately) are:

| Component | Specification | Notes |
|-----------|--------------|-------|
| Galvo Set | Jolooyo JY-15K-BL | 15 kpps, ±20° optical, OEM driver board included |
| Laser Module | MN-1M5AT | 3-channel, R: 638 nm ~1W, G: 520 nm ~1W, B: 445 nm ~3W |
| Galvo PSU | ±12.8V (measured) | Provides ±15V rail to OPA4134 via the OEM galvo driver |
| Housing | — | Reuse the original housing and beam path |

Other galvo sets and laser drivers may work, but will require tuning of the optimizer parameters (especially `galvo_rated_kpps`) and potentially adjustments to the analog output stage gain.

### The Replacement Controller Board

You will need to source and build:

| Component | Part Number | Quantity | Notes |
|-----------|------------|---------|-------|
| MCU | ESP32-S3-DevKitC-1 (N16R8) | 1 | The N16R8 variant specifically — 16 MB Flash, 8 MB OPI PSRAM |
| DAC | DAC8562 | 1 | 16-bit dual SPI DAC, available as MSOP-10 |
| Op-Amp | OPA4134UA | 1 | Quad op-amp, SOIC-14 |
| Optocouplers | 6N137 | 3 | High-speed, 1× per laser color channel |
| DMX Receiver | MAX485 module | 1 | Pre-built module recommended |
| Watchdog Timers | NE555 | 2 | Standard PDIP or SOIC |
| Temperature Sensors | DS18B20 | Up to 5 | 1-Wire, waterproof probe form factor recommended |
| Buck Converter | HW-613 or similar | 1 | Input from laser PSU (5–12V), output +5V |
| Perfboard | 15 × 9 cm (minimum) | 1 | Or custom PCB if you prefer |

**Passive components** (full list in the netlist):
- Resistors: 220Ω × 3, 1kΩ × 8, 4.7kΩ × 1, 10kΩ × 9, 22kΩ × 4, 100kΩ × 1, 50kΩ trimmer × 1, 100Ω × 5
- Capacitors: 100µF electrolytic × several, 10µF electrolytic × several, 100nF ceramic × 11, 1nF ceramic × 2
- Schottky diode: 1N5819 × 1 (reverse polarity protection)

The complete netlist with all values and wiring is in [`hardware/`](../hardware/).

### Test & Measurement Equipment

You will need, at minimum:
- **Multimeter** — for verifying supply rails and basic continuity checks
- **USB-to-serial adapter** — for the serial console (though the ESP32-S3 DevKitC-1 has USB CDC built in)

Strongly recommended:
- **Oscilloscope** — invaluable for checking DAC output waveforms, galvo drive signals, and tuning the ringing compensation
- **Laser power meter** — for calibrating white balance and verifying power levels

### Safety Equipment

Non-negotiable before powering up the laser:
- **Laser safety eyewear** — OD rating appropriate for 638 nm, 520 nm, and 445 nm at your power levels. If in doubt, over-specify the OD.
- **Beam stop** — a non-reflective, non-combustible target placed in the beam path during initial testing

---

## Required Software & Tools

### Development Environment

| Software | Version | Purpose |
|---------|---------|---------|
| [VS Code](https://code.visualstudio.com/) | Latest | IDE (recommended) |
| [PlatformIO IDE extension](https://platformio.org/install/ide?install=vscode) | Latest | Build system, flashing, serial monitor |
| [Git](https://git-scm.com/) | Any recent | Cloning the repository |

PlatformIO handles all library dependencies automatically — you do not need to install any Arduino libraries manually. The required libraries are declared in `platformio.ini` and downloaded on first build.

**Note:** PlatformIO requires Python 3.6+. It is bundled with the VS Code extension installation.

### Browser for WebUI

The GalvOS WebUI works in any modern browser. Specifically tested:
- Chrome / Chromium (recommended — best PWA support)
- Firefox
- Safari (iOS and macOS)
- Edge

No browser plugins or extensions are required.

### Optional but Useful

| Tool | Purpose |
|------|---------|
| Serial terminal (e.g. PuTTY, `screen`, VS Code serial monitor) | Reading debug output during firmware development |
| [Wireshark](https://www.wireshark.org/) | Debugging Art-Net UDP traffic |
| Any DMX controller or software (e.g. QLC+) | Testing DMX input |
| Python 3 | Running the patch validation scripts if you contribute code |

---

## The Build Environment

Once you have VS Code and PlatformIO installed, clone the repository:

```bash
git clone https://github.com/Andre1Becker/GalvOS.git
cd GalvOS
```

PlatformIO will detect `platformio.ini` automatically when you open the folder in VS Code. On first build, it will download the ESP32-S3 toolchain and all library dependencies. This takes a few minutes — this is normal.

The full build and flash workflow is covered in [Chapter 3 — Build & Configuration](03-build-and-config.md).

---

## Before You Begin Checklist

Work through this list before you power up anything with the laser connected.

### Safety Checklist
- [ ] I have read and understood the [Safety section](01-introduction.md#safety--read-this-first) in full
- [ ] I have appropriate laser safety eyewear for my laser's wavelengths and power
- [ ] I have a beam stop in place for initial testing
- [ ] I understand the legal requirements for operating Class IIIB/IV lasers in my jurisdiction
- [ ] My build environment is controlled — no uninvolved people in the beam path

### Hardware Checklist
- [ ] Power supply rails verified with a multimeter before connecting any ICs: +5V_BUCK, ±15V, +3.3V
- [ ] All fail-safe pull-up resistors fitted: R_FSR, R_FSG, R_FSB (10 kΩ each, GPIO7/8/21 → +3.3V)
- [ ] DAC8562 /CLR pull-up fitted (10 kΩ, GPIO13 / Pin5 → +5V_BUCK)
- [ ] AGND ↔ GND star-point connection made (0 Ω resistor, near DAC8562)
- [ ] OPA4134 supply decoupling capacitors fitted (100 µF + 100 nF on each rail)
- [ ] 6N137 decoupling capacitors fitted (100 nF ceramic directly across VCC/GND pins)
- [ ] SSR1 connected in the laser power rail path (not just the control signal)
- [ ] E-Stop connector wired and verified (J_ESTOP pull-up to 3.3V via R_ESTOP)

### Software Checklist
- [ ] Repository cloned
- [ ] PlatformIO extension installed and ESP32-S3 toolchain downloaded
- [ ] First build completes without errors (`pio run`)
- [ ] Firmware flashed to ESP32-S3 (`pio run --target upload_all`)
- [ ] ESP32 appears on Wi-Fi (AP mode: SSID "galvOS") or connects to your network
- [ ] WebUI accessible in browser
- [ ] Serial monitor shows boot log without crash/restart loops (`pio device monitor`)

### Initial Configuration Checklist (WebUI)
- [ ] Galvo orientation calibrated (invert_x / invert_y / swap_xy in Calibration tab)
- [ ] Output rate (`galvo_kpps`) set appropriately for your galvo set (Projection tab)
- [ ] White balance (gain_r / gain_g / gain_b) verified with the calibration patterns
- [ ] Visibility thresholds (thresh_r / thresh_g / thresh_b) calibrated per channel
- [ ] Temperature sensor readouts verified (Settings / status bar)
- [ ] Safety system tested: E-Stop button drops laser rail, watchdog timeout cuts relay

Only once all boxes are checked should you run the laser at full power in an uncontrolled environment.
