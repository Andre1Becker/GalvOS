# Optimizer Parameter Range Audit — 2026-08-17

Camera-validated practical min/max ranges for `OptimizerLiveConfig`, measured live on the
bench rig described in [Chapter 6](06-camera-autotuning.md).

**Nothing was applied or persisted.** All overrides went through `/api/calib-cam/params`
(RAM-only, restored on session stop). No `--apply`, no `/api/optimizer-save`, no NVS write,
no change to `include/config.h`, `docs/05-optimizer.md`, or any firmware source. `git status`
was clean before and after.

---

## 1. Blocking finding — the camera measurement chain was returning garbage

**This has to be fixed before any number this tool has ever produced can be trusted, including
the tuned values currently persisted on the device.**

`camConfig.json` had `binaryThreshold: 133`. On this rig the beam's post-background-subtraction
peak is **149** (raw capture max 164), so thresholding at 133 left **5 lit pixels** in the whole
warped canvas:

| binaryThreshold | lit px in DAC canvas | noise px |
| --- | --- | --- |
| 30 | 9084 | 1344 |
| 40 | 5650 | 359 |
| 45 | 5027 | 22 |
| **50** | **4188** | **0** |
| 60 | 2600 | 0 |
| 120 | 28 | 0 |
| **133 (as configured)** | **5** | **0** |

`computeMetrics()` has two guard branches that fire on a near-empty trace
(`optimizeGalvo.py:2867` and `:2886`): `brightnessNonUniformity` is forced to `1.0` and
`offsetX/YUnits` to `dacRange` — the literal "nothing visible" worst case. Both were firing on
every measurement, while `annotateCanvas()` drew from `dacImage` (pre-threshold) and therefore
still showed a perfect-looking square. That is why the failure was not obvious on screen.

Re-checking the previous session's own trial logs (`results/optimize_*.jsonl`, 2026-08-17
22:13–22:23) — **every single measurement hit at least one fallback branch**:

| Study | pattern-measurements | fully blind (`pdev` = 60000) | partial fallback |
| --- | --- | --- | --- |
| `optimize_Vector` | 40 | 0 | 27 |
| `optimize_Smooth` | 20 | 1 | 19 |
| `optimize_Waves` | 20 | 0 | 20 |
| `optimize_MultiObject` | 20 | 7 | 13 |

100% of them. Those four Optuna studies optimised against a cost function that was reading
fallback constants, so **the optimizer values currently persisted in NVS should be treated as
unvalidated**, not as tuned results. (See the live baseline in §3 — several are far from both
the source defaults and anything this audit found sensible.)

**Fix used for this audit:** `binaryThreshold: 50` (full trace, zero noise pixels, peak 141 so
no saturation). Exposure was left at `-9` so the stored background in `homography.npz` stayed
valid and no recalibration was needed. Written to a **separate** `scripts/optimizeGalvo/auditCamConfig.json`
(gitignored) — the user's `camConfig.json` was not modified. Applying the fix for real is a
one-line change to `camConfig.json`, or a run of `autotune-camera`.

---

## 2. Method

- Harness: `sweep.py` / `streak.py` (scratchpad, see Appendix B) reusing
  `optimizeGalvo.py`'s own `EspClient`, `Camera` and `computeMetrics`, so camera numbers come
  from exactly the same path as `optimizeGalvo.py measure`.
- Each measurement additionally samples `GET /api/optimizer-stats` **while the pattern is still
  live** — `emitted_lit`, `emitted_blank`, `planned_total`, `truncated`, `stage1_triggered`,
  `stage15_triggered`, `ringing_active`. This firmware-side ground truth is what makes several
  bounds below objective rather than a judgement call on a noisy image metric.
- ~3 s per measurement; ~330 live measurements total.

**Measurement noise floor** (n=6, `square`, unchanged params):

| Metric | Range | Half-spread | Usable detection limit |
| --- | --- | --- | --- |
| `pathDeviationRms` | 288.5–293.3 | ±2.4 | > ~8 units |
| `brightnessNonUniformity` | 0.151–0.174 | ±0.012 | > ~0.03 |
| `cornerHotspot` | 0.291–0.381 | ±0.045 | > ~0.10 |
| `blankLeakage` | 0.00 | 0 | — |

`cornerHotspot` is far too noisy for single-shot boundary detection; every corner-parameter
bound below is a 3-repeat mean.

**Two scale facts that matter for reading the numbers.** `dacPerPixel` = 75, so a
`pathDeviationRms` of ~285 on a well-formed square is dominated by beam width (~3.8 px), not by
path error — the rig's optical floor, and *tighter* than `camConfig.json`'s `pathDeviationRms`
diagnose threshold of 150, which no in-focus shape on this bench can meet. Treat pdev as a
*relative* signal. Anything below ~150 DAC units is sub-pixel and invisible to this camera.

---

## 3. Baseline (device live config — NOT the source defaults)

FW 6.73.4, `kpps = 42` / `rated_kpps = 44` (**not** the `OPT_DEFAULT_GALVO_KPPS` of 30 —
`applyPpsScaling` is therefore near-unity here), armed, interlocks green throughout.

