# Chapter 5 — The Optimizer

> If Chapter 3 was about telling the firmware what hardware you have, and Chapter 4 was about clicking buttons, this chapter is about understanding what happens between those two things. The optimizer is the core of GalvOS — the piece of software that turns a list of geometric vertices into a sequence of DAC samples that the galvo mirrors can actually follow without vibrating, blurring, or drawing lines in the dark.

## Table of Contents
- [Why an Optimizer?](#why-an-optimizer)
- [The Coordinate System](#the-coordinate-system)
- [Pipeline Overview](#pipeline-overview)
- [Stage 1 — Primitive Generation](#stage-1--primitive-generation)
- [Stage 2 — Transform](#stage-2--transform)
- [Stage 3 — Resample (optional)](#stage-3--resample-optional)
- [Stage 4 — Corner Dwell](#stage-4--corner-dwell)
- [Stage 5 — Blanking (Pillar 2)](#stage-5--blanking-pillar-2)
- [Stage 6 — Velocity Clamp](#stage-6--velocity-clamp)
- [Stage 7 — Acceleration Clamp](#stage-7--acceleration-clamp)
- [Stage 8 — ZV Ringing Compensation (Pillar 3)](#stage-8--zv-ringing-compensation-pillar-3)
- [Stage 9 — DAC Output](#stage-9--dac-output)
- [PPS Scaling](#pps-scaling)
- [Optimizer Profiles](#optimizer-profiles)
- [The Pattern Cache](#the-pattern-cache)
- [The Three Pillars](#the-three-pillars)
- [Known Effective Limit](#known-effective-limit)
- [Parameter Reference](#parameter-reference)

---

## Why an Optimizer?

A galvo laser projector works by bouncing a laser beam off two small mirrors (one for X, one for Y) that are driven by voice-coil actuators — essentially tiny loudspeakers with mirrors glued to them. The firmware commands each mirror to a position by writing a voltage to the galvo driver, and the mirror physically moves to follow that position.

Here is the problem: mirrors have mass. Mass has inertia. Inertia means the mirror cannot follow an instantaneous position command — it takes time to accelerate, overshoot, oscillate, and settle. If you command "jump to the far corner of the screen" and then immediately start drawing lines there, the lines will be drawn while the mirror is still settling, and they will look blurry, curved, or displaced.

Galvo scanners are rated in **kpps** (kilo-points-per-second) — but this rating is measured at a specific scan angle (the ILDA standard ±8° optical) and means the scanner can accurately follow a point-to-point trajectory at that rate for that angle. Wider angles, faster speeds, or sharper directional changes all reduce accuracy.

The optimizer's job is to take the clean geometric description produced by the pattern engine ("draw a hexagon with these six vertices") and turn it into a point stream the hardware can actually follow:

- **Enough points** at sharp corners so the mirror has time to change direction cleanly.
- **Smooth transitions** on blank jumps so the mirror arrives at the next shape without ringing.
- **The right density** of interior points so lines look solid without wasting the frame budget.
- **Protection** against commands that exceed the galvo's physical limits.

---

## The Coordinate System

Pattern vertices use floating-point coordinates in the range **[-32768, +32767]**, matching the DAC's 16-bit signed range. The center of the scan field is (0, 0). By default:

- **+X** = right
- **+Y** = down (screen convention, not mathematical convention)
- DAC code = coordinate + 0x8000 (maps [-32768..32767] onto [0x0000..0xFFFF])

Physical orientation is calibrated via `invert_x`, `invert_y`, and `swap_xy` in the Calibration tab — these are applied before the optimizer so all patterns work in the same logical coordinate space regardless of how your projector is physically oriented.

---

## Pipeline Overview

The optimizer sits between the pattern engine and the DAC output. Every frame, this pipeline runs in order:

```
Pattern Engine
  → [1] Primitive Generation   (vertices + colors as PathSegments)
  → [2] Transform              (rotation, scale, translation — affine)
  → [3] Resample               (optional: constant-spacing point density)
  → [4] Corner Dwell           (extra points at sharp direction changes)
  → [5] Blanking               (smoothstepped, distance-proportional jumps)
  → [6] Velocity Clamp         (subdivide lit steps that are too long)
  → [7] Acceleration Clamp     (limit velocity ramp rate)
  → [8] ZV Ringing Comp        (optional: input shaping on blank jumps)
  → [9] DAC Output             (LaserPoint[] → ISR → DAC8562)
```

Stages 3, 6, 7, and 8 are optional (disabled by default). When disabled, each stage produces output byte-identical to skipping it — there is no penalty for leaving them off until you need them.

---

## Stage 1 — Primitive Generation

Each pattern generates its geometry as a list of **`PathSegment`** objects. A `PathSegment` is a sequence of `PathVertex` entries (x, y, r, g, b coordinates) connected by straight lines, with a flag indicating whether the path is closed (polygon) or open (polyline).

```cpp
struct PathVertex { float x, y; uint8_t r, g, b; bool lift; };
struct PathSegment { const PathVertex* vertices; size_t count; bool closed; };
```

The `lift` flag on a vertex means "blank jump to this vertex from the previous point" — used for disconnected sub-paths like the strokes in a text glyph or the separate edges of a wireframe model.

**Pattern color rule:** All patterns specify only `255` or `0` as default channel values (e.g. pure red = `255,0,0`; yellow = `255,255,0`). Mixed intermediate colors come in through the color override system in the WebUI Global Controls.

**Wireframe note:** For 3D wireframe patterns, `buildWfChains()` must be called to group isolated 2-vertex edges into proper `PathSegment` chains. Without this, each edge is a 2-vertex segment with no `has_incoming || has_outgoing` relationship to its neighbours, and corner dwell never fires — because a 2-vertex segment has no corners.

---

## Stage 2 — Transform

Before any density calculations, every input vertex is passed through a **2×3 affine transform**:

```
x' = a·x + b·y + tx
y' = c·x + d·y + ty
```

This handles rotation (Z-axis), translation (H/V move), and scale (size) in one matrix multiply. The transform is built from the live controls in the WebUI (rotation angle, position, size) and published to the optimizer once per frame.

Non-affine effects — Y/X perspective tilt, DMX wave warp, auto-scale collapse — are applied as post-optimizer point passes because they cannot be expressed as a single affine matrix.

When the transform is the identity (no rotation, no move, size=100%), the output is byte-identical to the pre-transform result. There is no overhead for patterns that don't use transform effects.

---

## Stage 3 — Resample (optional)

By default, interior point density is **length-proportional**: a longer edge gets more points than a shorter edge, scaled by `pts_per_1000_units`. This is the default mode and produces good results for most patterns.

When `resample_enabled = true`, interior density switches to **constant spacing**: every edge gets `length / resample_spacing_units` points, regardless of length. This means:

- A 500-unit edge and a 5000-unit edge get the same points-per-unit density.
- Galvo velocity (DAC units/tick) is constant along every edge in the pattern.
- Useful for patterns where uniform galvo speed across all edges matters more than adaptive density.

The resample stage runs before corner dwell, so corner points are still added on top of the resampled interior density.

When disabled (default), output is byte-identical to the pre-resample optimizer.

---

## Stage 4 — Corner Dwell

A galvo mirror needs time to change direction at a sharp corner. If you command "go to vertex A, then immediately to vertex B at a 90° angle", the mirror will overshoot vertex A, try to reverse, oscillate, and arrive at B late and blurry. Corner dwell fixes this by inserting extra stationary points at each corner — giving the mirror time to decelerate, settle, and reaccelerate.

**How corner severity is calculated:**

The optimizer computes the **exterior angle** at each vertex — the angle between the incoming edge direction and the outgoing edge direction. A straight-through vertex has an exterior angle of 0°. A 90° right-angle corner has an exterior angle of 90°. A full reversal (the beam turns completely around) has an exterior angle of 180°.

If the exterior angle exceeds `corner_angle_deg` (default 25°), the vertex is classified as a corner. The number of extra dwell points is interpolated between `min_corner_pts` (softest qualifying corner) and `max_corner_pts` (sharpest, 180° reversal):

```
corner_pts = lerp(min_corner_pts, max_corner_pts, severity)
```

where `severity` runs from 0 (at `corner_angle_deg`) to 1 (at 180°).

**Edge spacing near corners:**

Interior points along an edge are not uniformly spaced — they are velocity-eased using a continuous `shapeEdgeT()` function. Points are denser near corners (where the mirror is decelerating or accelerating) and sparser in the middle of long straight runs (where the mirror has reached cruising speed). This avoids a velocity kink at the edge midpoint that would excite galvo ringing on every edge in the pattern.

---

## Stage 5 — Blanking (Pillar 2)

When the beam needs to move from one shape to another without drawing a line, the laser is turned off (blanked) and the galvo jumps to the next start point. This is a **blank jump**.

Naive blank jumps — "turn laser off, immediately command new position, turn laser back on" — leave visible artifacts: the mirror overshoots the landing point, and the first few drawn points of the next shape are positioned incorrectly while the mirror is still settling.

GalvOS's blanking pipeline (Pillar 2) solves this with two techniques:

### Distance-Proportional Sample Count

Short blank jumps (adjacent vertices in a wireframe) need fewer settling ticks than long diagonal jumps (jumping across the full scan field). The sample count scales with distance:

```
count = (distance / 1000.0) × blank_pts_per_1000_units
count = clamp(count, min_blank_samples, blank_samples)
```

This means a short jump between adjacent wireframe edges might use only 6 blank samples, while a long cross-screen jump uses up to 16 (or more, with higher `blank_samples`). Every jump pays only what it actually needs.

### S-Curve (Smoothstep) Easing

The galvo position during a blank jump is not commanded to jump instantly to the target — it follows a smoothstep trajectory:

```
position(t) = smoothstep(t) = 3t² - 2t³   for t ∈ [0, 1]
```

This S-curve has zero velocity at both endpoints: the mirror starts and ends the jump gently instead of receiving an instantaneous velocity command at both ends. The result is dramatically reduced overshoot at the landing point and cleaner first-drawn points of the next shape.

**Settle ticks:** The final `min_blank_samples` ticks of the blank jump are spent dwelling at the exact target position — giving the mirror additional time to settle. These settle ticks are carved from the total blank budget (not added on top), capped at `count / 2` to ensure there are always enough move ticks for smooth deceleration.

---

## Stage 6 — Velocity Clamp

When `vel_clamp_enabled = true`, the optimizer runs a post-pass over the lit (non-blank) point stream and subdivides any step that exceeds `max_step_units` DAC units per tick.

For example: if `max_step_units = 200` and a lit step covers 600 DAC units in a single tick, it is subdivided into three steps of 200 units each. The color is also linearly interpolated across the subdivided steps.

**What this protects against:** Very long lit steps command the galvo to cover a large distance in a single output tick. If the step is longer than the galvo can physically follow in one tick period (at the configured kpps), the mirror lags and the line appears curved or displaced.

Blank runs are exempt from velocity clamping — they are already eased by the Pillar 2 blanking stage.

**Default:** Disabled. `max_step_units` must be measured for your specific galvo hardware before enabling. Wrong values can over-subdivide the output (wasting the frame budget) or do nothing useful.

---

## Stage 7 — Acceleration Clamp

When `accel_clamp_enabled = true`, the optimizer runs a second post-pass and inserts a midpoint between any two consecutive steps where the step magnitude increases by more than `max_accel_units` DAC units/tick².

This limits how quickly the galvo velocity can increase — easing hard velocity ramps into corners and preventing sharp speed-up events that excite ringing.

**What this protects against:** Even if individual steps are within the velocity limit, a sudden jump from slow to fast (or vice versa) — a velocity ramp — can excite the galvo's mechanical resonance. The acceleration clamp smooths these ramps.

**Default:** Disabled. Tune alongside `vel_clamp_enabled`.

---

## Stage 8 — ZV Ringing Compensation (Pillar 3)

Even with smoothstepped blank jumps, the galvo mirror can ring at its natural mechanical resonance frequency after arriving at a new position. This appears as a visible oscillation in the first few drawn points of a shape — particularly visible at high kpps or wide scan angles.

GalvOS implements **Zero-Vibration (ZV) input shaping** on blank jumps. The idea is elegant: instead of commanding a single trajectory to the target position, the optimizer commands two overlapping trajectories — a smaller one at t=0, and a second smaller one delayed by exactly half the galvo's damped oscillation period. The mechanical response to the first trajectory and the second cancel each other out destructively, leaving no residual vibration at the landing point.

### How it works

The two impulses are sized using the galvo's damping ratio (ζ) and natural frequency (ω_n):

```
K   = exp(-ζ × π / √(1 - ζ²))
A1  = 1 / (1 + K)      ← amplitude of the first impulse
A2  = K / (1 + K)      ← amplitude of the second impulse
td  = π / (ω_n × √(1 - ζ²))   ← half the damped oscillation period
shift_pts = round(td / tick_period)   ← delay in output points
```

Each output point in the blank jump is then:

```
shaped[i] = A1 × unshaped[i] + A2 × unshaped[i - shift_pts]
```

where `unshaped[i]` is the smoothstep trajectory computed by Stage 5.

When `ringing_comp_enabled = false` (the default), A1=1 and A2=0 — the shaped output is byte-identical to the unshaped Pillar 2 trajectory. No overhead, no change.

### Measuring the parameters

`ring_freq_hz` and `ring_damping_ratio` must be **measured on your specific hardware** before enabling. Wrong values can make ringing worse, not better. The measurement procedure:

1. Configure a pattern with a long blank jump (full-width diagonal).
2. Connect an oscilloscope to the galvo position feedback signal (if available on your driver board) or observe the projected result closely with a camera at slow shutter speed.
3. Capture the step response of the galvo — the oscillation after a large step input.
4. Measure the oscillation frequency (Hz) → `ring_freq_hz`.
5. Measure the decay envelope over several cycles → compute `ring_damping_ratio` (typically 0.05–0.3 for galvo scanners).
6. Enter the values, enable `ringing_comp_enabled`, and verify the ringing is reduced.

---

## Stage 9 — DAC Output

The final `LaserPoint[]` array is written to the DAC ISR ring buffer. The ISR runs at `GALVO_SAMPLE_RATE_HZ` (default 30,000 Hz) on Core 1. Each tick:

1. One `LaserPoint` is dequeued from the ring buffer.
2. The X coordinate is converted to a 16-bit DAC code: `dac_x = x + 0x8000`.
3. The DAC code is clamped to `[dac_limit_min, dac_limit_max]`.
4. The DAC8562 SPI write is performed via raw hardware register access (not IDF polling) — this achieves ~30,300 samples/sec throughput.
5. The RGB PWM duty is updated via LEDC: the color goes through `mapVisibleRange()` → `applyGamma()` → `master_dimmer` scaling → `gain_r/g/b` white balance → LEDC duty.

**Critical constraint:** `updateSnapshot()` (which snapshots the live config for the ISR) must fire **once per frame boundary**, not once per point. At 30 kpps, each point has a 33 µs budget. Reading the live config struct inside the per-point ISR would exceed this budget and cause ring buffer underruns.

---

## PPS Scaling

The optimizer parameters are tuned at a specific galvo output rate. When you change `galvo_kpps` away from `galvo_rated_kpps`, the optimizer automatically rescales three parameters to compensate:

```
r = galvo_rated_kpps / galvo_kpps   (the "headroom ratio")

pts_per_1000_units ×= 1/r    ← fewer points/unit at lower kpps (more time per tick)
max_step_units     ×= r      ← larger steps allowed at lower kpps (more distance per tick)
max_accel_units    ×= r²     ← acceleration scales as the square of the rate
```

**At `galvo_kpps == galvo_rated_kpps`:** r = 1, all values unchanged.

**At half the rated speed (e.g., kpps=7.5 on a 15K galvo):** r = 2 → interior density is halved (each tick is twice as long, so you need half as many points to cover the same distance in the same time), velocity ceiling is doubled, acceleration ceiling is quadrupled.

This scaling is applied in `applyPpsScaling()`, which is called by every `liveOptimizerConfig()` implementation — all four pattern families (presets, curves, text, paint) go through this path.

The WebUI Optimizer tab shows the **effective values** (`opt_eff_*`) after scaling — these are the values the optimizer actually uses.

---

## Optimizer Profiles

GalvOS maintains six independent optimizer profiles, one per preset class. The active profile switches automatically when a preset is activated, based on `presetClassOf()`:

| Profile | Index | Preset class | Primary workload |
|---------|-------|-------------|-----------------|
| Vector | 0 | Closed polygons, stars, geometric shapes | Corner dwell |
| Smooth | 1 | Continuous closed curves, spirals | Interior density |
| Waves | 2 | Open polylines, wave patterns | Velocity clamp |
| Wireframe | 3 | 3D edge chains, wireframe models | Corner dwell + short blank jumps |
| MultiObject | 4 | Several separate closed objects | Long blank jumps |
| Particles | 5 | Isolated dots, starfields | Blank jumps only |

Each profile is independently tunable via the Optimizer tab. The profiles share the same parameter set (`OptimizerLiveConfig`) but can have completely different values. For example:

- A Vector profile might have high `max_corner_pts` (clean polygon corners) and moderate `blank_samples` (few objects, short jumps).
- A Particles profile might disable corner dwell entirely (dots have no corners) and optimize entirely for fast, accurate blank jumps.

Profiles are stored in NVS keyed by a per-profile suffix (`_s`, `_c`, `_w`, `_3`, `_sol`, `_sc`). The suffix is pinned to the profile index, not the profile name, so renaming a profile does not reset its stored values.

---

## The Pattern Cache

Computing geometry for a complex 3D preset at 30 kpps generates the same output every frame when nothing changes — it is wasteful to recompute it. GalvOS maintains a **single-slot PSRAM pattern cache** for static presets.

When a preset is activated that appears on the `isStaticPreset` allowlist, the optimizer computes the `LaserPoint[]` output once and caches it. Subsequent frames reuse the cached result directly, bypassing the entire optimizer pipeline.

The cache is invalidated (and recomputed on the next frame) whenever `gPatternCacheGen` is incremented. This counter is bumped on any optimizer parameter change or `galvo_kpps` change — because both affect the optimizer output.

**Size:** The cache holds one frame's worth of `LaserPoint[]` data in PSRAM. At `PATTERN_POINTS_MAX = 2048` points × 8 bytes each = 16 KB.

---

## The Three Pillars

The optimizer's main features are described by three "pillars":

**Pillar 1 — Adaptive Point Density**
Corner-aware, length-proportional point density. Corners get extra dwell points scaled to their severity. Edges get interior points proportional to their length. This is always active and forms the foundation of the optimizer.

**Pillar 2 — S-Curve Blanking**
Distance-proportional, smoothstep-eased blank jumps. Short jumps are cheap, long jumps are smooth. Settle ticks are carved from the blank budget. Always active.

**Pillar 3 — ZV Ringing Compensation**
Zero-Vibration input shaping on blank jump trajectories. Requires measured hardware parameters. Disabled by default. Hardware-verified on the JY-15K-BL at 30 kpps.

All three pillars are hardware-verified on the Jolooyo JY-15K-BL galvo set.

---

## Known Effective Limit

**`opt_max_pts_per_frame: 1300`** — no optical improvement is observed above this value on the JY-15K-BL hardware at 30 kpps. Above this point, the per-point time budget (33 µs) becomes the limiting factor, not the number of points. Setting this higher wastes frame budget without improving image quality.

The default is 1010 (tuned for a ~30 Hz flicker-free floor at 30 kpps: 30000 / 1010 ≈ 30 frames/sec).

---

## Parameter Reference

Full table of all optimizer parameters, their defaults, valid ranges, and effects.

| Parameter | Default | Range | Effect |
|-----------|---------|-------|--------|
| `corner_angle_deg` | 25.0° | 0–180° | Minimum exterior angle to classify as a corner and add dwell points. Lower values add dwell at gentler bends; 0 adds dwell at every vertex. |
| `min_corner_pts` | 2 | 0–255 | Points added at the softest qualifying corner (exterior angle just above `corner_angle_deg`). |
| `max_corner_pts` | 8 | 0–255 | Points added at the sharpest corner (full 180° reversal). |
| `pts_per_1000_units` | 6.0 | 0–100 | Interior point density: points added per 1000 DAC units of segment length. After PPS scaling. |
| `min_segment_pts` | 2 | 1–255 | Minimum interior points per segment, regardless of length. Prevents very short edges from having zero interior points. |
| `blank_samples` | 16 | 1–100 | Maximum blank jump sample count (ceiling for distance-proportional scaling). |
| `min_blank_samples` | 6 | 1–50 | Minimum blank jump sample count (floor + settle dwell ticks). |
| `blank_pts_per_1000_units` | 8.0 | 0–100 | Rate at which blank jump sample count scales with jump distance. |
| `stage1_blank_target` | 16 | 1–100 | Stage 1 budget reduction target: when the frame is over budget, blank_samples is first reduced to this value before falling back to `min_blank_samples`. |
| `max_pts_per_frame` | 1010 | 1–2048 | Total point budget per frame. When exceeded, interior density is scaled down uniformly. Effective ceiling: 1300 on JY-15K-BL. |
| `min_interior_pts_per_seg` | 8 | 0–255 | Interior points reserved per segment before blank budget is computed. Prevents very complex patterns from eliminating interior points entirely. |
| `resample_enabled` | false | bool | Enable constant-spacing resample stage. Off = length-proportional density (default). |
| `resample_spacing_units` | 160.0 | 1–10000 | Target spacing between resampled points in DAC units. Only used when `resample_enabled = true`. |
| `ringing_comp_enabled` | false | bool | Enable ZV input shaping. **Measure `ring_freq_hz` and `ring_damping_ratio` on hardware first.** |
| `ring_freq_hz` | 200.0 | 1–2000 | Galvo mechanical resonant frequency in Hz. Must be measured. |
| `ring_damping_ratio` | 0.15 | 0.01–0.9 | Galvo damping ratio ζ. Must be measured. Typical range: 0.05–0.3. |
| `vel_clamp_enabled` | false | bool | Enable velocity clamp post-pass. Tune `max_step_units` before enabling. |
| `max_step_units` | 200.0 | 1–65535 | Maximum lit-step size in DAC units per tick. Steps exceeding this are linearly subdivided. After PPS scaling. |
| `accel_clamp_enabled` | false | bool | Enable acceleration clamp post-pass. |
| `max_accel_units` | 800.0 | 1–65535 | Maximum per-tick change in step magnitude (DAC units/tick²). After PPS scaling. |
