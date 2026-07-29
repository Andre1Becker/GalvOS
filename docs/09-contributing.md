# Chapter 9 — Contributing

GalvOS is a one-person project that grew considerably beyond its original scope. Community contributions are genuinely welcome — there are known bugs to fix, patterns to add, animations to repair, and features that need a second pair of hands. This chapter explains how the codebase is structured, what the code standards are, and the exact workflow for submitting changes.

## Table of Contents

- [Where to Start](#where-to-start)
- [Repository Structure](#repository-structure)
- [Code Standards](#code-standards)
- [Adding a New Preset Pattern](#adding-a-new-preset-pattern)
- [Contributing a Community Preset](#contributing-a-community-preset)
- [Adding a New Calibration Pattern](#adding-a-new-calibration-pattern)
- [Adding a Modulator Module](#adding-a-modulator-module)
- [Adding a New API Endpoint](#adding-a-new-api-endpoint)
- [Modifying the WebUI](#modifying-the-webui)
- [Patch Workflow](#patch-workflow)
- [Commit Messages](#commit-messages)
- [Testing](#testing)
- [Areas That Need Help](#areas-that-need-help)

---

## Where to Start

Browse [Chapter 10 — Known Issues & To-dos](10-known-issues-and-todos.md) for a current list of open bugs and planned features. Pick something that matches your skills and interests:

- **C++ firmware bugs** — calibration pattern fixes, the `curve_patterns` dead-code cleanup
- **C++ new features** — new patterns, new modulator modules (see [Adding a Modulator Module](#adding-a-modulator-module)), extending the camera-in-the-loop auto-tuning API (Chapter 6) to more optimizer profiles
- **JavaScript/HTML** — WebUI improvements, a dedicated Dotter UI
- **Python** — `scripts/optimizeGalvo/` (camera auto-tuning tool, see Chapter 6)
- **Documentation** — screenshot capture (the WebUI got a full visual rewrite in v6.32.0 — most screenshots want re-taking), diagram creation, corrections
- **Hardware** — the Helios USB stub (TinyUSB vendor class vs. USB-CDC debug conflict), the planned ILDA output header

Open an issue or a discussion on GitHub before starting larger changes — it avoids duplicate work.

---

## Repository Structure

```text

GalvOS/
├── src/
│   ├── main.cpp                    # Entry point, global init, FreeRTOS task creation
│   ├── bpm_clock.{cpp,h}           # Global BPM clock (Manual/Tap/DMX, v6.21.0)
│   ├── sequencer.{cpp,h}           # BPM-synced preset sequencer (v6.22.0)
│   ├── modulator_engine.{cpp,h}    # 8-slot modulation matrix + registry (v6.23.0/v6.27.0)
│   ├── control/
│   │   ├── dmx_in.{cpp,h}          # DMX-512 receive (UART1 + MAX485)
│   │   └── encoder.{cpp,h}         # Rotary encoder (currently unconnected hardware)
│   ├── ilda/
│   │   └── ilda_player.{cpp,h}     # ILDA .ild file parser and player
│   ├── net/
│   │   ├── web_ui.{cpp,h}          # WebUI HTTP server — all API routes
│   │   ├── artnet_in.{cpp,h}       # Art-Net UDP receiver
│   │   ├── etherdream.{cpp,h}      # Ether Dream protocol
│   │   ├── helios_net.{cpp,h}      # Helios DAC network emulation (TCP 7768)
│   │   ├── helios_usb.{cpp,h}      # Helios USB DAC protocol (stub)
│   │   ├── osc_in.{cpp,h}          # OSC 1.0 receiver (UDP 9000)
│   │   ├── sacn_in.{cpp,h}         # sACN/E1.31 receiver (multicast, universe 1)
│   │   ├── community_presets.{cpp,h} # Community preset storage (LittleFS)
│   │   ├── backup_manager.{cpp,h}  # Config backup/restore (JSON snapshot)
│   │   ├── ota_update.{cpp,h}      # Over-the-air firmware update
│   │   └── ntp_client.{cpp,h}      # NTP time sync
│   ├── output/
│   │   └── galvo_out.{cpp,h}       # Galvo ISR, DAC8562 SPI, LEDC RGB PWM, ring buffer
│   ├── patterns/
│   │   ├── pattern_engine.{cpp,h}  # Frame scheduler, transform, preset dispatch
│   │   ├── point_optimizer.{cpp,h} # The optimizer pipeline (see Chapter 5)
│   │   ├── preset_patterns.{cpp,h} # All built-in presets + PresetClass assignment
│   │   ├── calib_patterns.{cpp,h}  # Calibration patterns
│   │   ├── curve_patterns.{cpp,h}  # Mathematical parametric curves (backend only — WebUI card removed)
│   │   ├── camera.{cpp,h}          # Camera module: 3D view targets for the modulator registry (v6.28.0)
│   │   ├── duplicator.{cpp,h}      # Duplicator module: grid/radial/spiral frame cloning (v6.29.1)
│   │   ├── dotter.{cpp,h}          # Dotter module: Points-Only dot scatter (v6.31.0)
│   │   ├── spatial_noise.{cpp,h}   # NOISE2D modulator type (v6.30.0)
│   │   ├── text_renderer.{cpp,h}   # Vector text glyph renderer
│   │   ├── paint_patterns.{cpp,h}  # Paint-by-finger canvas renderer
│   │   └── countdown_timer.{cpp,h} # Countdown timer preset
│   ├── safety/
│   │   └── safety.{cpp,h}          # Hardware interlock aggregation, E-Stop, watchdog
│   ├── sensors/
│   │   └── temp_monitor.{cpp,h}    # DS18B20 1-Wire + fan PWM control
│   ├── storage/
│   │   ├── sd_card.{cpp,h}         # SD card (FAT32, independent SPI3 bus)
│   │   └── playlist.{cpp,h}        # ILDA playlist management
│   └── util/
│       ├── log_buffer.{cpp,h}      # Ring log buffer (WebUI log stream)
│       ├── cpu_monitor.{cpp,h}     # Per-core CPU load tracking
│       ├── param_meta.{h}          # Parameter metadata for the modulator registry
│       └── stack_mon.{cpp,h}       # FreeRTOS task stack monitoring
├── include/
│   ├── config.h                    # RuntimeConfig, OptimizerLiveConfig, all shared types
│   ├── pinmap.h                    # GPIO assignments
│   ├── json_alloc.h                # PSRAM JSON allocator
│   └── mutex.h                     # Named mutexes
├── data/
│   └── index.html                  # Single-file WebUI PWA (HTML + CSS + JS)
├── community-presets/
│   ├── index.json                  # Preset index fetched by the WebUI's GitHub Browser
│   ├── builder.html                # Community Preset Builder (offline browser tool)
│   └── *.json                      # Community preset bundles
├── scripts/
│   ├── upload_all.py               # Custom PlatformIO target: flash firmware + LittleFS
│   ├── gzip_assets.py              # Pre-build hook: gzip data/ assets
│   ├── ov9281_probe.py             # Standalone OV9281 camera capability probe
│   ├── test_protocols.py           # Manual Ether Dream/Helios stream test client (v6.20.2)
│   └── optimizeGalvo/              # Host-side camera-in-the-loop auto-tuning tool (see Chapter 6)
│       └── optimizeGalvo.py        # OpenCV + Optuna, drives /api/calib-cam/*
├── hardware/
│   └── netlist.txt                 # Full wiring netlist
└── platformio.ini                  # Build configuration
```

---

## Code Standards

### Language

All code, comments, log messages, and commit messages must be in **English**. This applies to everything in the repository including `index.html`, JavaScript, and inline HTML strings.

### Style

- **camelCase** for all variable and function names
- Clean, readable code over clever one-liners
- Explicit over implicit — name your variables well
- Comments explain *why*, not *what* (the code explains what)
- No trailing whitespace; Unix line endings

### Memory

- Buffers larger than ~16 KB belong in PSRAM: use `ps_malloc()` or `heap_caps_malloc(MALLOC_CAP_SPIRAM)`
- All `JsonDocument` instances must use `SpiRamAllocator`: `JsonDocument doc(&jsonAllocator())`
- API responses that build JSON must use `sendJsonPsram(req, doc)` (chunked, PSRAM buffer)
- Never allocate large buffers on the stack — the FreeRTOS task stacks are fixed-size

### Thread Safety

- The galvo ISR runs on Core 1 at 30,000 Hz. Only IRAM-safe functions may be called from it.
- Shared state between Core 0 and Core 1 uses `std::atomic<>` for scalar flags, and named mutexes (from `mutex.h`) for structs.
- The `LOCK_STATE()` macro acquires `mtx::state` — use it when writing to `gLivePreset` rotation fields from the web handler.
- Never hold a mutex inside the ISR.

### Pattern Color Rule

All patterns that produce colored output must use only `255` or `0` as default channel values (e.g. pure red = `{255, 0, 0}`, yellow = `{255, 255, 0}`). Intermediate values like `{100, 243, 9}` are forbidden as defaults. Mixed colors arrive via the `col_override` system in `gLivePreset`.

### WebUI JavaScript

- No `localStorage`, `sessionStorage`, or any browser storage API — these fail in the Claude.ai artifact sandbox and are unavailable in the embedded WebUI context.
- Use `fetch()` for all API calls, not `XMLHttpRequest`.
- All state is held in JS variables for the session lifetime.
- Validate JSON syntax: extract all `<script>` blocks and run `node --check` before submitting.

---

## Adding a New Preset Pattern

Presets live in `src/patterns/preset_patterns.{cpp,h}`. The current count is 74 (`PRESET_COUNT = 74`).

### Step 1 — Declare the preset

In `preset_patterns.h`, add your preset to the `Preset` enum:

```cpp
enum class Preset : int8_t {
    // ... existing presets ...
    MyNewPattern = 74,
    // ...
};
```

Update `PRESET_COUNT` in the header:

```cpp
constexpr uint8_t PRESET_COUNT = 75;
```

### Step 2 — Assign a PresetClass

In `preset_patterns.cpp`, add a case in `presetClassOf()` to assign your pattern to the appropriate optimizer profile:

```cpp
PresetClass presetClassOf(Preset p) {
    switch (p) {
        // ...
        case Preset::MyNewPattern: return PresetClass::Vector;
        // ...
    }
}
```

Profile assignment guide:

- Closed polygons, straight edges → `Vector`
- Continuous smooth curves → `Smooth`
- Open polylines, wave shapes → `Waves`
- 3D edge chains → `Wireframe`
- Multiple separate objects → `MultiObject`
- Isolated points, starfield → `Particles`

### Step 3 — Add to the PRESETS table

Add a `PresetInfo` entry to the `PRESETS[]` array:

```cpp
const PresetInfo PRESETS[PRESET_COUNT] = {
    // ...
    { Preset::MyNewPattern, "My Pattern", PresetClass::Vector },
    // ...
};
```

### Step 4 — Implement the generator function

Add a `static void p_myNewPattern(LaserPoint* buf, size_t& n, ...)` function and call it from the dispatch switch in `pattern_engine.cpp`.

The generator writes `PathSegment` arrays and calls `optimizer::optimize()`:

```cpp
static void p_myNewPattern(LaserPoint* buf, size_t& n, size_t max,
                            const OptimizerConfig& cfg) {
    // Build vertices
    static PathVertex verts[6];
    for (int i = 0; i < 6; i++) {
        float angle = i * (2.0f * M_PI / 6.0f);
        verts[i] = PathVertex(cosf(angle) * 20000.f,
                              sinf(angle) * 20000.f,
                              255, 0, 0);  // pure red — no intermediate values
    }
    PathSegment segs[1] = { PathSegment(verts, 6, true) };  // closed hexagon
    n = optimizer::optimize(segs, 1, buf, max, cfg);
}
```

### Step 5 — Add an SVG thumbnail

Add a small SVG thumbnail for the preset grid in `data/index.html`. The `STATIC_PRESET_DEFS` object maps preset index to SVG string. Keep thumbnails simple — they are rendered at 80×80 px.

### Step 6 — Bump the version

Update `LASER_FW_VERSION` in `platformio.ini`. New preset = minor or patch bump depending on scope.

---

## Contributing a Community Preset

Not every contribution needs a compiler. Community presets are JSON bundles hosted in [`community-presets/`](../community-presets/) — the WebUI's Preset Manager fetches `index.json` from GitHub and lets users download them straight to their device. A bundle carries:

- **`meta`** — `id`, `name`, `author`, `description`, `tags`, `schema_version` (currently 1)
- **`optimizer_profile`** — a full optimizer tuning, same field names and bounds as `/api/optimizer-live`
- **`preset_params`** — which built-in preset to run (`preset_idx`) plus `col_r/g/b`, `speed`, `size_val`

See [`community-presets/shooting-stars-v1.json`](../community-presets/shooting-stars-v1.json) for a complete example.

### Workflow

1. **Open the builder** — [`community-presets/builder.html`](../community-presets/builder.html) runs entirely offline in your browser. No backend, no install, no excuses.
2. **Simple Mode** — pick a built-in preset, set color/speed/size, tune the optimizer fields, then **Export Device JSON**. That file is the deliverable.
3. **Advanced Mode** — a point/curve editor for custom geometry. Note: custom points have **no on-device format** (built-in presets are fixed C++ generators), so "Export Device JSON" is disabled there. Instead, **Export C++ Snippet** gives you a generator function to hand-paste into `preset_patterns.cpp` (then follow [Adding a New Preset Pattern](#adding-a-new-preset-pattern)), or **Save Session JSON** to keep working on it later.
4. **Test on your device** — POST the exported JSON to your own controller and activate it:

   ```bash
   curl -X POST http://<device-ip>/api/community/save -d @my-preset.json
   curl -X POST http://<device-ip>/api/community/activate -d '{"id":"my-preset"}'
   ```

5. **Submit a PR** — add `<id>.json` to `community-presets/` and append a matching entry (`id`, `name`, `author`, `description`, `tags`, `file`) to `community-presets/index.json`.

### Validation limits

The firmware rejects anything that fails these checks, so save yourself a review round-trip:

- Max **10 KB** per preset file
- `id`: lowercase `[a-z0-9-]` only, max 64 chars
- `schema_version` must be `1`
- `max_pts_per_frame` ≤ **1300** — a downloaded preset must not request a bigger frame budget than any built-in profile uses

---

## Adding a New Calibration Pattern

Calibration patterns live in `src/patterns/calib_patterns.{cpp,h}`.

Key rule: **do not call `applyGamma()` inside `colorOut()` if `rgbWrite()` in `galvo_out.cpp` will call it again.** Each color value must go through the gamma LUT exactly once. This is the double-gamma bug pattern — see [Chapter 7 — Troubleshooting](07-troubleshooting.md#color--calibration-issues).

Register the new pattern in the calibration pattern list (returned by `/api/calib-pattern/list`) and add a dispatch case in the calibration pattern handler.

---

## Adding a Modulator Module

Since v6.27.0 the modulator engine is registry-based, and this is the intended extension point for anything that should be *animatable*. A module lives in its own `.cpp/.h` under `src/patterns/` and registers its types/targets from its own `init()` (called after `modulator::init()` in `main.cpp`) via `registerModType()` / `registerWaveShape()` / `registerModTarget()` — **without editing `modulator_engine.h/.cpp` or the WebUI at all**. The WebUI's Bindings dropdown discovers new targets automatically through `GET /api/modulators/meta`.

Study the four existing modules as templates, in increasing complexity: `dotter.cpp` (one target), `camera.cpp` / `duplicator.cpp` (five targets each, consuming `modulator::apply()` in their render hook), and `spatial_noise.cpp` (registers a whole modulator *type* and shares the engine's BPM time base via `modulator::totalCycles()`). Pick target id constants that don't collide with the ones already assigned (see the `target_id` namespaces across those headers — currently 0–20 are taken).

---

## Adding a New API Endpoint

All endpoints are registered in `src/net/web_ui.cpp` inside the `webui::init()` function.

**Route registration rules:**

1. Register specific routes **before** any prefix-matching wildcard handler that would capture the same path. Failure to do this results in 404 on the specific route.
2. The two known order-sensitive cases are `/api/calib-pattern/stop` (before `/api/calib-pattern`) and `/api/text/vertices` (before `/api/text`).
3. Always place `serveStatic()` last.

**Memory rules for handlers:**

- Use `JsonDocument doc(&jsonAllocator())` for all JSON parsing and generation.
- Use `sendJsonPsram(req, doc)` for JSON responses, not `req->send()` with a serialized String.
- For small fixed-format responses, `snprintf` into a local `char buf[N]` is acceptable and avoids allocator overhead.

**Authentication:** Write endpoints must call `isAuthorised(req)` and `denyUnauth(req)` if the check fails:

```cpp
s_server.on("/api/my-endpoint", HTTP_POST,
    [](AsyncWebServerRequest* req) {},
    nullptr,
    [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
        if (!isAuthorised(req)) { denyUnauth(req); return; }
        // ... handle request
    });
```

---

## Modifying the WebUI

The entire WebUI is a single file: `data/index.html`. This is intentional — single-file deployment to LittleFS, served pre-compressed by `gzip_assets.py`.

**After any change to `data/`:** run `pio run --target upload_all` to rebuild the LittleFS image and reflash. A firmware-only `upload` does not update the WebUI.

**JS syntax check:** Before submitting, extract all `<script>` blocks and run:

```bash
node --check <script-file.js>
```

**HTML structure check:** Verify `<div>` / `</div>` balance globally:

```bash
grep -c "<div" data/index.html
grep -c "</div>" data/index.html
```

These counts must match. A single unclosed `<div>` can break entire tab sections silently.

---

## Patch Workflow

This is the non-negotiable workflow for generating patches. It exists to prevent patches that silently fail to apply because they were generated against a stale local copy.

### 1. Clone fresh

```bash
git clone --depth 1 https://github.com/Andre1Becker/GalvOS.git /tmp/galvos-patch
cd /tmp/galvos-patch
```

Always clone fresh before generating a patch. Never work from a local copy that may have drifted from the current `main`.

### 2. Note the HEAD commit hash

```bash
git rev-parse HEAD
```

Record this. If the upstream receives a commit between your clone and patch delivery, the hashes will not match — you must re-clone and regenerate.

### 3. Make edits using safe string replacement

For Python-based patching (the recommended approach for complex HTML or minified JS):

```python
with open("data/index.html", "r") as f:
    content = f.read()

old = 'exact string to replace — must appear exactly once'
new = 'replacement string'

count = content.count(old)
assert count == 1, f"Expected 1 occurrence, found {count}"
content = content.replace(old, new, 1)

with open("data/index.html", "w") as f:
    f.write(content)
```

The `assert count == 1` guard prevents silent no-ops where the old string was not found, and prevents unintended double-replacements.

For version strings in `platformio.ini` — always use Python replacement, not `sed`. The escaped quotes in `-D LASER_FW_VERSION=\\\"x.x.x\\\"` make `sed` patterns fragile.

### 4. Generate the diff

```bash
git diff > my-change.diff
```

### 5. Validate against a second fresh clone

```bash
git clone --depth 1 https://github.com/Andre1Becker/GalvOS.git /tmp/galvos-validate
git -C /tmp/galvos-validate apply --check /path/to/my-change.diff
```

`--check` performs a dry run without modifying files. If it exits clean (no output), the patch applies correctly against the current HEAD. If it errors, the upstream has moved — re-clone and regenerate.

### 6. Deliver the diff file

Submit a single standard `.diff` file. Do not include multiple variant files, do not inline patch hunks in issue comments.

---

## Commit Messages

English, imperative mood ("Add", "Fix", "Refactor", "Remove" — not "Added", "Fixed", "Refactored").

**Format:**

```text
<type>: <short summary (50 chars max)>

<body — what changed and why, wrapped at 72 chars>

<optional: Resolves / See also>
```

**Types:** `feat`, `fix`, `refactor`, `docs`, `test`, `chore`

**Examples:**

```text
fix: correct Hypotrochoid parameter mapping

The parameter mapping in p_hypotrochoid() applied the outer radius
to the inner slot, producing a star shape instead of the intended
curve. Swapped R and r parameters to match the mathematical definition.
```

```text
feat: add Shooting Star preset

Implements a single bright point moving on a randomized parabolic
arc with a 4-point fading trail. Uses Particles optimizer profile.
Adds SVG thumbnail at preset index 75. Bumps PRESET_COUNT to 75.
```

```text
fix: register /api/calib-pattern/stop before prefix handler

ESPAsyncWebServer matches routes in registration order. The stop
route was registered after /api/calib-pattern, causing 404 on stop
requests. Moved registration to before the prefix handler.
Resolves: "POST /api/calib-pattern/stop returns 404"
```

**Version bump rule:**

- Single isolated bug fix → patch bump (x.x.N)
- New feature, new preset, new API endpoint → minor bump (x.N.0)
- Broad refactor touching many call sites → minor bump even if technically a fix
- Architectural change → major bump (N.0.0)

---

## Testing

There is currently no automated test suite. Manual testing is required before submitting.

### Firmware changes

**Host-compile check** (for optimizer and pattern engine logic — no hardware needed):

```bash
g++ -std=gnu++11 -I include -D UNIT_TEST \
    src/patterns/point_optimizer.cpp \
    tests/cfg_stub.cpp \
    -o test_optimizer && ./test_optimizer
```

A `cfg_stub.h` shim replaces ESP32-specific headers (`config.h`, `pinmap.h`, Arduino types) with host-compatible stubs. This allows the optimizer's geometry logic to be tested on a regular Linux/macOS machine.

**On-hardware testing:**

1. Flash with `pio run --target upload_all`.
2. Open the WebUI and exercise the changed feature.
3. Open the serial monitor and verify no panics, overflow messages, or unexpected errors.
4. If the change touches the optimizer: run the Autotune and verify the output rate is stable.
5. If the change touches safety: verify E-Stop and ARM/DISARM still work correctly.

### WebUI changes

1. Run `node --check` on all modified `<script>` blocks.
2. Verify `<div>` / `</div>` balance with `grep -c`.
3. Flash with `pio run --target upload_all` (not just `upload`).
4. Test in Chrome, Firefox, and if possible Safari (iOS).
5. Test with browser DevTools → Network tab open: verify no API calls return errors.

### API endpoint changes

1. Test with `curl` before testing through the WebUI:

```bash
# Read state
curl http://galvOS.local/api/state

# Write with auth token
curl -X POST http://galvOS.local/api/preset \
  -H "Content-Type: application/json" \
  -H "X-Auth: <token>" \
  -d '{"idx": 5}'
```

2. Verify the route registration order is correct (see [Adding a New API Endpoint](#adding-a-new-api-endpoint)).

---

## Areas That Need Help

Roughly in priority order:

**Bug fixes (firmware):**

- Text: Bounce animation has no effect
- Text: Typewriter runs once only — add loop logic
- Text: Star Wars Scroll direction and rendering

**Hardware:**

- SD card / DAC SPI bus contention — root cause found and fixed in firmware (v5.90.0, SD moved to independent SPI3). Remaining work is physically rewiring the perfboard (SD → GPIO5/6/1/42) — see [Known Issues](10-known-issues-and-todos.md#critical-issues).

**New patterns (firmware):**

- Endless Spiral
- Endless Tunnel
- Mandelbrot Animation
- Shooting Star

**UI improvements (JavaScript/HTML):**

- Point limit display in the status/telemetry bar
- Point and stroke count shown under the Paint canvas
- Feature toggles (enable/disable ArtNet, DMX independently)
- kpps history graph on the Dashboard

**Camera-in-the-loop auto-tuning (Python + firmware, see Chapter 6):**

- Extend `/api/calib-cam/*` and `optimizeGalvo.py` to the Wireframe/Trails/Text optimizer profiles (currently camera-tunable: Vector, Smooth, Waves, MultiObject)
- Auto-tune galvo geometry calibration (offset/gain) from the camera, not just optimizer scan/dwell parameters

**Calibration (firmware + UI):**

- Fix calibration channel selector
- Fix ILDA Standard Test Pattern output
- Fix remaining broken calibration patterns

**Infrastructure:**

- Host-compile test harness (`cfg_stub.h`) — improve coverage
- Automated JS syntax check in CI
- OTA update reliability improvements
