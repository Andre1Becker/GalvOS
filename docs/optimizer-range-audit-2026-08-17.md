# Optimizer Parameter Range Audit — 2026-08-17 (updated through 2026-08-20)

Camera-validated practical min/max ranges for `OptimizerLiveConfig`, measured live on the
bench rig described in [Chapter 6](06-camera-autotuning.md). Originally a single-day audit;
four live follow-up sessions (2026-08-18 ×3, 2026-08-20) chased open threads it left behind.
This revision removes narrative that later sessions fully resolved and keeps only what's
still actionable.

**Nothing outside the two changes noted below was applied or persisted.** All range-finding
overrides went through `/api/calib-cam/params` (RAM-only, restored on session stop). The two
exceptions, both confirmed via a fresh `/api/config` read-back: `Waves`/`Smooth`/`Wireframe`
optimizer profiles were re-tuned and saved (§Changelog), and `MultiObject.max_corner_pts` was
raised 3→16 and saved (§Changelog). No other NVS write, and no change to `include/config.h`.

---

## Status

**Resolved** — see [Changelog](#changelog):

- The camera threshold bug that blinded every measurement in the original 2026-08-17 session
  (root-caused and fixed in fw v6.75.0, same day).
- `/api/optimizer-stats` Stage-1 visibility, the path-deviation diagnose threshold, and
  `/api/config`'s missing `galvo_kpps` (fw v6.75.0).
- A camera capture-buffer staleness bug that baked the *previous* pattern's tail into
  `diagnose` captures (`optimizeGalvo.py` v2.23.0).
- A degenerate-symmetry false positive (`circle` flagged `rot180` on a coin-flip margin).
- `MultiObject`/`segments`'s scale error — root-caused to an under-tuned `max_corner_pts`,
  fixed and persisted.
- `Waves`, `Smooth`, `Wireframe` optimizer re-tunes (applied, real measured improvement).

**Still open** — see [Open items](#open-items):

- Orientation mismatch confirmed real on 4 camera-loop patterns (`star`/`spiral`/`wireframe`/
  `text`) — never eyes-on-checked against the actual live presets.
- `star`'s scale undershoot: cause unknown (corner-dwell time ruled out).
- `spiral`: corner-dwell test result is untrustworthy, needs a clean `diagnose` retest.
- Three incidental findings (blankLeakage blind spot, fallback-constant flagging, an
  unexplained `max_safe_kpps` reading) that fw v6.75.0 *claimed* to fix but were never
  individually re-verified live.
- kpps re-testing of every PPS-scaled bound below — never performed.
- `Trails` profile — unmeasured, no viable approach yet.
- `curvature_gain` — unmeasurable with the existing camera patterns.
- `Vector`/`Particles` geometry offsets — still flagged "not fixable by autotune", cause
  unknown.

---

## Method

- Harness: `sweep.py` / `streak.py` (scratchpad), reusing `optimizeGalvo.py`'s own
  `EspClient`, `Camera` and `computeMetrics`, so camera numbers come from the same path as
  `optimizeGalvo.py measure`.
- Each measurement additionally sampled `GET /api/optimizer-stats` **while the pattern was
  still live** (`emitted_lit`, `emitted_blank`, `planned_total`, `truncated`,
  `stage1_triggered`, `stage15_triggered`, `ringing_active`) — firmware-side ground truth that
  makes several bounds below objective rather than a judgement call on a noisy image metric.
- ~3 s per measurement; ~330 live measurements total.

**Measurement noise floor** (n=6, `square`, unchanged params):

| Metric | Range | Half-spread | Usable detection limit |
| --- | --- | --- | --- |
| `pathDeviationRms` | 288.5–293.3 | ±2.4 | > ~8 units |
| `brightnessNonUniformity` | 0.151–0.174 | ±0.012 | > ~0.03 |
| `cornerHotspot` | 0.291–0.381 | ±0.045 | > ~0.10 |
| `blankLeakage` | 0.00 | 0 | — |

`cornerHotspot` is too noisy for single-shot boundary detection; every corner-parameter bound
below is a 3-repeat mean.

**Two scale facts that matter for reading the numbers.** `dacPerPixel` = 75, so a
`pathDeviationRms` of ~285 on a well-formed square is dominated by beam width (~3.8 px), not
path error — tighter than `camConfig.json`'s old fixed 150 diagnose threshold, which no
in-focus shape on this bench could meet (fixed in fw v6.75.0, now derived per-measurement).
Treat pdev as a *relative* signal; anything below ~150 DAC units is sub-pixel and invisible to
this camera.

---

## Baseline (2026-08-17, now partially stale)

FW 6.73.4, `kpps = 42` / `rated_kpps = 44`, armed, interlocks green throughout. Every bound in
the results table is conditioned on these values, since the optimizer's parameters interact
strongly.

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

**Known deltas since this snapshot** (see [Changelog](#changelog)): `Waves`, `Smooth`,
`Wireframe` were re-tuned and saved (values now differ from this table); `Text` was re-tested
and reverted (unchanged); `MultiObject.max_corner_pts` is now **16**, not 3.

---

## Practical Parameter Ranges

Pattern→profile: `square`/`star` → Vector, `circle` → Smooth, `spiral` → Waves,
`segments` → MultiObject.

| Parameter | Scenarios tested | Practical min (confirmed) | Practical max (confirmed) | Evidence at the boundary | Notes / caveats |
| --- | --- | --- | --- | --- | --- |
| `corner_angle_deg` | Vector×square, Vector×star | **0** (no defect at floor) | **= sharpest turn angle in the content** (90 for square, ~144 for star) | square: hot 0.460→0.132 across 0→90, then flat 0.194/0.185 at 120/179 with `emitted_lit` frozen at 597. star: hot still moving to 0.217 at 130, flat past 145 | Not a fixed number. `severity = (angle−cad)/(180−cad)` is 0 once `cad` ≥ the vertex's turn angle, so the parameter is a **no-op above the content's own sharpest corner**. Pentagram turn = 180−36 = 144, exactly where star saturates |
| `min_corner_pts` | Vector×square, Smooth×circle | **1** (no defect on either) | **5** on Vector×square | square: hot 0.281 at 5 → 0.362 at 6, crossing the 0.35 threshold; flat pdev throughout (288–303, within noise) | On a square (severity 0.4) `max_corner_pts` dominates. On a circle every vertex is soft, so this **is** the density knob: `lit` = 128 × mincp exactly, saturating at 1160 (mincp ≥ 10) where Stage 1 + Stage 1.5 both fire and blanking collapses 41→9. Clamped to ≤ `max_corner_pts` by `normalizeOptimizerConfig()` |
| `max_corner_pts` | Vector×square (n=3/point) | **4** | **8** | pdev 310.1 (cp=1) → 292.5 (3) → **280.2 (4)** → plateau 275–286 through 20. hot **0.338 (8)** → 0.418 (10), crossing 0.35 | Below 4 corners are measurably rounded (pdev +30, ≫ noise ±5); above 8 corner hotspot exceeds the diagnose bar. Narrower than `searchSpace.json`'s 4–14. `MultiObject`'s own value was found under this floor at 3 — see H.2 in the changelog |
| `pts_per_1000_units` | Vector×square (resample **off**), Smooth×circle | **5** | **12** | square: at 0.1 only 46 pts — pdev 459.8, hot 1.907, `unif` at the sparse fallback. 3.0 → pdev 323; **5.0 → 293.8**; 8.0 → 285.5 (plateau). **12.0 → `lit` 1158 / `planned` 1199 vs `maxppf` 1200**; identical at 20/30/40/50 | **Inert on Vector as shipped** — `resample_enabled=True` makes `edgeInteriorCount()` take the resample branch, so ppk is dead code on that profile. Max is the frame-budget ceiling and therefore scales with `maxppf` and total path length |
| `max_pts_per_frame` | MultiObject×segments | **500** | **800** | 100 → pdev hits the 60000 nothing-visible fallback, `lit` 44. 300 → `lit` 96. **500 → `lit` 296, streak at floor**. **800 → `lit` 456 (full)**; identical at 1000/1200/1500/2000 | Max = the content's `planned_total` (660 here); beyond that it cannot do anything. Strongly content-dependent |
| `blank_samples` | MultiObject×segments | **24–30** | **~30** (no further gain) | Direct streak metric: `streakMean` 0.601 (bs=1) → 0.036 (20) → **0.017 (30)** → 0.012/0.011/0.007 (60/80/100). `streakMax` 12→7→**3** | Below ~16, camera-visible diagonal streaks between segments (photographed). Above 30, pure cost: `emitted_blank` grows 154→356, eating frame budget for no measurable gain. Device's 40 sits just above the confirmed floor — reasonable |
| `min_blank_samples` | MultiObject×segments (2 regimes) | **25** *(only when it binds)* | **~30** | With `blppk` forced to 0.1: `streakMean` 0.441 (1) → 0.097 (16) → 0.045 (20) → **0.020 (25)** → floor. At stock `blppk`, **zero** effect across 1–40 (`emitted_blank` pinned at 204) | Masked in normal operation — the distance-proportional term dominates on long jumps. Only load-bearing for **short** jumps. Clamped to ≤ `blank_samples` |
| `blank_pts_per_1000_units` | MultiObject×segments | **2.0** | **2.0** (saturates) | `streakMean` 0.323 (0.1) → 0.154 (0.5) → 0.042 (1.0) → **0.011 (2.0)**; identical 4→50 with `emitted_blank` pinned at 204 | Max is where the per-jump window hits the `blank_samples` ceiling, so it moves with `blank_samples` and with the longest jump in the content. This is the Starfield v6.65.1 knob |
| `stage1_blank_target` | MultiObject×segments, `maxppf`=150 (n=3) | **8** (= `min_blank_samples`) | **14** | Deterministic and repeatable: 8 → `lit` 104/`blank` 44; 14 → 76/74; 20 → 44/104; **30 → 104/44 (collapses back)** | Blank-points-per-jump ≈ target, floored by `min_blank_samples` and **collapsing to that floor** once the budget cannot fit the target (a user raising this past budget gets the opposite of what they asked for — the surfaced diagnostics fix noted in the changelog only makes the collapse visible, it doesn't change the behavior) |
| `resample_enabled` + `resample_spacing_units` | Vector×square | **~50** | **~800** | 10 vs 25 identical (`lit` 1154/1158, budget-capped). 200 → 602; 400 → 318; **800 → 174, `unif` 0.058 (real)**; 1500/2000 → `unif` at the sparse fallback, pdev 353 | Below ~50 the budget ceiling absorbs it (no further effect); above ~800 the trace degrades to visible dots. Takes precedence over `pts_per_1000_units` whenever enabled |
| `curvature_resample_enabled` + `curvature_gain` | Smooth×circle | — | — | `lit` 901 for gain 0/0.5/1/2/5 — **identical to gate off**; only 10/20 move it to 1029. pdev flat 302–308 | **Weak/inconclusive.** A circle has constant curvature, so curvature-*adaptive* resampling has nothing to adapt to. Needs a mixed straight/curved pattern to isolate — none exists yet, see Open items |
| `min_spacing_units` | Smooth×circle | **1** (no defect) | **~300** | `lit` 901 flat for 1/20/100/300, drops to 773 at 1000/2000. pdev 306→315 | Only binds once it exceeds the geometry's natural spacing; then it *reduces* density. Weak effect, no defect found at either clamp end |
| `max_spacing_units` | Smooth×circle | **200** | **400** | **1.0 → `truncated` 1798** (planned 2998); 50 → truncated 1286; **200 → truncated 0**, `lit` 1157; 400 → 901; identical at 1000/2500/4000 | Below 200 it over-densifies into frame-budget truncation. Above 400 inert on this geometry (natural spacing already below it) |
| `ringing_comp_enabled` + `ring_freq_hz` | MultiObject×segments (Vector×square first, invalid) | **~525–600 Hz** (engagement threshold) | **2000** (clamp; still active) | `ringing_active` **false** at 60/200 Hz, **true** at 600/1200/2000. Predicted threshold `0.5/f × 42000 ≤ blank_samples(40)` → f ≥ 525 Hz — matches. Best `blankLeakage` 1.66 at 600 vs 2.97 with gate off | Below the threshold the ZV shaper silently never engages. Threshold scales with pps and `blank_samples`, so it is **not** a fixed frequency. Not testable on `square` at all (its single jump has `jump_distance_total = 0`) |
| `vel_clamp_enabled` + `max_step_units` | Waves×spiral | **400** | **400** (inert above) | **50 → `truncated` 2681** (planned 3541); 100 → 1147; 200 → 341; **400 → truncated 0**, identical to gate-off (`lit` 832/planned 849) through 32767 | Below 400 the clamp inserts so many points the frame truncates — parts of the spiral simply are not drawn. Note pdev *improves* to 239.9 at maxstep=50 **because** most of the shape is missing: a metric-improves-while-content-breaks trap |
| `accel_clamp_enabled` + `max_accel_units` | Waves×spiral | **400** | **400** (inert above) | **10 → `truncated` 845018** (planned 845878); 50 → 645510, `unif` fallback; 100 → 2036; **400 → truncated 0**, identical to gate-off through 32767 | Same shape of result as `max_step_units`, more violent. Both bounds are geometry- and pps-dependent, not universal |
| `jitter_enabled` + `jitter_amount_units` | Vector×square | **0** (no defect) | **~1200** | pdev flat 285–292 for 0/10/40/80/150/300/600/1200 (all within noise); **2000 → pdev 331.6** (+45, ≫ noise ±5) | Below ~150 units the effect is **sub-pixel** at 75 DAC-units/px and invisible to this rig — a resolution limit, not a proof of no effect. Only the clamp maximum produced a measurable defect |
| `reorder_segments` | MultiObject×segments | — | — | off → `streakMean` 0.030, `blank` 204, pdev 177.6. **on → 0.098, `streakMax` 26, `blank` 136, pdev 219.7** | Measurably **worse** on this pattern: it does shorten total jump travel (blank 204→136) but the shorter per-jump window leaves less settle time, so streaking triples. Single geometry only — do not generalise |
| `reorder_2opt` | MultiObject×segments | — | — | With `reorder_segments=false`: `blank` 204, pdev 177.4 — **identical to fully off**. With it true: 136/233.1, no improvement over `reorder_segments` alone | Confirmed **no-op unless `reorder_segments` is also on**. Gate dependency, as suspected |

---

## Parameters With No Reliable Result

| Parameter | Why |
| --- | --- |
| `min_interior_pts_per_segment` | `emitted_lit` stayed at **46 for every value 0–50** on `square` (resample off, ppk 0.1). Evidence points to it being a **floor applied only when Stage 2 crushes density**, not a general minimum — could not isolate that regime without also moving the budget parameters being measured against |
| `ring_damping_ratio` | Swept 0.0–0.9 on `square`, where `ringing_active` is **false at every frequency** — the shaper never engaged, so all 7 values measured the same unshaped output. The engagement fix (`segments`, ≥600 Hz) was found too late to re-sweep damping. **No bound; not attempted in the valid regime** |
| `curvature_gain`, `curvature_resample_enabled` | See results table — a constant-curvature circle cannot exercise curvature-*adaptive* logic. Values 0–5 are indistinguishable from the gate being off |
| `pts_per_1000_units` **on Vector as shipped** | Physically inert: `resample_enabled=True` routes `edgeInteriorCount()` down the resample branch. The 5/12 bounds above are valid **only with `resample_enabled=false`** |
| `min_blank_samples` **at stock `blank_pts_per_1000_units`** | Genuinely does not bind (`emitted_blank` pinned at 204 across 1–40). The 25 bound holds only in the short-jump regime |
| Wireframe, Text, Particles, Trails profiles | Camera patterns for Wireframe/Text/Particles now exist (added in fw v6.75.0), but none of the three were range-swept — see Open items. `Trails` still has no camera pattern at all |

---

## Open Items

Next-session-ready, cheapest first:

1. **Orientation, live-preset check.** `star`/`spiral`/`wireframe`/`text` reproducibly measure
   rotated/mirrored (rot180/mirror_y/mirror_x/mirror_y) relative to this tool's reference
   geometry — confirmed real, not a measurement artifact (changelog). What's never been
   checked is whether the *actual presets* sharing those point-generation paths render
   rotated/mirrored on a live show. Needs eyes-on comparison, not a camera-loop measurement.
   `text` is the interesting case: its glyph orientation was hardware-validated fixed already
   (v6.01.0/v6.05.3), yet `cam_text` (same `textrender::glyphOutlinePaths()`) still measures
   `mirror_y` — a live contradiction worth resolving first.
2. **`star`'s scale undershoot (X −5.9%, Y −5.7%).** Corner-dwell time is ruled out as the
   cause (2.3× `max_corner_pts` → no change) — it lands at a wrong *steady-state* position, not
   an under-settled one. Untested candidates: velocity-dependent approach into the vertex; a
   pixel-level look at the annotated capture right at the tip.
3. **`spiral`'s scale error, clean retest.** The `max_corner_pts` test on `spiral` used the CLI
   `measure` command, which — unlike `diagnose`'s `classifyProfile()` — does not apply
   `orientation.json`'s `mirror_y` compensation; the resulting capture was visibly broken
   (`pathCoveragePct` 19.7%, `cost: null`) and its scale-error reading (−9.6%→−4.5%) is not
   trusted. Re-test through `diagnose`.
4. **Three unverified incidental findings.** fw v6.75.0's release notes claimed to fix every
   item from the original audit's incidental-findings list, but only three were individually
   live-validated (see changelog). Still unconfirmed:
   - `blankLeakage` sampled only the rasterised ideal gap corridor and could read *lower* for a
     visibly worse blank-jump streak than for a clean one (anti-correlated over part of its
     range). `streak.py`'s direct corridor measurement was monotonic over the same sweep and
     could replace or supplement it.
   - Fallback constants (`brightnessNonUniformity=1.0`, `offsetX/YUnits=dacRange`) were
     indistinguishable from real readings in the `Metrics` record — no `valid`/NaN flag. The
     new camera patterns' `valid: true` field hints this may already be addressed, but that
     wasn't confirmed against the actual fallback path.
   - `/api/projection` reported `max_safe_kpps: 17.6` against an actual 42 kpps. Almost
     certainly an exposure-calculation output rather than an operational limit, but never
     confirmed — worth a look given the Class 4 classification.
5. **kpps re-testing.** Every PPS-scaled bound in the results table (`pts_per_1000_units`,
   `blank_pts_per_1000_units`, `resample_spacing_units`, `min`/`max_spacing_units`,
   `max_step_units`, `max_accel_units`) is only confirmed at the device's 42 kpps. No half-rate
   or double-rate runs were ever done, so read them as "valid at 42 kpps only" until re-checked
   through `applyPpsScaling`.
6. **`Trails`.** Its ground truth is a trajectory over time, not a static rasterisable ideal —
   no approach attempted, no weak proxy invented. Still needs one.
7. **`curvature_gain`.** Not measurable on this rig as currently equipped — every camera
   pattern is straight-only or curved-only/fixed-vertex. Needs a new calib pattern combining a
   straight run and a curved run in one frame, sized to trigger
   `curvature_resample_enabled`'s adaptive logic on the curved section only.
8. **`Vector`/`Particles` geometry offsets.** `diagnose --profile all` still flags both as
   "not fixable by autotune", unlike `MultiObject`/`segments` whose cause was found (changelog).
   No lead beyond the ruled-out ones below.

---

## Changelog

Resolved items, most recent first. All dates are live-device sessions unless noted.

- **2026-08-20 — `MultiObject`/`segments` scale error root-caused and fixed.**
  `/api/optimizer-stats` ruled out frame-budget truncation, Stage 1/1.5 crush, and
  velocity/accel clamping (all inactive). Corner dwell was the answer: `MultiObject`'s live
  `max_corner_pts` was 3, below the ~4 floor this audit's own results table established for
  corner rounding. Raised to 16 live → scale error roughly halved (−7.3%→−3.0%). Persisted
  (`/api/optimizer-live` + `/api/optimizer-save`, confirmed via `/api/config` read-back).
- **2026-08-18 — Second camera-pipeline bug found and fixed (`optimizeGalvo.py` v2.22.1→
  v2.23.0).** OpenCV's DSHOW backend queued frames internally with no flush; `grabAccumulated()`
  could bake the *previous* pattern's tail into the current capture (visible as a stray
  unmatched arc/line in annotated `diagnose` images — this is what had been misread as a
  "Y size off by +34.2%" geometry issue on `text`). Fixed via `CAP_PROP_BUFFERSIZE=1` +
  discarding 2 frames before every accumulation. Re-running after the fix reproduced the same
  four orientation mismatches (`star: rot180, spiral: mirror_y, wireframe: mirror_x,
  text: mirror_y`) with *sharper*, not weaker, fits — confirming they're real firmware
  coordinate-convention defects, not a measurement artifact (open item #1 above is what's left
  of this thread).
- **2026-08-18 — `measurementRepeats` validated live; `Smooth`/`Wireframe` re-tuned for
  real.** `measurementRepeats: 3` closed the ~30% search-vs-remeasure noise gap seen in the
  first follow-up session down to <3% for all three retested profiles. `Smooth` and
  `Wireframe` re-tunes now agree between search and independent re-measurement and were
  applied; `Text` was retested the same way and is still genuinely worse (not noise) — stays
  reverted. Also: the "smaller corner radius" theory for the geometry-offset findings was
  checked against source and live `/api/config` (`dac_limit_min=7000`/`max=63000`, tighter than
  design) and ruled out — `optimizeGalvo.py` reads back the ESP32's actual reported `corner_r`
  rather than assuming a fixed range, so a narrow `corners4` doesn't misplace other patterns.
  And: two back-to-back `diagnose` runs with no recalibration gave identical orientation
  transforms, ruling out single-shot detector noise as an explanation — except `circle`'s
  `rot180` flag turned out to be a real bug of its own: a geometrically-symmetric shape scoring
  a coin-flip-level (~0%) margin over identity. Fixed by requiring a ≥20% margin before trusting
  a non-identity transform (`ORIENTATION_MARGIN_FRAC`, v2.22.1); real mismatches all clear
  45–90% and were unaffected.
- **2026-08-18 — `Waves` re-tuned and applied.** 20-trial Optuna study, cost 11.589→11.301,
  confirmed better on independent re-measurement.
- **2026-08-18 — fw v6.75.0 fixes validated live.** `/api/optimizer-stats` now reports
  `stage1_blank_samples`/`stage1_blank_clamped`; `diagnose`'s path-deviation bar is now derived
  per-measurement instead of a fixed unreachable 150; `/api/config` now reports
  `galvo_kpps`/`galvo_rated_kpps`. Three new camera patterns (`cam_wireframe`, `cam_text`,
  `cam_particles`) landed in the same firmware release and were confirmed to capture, score
  `valid: true`, and correctly detect real smearing on `Particles`' untuned params — but were
  never range-swept (see Open items #4/#6).
- **2026-08-17 — Root blocking bug: camera threshold blinded every measurement.**
  `camConfig.json`'s `binaryThreshold: 133` left 5 lit pixels in the whole warped canvas
  (post-background-subtraction peak was 149); `computeMetrics()`'s near-empty-trace guard
  branches fired on effectively every measurement in the four Optuna studies run just before
  this audit, silently forcing fallback constants — while the annotated preview drew from the
  pre-threshold image and still looked fine. Root cause (`autotune-camera` had no
  signal-vs-noise term in its own objective) fixed in fw v6.75.0, landed the same day
  independently of this audit. Workaround used for this audit's own measurements:
  `binaryThreshold: 50` in a local, gitignored `auditCamConfig.json` (0 noise px, peak 141, no
  saturation) — the user's real `camConfig.json` was never touched.

---

## Tooling

Scratchpad-only, nothing committed except the gitignored camera config:

- `scripts/optimizeGalvo/auditCamConfig.json` — `camConfig.json` with `binaryThreshold: 50`
  (gitignored via `scripts/optimizeGalvo/*.json`). The one artefact from this audit worth
  keeping.
- `sweep.py` / `streak.py` — parameter sweep harness and the direct blank-jump streak metric,
  both reusing `optimizeGalvo`'s `EspClient`/`Camera`/`computeMetrics`.
- `agg.py`, `shot.py`, `diagThresh.py`, `diagNoise.py` — aggregation, annotated-frame capture,
  and the threshold/noise diagnostics behind the original §1 finding.
