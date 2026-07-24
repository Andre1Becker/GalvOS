# Chapter 11 — Glossary & Terminology

> A laser show controller touches a surprisingly wide range of disciplines — electronics, embedded systems, optics, networking, signal processing, and photobiology. This glossary collects the abbreviations and technical terms used throughout the GalvOS documentation and explains each one in plain language.

Terms are grouped by topic. If you are looking for a specific abbreviation, use your browser's Find function (Ctrl+F / Cmd+F).

---

## Table of Contents

- [Laser & Optics](#laser--optics)
- [Galvo & Scanner](#galvo--scanner)
- [Hardware Components](#hardware-components)
- [Signal & Electrical](#signal--electrical)
- [Firmware & Embedded Systems](#firmware--embedded-systems)
- [Memory Architecture](#memory-architecture)
- [Networking & Protocols](#networking--protocols)
- [Show Protocols & Standards](#show-protocols--standards)
- [Optimizer & Signal Processing](#optimizer--signal-processing)
- [Color & Photometry](#color--photometry)
- [Build System & Tools](#build-system--tools)
- [Safety & Regulatory](#safety--regulatory)

---

## Laser & Optics

**Beam**
The collimated (narrowly focused) ray of light emitted by the laser. In a galvo projector, the beam reflects off two mirrors to reach the projection surface.

**Blanking**
Turning the laser off while the galvo mirrors are moving between shapes — so no visible line is drawn during the repositioning jump. "Blank jump" refers to the combination of moving mirrors + laser off. The quality of blanking (how cleanly it starts and stops) directly affects image quality.

**Class IIIB / Class IV laser**
Laser hazard classifications under IEC 60825-1. Class IIIB: 5 mW–500 mW, can cause immediate eye injury from direct or specular reflection exposure. Class IV: >500 mW (the Mikoy 5W falls here), can additionally cause skin burns and fire ignition. Both require appropriate safety controls. See [Chapter 1 — Safety](01-introduction.md#safety--read-this-first).

**Collimation**
The degree to which a laser beam's rays are parallel. A perfectly collimated beam does not diverge with distance and produces a sharp dot at any throw distance. Real beams diverge slightly — this divergence angle is a key laser specification.

**Dwell**
Holding the galvo mirrors at a fixed position for one or more output ticks. Dwell points are inserted at corners (corner dwell) and during blank jumps (settle ticks) to give the mirrors time to reach and stabilise at a commanded position.

**Fail-safe**
A design principle where hardware defaults to the safe state when power is lost or firmware crashes. In GalvOS, the 10 kΩ pull-up resistors on GPIO 7/8/21 hold the laser TTL lines HIGH at boot (before firmware initialises), which keeps the optocouplers dark and the laser TTL signal at 1.65 V — meaning laser ON at the TTL level. The laser power rail is separately held off by the SSR until all safety conditions are met.

**ILDA angle**
The International Laser Display Association's standard test angle of ±8° optical. Galvo kpps ratings are specified at this angle — at wider angles, the achievable scan rate decreases proportionally.

**OD (Optical Density)**
The logarithmic measure of how much a laser safety filter attenuates the beam. OD 3 = 1000× attenuation (0.1% transmission). OD 4 = 10,000× attenuation. Laser safety eyewear must be rated for the specific wavelengths and power levels in use, with sufficient OD to reduce the transmitted power below the Maximum Permissible Exposure (MPE).

**Optical half-angle**
The angle from the centre line (0°) to the edge of the scan field — the "radius" of the scan in angular terms. A projector with ±20° optical half-angle sweeps a 40° full field. Often confused with mechanical angle; the relationship between them depends on the mirror geometry (typically: optical = 2× mechanical for a flat mirror).

**Ringing**
Mechanical oscillation of the galvo mirror after a fast position change. The mirror overshoots the target position, bounces back, overshoots again in the other direction, and repeats — decaying exponentially. Visible as waviness or ghosting in the projected image near sharp corners or after blank jumps. Addressed by ZV input shaping (Pillar 3 of the optimizer) and corner dwell (Pillar 1).

**Specular reflection**
A mirror-like reflection from a flat, shiny surface (glass, polished metal, water). Unlike diffuse reflections, a specular reflection preserves the beam's collimation — the reflected beam is just as dangerous as the direct beam. A 5 W laser striking a pane of glass at a distance produces a specular reflection that can cause eye injury.

**Spot size**
The diameter of the laser beam on the projection surface. Determined by the beam divergence and throw distance. A small spot size produces sharp, fine details; a large spot size produces visible "fat" lines.

**Throw distance**
The distance between the projector aperture and the projection surface. Affects spot size, image size, and peak irradiance.

**V(λ) — Luminous Efficiency Function**
The CIE standard function describing the relative sensitivity of the human eye to different wavelengths of light. Peaks at 555 nm (green). Used in GalvOS to compute visible power (the photometrically weighted power a human would perceive) from per-channel laser powers. Red at 638 nm: V(λ) ≈ 0.235; green at 520 nm: V(λ) ≈ 0.710; blue at 445 nm: V(λ) ≈ 0.040.

**B(λ) — Blue-Light Hazard Weighting Function**
The CIE weighting function for photochemical retinal hazard (blue-light hazard). Peaks at 435–440 nm — which is very close to the 445 nm blue laser diode used in the Mikoy 5W (B(λ) ≈ 0.22). Blue-light hazard is separate from the thermal hazard modelled by V(λ) and can accumulate with prolonged low-level exposure even when the beam appears dim.

**Wavelength (λ, lambda)**
The physical property that determines the perceived color of light. Measured in nanometres (nm). GalvOS hardware: red 638 nm, green 520 nm, blue 445 nm.

---

## Galvo & Scanner

**Galvo / Galvanometer scanner**
A small voice-coil actuator with a mirror attached to its shaft. An electric current deflects the mirror by a proportional angle. Two galvos (X and Y) together steer the laser beam to any point in a 2D scan field. The name comes from the galvanometer — the same principle used in old-fashioned current meters.

**JY-15K-BL**
The Jolooyo galvo set used in GalvOS. Rated at 15 kpps at the ILDA standard ±8° angle. Mechanical half-angle ±25°; optical half-angle ±20°; rotor inertia 0.025 g·cm². Driven by the OEM TDA2030AL + 4× LM324 PID servo board, or the GalvOS OPA4134 differential output stage.

**kpps (kilo-points-per-second)**
The scan rate — how many distinct mirror positions (DAC samples) the galvo can accurately follow per second, in thousands. The JY-15K-BL is rated at 15 kpps at ±8°. In GalvOS, `galvo_kpps` sets the actual ISR output rate; `galvo_rated_kpps` stores the datasheet reference value used for PPS scaling.

**PID (Proportional-Integral-Derivative) controller**
The servo control algorithm inside the galvo driver board that continuously adjusts the coil current to minimise the error between the commanded mirror position and the actual mirror position. The OEM galvo driver uses a 4× LM324 op-amp PID implementation. The trim potentiometers on the driver board adjust P, I, D gains and damping.

**PPS (points per second)**
The galvo output rate without the "kilo" prefix. 30,000 PPS = 30 kpps. Used in log output and some API fields.

**Scan angle**
How far the galvo mirrors deflect from their centre position. Wider scan angles produce a larger image but require more time to traverse and reduce the achievable scan rate. See `scan_angle_mech_deg` and `exit_angle_deg` in the Projection tab.

---

## Hardware Components

**6N137**
High-speed optocoupler used to galvanically isolate the ESP32's GPIO outputs from the laser TTL signal lines. The LED side is driven by the ESP32 (via a 220 Ω series resistor); the output collector side drives the laser TTL. Switching speed: up to 10 MHz. In GalvOS v3.2.2, a 1 kΩ pull-up + 1 kΩ pull-down divider on the output produces 1.65 V HIGH and 0 V LOW — matching the MN-1M5AT active-HIGH input requirement.

**DAC8562**
16-bit dual-channel SPI digital-to-analogue converter used to generate the X and Y galvo drive voltages. Output range: 0–2.5 V (relative to its internal 2.5 V reference). Two channels = one per galvo axis. Driven from the ESP32's SPI2 peripheral via raw hardware register writes (not the IDF polling API) to achieve the throughput needed for 30 kpps.

**DS18B20**
Digital temperature sensor using the 1-Wire protocol. Each sensor has a unique 64-bit serial number, allowing up to 5 sensors on a single GPIO pin (GPIO18 in GalvOS). Accuracy: ±0.5°C from −10°C to +85°C. Used to monitor laser diodes, galvo driver, PSU, chassis, and ambient temperature.

**MAX485**
RS-485 transceiver IC. Used in GalvOS as a DMX-512 receiver: the differential RS-485 signal from the XLR connector is converted to a UART-compatible single-ended signal for the ESP32. DE and RE pins are wired to GND (receive-only mode — never transmits).

**MN-1M5AT**
Three-channel constant-current buck (step-down) laser driver used in the Mikoy 5W. Active-HIGH TTL control: HIGH on the enable pin = laser ON; LOW = laser OFF. GalvOS drives it via 6N137 optocouplers with logic-inverted PWM signals.

**NE555**
Classic timer IC used in two roles in GalvOS: (1) scan-fail detector — triggered by AC-coupled DAC output activity; times out if the galvo stops scanning; (2) hardware watchdog — retriggered by a firmware heartbeat pulse on GPIO14; times out if firmware hangs and cuts the laser power relay.

**OPA4134**
Precision JFET-input audio op-amp used as a differential amplifier to convert the DAC8562's 0–2.5 V unipolar output to the ±5 V bipolar galvo drive signal. Formula: VOUT = 2 × (2.5 V − VDAC). Four independent op-amp channels in one SOIC-14 package; GalvOS uses two (X and Y), one as a VREF buffer, and one tied in unity gain as a safe termination.

**SSR (Solid State Relay)**
A relay with no moving parts — switches a load circuit using a semiconductor (TRIAC or SCR) instead of physical contacts. In GalvOS, SSR1 sits in the laser power rail. It is controlled by GPIO38 (PIN_LASER_ENABLE) via a 1 kΩ current-limiting resistor. The hardware watchdog NE555 can cut this relay independently of firmware.

---

## Signal & Electrical

**AGND (Analogue Ground)**
A separate ground reference for the analogue signal chain (DAC8562, OPA4134, galvo signal path). Connected to the digital GND at a single star point (0 Ω resistor near the DAC8562) to prevent digital switching noise from coupling into the galvo drive signals.

**dBm**
Decibels relative to 1 milliwatt. Used to express Wi-Fi signal strength. −30 dBm = excellent (near the access point); −70 dBm = usable; −90 dBm = very weak / unreliable. Displayed as RSSI in GalvOS.

**Differential amplifier**
An amplifier whose output is proportional to the difference between two input signals. The OPA4134 in GalvOS is wired as a difference amplifier: one input takes the DAC output, the other takes the 2.5 V reference. The output swings symmetrically around zero as the DAC swings around mid-scale. This converts the DAC's 0–2.5 V range to the galvo's required ±5 V range.

**PWM (Pulse-Width Modulation)**
A technique for controlling power (brightness, speed) by rapidly switching a signal on and off. The fraction of time it is on (duty cycle) determines the effective level: 50% duty = half power. GalvOS uses PWM for RGB laser control (LEDC peripheral, 50 kHz, 8-bit = 256 brightness levels) and fan speed control (25 kHz).

**RSSI (Received Signal Strength Indicator)**
A measure of the radio signal power received by the Wi-Fi chip. Expressed in dBm in GalvOS. More negative = weaker signal. See dBm above.

**SPI (Serial Peripheral Interface)**
A synchronous serial communication protocol using four signals: SCK (clock), MOSI (data from master to slave), MISO (data from slave to master), and CS/SS (chip select, one per device). GalvOS uses SPI2 for the DAC8562 and an independent SPI3 bus for the SD card — separate peripherals, separate GPIOs, no bus sharing.

**TTL (Transistor-Transistor Logic)**
A family of digital logic signals where a HIGH voltage (typically 3.3 V or 5 V) represents a logical 1 and LOW (0 V) represents a logical 0. In GalvOS, the laser control signals are called "TTL" to distinguish them from the analogue galvo drive signals, even though the actual signal levels are 0 V / 1.65 V after the voltage-divider network.

**VCC / VDD**
The positive supply voltage for a digital circuit. VCC historically refers to bipolar transistor circuits (Collector supply); VDD to MOSFET/CMOS circuits (Drain supply). Used interchangeably in GalvOS datasheets.

---

## Firmware & Embedded Systems

**Arduino framework**
A C++ abstraction layer over the ESP-IDF that provides familiar `setup()` / `loop()` structure and a large ecosystem of libraries. GalvOS uses the Arduino framework via PlatformIO, but accesses hardware directly where performance requires it (SPI register writes for the DAC, hardware timer for the galvo ISR).

**Core 0 / Core 1**
The two Xtensa LX7 CPU cores in the ESP32-S3. In GalvOS: Core 0 runs the Wi-Fi stack, WebUI, DMX, Art-Net, NTP, and safety monitoring. Core 1 runs the galvo ISR and pattern engine. FreeRTOS assigns tasks to cores at creation time.

**ESP32-S3**
The Espressif microcontroller at the heart of GalvOS. Dual-core Xtensa LX7 at 240 MHz, with built-in Wi-Fi and Bluetooth, 512 KB internal SRAM, and OPI PSRAM support up to 8 MB. The N16R8 module variant adds 16 MB SPI flash and 8 MB octal PSRAM.

**ESP-IDF**
Espressif IoT Development Framework — the official low-level C SDK for ESP32 chips. The Arduino framework runs on top of ESP-IDF. GalvOS uses ESP-IDF functions directly for logging (`ESP_LOGI`), timers, and hardware peripherals where the Arduino abstraction is too slow or limited.

**FreeRTOS**
The real-time operating system running on both cores. Provides tasks (threads), queues, semaphores, and mutexes. GalvOS creates ~8 FreeRTOS tasks: galvo ISR handler, pattern engine, safety monitor, temperature monitor, CPU monitor, watchdog heartbeat, NTP sync, and the AsyncTCP/WebSocket event loop.

**ISR (Interrupt Service Routine)**
A function that executes in response to a hardware interrupt, interrupting normal program flow. The galvo ISR in GalvOS is triggered by a hardware timer at `GALVO_SAMPLE_RATE_HZ` (30,000 times/second). It dequeues one `LaserPoint` per tick and writes it to the DAC and LEDC. ISRs must be fast and must not call non-IRAM-safe functions.

**LEDC (LED Control peripheral)**
The ESP32's dedicated PWM hardware for LED control, repurposed in GalvOS for RGB laser modulation. Supports multiple channels, each with independently configurable frequency and duty cycle. GalvOS attaches the three laser GPIOs (7, 8, 21) to LEDC channels once at `setup()` — never per tick, because `ledcAttachPin()` is too expensive to call at 30 kpps.

**OTA (Over-The-Air update)**
Firmware update delivered over a network connection (Wi-Fi) without a physical cable. GalvOS supports OTA via an HTTP update endpoint (`http://<device>/update`, username `admin`, password = chip-ID hex). The partition table includes two app slots (app0, app1) so the running firmware is preserved until the new image is verified.

**PlatformIO**
The build system and IDE extension used to compile, link, and flash GalvOS firmware. Handles toolchain download, library management, upload targets, and the serial monitor. Configured via `platformio.ini`.

**RTC memory**
A small area of RAM (8 KB) on the ESP32 that retains its contents through a software reset (`esp_restart()`) but is cleared on power loss. GalvOS stores the last safety failsafe reason in RTC memory so it survives restarts and can be displayed in the Dashboard after a crash.

**UART (Universal Asynchronous Receiver-Transmitter)**
A serial communication standard. GalvOS uses UART0 for the debug serial console (TX on GPIO43, RX on GPIO44) and UART1 for DMX-512 reception (RX on GPIO4 via MAX485). The ESP32-S3 DevKitC-1 exposes UART0 through its built-in USB-to-serial bridge.

**USB CDC (Communications Device Class)**
A USB protocol that makes the device appear as a virtual serial port to the host computer. The ESP32-S3 has native USB hardware (on GPIO19/20) that can present as a USB CDC device without an external USB-to-serial chip. GalvOS uses the UART bridge on the DevKitC-1 board (separate chip) rather than native CDC, for better reset-into-bootloader compatibility.

---

## Memory Architecture

**DRAM (Data RAM)**
The ESP32-S3's internal SRAM, accessible to both cores at full speed. Limited: ~512 KB total, ~76 KB free at runtime in GalvOS after the OS, stacks, and static data. Used for time-critical ISR data, small buffers, and atomic flags. Large allocations here cause heap exhaustion and crashes.

**IRAM (Instruction RAM)**
A special region of internal RAM where code can be placed using the `IRAM_ATTR` attribute. Code in IRAM executes faster than code fetched from flash and can run during flash cache misses. The galvo ISR is placed in IRAM.

**LittleFS**
A fail-safe filesystem designed for embedded flash storage. GalvOS uses LittleFS on a 5 MB flash partition to store `index.html.gz` (the WebUI) and associated assets. LittleFS is more resilient to power loss during writes than SPIFFS (the older ESP32 filesystem it replaced). In PlatformIO, the LittleFS partition is still labeled `spiffs` for historical compatibility.

**NVS (Non-Volatile Storage)**
The ESP32's key-value store built on flash memory. Survives power cycles. GalvOS stores all runtime configuration (Wi-Fi credentials, calibration values, optimizer profiles, safety thresholds) in NVS under two namespaces: `"laser"` (RuntimeConfig) and `"projection"` (ProjectionConfig). Reset via the WebUI factory reset button or via `esptool.py erase_region`.

**OPI (Octal Peripheral Interface)**
A high-speed PSRAM interface using 8 data lines simultaneously. The N16R8 module uses OPI PSRAM, configured via `board_build.psram_type = octal` and `board_build.arduino.memory_type = qio_opi` in `platformio.ini`. Provides ~80 MB/s bandwidth to PSRAM vs. ~40 MB/s for quad-SPI PSRAM.

**PSRAM (Pseudo-Static RAM)**
External RAM connected to the ESP32 via SPI. In the N16R8 module: 8 MB, OPI interface, mapped into the address space alongside internal DRAM. Slower than internal DRAM (higher latency) but vastly larger. GalvOS uses PSRAM for all large allocations: pattern buffers, JSON serialisation, the pattern cache, and the Paint body buffer. Allocated via `ps_malloc()` or `heap_caps_malloc(MALLOC_CAP_SPIRAM)`.

---

## Networking & Protocols

**AP mode (Access Point mode)**
The ESP32 creates its own Wi-Fi network. Other devices connect to it directly. GalvOS starts in AP mode on first boot (SSID: "galvOS", no password, IP: 192.168.4.1). Useful for initial configuration or when no external Wi-Fi network is available.

**Art-Net**
A royalty-free protocol for transmitting DMX-512 data over Ethernet/UDP/IP networks. Allows up to 32,768 universes (vs. one universe per cable for DMX). GalvOS receives Art-Net universe 0 by default, configurable in the WebUI. Useful for sending laser control data from a lighting console via Wi-Fi.

**DHCP (Dynamic Host Configuration Protocol)**
Automatically assigns IP addresses to devices joining a network. GalvOS uses DHCP by default in STA mode; a static IP can be configured in the WebUI Configuration tab.

**Ether Dream**
An open-source laser DAC protocol over Ethernet. GalvOS includes experimental Ether Dream receiver support, allowing compatible laser software (Pangolin, BEYOND, etc.) to send ILDA-style point streams to GalvOS over the network.

**IP address**
The numerical identifier for a device on a network. In AP mode, GalvOS always uses 192.168.4.1. In STA mode, the assigned address is shown in the Dashboard → System card and in the serial boot log.

**mDNS (Multicast DNS)**
A protocol that allows devices to be discovered by hostname on a local network without a DNS server. GalvOS registers as `galvOS.local` (or whatever `hostname` is configured to). Works on most home networks; may not work on corporate networks or through VPNs.

**NTP (Network Time Protocol)**
Used to synchronise the ESP32's clock to an internet time server. GalvOS uses `pool.ntp.org` by default. Timezone is configured using a POSIX TZ string. NTP sync status is shown in the Dashboard → System card.

**POSIX TZ string**
A compact string that encodes a timezone's offset from UTC and its daylight saving rules. Example for Central European Time: `"CET-1CEST,M3.5.0,M10.5.0/3"`. Reference: https://www.gnu.org/software/libc/manual/html_node/TZ-Variable.html

**PWA (Progressive Web App)**
A web application that can be installed on a device and launched like a native app. GalvOS's `index.html` is a PWA — it can be added to the home screen on iOS or Android and runs without browser chrome. The app icon, name, and display mode are configured in the WebUI manifest.

**REST API**
Representational State Transfer — a style of web API where resources are addressed by URLs and standard HTTP methods (GET, POST, PUT, DELETE). GalvOS exposes a REST API at `/api/` for all configuration and control. See [Chapter 8 — API Reference](08-api-reference.md).

**STA mode (Station mode)**
The ESP32 connects to an existing Wi-Fi network as a client, like a phone or laptop would. Configured with SSID and password in the WebUI Configuration tab. After connecting, the device is accessible at its DHCP-assigned IP or via mDNS.

**UDP (User Datagram Protocol)**
A connectionless network protocol. Faster than TCP but provides no delivery guarantee. Art-Net uses UDP. GalvOS listens for Art-Net on UDP port 6454.

**WebSocket**
A persistent, full-duplex TCP connection between the browser and the server. GalvOS uses a WebSocket to stream the firmware log to the WebUI Log tab in real time, without the overhead of repeated HTTP polling.

---

## Show Protocols & Standards

**DMX-512 (Digital Multiplex)**
The standard control protocol for stage and entertainment lighting. 512 channels per universe, each carrying a value 0–255, sent as a serial RS-485 signal at 250 kbaud. GalvOS receives DMX via a MAX485 module on GPIO4. Channel 1 is the master dimmer; channels 2–25 map to laser parameters. See [Chapter 3 — RuntimeConfig → DMX/Art-Net](03-build-and-config.md).

**ILDA (International Laser Display Association)**
The trade association for the professional laser display industry. Also the name of the standard file format for laser show content (`.ild` files), which store sequences of X/Y position + RGB color + blanking points. The ILDA standard test pattern is used to verify galvo scanner performance. The ILDA kpps rating uses ±8° optical as the reference angle.

**Universe (DMX/Art-Net)**
A group of 512 DMX channels. A single DMX cable carries one universe. Art-Net allows multiple universes over one IP network. GalvOS responds to one universe at a time, configurable via `artnet_universe` in the WebUI.

---

## Optimizer & Signal Processing

**Adaptive density**
The optimizer's Pillar 1: the number of interior points added to each edge scales with the edge length and is concentrated near corners. Longer edges get more points; sharper corners get more dwell time.

**Calib-cam (camera-in-the-loop calibration)**
The `/api/calib-cam/*` REST API (since v6.03.0) that lets a host-side tool (`optimizeGalvo.py`) select a camera-reference pattern and override optimizer parameters live, RAM-only, while measuring the projected result with a camera. See [Chapter 6 — Camera-in-the-Loop Auto-Tuning](06-camera-autotuning.md).

**Homography**
A projective transformation that maps points from one plane to another — here, camera pixel coordinates to DAC coordinate space. `optimizeGalvo.py`'s `calibrate` command computes this once from 4 reference dots (the `corners4` pattern) so it can translate every later camera measurement back into DAC units for scoring.

**Optuna**
An open-source hyperparameter optimization framework using Tree-structured Parzen Estimator (TPE) search. `optimizeGalvo.py` uses it to search optimizer parameter space (or camera capture settings) for the combination that minimizes a measured cost, resuming from a persistent SQLite study on interruption.

**Acceleration clamp**
Optional optimizer stage that limits how quickly the commanded galvo velocity can change from one tick to the next. Prevents sharp velocity ramps that can excite galvo resonance even when individual step sizes are within bounds.

**Affine transform**
A geometric transformation that preserves parallel lines — includes rotation, scaling, translation, and shearing. The optimizer's Transform stage applies a 2×3 affine matrix to all vertices, combining rotation, scale, and position into a single operation.

**Corner dwell**
Extra stationary laser points inserted at vertices where the path changes direction sharply. Gives the galvo mirror time to decelerate, reach the corner position, and reaccelerate onto the next edge. The number of dwell points scales with corner severity.

**Frame budget**
The maximum number of `LaserPoint` entries the optimizer is allowed to produce per frame (`opt_max_pts_per_frame`, default 1010). Determines the frame rate: 30,000 pps ÷ 1010 pts/frame ≈ 30 fps. Effective ceiling on JY-15K-BL hardware: 1300 points per frame.

**LaserPoint**
The fundamental output unit of the optimizer: a struct containing `int16_t x, y` (galvo position in DAC units) and `uint8_t r, g, b` (laser brightness per channel). The ISR consumes one `LaserPoint` per tick.

**LUT (Look-Up Table)**
A pre-computed array that maps an input value to an output value, avoiding expensive runtime computation. GalvOS uses a 256-entry LUT for CIE 1931 gamma correction: `gamma_lut[input_0_255]` → corrected output value.

**PathSegment**
The input unit to the optimizer: a sequence of `PathVertex` entries connected by straight lines, with a `closed` flag (polygon vs. polyline). Patterns generate arrays of `PathSegment` objects; the optimizer converts them to `LaserPoint` arrays.

**PPS scaling**
The automatic adjustment of optimizer parameters when `galvo_kpps` differs from `galvo_rated_kpps`. Point density scales as 1/r, velocity ceiling as r, acceleration ceiling as r², where r = rated_kpps / galvo_kpps. Ensures the optimizer produces appropriate output at any configured scan rate.

**Resample**
Optional optimizer stage that replaces length-proportional point density with constant-spacing density. Every edge gets the same number of points per unit length, producing uniform galvo velocity across the whole pattern.

**Ring buffer**
A fixed-size circular buffer used as a queue between the pattern engine (producer) and the galvo ISR (consumer). The pattern engine writes frames of `LaserPoint` arrays into the ring buffer; the ISR reads one point per tick. An overflow means the producer is faster than the consumer; an underrun means the consumer is faster.

**S-curve / Smoothstep**
A mathematical curve (3t² − 2t³) that starts at 0, ends at 1, and has zero velocity (zero first derivative) at both endpoints. Used for blank jump trajectories in GalvOS: the galvo position follows an S-curve from start to end, so the mirror starts and stops the jump gently with no abrupt velocity changes.

**Velocity clamp**
Optional optimizer stage that subdivides any lit step exceeding `max_step_units` DAC units/tick. Prevents the galvo from receiving position commands it cannot physically execute in one tick period.

**ZV (Zero-Vibration) input shaping**
A signal processing technique for eliminating residual vibration in mechanical systems. The original command is replaced by two smaller impulses timed so their mechanical responses cancel each other. In GalvOS, ZV shaping is applied to blank jump trajectories to eliminate galvo ringing at the landing point. Requires knowing the galvo's resonant frequency (`ring_freq_hz`) and damping ratio (`ring_damping_ratio`). See [Chapter 5 — The Optimizer](05-optimizer.md#stage-8--zv-ringing-compensation-pillar-3).

---

## Color & Photometry

**CIE 1931**
The Commission Internationale de l'Éclairage 1931 color matching standard. Defines the luminous efficiency function V(λ) and the standard observer's color matching functions. GalvOS applies a CIE 1931 perceptual gamma correction curve to laser brightness so equal numerical PWM steps appear equal in perceived brightness.

**Gamma correction**
A nonlinear adjustment of brightness values to compensate for the difference between physical power (linear) and perceived brightness (approximately logarithmic). Without gamma correction, mid-values of a 0–255 brightness range appear much brighter than the perceptual midpoint. GalvOS's CIE 1931 gamma LUT corrects for this. `gamma_enable = true` (default).

**Gain (color channel)**
A scaling factor applied to the raw color value from the pattern, used to equalize the perceived brightness of the three laser channels (white balance). `gain_r`, `gain_g`, `gain_b` in RuntimeConfig. Values less than 255 reduce that channel's brightness. Default values (115, 43, 255) reflect the different perceived brightnesses of 1W red, 1W green, and 3W blue at their specific wavelengths.

**mapVisibleRange()**
A firmware function that remaps the logical 0–255 color range onto [thresh_x, 255]. This ensures "0% brightness" always means the laser is off (below threshold = invisible) and "100%" always means full power, regardless of the diode's physical dead zone. `mapVisibleRange(255, any_threshold)` always returns 255 — a full-brightness value bypasses threshold mapping entirely.

**Threshold (thresh_r/g/b)**
The minimum PWM duty at which each laser diode actually emits visible light. Below this value, the diode is in its dead zone (subthreshold current — no stimulated emission). Calibrated via the threshold test beam in the Calibration tab.

**White balance (AWB — Auto White Balance)**
Adjusting the relative brightness of R, G, B channels so a nominally "white" output (R=G=B=255) appears as neutral white rather than tinted. GalvOS computes auto white balance gains from the configured laser powers and V(λ) weighting. Manual fine-tuning via the Three-Circle calibration pattern.

---

## Build System & Tools

**CERN-OHL-S**
CERN Open Hardware Licence version 2, Strongly Reciprocal. The license covering GalvOS hardware designs. Requires that derivatives are released under the same license with complete design files.

**esptool.py**
The official Espressif command-line tool for flashing ESP32 chips over UART. Used by PlatformIO internally. Can also be used directly for operations PlatformIO doesn't expose, such as erasing individual flash partitions (`erase_region`).

**GPL v3 (GNU General Public License version 3)**
The license covering GalvOS firmware and software. Requires that derivative works be released under the same license with full source code.

**LittleFS image**
The binary filesystem image containing the WebUI files, assembled by PlatformIO from the `data/` directory and flashed to the LittleFS flash partition. Must be reflashed with `pio run --target upload_all` after any change to files in `data/`.

**platformio.ini**
The PlatformIO project configuration file. Defines the target board, framework, library dependencies, build flags, flash settings, and upload configuration. The top-level file for all build parameters in GalvOS.

**node --check**
A Node.js command that parses a JavaScript file for syntax errors without executing it. Used in the GalvOS patch workflow to validate WebUI JavaScript before submitting changes.

---

## Safety & Regulatory

**CE marking**
A declaration by the manufacturer that a product meets applicable EU directives (safety, EMC, etc.). Modifying the hardware voids the original CE marking. The modified device is no longer CE-marked and cannot legally be placed on the EU market as a consumer product.

**E-Stop (Emergency Stop)**
A hardware safety switch that immediately cuts the laser power rail when pressed, regardless of firmware state. Connected to GPIO47 (PIN_ESTOP) via a 10 kΩ pull-up resistor. Normally open = safe; switch press shorts to GND = E-Stop active. One of the five conditions that must be satisfied to ARM the laser.

**IEC 60825-1**
The international standard for laser product safety classification and labelling. Defines the laser hazard classes (1, 1M, 2, 2M, 3R, 3B, 4). The Mikoy 5W falls in Class 3B or 4 depending on configuration. Compliance is required for commercial laser products in most jurisdictions.

**MPE (Maximum Permissible Exposure)**
The maximum laser power or energy density that the eye or skin can be exposed to without damage, as defined in IEC 60825-1 and ANSI Z136.1. Used to calculate minimum safe distances and required OD for protective eyewear.

**Scan-fail safety**
A hardware interlock that detects whether the galvo mirrors are actually scanning. In GalvOS, the NE555 (U11) is triggered by AC-coupled activity on the DAC VOUTA output. If the galvos stop moving (firmware hang, DAC failure), the NE555 times out and the safety system disarms the laser. This prevents the stationary beam from burning a spot.
