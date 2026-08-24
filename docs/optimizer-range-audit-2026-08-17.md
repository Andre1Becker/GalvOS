# Optimizer Parameter Range Audit — 2026-08-17 (updated through 2026-08-24)

Camera-validated practical min/max ranges for `OptimizerLiveConfig`, measured live on the
bench rig described in [Chapter 6](06-camera-autotuning.md). Originally a single-day audit;
six live follow-up sessions (2026-08-18 ×3, 2026-08-20, 2026-08-22, 2026-08-24) chased open
threads it left behind. This revision removes narrative that later sessions fully resolved and
keeps only what's still actionable.

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
- `spiral`'s scale-error retest — done through `diagnose` (not `measure`), clean reading
  obtained: X −5.6%, Y −3.4%. The old `measure`-derived −9.6%→−4.5% number is superseded.
- Two of fw v6.75.0's three incidental findings, individually re-verified live: fallback
  constants are flagged (`valid`/NaN), not silently returned, and `max_safe_kpps` is confirmed
  a pure ILDA-derate formula (`rated_kpps × ilda_test_angle/exit_angle`, capped ≤60), unenforced
  anywhere in the tree — live reading `6.0` today (`15 × 8/20`), not the doc's stale `17.6`
  (that number was `44 × 8/20`, from before the rated_kpps 45→15 migration).
