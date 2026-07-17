# Chapter 6 — Troubleshooting

> **Status:** Skeleton — content to be filled in Session 6

## Contents
- [Before You Debug](#before-you-debug)
- [Boot & Connectivity Issues](#boot--connectivity-issues)
- [Galvo Issues](#galvo-issues)
- [Laser Issues](#laser-issues)
- [Optimizer / Pattern Quality Issues](#optimizer--pattern-quality-issues)
- [Color & Calibration Issues](#color--calibration-issues)
- [WebUI Issues](#webui-issues)
- [ILDA / SD Card Issues](#ilda--sd-card-issues)
- [Safety System Issues](#safety-system-issues)
- [Memory & Stability Issues](#memory--stability-issues)
- [Known Limitations](#known-limitations)

---

## Before You Debug

<!-- TODO: serial monitor setup, log output format, how to read telemetry bar -->

---

## Boot & Connectivity Issues

| Symptom | Likely Cause | Fix |
|---------|-------------|-----|
| No Wi-Fi AP appears | <!-- TODO --> | <!-- TODO --> |
| WebUI unreachable | <!-- TODO --> | <!-- TODO --> |
| Firmware version wrong | <!-- TODO --> | <!-- TODO --> |

---

## Galvo Issues

| Symptom | Likely Cause | Fix |
|---------|-------------|-----|
| Galvos go crazy on boot | <!-- TODO: SD card issue --> | <!-- TODO --> |
| Output mirrored horizontally | <!-- TODO --> | invert_x in calibration |
| Output mirrored vertically | <!-- TODO --> | invert_y in calibration |
| Output rotated 90° | <!-- TODO --> | swap_xy in calibration |
| Ringing / vibration visible | <!-- TODO --> | Enable accel clamp, ringing comp |
| Blanked jumps visible as lines | <!-- TODO --> | Increase blank_samples |
| Corners look rounded | <!-- TODO --> | Increase corner_pts |

---

## Laser Issues

| Symptom | Likely Cause | Fix |
|---------|-------------|-----|
| Laser ON at boot (fail-safe) | <!-- TODO: pull-up correct direction --> | Check R_FSR/G/B wiring |
| Laser won't turn on | <!-- TODO --> | <!-- TODO --> |
| One color channel missing | <!-- TODO --> | <!-- TODO --> |
| Colors washed out after animation | <!-- TODO --> | ↺ Reset Colors button |
| master_dimmer = 0 → nothing visible | <!-- TODO --> | Check max(master_dimmer, ui_master_dimmer) |

---

## Optimizer / Pattern Quality Issues

| Symptom | Likely Cause | Fix |
|---------|-------------|-----|
| Pattern flickers | <!-- TODO --> | <!-- TODO --> |
| Pattern too dim at high kpps | <!-- TODO --> | <!-- TODO --> |
| No improvement above 1300 points | Expected — known limit | opt_max_pts_per_frame: 1300 is the effective ceiling |
| Wireframe corners look bad | buildWfChains() not called | <!-- TODO --> |

---

## Color & Calibration Issues

| Symptom | Likely Cause | Fix |
|---------|-------------|-----|
| Base color sliders have no effect | threshold bypass not used | Run threshold calibration with test beam |
| Double-gamma / blown-out colors | <!-- TODO: double-gamma bug pattern --> | <!-- TODO --> |
| mapVisibleRange(255, any) always 255 | Expected behavior | Use test beam for threshold calibration |

---

## WebUI Issues

| Symptom | Likely Cause | Fix |
|---------|-------------|-----|
| WebUI loads but controls don't respond | <!-- TODO --> | <!-- TODO --> |
| Preset grid empty | /api/presets unreachable | <!-- TODO --> |
| Route not found (404) | Route registration order | Specific routes must be before prefix routes |

---

## ILDA / SD Card Issues

| Symptom | Likely Cause | Fix |
|---------|-------------|-----|
| SD card causes galvo issues | Known open issue | Do not populate SD card until fixed |
| ILDA files not listed | <!-- TODO --> | FAT32, Class 10 UHS-I required |

---

## Safety System Issues

| Symptom | Likely Cause | Fix |
|---------|-------------|-----|
| Laser kills immediately on power | Watchdog timeout | Check GPIO14 heartbeat pulse |
| E-Stop ignored | <!-- TODO --> | Check J_ESTOP wiring, R_ESTOP pull-up |
| Scan-fail fires spuriously | NE555 timing | Adjust C_T / R_T values |

---

## Memory & Stability Issues

| Symptom | Likely Cause | Fix |
|---------|-------------|-----|
| Crash / restart loop | Heap exhaustion | Check internal DRAM (~76KB free post-optimization) |
| Large buffer allocation fails | Wrong heap | Use ps_malloc / heap_caps_malloc(MALLOC_CAP_SPIRAM) |
| JSON serialization crash | Wrong allocator | Use SpiRamAllocator for all JsonDocument |

---

## Known Limitations

<!-- TODO: SD card galvo issue, text animation bugs, some calibration patterns, 
     Chaos Bouncer / Hypotrochoid wrong output, Star Wars Scroll direction -->