| profile | cad | mincp | maxcp | ppk | blank | minbl | blppk | s1tgt | minint | maxppf | resamp | rspc |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Vector | 25 | 6 | 8 | 5 | 40 | 8 | 2 | 14 | 4 | 1200 | **True** | 200 |
| Smooth | 25 | 6 | 8 | 0.1 | 40 | 8 | 0.1 | 14 | 9 | 1200 | False | 200 |
| Waves | 55.7 | 2 | 6 | 3.2 | 30 | 8 | 8 | 12 | 11 | 860 | False | 160 |
| Wireframe | 25 | 10 | 15 | 6.7 | 54 | 8 | 4 | 14 | 17 | 1100 | False | 200 |
| MultiObject | 34.0 | 2 | 3 | 3.8 | 40 | 8 | 1.94 | 10 | 6 | 1000 | False | 160 |
| Particles | 25 | 2 | 4 | 6 | 40 | 32 | 0.9 | 8 | 4 | 1300 | False | 10 |
| Trails | 60 | 3 | 3 | 8.3 | 16 | 6 | 8 | 12 | 8 | 1000 | False | 160 |
| Text | 87 | 10 | 20 | 0.1 | 61 | 43 | 0.1 | 23 | 4 | 1400 | False | 200 |

Every bound below is conditioned on these values, since the optimizer's parameters interact
strongly (§4 notes where).

---

## 4. Results

Pattern→profile: `square`/`star` → Vector, `circle` → Smooth, `spiral` → Waves,
`segments` → MultiObject.