- `blankLeakage`'s anti-correlation, streak-swept against an independent corridor metric
  (`streak.py`) — does **not** reproduce today; see [Open items](#open-items) for why that's
  not quite the same as "fixed."
- PPS-scaled bounds re-tested at half/double the reference kpps — do **not** uniformly hold;
  see [Open items](#open-items).

**Still open** — see [Open items](#open-items):

- Orientation mismatch confirmed real on 4 camera-loop patterns (`star`/`spiral`/`wireframe`/
  `text`) — never eyes-on-checked against the actual live presets.
- `star`'s scale undershoot: four independent `OptimizerLiveConfig` knobs now tried
  (corner-dwell count, corner-severity threshold, blank-jump teleport dwell, a joint Optuna
  search with clamps unlocked) and none reach clean — `optimize`'s cost function is also
  provably blind to scale error, so it was never capable of finding a fix either way, confirming
  this is outside the `OptimizerLiveConfig` search space entirely. **Root cause now found,
  2026-08-24, via direct hardware capture (LA1010 SPI + Hantek analog Y)**: vertex 0's
  *commanded* DAC code doesn't undershoot at all (equal to or above vertices 1-4), but the
  *analog* galvo Y output does — it settles at only ~57% of the achieved-volts-per-code that
  vertices 1-4 get, and never cleanly (continuous jitter, no settling trend). Isolated to the
  one vertex approached via a blank jump instead of a live lit edge — see [Open items](#open-items)
  item 2 for the full readout. Not yet reconciled with the camera-side observation that X/Y
  undershoot nearly equally (a single Y-dominant vertex defect arguably shouldn't read that way
  through a homography-based scale-error fit) — flagged, not resolved, and the hardware finding
  is from one session, not yet reproduced.
- `blankLeakage` still carries a nonzero cost weight (2.0) despite fw v6.75.0's commit claiming
  "nothing load-bearing rests on it any more" — a real code/comment mismatch, currently
  harmless only because today's measured direction happens to agree with reality.
- PPS-scaled bounds break past the reference rate they were tuned at, with no downstream
  re-clamp — see [Open items](#open-items) for the numbers.
- `Trails` profile — unmeasured, no viable approach yet.
- `curvature_gain` — unmeasurable with the existing camera patterns.
- `Vector`/`Particles` geometry offsets — still flagged "not fixable by autotune". For `Vector`
  this is now understood *why* autotune can't fix it (cost function blind to scale error, see
  above) even though the underlying cause remains unknown.

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
2. **`star`'s scale undershoot (reproduced again 2026-08-24: X −5.6% to −6.3%, Y −5.0% to −5.5%
   across a full controlled sweep — essentially the same as every prior reading back to the
   original −5.9%/−5.7%. Real, stable, not noise — confirmed unreachable by `optimize` as
   currently built, AND every timing/dwell-budget theory tried so far is now ruled out; see
   below).** Concentrates entirely at `cam_star`'s `k==0`/`isFirst`
   vertex, not spread evenly across all 5 tips the way a generic dwell-count or arrival-speed
   effect would be — confirmed by pixel-level inspection of the annotated capture, mapped back
   through the tool's `rot180` orientation detection. `square` (same profile, same calibration)
   stays clean throughout, which rules out `diagnose`'s generic "suspect galvo calibration
   drift" explanation — a real calibration problem would show up there too.

   That one vertex is architecturally different from vertices 1-4 in three confirmed ways
   (`point_optimizer.cpp`/`.h`): it's the only one approached via a **blank jump** rather than
   a live lit edge; its frame-start corner dwell has its **first point forced blank** ("lift",
   `point_optimizer.h:30-33`) instead of a lit dwell point; and it's the only vertex counted
   **twice** in the point stream (frame-start dwell + a second "trailing" dwell closing the
   loop, `point_optimizer.cpp:915-931`). Net effect: vertex 0 gets more raw dwell points than
   the others, but a chunk of that time is spent on a blank-jump landing + a blanked
   confirmation sample rather than a live corner approach.

   **Ruled out (full trail condensed from 2026-08-22/24 sessions):**
   - *Corner-dwell time* — 2.3× `max_corner_pts` produced no change.
   - *Stale vertex-0 jump-in replaying a cached buffer* — retracted same session:
     `configFromLive()` never sets `hasPrevPos`, so `emitBlankJump()` always takes the
     no-interpolation branch for vertex 0 (nothing stale to replay); raw pre-warp photo
     inspection also showed every corner dwell blooming similarly, not just vertex 0 — more
     likely a homography-warp artifact than a firmware defect.
   - *Naive clamp flip* — turning on `vel_clamp_enabled`/`accel_clamp_enabled` at the profile's
     pre-existing, previously-inert `max_step_units=200`/`max_accel_units=800` made it
     measurably WORSE (89.8%→52.0% INVALID path coverage) — budget crush, not a fair test of
     whether decelerating into the corner helps.
   - **A proper joint `optimize --profile Vector` Optuna search, clamps unlocked in
     `searchSpace.json`, camera exposure re-tuned first (`autotune-camera --profile Vector
     --fresh --apply`) — see 2026-08-24 changelog.** 20/20 trials valid this time (the prior
     session's camera-timing confound is gone). Best trial (cost 61.17) left both clamps
     `false`, same as baseline, and dropped `square`'s cost 22.09→3.63 — but pushing those exact
     params live (`/api/optimizer-live`, RAM-only) and reading `diagnose --profile Vector`
     twice independently gave X −5.6% then −5.1%, statistically the same as the −5.5% baseline.
     **Root cause: `computeMetrics()`'s `cost` formula (`optimizeGalvo.py`) never includes
     `scaleErrorXPct`/`scaleErrorYPct` — they're computed and used by `diagnose`'s
     `classifyProfile()`, but Optuna's objective in `optimize`/`autotune-camera` is built
     entirely from `pathDeviationRms`/`blankLeakage`/`cornerHotspot`/`brightnessNonUniformity`/
     saturation/blob terms.** `optimize` was never capable of finding a fix for this defect,
     regardless of trial count or which parameters are unlocked — it cannot see the thing it
     would need to reduce. This applies to every profile's search, not just `Vector`'s.
     Reverted live config to baseline immediately after the second read (confirmed via
     `/api/config`); nothing applied or persisted.

   **The vertex-0 "teleport dwell" theory itself — RULED OUT, 2026-08-24, via a controlled
   single-variable test.** `optimize` couldn't test it (blind cost function, above), so a
   standalone script held every other Vector param at baseline and swept only `blank_samples`
   across `searchSpace.json`'s full range (8→64, `measureRepeated`'s 3-rep averaging, reading
   `scaleErrorXPct`/`YPct` directly off the `Metrics` object rather than through `diagnose`'s
   5%-threshold filter). Result: **flat**. X ranged −5.63% to −6.27%, Y −5.01% to −5.47%, across
   the entire 8× range — no trend, the whole spread sits inside the doc's own established noise
   floor. If vertex 0's parked "teleport" ticks (the mechanism `blank_samples` directly controls)
   were stealing precision from the corner approach, this count should have moved the number.
   It didn't. (One methodological trap surfaced and was fixed while building this test: applying
   `Vector`'s tuned `profileCamera` override — `binaryThreshold=60`, tuned by `autotune-camera`
   against the *same* blind cost function — inflated the traced blob enough to read a spurious
   +48% X "scale error" on EVERY value, independent of `blank_samples`. `diagnose` never applies
   that override for exactly this kind of reason; matching its recipe exactly fixed it. A second,
   smaller instance of the same root cause: a capture setting tuned against a cost function blind
   to scale error can itself corrupt a scale-error reading.)

   **Reference-geometry thread — checked, cleared.** `idealPolylines('star', r)`
   (`optimizeGalvo.py`) and `cam_star()` (`calib_patterns.cpp`) use the identical formula —
   `angle = k*(4π/5) − π/2`, radius `r/2` both sides, `r=30000` on both (the file's own comment
   says the host geometry was deliberately written to mirror the firmware's). No definition
   mismatch to find here; this thread is closed.

   **The single-vertex premise itself doesn't fit the sweep data.** Vertex `k==0` sits at angle
   −90°, i.e. `x=0` exactly — if only that one vertex were rounded, X scale error should read
   near zero (its own X-extremes come from the *other* four vertices, untouched) while Y carries
   the whole defect. It doesn't: every clean reading this session has X and Y within ~1pp of each
   other (−5.6%/−5.0% to −6.3%/−5.5%). That's the signature of a roughly uniform, all-axis
   shrink, not a single point pulled toward center — which also fits the ALREADY-retracted
   "stale jump-in" hypothesis's own discarded evidence (raw-photo inspection: every corner
   blooms similarly, not just vertex 0). The 2026-08-22 pixel read that singled out one tip as
   uniquely rounded may itself have been the homography-warp artifact that retraction already
   flagged as the more likely explanation, applied a little too narrowly at the time.

   **Follow-up test motivated by "uniform shrink across all 5 sharp tips": sweep
   `corner_angle_deg` instead of dwell count — result contradicts the hypothesis, cleanly.**
   If corner-severity-driven dwell compensation were CAUSING the shrink, raising
   `corner_angle_deg` past the pentagram's 144° turn angle (`severity` → 0, no compensation at
   all) should make `star` read as clean as `square` (whose 90° turn already sits at low
   severity, 0.265, at baseline). Instead: `star` gets monotonically WORSE as `corner_angle_deg`
   rises — X/Y −6.2%/−4.9% at `cad=0` (max severity), −5.9%/−5.8% at baseline (57.57°), climbing
   to **−8.5%/−7.5% at `cad=150-170`** (severity ≈ 0, i.e. no dwell compensation). `square`
   stays flat (+0.5%/−0.5%) across the same full range, immune regardless of its own severity.
   Reads backwards from "compensation causes the shrink": more corner dwell mildly *helps*,
   removing it makes the tip miss by nearly 2× more. Baseline (57.57°) is already close to
   this knob's own optimum — consistent with why raising `max_corner_pts` (Open Item #2, first
   paragraph) found no further room either.

   **Where this leaves it, honestly.** Four independent tunable knobs now tried against this
   defect — corner-dwell count (`max_corner_pts`), corner-severity threshold
   (`corner_angle_deg`), blank-jump teleport dwell (`blank_samples`), and a full joint Optuna
   search with clamps unlocked — and not one gets `star` within noise of clean. The best any of
   them manage is the existing baseline, already near its own local optimum on the two knobs
   that show any gradient at all. This is no longer "haven't found the right parameter yet" — it
   increasingly looks like the `OptimizerLiveConfig` search space, as it exists today, cannot
   reach a fix for this. Two threads left, both outside that space:
   - `spiral` (`Waves` profile) shows a similar-magnitude undershoot via an unrelated
     point-generation path. Worth a quick check of whether the two share anything (both are the
     only two calib patterns with an "isFirst" open/self-intersecting geometry going through a
     single `optimize()` call) — but given corner-dwell knobs don't explain `star` either, don't
     expect this to resolve either just because it's cheap to check.
   - A genuine physical read: does a 15kpps galvo (36° pentagram tip, a very sharp direction
     reversal at speed) actually reach the ideal apex, or is ~5-6% short of a sharp spike simply
     what this rig's real mechanical dynamics produce, already close to as-compensated-as this
     parameter space allows? If so, "star's undershoot" reclassifies from an open defect to a
     documented, near-optimally-compensated physical limitation — not nothing, but a different
     kind of finding than a bug to keep hunting for.
   Not a `costWeights` change or an `emitBlankJump()` firmware patch at this point — neither is
   what this session's evidence points to.

   **2026-08-24, hardware capture session (Kingst LA1010 + Hantek 6022BE, see
   [`sigrok-capture-tool.md`](sigrok-capture-tool.md)) — resolves both remaining threads above,
   and reverses the "not an `emitBlankJump()` firmware patch" conclusion.** First hardware-level
   look at this defect rather than another camera measurement or Optuna sweep. Star pattern
   armed live via `/api/calib-cam/start`, DAC8562 SPI bus and galvo analog Y output captured
   separately (same repeating static pattern, so cross-loop self-consistency stands in for a
   shared trigger).

   *SPI capture (commanded DAC codes) — vertex 0 does NOT undershoot at the command level.*
   Decoded 12.7k clean frames (0 malformed). Peak commanded code at vertex 0's dwell plateau:
   13650.0 (the pattern's exact maximum), vs. 13649.1–13649.8 for vertices 1-4 — identical
   within DAC-LSB quantization. Plateau *mean* is actually ~0.9% higher at vertex 0 (13568.2 vs.
   13450.1), and its dwell run is 2.4× longer (83 samples vs. 34) — consistent with the
   architectural double-count already identified above, just showing that extra time reads as
   *more* commanded amplitude, not less. This rules out `point_optimizer`/geometry generation as
   the cause outright — closes the "spiral shares something with star" thread too, since
   whatever this is, it isn't in the point-generation path either pattern shares.

   *Analog capture (actual galvo Y voltage) — vertex 0 never gets there.* Commanded Y codes:
   vertex 0 = −13650 (unique, unambiguous), vertices 1/4 = +11043 (tied). Achieved, read
   directly off the scope over 17 consecutive loops: vertex 0 settles at only **−1.75 V**, and
   never cleanly — only 222 of ~3.1M samples fall within 0.05 V of that minimum, and a 1.1 ms
   zoom on the dwell window shows continuous ±0.2 V jitter with no settling trend toward a
   target. Vertices 1/4 settle at +2.49 V, rock solid — 69,926 samples within 0.05 V of the max.
   Expressed as achieved-volts-per-commanded-code (cancels out the uncalibrated absolute
   conversion constant, so this comparison holds regardless of it): vertex 0 achieves only
   **~57% of the V/code ratio vertices 1/4 get.** A command-independent, purely physical
   shortfall, isolated to the one vertex approached via a blank jump instead of a live lit edge.

   **This answers Open Item #2's second thread directly: it is not a generic "sharp 36° tip at
   15kpps" mechanical limit** — vertices 1-4 are equally sharp reversals, lit-edge-approached,
   and settle perfectly. The defect tracks the *approach mechanism* (blank jump vs. lit edge),
   not turn geometry or speed. That reopens `emitBlankJump()`'s landing/settling behavior on the
   near-zero-distance, steady-state-loop case as the concrete next step — the same one flagged
   as unconfirmed and left untouched two sessions ago, now backed by a direct hardware read
   instead of a hypothesis. Likely needs either more dwell time budgeted specifically for this
   transition, or its own ZV-shaper/timing treatment rather than the generic blank-jump path.
   Caveat: single capture session, not yet cross-validated by a repeat run the way other
   findings in this doc are — next session should reproduce before this becomes a firmware
   change.
3. **Three incidental findings, individually re-verified 2026-08-22 — two closed, one
   reopened in a different shape than expected.**
   - **`blankLeakage`'s anti-correlation: does not reproduce today.** Live sweep on `segments`
     (blank_samples 1→100, `streak.py`) shows `blankLeakage` falling monotonically 28.3→19.6 —
     *correlated* with the beam getting cleaner, cross-checked against an independently-coded
     corridor mean (`streak.py`'s `directCorridorMean()`, same ideal-gap coordinates, its own
     `cv2.line` mask — doesn't call `_darkCorridorMaskFor`) that tracked it to 3 decimal places.
     The original 0.947→2.56 rising reading almost certainly predates the later exposure/shutter
     fix (the "every camera measurement ever taken was a random 1.7% slice" entry, itself dated
     after v6.75.0) — under a broken sub-frame shutter the same sweep would have been measuring
     something else entirely. **Not closed clean, though:** `blankLeakage` is still summed into
     `cost` at weight 2.0 (`optimizeGalvo.py`'s `costWeights`, computeMetrics's `cost` formula),
     contradicting the v6.75.0 commit's "nothing load-bearing rests on it any more" — currently
     harmless only because the measured direction happens to agree with reality today. Zeroing
     that weight (or dropping the term) would make the claim true instead of coincidentally true.
   - **Fallback-constant flagging — CONFIRMED.** `computeMetrics()` builds an `invalidReasons[]`
     list on every blind-capture branch, sets `valid = not invalidReasons`, and forces
     `cost = NaN` when invalid. Matches the claim exactly; no live capture needed to confirm it,
     the mechanism is unambiguous in source.
   - **`max_safe_kpps` — CONFIRMED, live.** `rated_kpps × (ilda_test_angle/exit_angle)`,
     capped ≤60 (`web_ui.cpp`'s `/api/projection` handler), referenced nowhere else in the tree
     — a pure ILDA scan-angle derate, exactly as the v6.75.0 commit's own A.6 note concluded.
     Live right now: `15 × (8/20) = 6.0`, matching the reported field exactly. This audit's own
     `17.6` figure above is stale (`44 × 8/20`, before the rated_kpps 45→15 migration) — update
     to `6.0` wherever it's quoted.
4. **kpps re-testing — done 2026-08-22, bounds do NOT uniformly hold.** Hand-computed
   `applyPpsScaling` against all 8 live profiles (rated=15, reference output=44 kpps) and the
   firmware's own `constrain()` bounds for `pts_per_1000_units`, `blank_pts_per_1000_units`,
   `resample_spacing_units`, `min`/`max_spacing_units`, `max_step_units`, `max_accel_units`.
   - **Already out of bounds at the reference rate, no rate change needed:** Smooth (profile 1)'s
     effective `pts_per_1000_units` = 56.6, past its own 50-unit ceiling (raw 19.29, tuned above
     the 11 default).
   - **Double-rate (88 kpps):** density exceeds the ceiling on 5/8 profiles (Vector/Smooth/Waves/
     Trails/Text, up to 113 — 2.26× over), and `max_step_units` drops *below* its 50-unit floor
     for every profile still on the shared default raw=200 (34.1) — inert today only because
     `vel_clamp_enabled` is false on all of them; the corner-clamp A/B test in item 2 above used
     this exact raw=200 value and made `star` measurably worse once enabled, so this floor
     violation is not a hypothetical concern.
   - **Half-rate (22 kpps):** everything lands back inside bounds — density falls as the PPS
     ratio rises.
   - `resample_spacing_units`/`min`/`max_spacing_units`/`max_accel_units` stayed in bounds across
     the tested range for this device's current tuning (their raw defaults sit far enough from
     their bounds), not because anything re-clamps them.
   - **Root cause, confirmed in `point_optimizer.cpp`:** nothing re-clamps the *effective*
     (post-scaling) value — `constrain()` in `web_ui.cpp` only bounds what gets written into the
     raw NVS-backed field; `applyPpsScaling()`/`configFromLive()` apply the multiplier
     unconditionally (`point_optimizer.h:461-474`) and the optimizer consumes the result as-is
     (`point_optimizer.cpp:734`, `:1756-1757`). The density overshoot is likely rescued in
     practice by the separate Stage 1/1.5 frame-budget crush; the *bound itself* is simply not
     honored past the reference rate it was tuned at. Not fixed here — flagged with numbers.
5. **`Trails`.** Its ground truth is a trajectory over time, not a static rasterisable ideal —
   no approach attempted, no weak proxy invented. Still needs one.
6. **`curvature_gain`.** Not measurable on this rig as currently equipped — every camera
   pattern is straight-only or curved-only/fixed-vertex. Needs a new calib pattern combining a
   straight run and a curved run in one frame, sized to trigger
   `curvature_resample_enabled`'s adaptive logic on the curved section only.
7. **`Vector`/`Particles` geometry offsets.** `diagnose --profile all` still flags both as
   "not fixable by autotune", unlike `MultiObject`/`segments` whose cause was found (changelog).
   For `Vector` the *reason* "not fixable by autotune" is now understood — see Open Item #2 —
   `optimize`'s cost function doesn't score scale error at all. `Particles` unverified but
   likely the same mechanism, since it shares the identical cost formula.

---

## Changelog

Resolved items, most recent first. All dates are live-device sessions unless noted.

- **2026-08-24 (hardware capture session) — `star`'s vertex-0 undershoot root-caused to an
  analog settling failure, via direct DAC8562 SPI + galvo analog Y capture (Kingst LA1010 +
  Hantek 6022BE, see [`sigrok-capture-tool.md`](sigrok-capture-tool.md)).** First hardware-level
  look at this defect rather than another camera/Optuna session. SPI decode of the commanded DAC
  codes: vertex 0 gets the full commanded amplitude (13650.0, the pattern's exact max) — equal
  to or above vertices 1-4 (13649.1-13649.8) — ruling out `point_optimizer`/geometry generation.
  Analog Y capture over 17 loops: vertex 0 settles at only −1.75 V against a commanded −13650
  code, and never cleanly (only 222/~3.1M samples within 0.05 V of that minimum, continuous
  ±0.2 V jitter, no settling trend); vertices 1/4 settle at +2.49 V rock-solid (69,926 samples
  within 0.05 V of the max) against a smaller commanded +11043 code. Achieved-volts-per-
  commanded-code (a ratio, so the uncalibrated absolute V/code constant cancels out): vertex 0
  reaches only ~57% of what vertices 1/4 reach. Isolated to the one vertex approached via a
  blank jump instead of a live lit edge — reopens `emitBlankJump()`'s landing/settling behavior
  on the near-zero-distance, steady-state-loop case, previously flagged as unconfirmed
  (2026-08-22) and left untouched. Single session, not yet reproduced by a repeat capture; also
  not yet reconciled with the camera-side X/Y-symmetric undershoot reading two entries below.
- **2026-08-24 (continued yet further) — `corner_angle_deg` sweep contradicts the corner-
  severity hypothesis, and the single-vertex premise itself no longer fits the data.** Prompted
  by the `blank_samples` sweep's own result: X and Y undershoot `star` by nearly equal amounts
  (~-6%/-5%), not the Y-dominant signature expected if only vertex `k==0` (which sits at
  `x=0` exactly) were defective — closer to a uniform, all-5-tip shrink. Checked
  `idealPolylines('star')` against `cam_star()` for a reference-geometry mismatch first (cheap,
  code-only): identical formulas, both sides, ruled out. Then tested the "uniform corner-
  severity effect" theory directly: swept `corner_angle_deg` on `star` (0→170°, spanning
  `cornerSeverity()`'s full range past the pentagram's 144° turn) and `square` (0→85°) with
  everything else at baseline. Result contradicts the hypothesis: raising `corner_angle_deg`
  (LESS dwell compensation) makes `star` monotonically WORSE, from −6.2%/−4.9% at `cad=0` to
  −8.5%/−7.5% at `cad=150-170`, while `square` stays flat across its own full range regardless
  of severity. More corner dwell mildly helps `star`; baseline (57.57°) is already near this
  knob's own optimum, consistent with `max_corner_pts` finding no further room either. Net: four
  independent `OptimizerLiveConfig` knobs (corner-dwell count, corner-severity threshold,
  blank-jump teleport dwell, joint Optuna search) have now been tried against this defect and
  none reach clean. See Open Item #2 for the reframing this prompts — the search space itself
  may not contain a fix, and the remaining leads (`spiral`'s shared architecture, a genuine
  physical/mechanical read on the galvo's real performance at a 36°-tip spike) sit outside it.
- **2026-08-24 (continued further still) — controlled `blank_samples` sweep rules out the
  vertex-0 "teleport dwell" theory outright.** Since `optimize` can't test scale-error theories
  (prior entry), wrote a standalone script (scratchpad, not committed — same convention as
  `sweep.py`/`streak.py` elsewhere in this doc) reusing `optimizeGalvo.py`'s own `EspClient`/
  `Camera`/`measureRepeated`/`computeMetrics` to hold every other `Vector` param at baseline and
  sweep only `blank_samples` (8→64, `searchSpace.json`'s full range), reading
  `scaleErrorXPct`/`YPct` directly off each averaged `Metrics` object. Result: flat — X −5.63%
  to −6.27%, Y −5.01% to −5.47% across the whole range, no trend, inside the noise floor. The
  parked-tick count at vertex 0's blank-jump landing has zero measurable effect on the
  undershoot. One bug found and fixed while building this: applying `Vector`'s tuned
  `profileCamera` override (`binaryThreshold=60`) inflated the traced blob into a spurious +48%
  X reading on every value — `diagnose` never applies that override, and matching its exact
  recipe (global `binaryThreshold=112`, no profile-camera override) fixed it immediately. Net:
  corner-dwell count, clamp-enabled arrival speed, and now teleport-dwell count are all ruled
  out — three independent timing/dwell-budget theories, three controlled tests, zero effect.
  See Open Item #2 for the two untested threads this leaves (a shared cause with `spiral`, or a
  measurement-side `idealPolylines('star')` definition mismatch) — neither a `costWeights`
  change nor an `emitBlankJump()` patch is indicated by the evidence at this point.
- **2026-08-24 (continued) — camera exposure re-tuned, `Vector` Optuna search re-run clean, but
  root-caused to a blind spot in `optimize`'s own cost function rather than a firmware fix.**
  Followed the prior entry's own suggested fix: `autotune-camera --profile Vector --fresh
  --apply` first. Exposure itself didn't move (`-4`, 62.5ms — it already matched the un-clamped
  baseline's ~28ms/pass draw time from the table below; the *search space's* wider draw times
  were the actual problem, not the baseline capture). `binaryThreshold` 112→60 and `accumFrames`
  6→7 did change and were applied+saved to `camConfig.json`'s `profileCamera['Vector']`;
  verified square 99.8%/star 96.2% path coverage post-tune. Re-ran `optimize --profile Vector
  --trials 20 --fresh` with the same clamp-unlocked `searchSpace.json` from the aborted run:
  **all 20 trials valid, zero invalid-measurement recurrence** — the capture-rig limitation is
  fixed. Best trial (cost 61.17) kept both `vel_clamp_enabled`/`accel_clamp_enabled` at baseline
  `false` (Optuna's own choice, not a search-space restriction) and cut `square`'s cost
  22.09→3.63; `star`'s cost only 0.218→0.189. Pushed the best params live via
  `/api/optimizer-live` (RAM-only, independent of the calib-cam session) and ran `diagnose
  --profile Vector` twice: X scale error read −5.6% then −5.1%, statistically identical to the
  −5.5% baseline — **no real improvement on the actual defect**, despite the cost function
  reporting a big win. Traced why: `computeMetrics()`'s `cost` formula sums
  `pathDeviationRms`/`blankLeakage`/`blankCorridorLitPct`/`cornerHotspot`/
  `brightnessNonUniformity`/saturation/blob terms only — `scaleErrorXPct`/`scaleErrorYPct` are
  computed in the same function but routed only to `diagnose`'s `classifyProfile()`, never into
  `cost`. Every profile's `optimize` search shares this formula, so this isn't a `Vector`-only
  gap. Reverted live config to baseline immediately (confirmed via `/api/config` read-back: all
  changed fields — `corner_angle_deg`, `max_corner_pts`, `blank_samples`,
  `pts_per_1000_units`, etc. — match the pre-search snapshot exactly); nothing applied, nothing
  persisted to NVS. See Open Item #2 for the condensed dead-end trail this supersedes, and
  Open Item #7. Not pursuing the `Waves`/`spiral` clamp-unlock fallback this session — it would
  hit the identical blind cost function and burn another full search for the same non-result.
- **2026-08-24 — Optuna search on `Vector` with vel/accel clamp unlocked: aborted, camera-timing
  confound found, no result either way.** Added `vel_clamp_enabled`/`max_step_units`/
  `accel_clamp_enabled`/`max_accel_units` to `searchSpace.json`'s `Vector` entry (categorical
  gate + float ceiling, `max_step_units` 200-3000 / `max_accel_units` 200-6000) so density,
  corner dwell, and the clamp would get tuned together instead of one guessed value. `optimize
  --profile Vector --trials 20` ran 16 trials before aborting on Optuna's own ≥10%-invalid
  safety guard (2/16 trials produced no valid measurement). Root cause, printed by the tool
  itself: some trials' `blank_samples`/density combination pushed `square`'s per-pass draw time
  to 121.6ms against a fixed 62.5ms camera shutter (an exposure tuned for the un-clamped
  baseline, never re-tuned for what this wider search space could produce) — those captures
  only ever see a ~52-55% slice of the path. An `optimizeGalvo.py` capture-rig limitation, not a
  firmware result. Best-so-far before the abort (trial 13, cost 0.317, both clamps `true`,
  `corner_angle_deg` 10.1) doesn't beat the unclamped baseline decisively enough on 16 trials to
  conclude anything either way. Nothing applied or persisted (non-interactive session, and the
  run errored out before reaching the apply step regardless); live config confirmed back to the
  original snapshot via a fresh `/api/config` read-back (`calib_active: false`,
  `opt_profiles[0]` unchanged) — the calib-cam session's own crash-cleanup handled the revert,
  no manual fix needed. **If this is picked back up: run `autotune-camera --profile Vector
  --fresh --apply` first** (the tool's own suggested fix) so the shutter matches the wider
  search space's actual duty cycle, before re-running the `Vector` search.
- **2026-08-22 (continued further) — fw v6.75.0's three incidental findings individually
  re-verified; kpps re-testing done; both closed with real findings, not rubber stamps.**
  `blankLeakage`'s reported anti-correlation does not reproduce under today's corrected
  exposure/shutter (streak-swept `segments` blank_samples 1→100 via new `streak.py`, monotonic
  28.3→19.6, cross-checked against an independently-coded corridor mean matching to 3 decimals)
  — but the metric still carries a live, nonzero `cost` weight the commit claimed it didn't.
  Fallback-constant flagging and `max_safe_kpps` (advisory ILDA derate, `6.0` live today, not
  the stale `17.6`) both confirmed exactly as claimed. Separately, hand-computing
  `applyPpsScaling` for all 8 live profiles at half/double the reference kpps found the
  PPS-scaled bounds do NOT uniformly hold past the rate they were tuned at — Smooth is already
  over its density ceiling at the reference rate, and 5/8 profiles exceed it at double-rate,
  with `max_step_units` also dropping below its own floor for every profile on the shared
  default. Root cause: `applyPpsScaling()` has no post-scale re-clamp. See Open Items #3/#4 for
  the full numbers.
- **2026-08-22 (continued) — the vertex-0/blank-jump theory below was retracted same session;
  a live accel/vel-clamp A/B test made `star` worse, not better.** See Open Item #2's second
  and third paragraphs for the full trail — raw-photo pixel inspection and a `configFromLive()`
  code read killed the "stale jump-in" idea before it reached hardware; a live RAM-only
  `/api/optimizer-live` test enabling `vel_clamp_enabled`/`accel_clamp_enabled` at the profile's
  existing (previously-inert) `max_step_units`/`max_accel_units` collapsed path coverage from
  89.8% to an invalid 52.0% (budget crush, not a clean result either way) — reverted and
  confirmed back to baseline. Net: `star`'s cause is still open, narrowed to "not the vertex-0
  jump-in, not a naive clamp flip" rather than to a fix.
- **2026-08-22 — `spiral`'s scale-error retest, done clean; `star`'s undershoot narrowed to
  one specific vertex.** Started with a stale `homography.npz` (2 days old): the first
  `diagnose --profile Vector,Waves` on both `star` and `spiral` came back INVALID MEASUREMENT
  (path coverage 18-25%, offset X+10162 units on `star` — the camera/projection surface had
  moved enough since the 2026-08-20 session to break the pixel↔DAC mapping entirely). Re-ran
  `calibrate` first, per the tool's own guidance. After that: `spiral` (Waves profile) scores
  X −5.6%, Y −3.4%, `cost` 0.137, path coverage 78.1% — a clean, trusted reading through
  `diagnose`'s `classifyProfile()`, replacing the `measure`-derived −9.6%→−4.5% number that
  Open Item #3 (previous revision) flagged as unusable. `star` (Vector profile) reproduced its
  scale undershoot (X −5.5%, Y −4.6%, essentially unchanged from the original session) with a
  now-valid 90.0% path coverage, plus a newly-flagged `corner hot 0.50` (over the 0.35
  threshold) it hadn't tripped before. Pixel-level inspection of the annotated capture (all 5
  tips, zoomed) found the undershoot is not evenly spread across the star's 5 points — 4 tips
  are sharp, 1 is visibly rounded — and traced that one tip to `cam_star`'s `k==0` vertex via
  the tool's own `rot180` orientation mapping. See Open Item #2 for the full mechanism
  (blank-jump approach + a blanked landing-confirmation sample + double-counted dwell, all
  unique to that one vertex in `point_optimizer.cpp`/`.h`) — not yet fixed, just narrowed from
  "cause unknown" to a specific, checkable code path.
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
