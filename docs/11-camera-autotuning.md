# Chapter 11 — Camera-in-the-Loop Auto-Tuning

> Chapter 5 explained what the optimizer does. This chapter explains how to stop guessing at its parameters by hand and let a camera and a search algorithm do it instead. Turning `max_corner_pts` up by one, projecting, squinting at the beam, turning it back down — that loop gets old fast. This one replaces the squinting with a webcam and a cost function.

## Table of Contents

- [What This Is](#what-this-is)
- [Requirements](#requirements)
- [Installation](#installation)
- [Files](#files)
- [The Camera Patterns](#the-camera-patterns)
- [Workflow](#workflow)
  - [0. wizard](#0-wizard)
  - [1. check](#1-check)
  - [2. preview](#2-preview)
  - [3. calibrate](#3-calibrate)
  - [4. measure](#4-measure)
  - [5. optimize](#5-optimize)
  - [6. diagnose](#6-diagnose)
  - [7. autotune-camera](#7-autotune-camera)
- [How Scoring Works](#how-scoring-works)
- [Session Semantics — Nothing Sticks Until You Apply It](#session-semantics--nothing-sticks-until-you-apply-it)
- [Safety](#safety)
- [Version History](#version-history)

---

## What This Is

Firmware v6.03.0 added a REST API, `/api/calib-cam/*`, that lets an external program select a calibration pattern, live-override the active optimizer profile's parameters (RAM-only, never touching NVS), and read back exactly what has changed. `scripts/optimizeGalvo/optimizeGalvo.py` is the program that actually drives it: it opens a mono/global-shutter USB camera, projects one of six reference patterns, measures the result, and runs an [Optuna](10-glossary.md) search to find the parameter combination that produces the cleanest beam.

This is **not** a replacement for the manual Galvo Calibration card in the WebUI (Chapter 4) — offset, gain, swap, and invert are still set by hand, because those are fixed hardware/wiring properties, not something a search should be exploring every run. What this tool auto-tunes is the **optimizer**: corner dwell, blanking, resample, ringing compensation — the parameters in [Chapter 5](05-optimizer.md#parameter-reference) that trade off against each other and are genuinely tedious to hand-tune by eye.

It replaces the closing item in [Chapter 9's Planned Features](09-known-issues-and-todos.md#planned-features) — "auto-tuning via global shutter camera" was the plan; this is the implementation.

---

## Requirements

- A mono or global-shutter USB camera. The tool was built and tested against an **OV9281**-based module — a rolling-shutter webcam will smear a fast-moving beam and produce misleading measurements.
- GalvOS firmware ≥ v6.04.1 (for per-channel pattern color; v6.03.0 works but always draws white).
- Python 3.12 or 3.13 is the safest bet — if wheels for `opencv-python`/`optuna`/`numpy` aren't published yet for your interpreter version, use a 3.12/3.13 venv rather than fighting a source build.
- The ESP32 and the machine running the script on the same network, with a known base URL (hostname or IP).

## Installation

```bash
cd scripts/optimizeGalvo
python -m venv .venv
.venv\Scripts\activate        # or: source .venv/bin/activate
pip install -r requirements.txt
```

`requirements.txt`: `opencv-python`, `numpy`, `optuna`, `requests`.

## Files

| File | Purpose |
|------|---------|

| `camConfig.json` | Runtime config — ESP32 base URL, camera index/resolution/exposure, DAC calibration range, cost weights, diagnose thresholds, HTTP timeout/retry settings. Created by `wizard` on first run. |
| `homography.npz` | Pixel→DAC homography matrix plus the stored background frame, written by `calibrate`. Required by `measure`, `optimize`, and `diagnose`. |
| `searchSpace.json` | Parameter ranges per camera-tunable optimizer profile (`Vector`, `Smooth`, `Waves`, `MultiObject`). Edit this if you widen a parameter's firmware-side limits. |
| `results/` | `optuna_study.db` (resumable search state), per-trial `.jsonl` logs, best-parameter JSON snapshots, and saved camera frames from `measure`/`calibrate`. |

Override the config path with `--config`, e.g. to keep separate configs for multiple camera rigs.

## The Camera Patterns

Six patterns exist on the firmware side purely for this tool (`calib_patterns.cpp`, indices 11–16), geometrically matching the ground truth the script rasterizes internally so measured error is directly comparable to the ideal:

| Pattern | Used for |
|---------|----------|

| `corners4` | 4 static dots at the DAC-range corners — the homography reference. Uses a fixed manual dwell so it stays camera-visible regardless of what corner-dwell overrides a search throws at it. |
| `square` | Sharp 90° corners — corner hotspot + path deviation. |
| `star` | 5-point pentagram (self-intersecting) — corner hotspot + path deviation. |
| `segments` | 4 parallel vertical lines — blank-jump leakage between disconnected strokes. |
| `circle` | Continuous curve, no real corners — path deviation + brightness uniformity. |
| `spiral` | Dense continuous curve — path deviation + brightness uniformity under high interior density. |

Since v6.04.1, `/api/calib-cam/start` accepts a `channel` field (default: **blue**, channel 3) instead of always drawing white. A mono camera can see the R/G/B beams smear apart or land at slightly different positions if the laser diodes aren't perfectly co-boresighted — measuring on one channel avoids that artifact entirely. `optimizeGalvo.py` drives this from `camPatternChannel` in `camConfig.json`.

## Workflow

Run `optimizeGalvo.py <command> --help` for any command's full description; the summaries below are the short version. Two flags apply across most commands: `--no-view` (disable the live camera preview window, e.g. for headless runs) and `--zoom {1,2,3}` (digital zoom on that window, live-adjustable with the `1`/`2`/`3` keys).

### 0. wizard

Interactive first-run setup — prompts for ESP32 base URL, camera index/resolution, exposure, DAC calibration range, and HTTP timeout, showing current/default values in brackets. Runs automatically the first time any command is used with no config file yet; run it directly later to change settings.

### 1. check

```bash
python optimizeGalvo.py check
```

GETs `/api/status` and prints firmware version, network info, and safety-interlock state. Opens no camera — run this first whenever something isn't working, to rule out a wrong base URL or a WiFi/mDNS problem before chasing camera or optics issues. Exits 1 on failure.

### 2. preview

```bash
python optimizeGalvo.py preview
```

Live grayscale feed with saturation %, peak pixel value, and measured fps overlaid. `+`/`-` adjust exposure live (auto-saved), `1`/`2`/`3` zoom, `s` saves a snapshot, `space` freezes the frame, `q` quits. Use this to physically aim/focus the camera and dial in exposure — visible beam trace, not blown out — before calibrating.

### 3. calibrate

```bash
python optimizeGalvo.py calibrate
```

Captures a dark background, projects `corners4`, detects the four dots, and solves the pixel→DAC homography, saving it (with the background) to `homography.npz`. Required once before `measure`/`optimize`/`diagnose`; re-run whenever the camera or projection surface moves. Saves a labeled snapshot marking which dot was identified as TL/TR/BR/BL — check it against the real physical layout if later measurements look rotated or mirrored.

### 4. measure

```bash
python optimizeGalvo.py measure --pattern square
```

Projects one pattern with currently-live parameters, captures, and prints path-deviation RMS, blank-leakage, corner hotspot, brightness non-uniformity, and the weighted cost. Requires `homography.npz`.

### 5. optimize

```bash
python optimizeGalvo.py optimize --profile Vector --trials 40 --apply
```

The main event. Queries the ESP32 for its optimizer profiles and preset membership, runs one Optuna study per selected profile against its `searchSpace.json` ranges, and prints a before/after report — every parameter marked changed, unchanged-not-searched, or unchanged-behind-a-disabled-gate. Each trial calls `/api/calib-cam/params` with candidate values and sums the cost across the profile's pattern(s).

- `--profile Vector,Smooth` or `--profile all` selects profiles directly; `--preset "Milky Way"` tunes whichever profile drives a named preset instead (errors out for Wireframe/Trails/Text — not camera-tunable, no camera pattern exists for them). Omit both for an interactive menu.
- Studies persist in SQLite (`--storage`, default `results/optuna_study.db`) under `--study-name` (default: the profile name) — Ctrl+C or a crash loses nothing, just re-run the same command to resume. `--fresh` starts over instead.
- `--apply` applies the winning values to the ESP32 and persists them via `/api/optimizer-save` without asking. **Without it, results only go to the JSON file** — see [Session Semantics](#session-semantics--nothing-sticks-until-you-apply-it).
- `space` pauses between trials, `q` aborts early (like Ctrl+C; completed trials are kept).

### 6. diagnose

```bash
python optimizeGalvo.py diagnose --profile all --autotune
```

Measures each selected profile's current live output (no overrides — a read, not a search) and classifies it: **OK**, a **geometry issue** (size/position off vs. ideal — points at galvo gain/offset drift or a moved camera, fix with the Calibration tab or re-running `calibrate`, not by retuning), or an **optimizer settings issue** (path deviation/leakage/hotspot/uniformity out of tolerance while geometry is clean — genuinely fixable by tuning). `--autotune` runs `optimize` automatically on anything flagged with a settings issue, with the same `--trials`/`--study-name`/`--storage`/`--apply` options.

### 7. autotune-camera

```bash
python optimizeGalvo.py autotune-camera --trials 30 --apply
```

Added in optimizeGalvo v2.4.0. Tunes the camera's own **capture** settings — exposure, gain, `binaryThreshold`, `accumFrames` — instead of firmware parameters, which are left exactly as currently live. Useful when `measure`/`diagnose` results look inconsistent for reasons that turn out to be the camera, not the beam: washed-out captures, blooming, background noise. Since v2.5.0, saturated pixels are flagged as their own metric (`saturationFrac`) — a global-shutter sensor blooms into neighboring pixels at saturation, which can otherwise inflate path-deviation/corner-hotspot readings with a camera artifact rather than a real scan problem; `diagnose` will suggest running this command instead of `optimize` when it detects that. On apply, updates `camConfig.json` and refreshes `homography.npz`'s stored background to match the new exposure/gain (background is exposure-dependent, so a stale one would corrupt every later diff-subtraction).

---

## How Scoring Works

Every measurement reduces to a single weighted cost (`costWeights` in `camConfig.json`):

| Metric | What it catches |
|--------|-----------------|

| `pathDeviationRms` | How far the traced beam deviates from the ideal geometry — corner dwell and interior density problems. |
| `blankLeakage` | Laser visible during a blank jump — insufficient hold-off or blanking overshoot. |
| `cornerHotspot` | Excess brightness pooling at a corner — too much dwell relative to the rest of the shape. |
| `brightnessNonUniformity` | Uneven brightness along a continuous curve — interior density or velocity-easing issues. |
| `saturationFrac` (v2.5.0+) | Fraction of the traced beam at raw sensor saturation — flags a camera-artifact-inflated reading rather than a real defect. |

`optimize` and `diagnose` share this cost function; `autotune-camera` adds `saturationFrac` and a background-brightness penalty on top, since capture-quality problems (washed-out frames) don't show up cleanly in the four geometry metrics above.

## Session Semantics — Nothing Sticks Until You Apply It

A calib-cam session's overrides are **RAM-only** — they never touch NVS on their own. Starting a session (`/api/calib-cam/start`) snapshots the target profile's current values; every override applies on top of that snapshot; and `/stop` (or an E-Stop trip, which force-stops the session from `pattern_engine::task()`) **restores the snapshot**, discarding every change. This is deliberate — an interrupted or aborted tuning run must never leave a normal preset's optimizer profile silently altered.

Practical upshot: `optimize --apply` (or `diagnose --autotune --apply`) is what makes a result permanent — it calls `/api/optimizer-live` with the winning values and `/api/optimizer-save` to persist them, before the session ends. Without `--apply`, an interactive run prompts you per profile; a non-interactive run (e.g. from a script or CI) applies nothing and the tuned values only exist in the `results/best_<profile>_<timestamp>.json` file.

## Safety

This tool projects real geometry through the real laser via the normal `calib_active` render path — every hardware safety interlock in [Chapter 1](01-introduction.md#safety-interlock-chain) still applies unchanged (E-Stop, scan-fail, watchdog, ARM). Running an unattended multi-hour `optimize` session does not relax any of the precautions in [Chapter 1's Safety section](01-introduction.md#safety--read-this-first) — beam containment and eye protection rules apply exactly as they do to any other armed session.

## Version History

| Version | Change |
|---------|--------|

| fw v6.03.0 | Initial `/api/calib-cam/*` API and the 6 camera patterns. |
| fw v6.04.1 | Patterns default to blue; `channel` field on `/api/calib-cam/start`. |
| fw v6.05.0 | Fixed closed shapes not reconnecting when a heavily-tuned-down `max_pts_per_frame` starved corner dwell — see [Chapter 5](05-optimizer.md#stage-4--corner-dwell). |
| optimizeGalvo v2.4.0 | Added `autotune-camera`. |
| optimizeGalvo v2.5.0 | Added `saturationFrac` blooming metric. |