| Parameter | Scenarios tested | Practical min (confirmed) | Practical max (confirmed) | Evidence at the boundary | Notes / caveats |
| --- | --- | --- | --- | --- | --- |
| `corner_angle_deg` | Vector×square, Vector×star | **0** (no defect at floor) | **= sharpest turn angle in the content** (90 for square, ~144 for star) | square: hot 0.460→0.132 across 0→90, then flat 0.194/0.185 at 120/179 with `emitted_lit` frozen at 597. star: hot still moving to 0.217 at 130, flat past 145 | Not a fixed number. `severity = (angle−cad)/(180−cad)` is 0 once `cad` ≥ the vertex's turn angle, so the parameter is a **no-op above the content's own sharpest corner**. Pentagram turn = 180−36 = 144, exactly where star saturates |
| `min_corner_pts` | Vector×square, Smooth×circle | **1** (no defect on either) | **5** on Vector×square | square: hot 0.281 at 5 → 0.362 at 6, crossing the 0.35 threshold; flat pdev throughout (288–303, within noise) | On a square (severity 0.4) `max_corner_pts` dominates. On a circle every vertex is soft, so this **is** the density knob: `lit` = 128 × mincp exactly, saturating at 1160 (mincp ≥ 10) where Stage 1 + Stage 1.5 both fire and blanking collapses 41→9. Clamped to ≤ `max_corner_pts` by `normalizeOptimizerConfig()` |
| `max_corner_pts` | Vector×square (n=3/point) | **4** | **8** | pdev 310.1 (cp=1) → 292.5 (3) → **280.2 (4)** → plateau 275–286 through 20. hot **0.338 (8)** → 0.418 (10), crossing 0.35 | Below 4 corners are measurably rounded (pdev +30, ≫ noise ±5); above 8 corner hotspot exceeds the diagnose bar. Narrower than `searchSpace.json`'s 4–14. cp=8 is marginal (0.338 ± 0.024 vs 0.35) |
| `pts_per_1000_units` | Vector×square (resample **off**), Smooth×circle | **5** | **12** | square: at 0.1 only 46 pts — pdev 459.8, hot 1.907, `unif` at the sparse fallback. 3.0 → pdev 323; **5.0 → 293.8**; 8.0 → 285.5 (plateau). **12.0 → `lit` 1158 / `planned` 1199 vs `maxppf` 1200**; identical at 20/30/40/50 | **Inert on Vector as shipped** — `resample_enabled=True` makes `edgeInteriorCount()` take the resample branch, so ppk is dead code on that profile (see §5). Max is the frame-budget ceiling and therefore scales with `maxppf` and total path length |
| `max_pts_per_frame` | MultiObject×segments | **500** | **800** | 100 → pdev hits the 60000 nothing-visible fallback, `lit` 44. 300 → `lit` 96. **500 → `lit` 296, streak at floor**. **800 → `lit` 456 (full)**; identical at 1000/1200/1500/2000 | Max = the content's `planned_total` (660 here); beyond that it cannot do anything. Strongly content-dependent |
| `blank_samples` | MultiObject×segments | **24–30** | **~30** (no further gain) | Direct streak metric: `streakMean` 0.601 (bs=1) → 0.036 (20) → **0.017 (30)** → 0.012/0.011/0.007 (60/80/100). `streakMax` 12→7→**3** | Below ~16, camera-visible diagonal streaks between segments (photographed). Above 30, pure cost: `emitted_blank` grows 154→356, eating frame budget for no measurable gain. Device's 40 sits just above the confirmed floor — reasonable |
| `min_blank_samples` | MultiObject×segments (2 regimes) | **25** *(only when it binds)* | **~30** | With `blppk` forced to 0.1: `streakMean` 0.441 (1) → 0.097 (16) → 0.045 (20) → **0.020 (25)** → floor. At stock `blppk`, **zero** effect across 1–40 (`emitted_blank` pinned at 204) | Masked in normal operation — the distance-proportional term dominates on long jumps. Only load-bearing for **short** jumps. Clamped to ≤ `blank_samples` |
| `blank_pts_per_1000_units` | MultiObject×segments | **2.0** | **2.0** (saturates) | `streakMean` 0.323 (0.1) → 0.154 (0.5) → 0.042 (1.0) → **0.011 (2.0)**; identical 4→50 with `emitted_blank` pinned at 204 | Max is where the per-jump window hits the `blank_samples` ceiling, so it moves with `blank_samples` and with the longest jump in the content. This is the Starfield v6.65.1 knob |
| `stage1_blank_target` | MultiObject×segments, `maxppf`=150 (n=3) | **8** (= `min_blank_samples`) | **14** | Deterministic and repeatable: 8 → `lit` 104/`blank` 44; 14 → 76/74; 20 → 44/104; **30 → 104/44 (collapses back)** | Blank-points-per-jump ≈ target, floored by `min_blank_samples` (targets 1/2/4/8 all give 44) and **collapsing to that floor** once the budget cannot fit the target. At 20 the trace is already too sparse (`unif` fallback in 3/3 reps). Non-monotonic — see Appendix A.1 |
| `resample_enabled` + `resample_spacing_units` | Vector×square | **~50** | **~800** | 10 vs 25 identical (`lit` 1154/1158, budget-capped). 200 → 602; 400 → 318; **800 → 174, `unif` 0.058 (real)**; 1500/2000 → `unif` at the sparse fallback, pdev 353 | Below ~50 the budget ceiling absorbs it (no further effect); above ~800 the trace degrades to visible dots. Takes precedence over `pts_per_1000_units` whenever enabled |
| `curvature_resample_enabled` + `curvature_gain` | Smooth×circle | — | — | `lit` 901 for gain 0/0.5/1/2/5 — **identical to gate off**; only 10/20 move it to 1029. pdev flat 302–308 | **Weak/inconclusive.** A circle has constant curvature, so curvature-*adaptive* resampling has nothing to adapt to. Needs a mixed straight/curved pattern to isolate; not attempted |
| `min_spacing_units` | Smooth×circle | **1** (no defect) | **~300** | `lit` 901 flat for 1/20/100/300, drops to 773 at 1000/2000. pdev 306→315 | Only binds once it exceeds the geometry's natural spacing; then it *reduces* density. Weak effect, no defect found at either clamp end |
| `max_spacing_units` | Smooth×circle | **200** | **400** | **1.0 → `truncated` 1798** (planned 2998); 50 → truncated 1286; **200 → truncated 0**, `lit` 1157; 400 → 901; identical at 1000/2500/4000 | Below 200 it over-densifies into frame-budget truncation. Above 400 inert on this geometry (natural spacing already below it) |
| `ringing_comp_enabled` + `ring_freq_hz` | MultiObject×segments (Vector×square first, invalid) | **~525–600 Hz** (engagement threshold) | **2000** (clamp; still active) | `ringing_active` **false** at 60/200 Hz, **true** at 600/1200/2000. Predicted threshold `0.5/f × 42000 ≤ blank_samples(40)` → f ≥ 525 Hz — matches. Best `blankLeakage` 1.66 at 600 vs 2.97 with gate off | Below the threshold the ZV shaper silently never engages. Threshold scales with pps and `blank_samples`, so it is **not** a fixed frequency. Not testable on `square` at all (its single jump has `jump_distance_total = 0`) |
| `vel_clamp_enabled` + `max_step_units` | Waves×spiral | **400** | **400** (inert above) | **50 → `truncated` 2681** (planned 3541); 100 → 1147; 200 → 341; **400 → truncated 0**, identical to gate-off (`lit` 832/planned 849) through 32767 | Below 400 the clamp inserts so many points the frame truncates — parts of the spiral simply are not drawn. Note pdev *improves* to 239.9 at maxstep=50 **because** most of the shape is missing: a metric-improves-while-content-breaks trap |
| `accel_clamp_enabled` + `max_accel_units` | Waves×spiral | **400** | **400** (inert above) | **10 → `truncated` 845018** (planned 845878); 50 → 645510, `unif` fallback; 100 → 2036; **400 → truncated 0**, identical to gate-off through 32767 | Same shape of result as `max_step_units`, more violent. Both bounds are geometry- and pps-dependent, not universal |
| `jitter_enabled` + `jitter_amount_units` | Vector×square | **0** (no defect) | **~1200** | pdev flat 285–292 for 0/10/40/80/150/300/600/1200 (all within noise); **2000 → pdev 331.6** (+45, ≫ noise ±5) | Below ~150 units the effect is **sub-pixel** at 75 DAC-units/px and invisible to this rig — a resolution limit, not a proof of no effect. Only the clamp maximum produced a measurable defect |
| `reorder_segments` | MultiObject×segments | — | — | off → `streakMean` 0.030, `blank` 204, pdev 177.6. **on → 0.098, `streakMax` 26, `blank` 136, pdev 219.7** | Measurably **worse** on this pattern: it does shorten total jump travel (blank 204→136) but the shorter per-jump window leaves less settle time, so streaking triples. Single geometry only — do not generalise |
| `reorder_2opt` | MultiObject×segments | — | — | With `reorder_segments=false`: `blank` 204, pdev 177.4 — **identical to fully off**. With it true: 136/233.1, no improvement over `reorder_segments` alone | Confirmed **no-op unless `reorder_segments` is also on**. Gate dependency, as suspected |

---

## 5. No reliable result

| Parameter | Why |
| --- | --- |
| `min_interior_pts_per_segment` | `emitted_lit` stayed at **46 for every value 0–50** on `square` (resample off, ppk 0.1). Evidence points to it being a **floor applied only when Stage 2 crushes density**, not a general minimum — Stage 2 was not active in any isolating setup I could build without simultaneously changing the budget parameters I would be measuring against. Not isolatable here |
| `ring_damping_ratio` | Swept 0.0–0.9 on `square`, where `ringing_active` is **false at every frequency** — the shaper never engaged, so all 7 values measured the same unshaped output (pdev 285–293, all within noise). The engagement fix (`segments`, ≥600 Hz) was found too late to re-sweep damping. **No bound; not attempted in the valid regime** |
| `curvature_gain`, `curvature_resample_enabled` | See §4 — a constant-curvature circle cannot exercise curvature-*adaptive* logic. Values 0–5 are indistinguishable from the gate being off |
| `pts_per_1000_units` **on Vector as shipped** | Physically inert: `resample_enabled=True` routes `edgeInteriorCount()` down the resample branch. The 5/12 bounds above are valid **only with `resample_enabled=false`** |
| `min_blank_samples` **at stock `blank_pts_per_1000_units`** | Genuinely does not bind (`emitted_blank` pinned at 204 across 1–40). The 25 bound holds only in the short-jump regime |
| Wireframe, Text, Particles, Trails profiles | No camera pattern exists for any of them; none added this session (see §6) |

