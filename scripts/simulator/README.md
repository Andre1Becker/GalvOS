# GalvOS Browser Simulator

`docs/index.html` (published at <https://www.galvos.de>) is a **generated file**.
Do not edit it by hand — it is `data/index.html` plus the thin simulator layer
in this folder, assembled by `build_simulator.py`.

| File | Role |
| ---- | ---- |
| `build_simulator.py` | Assembles `docs/index.html` from WebUI + this layer |
| `sim_engine.js` | Preset engine, simplified optimizer, mock `/api/*` backend |
| `overlay.html` | Launch/disclaimer card, shown once per session |
| `check_simulator.py` | Headless-Chrome smoke test of the generated page |

## Why generated

The simulator *is* the WebUI: same markup, same CSS, same JS bundle, so every
tab, slider and endpoint behaves like the real device without a board attached.
Maintaining it as a fork meant re-reviewing a 6000-line diff after every UI
change; as a transform, re-syncing is a rerun.

## Re-syncing after a WebUI or firmware change

```bash
# 1. bump the identity block at the top of sim_engine.js:
#    SIM_UI_VERSION  -> data/index.html's UI_VERSION      (always)
#    FW_VERSION      -> platformio.ini's LASER_FW_VERSION (always)
#    FW_COMMIT       -> git rev-parse --short HEAD        (always)
#    SIM_VERSION     -> bump when the sim layer itself changed
# 2. regenerate
.venv/Scripts/python.exe scripts/simulator/build_simulator.py
# 3. verify (fails on any page error)
.venv/Scripts/python.exe scripts/simulator/check_simulator.py
```

`build_simulator.py` refuses to run if `sim_engine.js`'s `FW_VERSION` /
`SIM_UI_VERSION` disagree with `platformio.ini` / `data/index.html`, and if any
of its markup anchors in `data/index.html` no longer match exactly once.

## Scope of the simulation

Ported faithfully (kept in sync with the firmware sources):

- all 81 presets and their live parameters
- interior density, corner dwell, distance-proportional blank jumps and the
  frame-budget squeeze, per optimizer profile (`OPT_PROFILE_DEFAULTS`)
- `presetClassOf()`'s preset → profile mapping
- rotation, mirror, both kaleidoscope modes, points-only mode, autoscale,
  per-side colors, color override, master dimmer
- wall-clock animation phase (`kAnimPhaseFrameMs`), so presets animate at
  device speed regardless of the browser's frame rate
- `/api/state`, `/api/config` (including all 8 optimizer profiles and their
  effective PPS-scaled values), `/api/optimizer-stats` measured off the frame
  actually rendered, plus the warp/brightness/weld/seg-color/text/paint/zone/
  timer/modulator endpoints
- Text Mode (all 3 fonts, all 11 animations, rainbow/flip) and Paint by
  Finger both actually drive the beam preview, matching pattern_engine.cpp's
  source priority (Text > Paint > Preset) -- not just mocked as API state
- Laser Welding (weld_path.h/weld_patterns.cpp): the torch/afterglow/spark
  effect over either source's path, including direction (Fwd/Rev/Ping-Pong)
  and the Reverse seek-to-end behavior

Deliberately not simulated: SD card, ILDA/SVG playback, DMX/Art-Net input, OTA,
arming (refused, there is no hardware), the ZV ringing shaper's sample-level
trajectory, the Dotter, and BPM-synced modulator output.