---

## 6. Scope not covered

Stated plainly rather than left implied:

- **kpps re-testing (task step 5) was not performed.** Everything here is at the device's
  configured `kpps = 42` / `rated 44`. No half-rate or double-rate runs, so every
  PPS-scaled bound (`pts_per_1000_units`, `blank_pts_per_1000_units`, `resample_spacing_units`,
  `min`/`max_spacing_units`, `max_step_units`, `max_accel_units`) should be read as
  **"valid at 42 kpps only"** until re-checked through `applyPpsScaling`.
- **No new calib patterns were added.** The proposed `particles`, `CAM_WIREFRAME` and `CAM_TEXT`
  patterns were not built. Once the §1 blocker surfaced, restoring measurement integrity and
  then getting real data for the 20 parameters the *existing* six patterns can actually reach
  was the higher-value use of the session; adding three firmware patterns plus their host-side
  `idealPolylines()` mirrors is a substantial change that would also have needed a firmware
  flash (and therefore a disarm) mid-audit. Those four profiles remain unmeasured.
- **Trails** remains open, as the task anticipated — its ground truth is a trajectory over time,
  not a static rasterisable ideal. No approach attempted; no weak proxy invented.
- Several parameters have **only one or two scenarios**, short of the 2–3 asked for — flagged
  per-row in §4. `reorder_segments`/`reorder_2opt`, `max_pts_per_frame` and the whole blanking
  group rest on `segments` alone, because it is the only existing pattern with multiple real
  blank jumps.

---

## Appendix A — Incidental findings (not fixed, out of scope)

**A.1 `stage1_blank_target` collapses to the floor instead of degrading gracefully.**
Reproducible (n=3, identical emitted counts): with `maxppf`=150, targets 8/14/20 give
blank-per-jump of ~8.8/14.8/20.8 as intended, but target 30 — which the budget cannot fit —
silently reverts to the **minimum** (44 blank, same as target 8) rather than to the largest
value that would still fit. A user raising this parameter past the budget gets the *opposite*
of what they asked for, with no signal.

**A.2 `blankLeakage` misses the blank-jump streak it exists to catch.** `computeMetrics()`
samples only the rasterised **ideal** gap corridor (`gapMask`). At `blank_samples=1` the beam
visibly streaks diagonally across `segments` (photographed, `shot_seg_blank1.png`) but does not
follow the ideal jump path, so the metric read **0.95** — *lower* than the 2.95 it reports for
the clean `blank_samples=40` case. The metric is anti-correlated with the defect over part of
its range. The corridor-between-lines measurement in `streak.py` is monotonic over the same
sweep and could replace or supplement it.

**A.3 Fallback constants are indistinguishable from real readings.** `brightnessNonUniformity =
1.0` and `offsetX/YUnits = dacRange` are returned both for "nothing visible" and as legitimate-
looking values, with no flag in the `Metrics` record. This is what let §1 go unnoticed through
four complete Optuna studies. A `valid: false` field (or NaN) would make it impossible to
optimise against.

**A.4 `pathDeviationRms`'s diagnose threshold is unreachable on this rig.** `camConfig.json`
sets 150; a well-formed, correctly-tuned square measures ~285, dominated by beam width at
75 DAC-units/px. Any `diagnose` run will report a path-deviation "settings issue" permanently.

**A.5 `/api/config` does not expose `galvo_kpps`.** It is only readable from `/api/projection`
(`kpps`/`rated_kpps`). The Optimizer tab and any host tool reasoning about PPS-scaled parameters
have to know to look elsewhere. Minor, but it cost time here.

**A.6** `/api/projection` reports `max_safe_kpps: 17.6` while the device runs at 42 kpps. Almost
certainly an exposure-calculation output rather than an operational limit, but it is worth
someone confirming which, given the Class 4 classification. Not investigated — outside this task.

## Appendix B — Tooling added

No firmware, docs, or tracked config was modified. Everything below is scratchpad-only except
the gitignored camera config:

- `scripts/optimizeGalvo/auditCamConfig.json` — `camConfig.json` with `binaryThreshold: 50`.
  Gitignored (`scripts/optimizeGalvo/*.json`), so the repo stays clean. **This is the §1 fix and
  the one artefact worth keeping.**
- `sweep.py` — parameter sweep harness; reuses `optimizeGalvo`'s `EspClient`/`Camera`/
  `computeMetrics`, adds live `/api/optimizer-stats` sampling and repeat-averaging.
- `streak.py` — direct blank-jump streak metric for `cam_segments` (see A.2).
- `agg.py`, `shot.py`, `diagThresh.py`, `diagNoise.py` — aggregation, annotated-frame capture,
  and the threshold/noise diagnostics behind §1.

## Appendix C — Verification note

Two apparent findings were discarded after repeat testing rather than reported:

- `ring_freq_hz=60` produced `pathDeviationRms` 1944.5 on first measurement. Three repeats gave
  174.2/179.1/179.2 against a gate-off control of 171.9/181.9/174.4 — a one-off capture
  artefact, not an effect.
- The `stage1_blank_target` non-monotonicity *did* reproduce exactly (n=3) and is reported as
  A.1.

Safety chain was green for the entire session (`estop_ok`, `scanfail_ok`, `laser_armed` all
true, `last_failsafe` empty, uptime continuous at 5307 s with no reboot). The laser was armed by
the operator before the session and never armed by this process.

## Appendix D — 2026-08-18 follow-up: live validation + re-tune

Firmware v6.75.0 (`6d07c1f`) landed same-day, ahead of and independently of this follow-up
session, and fixed every item in Appendix A plus the root cause of §1's `binaryThreshold`
misconfiguration (`autotune-camera`'s own objective had no signal-vs-noise term). This session
validated those fixes live and re-ran the search on the camera-tunable profiles.

**Live validation, all confirmed on the armed device:**

- A.1: `/api/optimizer-stats` now reports `stage1_blank_samples` / `stage1_blank_clamped`.
- A.4: `diagnose`'s path-deviation bar is now derived per-measurement (e.g. observed
  `threshold 989 = 1.5 x measured beam width 659u`), not the fixed unreachable 150.
- A.5: `/api/config` reports `galvo_kpps: 42` / `galvo_rated_kpps: 44`.
- The three new camera patterns (`cam_wireframe`, `cam_text`, `cam_particles`) all capture,
  score `valid: true`, and the isolated-dot blob metrics (blobCount/elongation/centroidError)
  correctly detected real smearing on Particles' untuned live params.

**Two new findings, logged only — not investigated this session:**

- **Orientation mismatch on 5 of 7 camera patterns** (star, spiral, wireframe, particles,
  text): `diagnose` only fits the measured trace to its ideal reference after applying
  rot180/mirror_y/mirror_x/rot90/mirror_y respectively. The tool auto-compensates for
  *scoring*; whether the corresponding real presets are actually projected
  rotated/mirrored live was not checked.
- **Residual geometry offset after a fresh `calibrate`**: star, `segments` (MultiObject), and
  `particles` still showed real position/size offsets post-recalibration while every other
  pattern came back clean — `diagnose` classifies these "not fixable by autotune". Cause not
  established; a plausible lead is the corners4 reference dots sampling a smaller radius than
  these patterns' actual extent, but this is a guess, not a finding.

Because of the second point, `optimize` was **not** re-run on Vector/MultiObject/Particles as
originally planned — `diagnose --profile all` flagged only **Smooth, Waves, Wireframe, Text**
as geometry-clean, so only those four were tuned (`--fresh --apply`, 20 trials each, study
prefix `finalwave2`).

**Re-tune result — before/after cost from the tool's own post-apply re-measurement, not the
in-search "best" value:**

| Profile | cost before → after | kept? |
| --- | --- | --- |
| Smooth | 9.838 → 9.881 (worse) | reverted |
| Waves | 11.589 → 11.301 (better) | **applied** |
| Wireframe | 11.325 → 11.387 (worse) | reverted |
| Text | 10.645 → 11.252 (worse) | reverted |

Smooth's own trial log is the clearest evidence of a live noise problem: trial 0 measured
cost 7.597 and was never beaten across the other 19 trials, but re-measuring those exact
"best" params moments later in the before/after step gave 9.881 — a ~30% discrepancy on
identical parameters. Each trial here is a single un-repeated capture (unlike this audit's own
`sweep.py`, which repeat-averages); a future re-tune should use that instead of Optuna's raw
per-trial objective. Smooth, Wireframe, and Text were rolled back to their pre-tune values via
`/api/optimizer-live` + `/api/optimizer-save` (params reconstructed from the run's own
before/after log, then confirmed byte-for-byte against a fresh `/api/config` read); Waves' gain
was real (reproduced in the before/after capture, not just the search) and was left applied.

Safety chain stayed green throughout (`estop_ok`/`scanfail_ok`/`laser_armed` true, uptime
continuous 15427 s → 17708 s, no reboot). The laser was already armed by the operator before
this session (confirmed with the operator before taking camera-pattern control) and was not
armed by this process.

## Appendix E — 2026-08-18 code-only follow-up (no live device this session)

Picked up Appendix D's three open items. **No camera or ESP32 was reachable/armed this
session** — everything below is either a code change (E.1) or a code-reading investigation
(E.2/E.3), not a new measurement. Nothing here should be read as validated on hardware.

### E.1 `measurementRepeats` — Optuna's single-shot objective, fixed

Added `cfg["measurementRepeats"]` (default `1`, `camConfig.json`) plus `averageMetrics()` /
`measureRepeated()` in `optimizeGalvo.py`. `cost` is a weighted linear sum with no interaction
terms (`computeMetrics`), so averaging the field is equivalent to recomputing it from averaged
metrics — simpler, so that's what it does. Wired into **both** places that previously scored a
single capture: the per-trial objective in `runStudyForProfile()`, and — as important, and
previously untouched — the before/after re-measurement step in `runOptimize()` that actually
*caught* Appendix D's Smooth discrepancy (7.6 vs 9.9 for identical params); that step was
itself single-shot, so it was only ever luck that it disagreed loudly enough to notice.

Default (`repeats=1`) is a byte-identical no-op: `averageMetrics()` short-circuits to returning
the original `Metrics` object unchanged for a single rep (verified — `averageMetrics([a]) is a`).
Verified only via a standalone import + unit check of `averageMetrics()`'s arithmetic (mean of
two synthetic `Metrics`, `cost` and `traceLitPx` checked); **not exercised against a real
capture or a real Optuna study** — no device this session. Opt in by setting
`"measurementRepeats": 3` (or similar) in `camConfig.json`; multiplies every trial's
settle+capture time by that factor, so start small. Next live session: re-run the Smooth study
(`--fresh`) with `repeats=3–5` and check whether the in-search "best" now agrees with the
before/after re-measurement.

### E.2 Orientation mismatch — ruled out as a code/reference divergence, narrowed to a question

Compared `calib_patterns.cpp`'s `cam_star`/`cam_spiral`/`cam_wireframe`/`cam_text`/
`cam_particles` line-by-line against `optimizeGalvo.py`'s `idealPolylines()` /
`_wireframeIdeal()` / `_textIdeal()` / `_particlesIdeal()`: star's angle formula, spiral's
theta/radius linspace, wireframe's yaw-35°/pitch-25° rotation coefficients and 4-chain vertex
table, particles' 12-dot visit order and grid pitch, and the `G`/`A`/`L` glyph point arrays
(`text_renderer.cpp`'s `FONT_G`/`FONT_A`/`FONT_L`) against the host's transcribed
`_TEXT_GLYPHS` — every one is bit-identical. **The orientation-mismatch and geometry-offset
findings are not a code/reference divergence between firmware and host** — checked, not
guessed.

That points at the measurement pipeline instead. One real, load-bearing blind spot is already
flagged in the tool's own comment (`optimizeGalvo.py:1650-1657`): `orderCorners()` labels the 4
detected corner dots purely by pixel-space sum/diff (smallest x+y = top-left, etc.), which
assumes the camera is roughly axis-aligned with DAC space. `corners4`'s own dot layout — a
plain square — is symmetric under all 8 D4 transforms, so a *consistent* mislabeling (camera
physically rotated/mirrored relative to that assumption) is invisible to `calibrate()` itself
and bakes a wrong-orientation homography that only surfaces later, on patterns asymmetric
enough to reveal it. That lines up with square/circle never showing the defect. `segments`
partially lines up too — its 4 parallel vertical lines are invariant under `mirror_x`/
`mirror_y`/`rot180` (only `rot90`-class errors would show), which is a narrower blind spot than
the corners' full 8-way symmetry but a real one.

**It does not fully explain Appendix D's specific numbers, though.** A single mislabeled-corner
cause is one fixed homography error for the whole session — every asymmetric pattern should
then need the *same* D4 correction. Appendix D reports five *different* transforms (rot180 /
mirror_y / mirror_x / rot90 / mirror_y) across star / spiral / wireframe / particles / text,
measured in what reads as one session. That is inconsistent with one global camera-mount cause,
so this is being handed back as a narrowed question, not a fix. Next live session, cheapest
first:

1. Re-run `calibrate`, and actually check the saved `*_labeled.png` against the physical
   TL/TR/BR/BL layout by eye — the tool already asks for this; Appendix D's session may not
   have done it.
2. Run `diagnose --profile all` twice in a row with **no** recalibration in between. If the same
   pattern gets a *different* best-fit transform between the two runs, `detectOrientation()`'s
   single-shot fit is itself noise-sensitive — the same root cause as E.1, just not plumbed
   there yet (it scores whatever single trace it's handed, not a `measureRepeated()` capture).
   If that turns out to be it, point `measurementRepeats` at the capture `detectOrientation()`
   scores against next, rather than treating the five transforms as five real defects.
3. If the five transforms reproduce identically run over run, they're not explained by one
   camera-mount error and warrant checking whether the optimizer stage (Stage 1/1.5 density
   crush, corner-dwell, resampling) does something orientation-dependent on these specific
   shapes under the live config — not ruled out here, no live `/api/optimizer-stats` to check.

### E.3 Geometry offset "smaller radius" guess — checked against source, doesn't hold under defaults

`corners4Radius()` (`calib_patterns.cpp`) derives the corner-dot radius from the *live*
`dac_limit_min`/`max` window, clamped to `CAM_R` = 30000; every other camera pattern (star,
segments, particles, ...) draws at the *fixed* `CAM_H` = 15000 regardless of `dac_limit`. Under
`config.h`'s stock defaults (`dac_limit` ≈ ±31129), `corners4Radius()` clamps to exactly 30000 —
the same value `optimizeGalvo.py`'s own `cfg["dacRange"]` assumes when scoring every pattern.
No mismatch under defaults, so this guess needs the audited device's *live* `dac_limit_min/max`
(persisted in NVS, can drift from source defaults) to actually be tighter than the design
radius for the mechanism to apply at all. And even then: since `findHomography` solves for
absolute DAC coordinates from whatever `corner_r` the ESP32 actually reports, a narrower
`corners4` shouldn't by itself *misplace* other patterns' geometry — it would just fit the
homography from a smaller, slightly less pixel-precise square. Worth reading `/api/config`'s
live `dac_limit_min`/`max` next session as a near-zero-cost first check, but this is not a
confirmed mechanism — the "smaller radius" idea from Appendix D remains a guess, now a
narrower one.

## Appendix F — 2026-08-18 live follow-up: repeats validation, orientation margin fix, re-tune

Laser was already armed by the operator before this session; not armed by this process.
`/api/backup` taken before any write (`backup_2026-08-18_pre-session.json`, kept locally, not
committed — device config, not source). Safety chain green throughout (`estop_ok`/`scanfail_ok`/
`laser_armed` true, uptime continuous 19266s → 20593s, no reboot).

### E.3 closed — live `dac_limit` confirmed tight, but doesn't explain the geometry offset

Live `/api/config`: `dac_limit_min=7000` / `dac_limit_max=63000` — asymmetric and tighter than
the ±31129 stock default, so E.3's premise (a live window narrower than the design radius) is
real on this device, not just a hypothetical. But E.3's own reasoning already covered this case:
`optimizeGalvo.py` reads back the actual `corner_r` the ESP32 reports (`startResp["corner_r"]`)
rather than assuming the fixed `dacRange`, so a narrower `corners4` doesn't misplace other
patterns' geometry — it only makes the reference square itself slightly less pixel-precise.
Confirmed, not just argued: **not a contributor to the geometry-offset finding.**

### E.1 closed — `measurementRepeats=3` validated live, fixes the noise gap

Set `measurementRepeats: 3` in `camConfig.json` (local, gitignored — anyone reproducing this
needs to re-apply it) and re-ran `optimize --fresh` on the three profiles Appendix D flagged as
noise-corrupted:

| Profile | in-search best | before → after (independent re-measurement) | gap | verdict |
| --- | --- | --- | --- | --- |
| Smooth | 3.6810 | 4.2325 → 3.6633 | 0.5% | real improvement, **applied** |
| Wireframe | 3.3455 | 3.8537 → 3.4427 | 2.9% | real improvement, **applied** |
| Text | 3.1051 | 2.7706 → 3.1343 | 0.9% | genuinely worse, reverted (not applied) |

Every gap is under 3%, against the ~30% Smooth saw at `repeats=1` in Appendix D. The fix works:
Optuna's per-trial objective now agrees with an independent re-measurement of the same params.
Text is the interesting negative result — it reproduces Appendix D's verdict (worse, not
noise) even with tripled averaging, so Text's search genuinely can't beat its current baseline
with the present `searchSpace.json` ranges; that's a real finding about the search space, not
a measurement artifact.

Smooth and Wireframe applied via `/api/optimizer-live` + `/api/optimizer-save`, verified
byte-for-byte against a fresh `/api/config` read after saving. Text left untouched. Vector,
MultiObject and Particles remain skipped this session too — `diagnose --profile all` still
flags all three as real geometry issues ("not fixable by autotune"), unchanged from Appendix D.

### E.2 narrowed further — orientation mismatch is real and reproducible, one false-positive fixed

Two `diagnose --profile all` runs back-to-back, no recalibration between them (this session's
own fresh `calibrate`, not Appendix D's): both runs picked the **identical** transform set
(`star: rot180, circle: rot180, spiral: mirror_y, wireframe: mirror_x, text: mirror_y`) —
E.2's hypothesis #2 (single-shot `detectOrientation()` noise) is **ruled out**, at least within
one calibration session.

`circle`'s entry was the tell: `detectOrientation()`'s own log showed `rot180` beating `identity`
by 5.9 vs 5.9 DAC-units — a dead tie. A circle centered at the origin is geometrically invariant
under 180° rotation; `detectOrientation()` did plain `min(scores, key=scores.get)` with no
margin, so a coin-flip-level noise difference was enough to lock in and cache a spurious
compensation. That also explains why Appendix D's session flagged a *different* 5-of-7 set
(no `circle`, but `particles`) than this session's raw first pass (`circle` flagged, `particles`
not) — not evidence against one stable root cause, just two coin flips landing differently.

Fixed in `optimizeGalvo.py` (`detectOrientation()`, v2.22.0 → v2.22.1): a non-identity transform
is now only trusted if it beats identity's score by ≥20% (`ORIENTATION_MARGIN_FRAC = 0.8`); real
mismatches observed live all clear 45–90%, circle's tie was ~0%. Re-ran `diagnose --profile all`
with the orientation cache cleared: `circle` no longer flags, `star`/`spiral`/`wireframe`/`text`
still do — now an exact match to Appendix D's non-`particles` findings, and reproduced across two
independent calibration sessions on two different days. **These four are a real, reproducible
coordinate-convention mismatch between this tool's reference geometry and the firmware's actual
output — not measurement noise, and not a degenerate-symmetry false positive.**

What this still does not establish, per `detectOrientation()`'s own doc comment: whether the
real presets sharing these patterns' point-generation paths are actually projected rotated or
mirrored live. That needs a human eyeballing a live preset against its expected orientation, not
a camera-loop measurement — out of scope for this session, next cheapest step for whoever picks
this up.

### Task 5 — `curvature_gain`: honest verdict, not a fix

No new camera pattern was added this session (would need a firmware change + flash, out of scope
here). Confirms Task 4's finding stands: no existing camera-loop pattern exercises mixed
straight/curved geometry (`corners4`/`square`/`segments` are straight-only, `star`/`spiral`/
`wireframe`/`particles`/`text` are curved-only or fixed-vertex, and idx 8 "Opt Density Ramp" —
the one plausible-sounding candidate — is five straight lines, not mixed). **Verdict:
`curvature_gain` is not measurable on this rig as currently equipped.** Closing this honestly
rather than inventing a weak proxy: a real answer needs a new calib pattern combining a straight
run and a curved run in one frame, sized to trigger `curvature_resample_enabled`'s adaptive
logic on the curved section only — a scoped follow-up task of its own, not a sub-task of this
audit.

## Appendix G — 2026-08-18 second live follow-up: a second blocking measurement bug, and orientation confirmed real once it's fixed

Prompted by Andre repeatedly reporting presets rendering wrong across many past sessions
(several already root-caused and hardware-fixed: `text_renderer.cpp` glyph orientation in
v6.01.0/v6.05.3, Paint-by-Finger's X-mirror saga through v1.17.2) — worth checking whether
Appendix F's four still-open orientation mismatches (`star`/`spiral`/`wireframe`/`text`) are
that same class of real, still-unfixed defect, or something else. Laser already armed by the
operator (`laser_armed:true` via `/api/status`), not armed by this process.

### A second blocking measurement-chain bug, same family as §1 but not the same one

Two candidate explanations were checked and ruled out first: the camera itself is not mounted
rotated/mirrored (confirmed via the script's own live camera-preview option — the physical
scene reads correctly), and `runCalibrate()`'s `orderCorners()` did not mislabel the four
corner dots this session (`calibrate_2026-08-18_18-04-23_labeled.png` — TL/TR/BR/BL land on a
normal, correctly-ordered rectangle, matching the physical layout).

What actually explained it: Andre spotted extra strokes on both the raw and annotated
`diagnose` captures that aren't part of the live pattern at all — e.g.
`diagnose_Wireframe_wireframe_2026-08-18_18-15-56_annotated.png` showed a smooth **red curved
arc** with no matching cube edge, sitting exactly where a leftover trace from `spiral` (the
pattern measured immediately before `wireframe` in the same `diagnose --profile
Vector,Waves,Wireframe,Text` run) would land. `text`'s capture from the same run showed a
spurious diagonal tail off the `L` glyph, similarly explaining its reported "Y size off by
+34.2%, Y position off by +2250 units" geometry-issue finding — a defect that, as it turns out,
does not exist.

Root cause: `Camera` (`optimizeGalvo.py`) never set `cv2.CAP_PROP_BUFFERSIZE`, and
`grabAccumulated()` called `grabGray()` immediately with no flush. OpenCV's DSHOW backend
queues frames internally regardless of whether anything calls `.read()`; the several call
sites that `time.sleep(cfg["patternSwitchSettleSeconds"])` right before starting an
accumulation (`measureOnce()`'s own comment already predicted this exact failure mode, just
never guarded against it) could have that first `.read()` return a frame the driver queued
during the sleep — i.e. from before or mid pattern-switch — baking the previous pattern's tail
into the current `max()`-accumulated capture as an extra, unrelated shape. This is a *different*
bug from §1 (that one was a bad threshold silently starving every measurement to near-zero lit
pixels; this one contaminates otherwise-valid measurements with stray real light from the wrong
pattern) but the same *family*: a capture-pipeline defect invisible in the numbers unless you
go looking at the actual pixels, that had been quietly corrupting results for as long as the
tool has existed.

**Fixed in `optimizeGalvo.py` v2.22.1 → v2.23.0:** `cv2.CAP_PROP_BUFFERSIZE=1` set at camera
open (best-effort — not every backend honors it, see the code comment), plus
`grabAccumulated()` now unconditionally discards two frames before starting its real
accumulation, so staleness is guarded regardless of whether the backend respects the buffer
hint. `orientation.json` was deleted (stale pre-fix cache) so the re-run below is a genuine
fresh `detectOrientation()` fit against clean captures, not a replayed cached decision.

### Before/after: the fix demonstrably worked

| pattern | before fix (contaminated) | after fix (clean) |
| --- | --- | --- |
| `text` | GEOMETRY ISSUE: Y size +34.2%, Y position +2250 units, path dev 1205 | no geometry issue at all — only a pre-existing OPTIMIZER SETTINGS issue (corridor leakage) |
| `wireframe` (visual) | stray unmatched red arc, no corresponding cube edge | clean — the traced cube's only deviation from the green reference is a real, small scale/position offset (`off-path 0`) |
| star/spiral/wireframe/text orientation fit quality | e.g. star: 11.3 vs. 36.7 unrotated | e.g. star: **2.1 vs. 35.4 unrotated** — sharper separation, not weaker |

The contamination was inflating noise, not manufacturing the orientation signal: after removing
it, all four mismatches reproduce **more** cleanly than before, not less.

### Orientation mismatch: confirmed real, with clean visual proof this time

Re-ran `diagnose --profile Vector,Waves,Wireframe,Text` against the fixed pipeline with a
cleared orientation cache. All four transforms reproduced exactly as in Appendix F
(`star: rot180, spiral: mirror_y, wireframe: mirror_x, text: mirror_y`), now at `off-path 0` /
100% or near-100% path-coverage clean fits — see
`results/diagnose_Vector_star_2026-08-18_18-27-40_annotated.png` (a pentagram traced
point-down, cyan trace sitting exactly on a 180°-rotated reference, zero off-path pixels) and
`results/diagnose_Waves_spiral_2026-08-18_18-27-40_annotated.png` (traced spiral winds the
opposite handedness from the reference spiral, also zero off-path pixels). This is about as
clean as a camera measurement gets on this rig — **the four-pattern orientation mismatch is a
real coordinate-convention defect in firmware geometry generation, not a measurement artifact,
not a camera-mount issue, and not corner mislabeling.**

Separately, with the contamination gone, small independent **geometry** (not orientation)
issues remain on three of the four — worth tracking as their own item, not folded into the
orientation question:

- `star`: X size off by −5.4%
- `wireframe`: Y size off by −15.8%, Y position off by +2062 DAC units
- `spiral`/`text`: clean, only pre-existing OPTIMIZER SETTINGS issues (corridor leakage) remain

### Still open

Per `detectOrientation()`'s own doc comment, this still only proves the *camera-loop test
pattern's* geometry is rotated/mirrored relative to this tool's reference — not that the real
presets sharing `star`/`spiral`/`wireframe`/`text`'s point-generation path are actually
projected wrong on a live show. `text`'s case is the one genuine open contradiction: its glyph
orientation was hardware-validated fixed back in v6.01.0/v6.05.3, yet `cam_text` (which reuses
`textrender::glyphOutlinePaths()`, the same function that fix touched) still measures
`mirror_y` today. Next cheapest step is still what Appendix F called for: eyes-on comparison of
the live `Text` preset (and `star`/`spiral`/`Wireframe`-class presets) against their expected
orientation — this session upgraded the *measurement's* credibility, it did not do that
eyes-on check itself.
