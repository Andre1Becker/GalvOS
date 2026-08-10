# Claude Code Prompts — Optimizer Engine

Base commit: `13d6b8f` · Current version: `LASER_FW_VERSION=6.41.3` (platformio.ini line 23)
Order is binding within a wave; waves run sequentially.

---

## Standard preamble (prepend to every prompt)

```
Repo: GalvOS, worked locally (do NOT clone). Rules for this session:
- Before starting: `git status` must be clean. If not, stop and report — do
  not modify a dirty working tree. (The docs/optimizer-refactor/ state files are
  excluded via .git/info/exclude and will not appear in status; that is
  expected.) Create a branch opt/<prompt-number> from the current HEAD and work
  on it.
- Verify against the actual committed state via `git show HEAD:<file>`, not
  against assumptions about what earlier sessions did.
- Read docs/optimizer-refactor/{STATE,CONTRACT,DECISIONS}.md first; they are
  local, untracked, and carry the cross-session memory. Do not re-derive
  decisions already recorded there. Do not `git add` them.
- All code artifacts (code, comments, log strings, UI text, commit message) English ONLY.
- camelCase for new symbols, clean code, no comment archaeology ("this was the bug...").
- JSON responses via sendJsonPsram(), JsonDocument always with SpiRamAllocator.
- Large buffers via heap_caps_malloc(MALLOC_CAP_SPIRAM) + memreg::track(), never static DRAM.
- ESPAsyncWebServer: register specific routes before prefix-catch routes.
- Bump LASER_FW_VERSION in platformio.ini: patch for a localized fix, minor for a refactor across many call sites.
- Deliver a clean unified diff. Verify it applies onto a clean HEAD checkout:
  `git stash -u` (only if needed), `git apply --check`, then restore.
- Do NOT commit and do NOT merge. Propose a short, precise commit message.
- When done, append your result summary to docs/optimizer-refactor/STATE.md
  (prompt number, branch, files touched, decisions made, invariants now green,
  version bump) and record any "analyze-first" outcome in DECISIONS.md.
```

---

## Governing instructions (read before any wave)

The prompt roadmap below is fundamentally sound. Preserve its wave structure and
dependencies. The goal is not merely to compile: it is to make the GalvOS
optimizer physically correct, budget-correct, measurable, deterministic, and
structurally maintainable, while minimizing unintended behavioral changes.

### 1. Establish the Optimizer Contract first — as known-red tests

Write host-side regression tests for the core invariants BEFORE implementing any
wave. Several of these tests will fail against the current code — that failure is
the specification, not a defect. Commit them red, then let each wave turn its
invariants green. This makes roadmap progress measurable instead of binary.

Invariants the Contract must cover, at minimum:
- Frame budget is never exceeded (measured across all callers of a frame, not
  per optimize() call — see prompt 7).
- Every blank jump ends at its target within +-1 DAC unit (see prompt 2).
- No silent point loss: emitted lit + blank + truncated == accounted total.
- Velocity/accel physical limits hold: no emitted step > max_step_units, no
  emitted acceleration ||v_i - v_(i-1)|| > max_accel_units after prompt 11.
  This is the invariant that protects the hardware — treat it as load-bearing.
- All emitted coordinates lie within valid DAC range.
- Memory: alloc/free symmetry. Assert an allocation counter returns to its
  baseline after N frames (host build with a counting allocator shim); do not
  rely on "checked it manually".
- Determinism: identical config + identical input + identical frame index
  produces identical output. The optimizer consumes modulator/noise bindings
  (OPT_DENSITY) — fix the seed/frame index in the test so this is testable.
- Stats consistency: gLastStats fields agree with the actually emitted points.

### 2. Two output-equivalence classes — do not conflate them

Prefer defined numerical tolerances over bit-for-bit output. But there are two
distinct classes and they have different acceptance criteria:

- **Flag OFF / unchanged single-call callers:** output must be bit-identical.
  This is the safety net for every refactor prompt (15, 16, 18) and for the
  default-off gates (hasPrevPos, frameBudgetRemaining, reorderSegments).
- **Flag ON / deliberately changed callers:** prompts 6, 7, 20 change output on
  purpose and by far more than +-1 DAC unit (a teleport becomes a ramp; the
  budget reshapes point counts). Do NOT diff their output against baseline.
  Validate them against the Contract invariants only (ends at target, budget
  held, no point loss, limits respected).

Where a prompt says "byte-identical" (15, 16) it means the flag-OFF class.
Float reordering from caching (acosf/sqrtf order, occasional lroundf +-1) is
acceptable there only within +-1 DAC unit / +-1 point; if a change cannot meet
even that, treat it as a behavior change and justify it explicitly.

### 3. Keep requested / required / effective values separate

Do not silently modify user-facing values such as blank_samples. Distinguish
the requested value (from WebUI/NVS), the required value (what physics forces,
e.g. the ZV shift_pts floor in prompt 4), and the effective value actually used.
Expose the effective value through Stats/WebUI. Prompt 4 is the canonical case:
it may raise the effective blank_samples but must not overwrite the requested
one, and must report when it could not.

### 4. Respect the existing wave order

Instrumentation -> P0 correctness -> call-site fixes -> semantic decisions ->
structural refactor -> optimization & features -> cleanup. Do not skip
dependencies or combine unrelated waves.

### 5. Analyze before changing semantics

For prompts marked "analyze first" (12, 13, 14, 19, 21, 22), inspect the actual
code and all call sites, state the finding and the chosen solution briefly, then
modify. Do not implement the proposed solution blindly if the code shows a
better minimal one — the prompt's fix is a hypothesis, not a mandate.

### 6. Validate every change

Build and run the relevant host tests. Check alloc/free symmetry, point-budget
accounting, and output against the Contract invariants. Produce a clean unified
diff and verify it applies onto a clean HEAD checkout (`git apply --check`). Do
not commit and do not merge — the human applies and commits with their token.

---

## Wave overview

| Wave | Content | Prompts | Order |
|---|---|---|---|
| 0 | Instrumentation | 1 | — |
| 1 | Blanking chain (P0) | 2–5 | strict |
| 2 | Call-site chain (P0) | 6–7 | strict |
| 3 | Independent corrections (P1) | 8–11 | free |
| 4 | Clarify semantics | 12–14 | free |
| 5 | Performance & structure | 15–18 | strict |
| 6 | Tuning & features | 19–22 | free |
| 7 | Cleanup | 23 | last |

**Correction to the earlier recommendation:** the refactor (16) comes **before** the bisection (17), not after — byte-identical restructuring first, behavior change on top.

---

# Wave 0 — Instrumentation

First, because every following wave benefits from it measurably. After prompt 16 (refactor) the telemetry needs a small follow-up.

## 1. Optimizer telemetry

```
New feature: the optimizer currently does not measure what it produces.

Introduce an optimizer::Stats struct (emittedLit, emittedBlank,
jumpDistanceTotal, jumpCount, truncated, stage1Triggered, stage15Triggered,
stage2Scale, ringingActive) that optimize() fills per call, plus an
optimizer::gLastStats with frame accumulation (reset at frame start by
pattern_engine, analogous to gLiveTransform).

Expose it via a new route GET /api/optimizer-stats in web_ui.cpp
(response via sendJsonPsram(), JsonDocument with SpiRamAllocator; register
the route before any /api/optimizer prefix-catches).

Benefit: (a) makes visible exactly the class of bugs that is otherwise only
reconstructable by reading code (silent truncation, never-active ZV shaper,
budget under-utilization); (b) gives scripts/optimizeGalvo/optimizeGalvo.py
a ground-truth signal that the Optuna loop today only estimates indirectly
via the camera.

WebUI: compact display in the optimizer tab, no separate tab.
```

---

# Wave 1 — Blanking chain (P0)

Strict order: 2 changes the jump length, 3 corrects the budget that builds on it, 4 activates the stage only then, 5 clears up the edge cases.

## 2. ZV shaper endpoint bug

```
In src/patterns/point_optimizer.cpp, emitBlankJump(): the ZV convolution
shaped[i] = A1*u[i] + A2*u[i-shift] is emitted over the same point count
`total` as the unshaped trajectory. Correctly, the motion would have to be
extended by shift_pts. Consequence: the last blank point does not lie on
(x1,y1). Measured at blank_samples=100, ring_freq_hz=200, zeta=0.15,
30 kpps: shift_pts=76, A1=0.617/A2=0.383, endpoint at 68% of the distance.
Immediately after follows a lit corner point at the real vertex.

Fix: guarantee that the last shift_pts entries of u[] already lie on
(x1,y1) before shaping — i.e. enforce settle >= shift_pts and, if needed,
extend the trajectory by the difference (increase count accordingly,
kMaxBlankPts as a hard upper bound). If the budget is insufficient, set
shape_active=false instead of partial shaping.

Additionally: the budget in optimize() (blank_overhead) must cover the
extended jump length — prompt 3 cleans up this formula immediately after,
so keep the term correct here but keep it in one place.

Add a test case in the host build (cfg_stub.h path) that verifies: the last
emitted blank point == (x1,y1) within 1 DAC unit, over ring_freq_hz
50..1000 Hz and blank_samples 8..100.
```

## 3. blank_overhead over-reserves

```
src/patterns/point_optimizer.cpp line 751 computes
blank_overhead = (blank_samples + min_blank_samples) * (segment_count + 1).
But emitBlankJump() emits at most count <= blank_samples points; the settle
ticks are carved OUT of count (lines 132-139). The comment at lines 747-750
directly contradicts this.

Fix: set blank_overhead to the actually possible maximum length of a jump —
including the ZV extension from prompt 2 that landed immediately before.
Align the same expression in Stage 1 (retry_overhead, line 818) and in the
recompute at line 822; pull it into a helper maxBlankJumpPts(cfg) so there
is only one source. Remove the contradictory comments.
```

## 4. ZV shaper never active with defaults

```
In src/patterns/point_optimizer.cpp: shape_active requires shift_pts < total.
At ring_freq_hz=200 / galvo_kpps=30, shift_pts=76, blank_samples default
16 -> Pillar 3 is permanently inactive with factory settings, with no
feedback whatsoever to the user.

Fix:
1. In computeZvShaper(), return the required minimum jump (shift_pts) as a
   field.
2. In optimize(): if ringing_comp_enabled but blank_samples < required,
   raise blank_samples to the required value, provided the budget
   (effective_cap) allows it.
3. If not possible: set a status flag that is exposed via the existing
   opt_eff_* output in web_ui.cpp as opt_eff_ringing_active (bool) and
   opt_eff_ring_shift_pts (int), so the WebUI can show that compensation is
   not taking effect.
   Also set Stats.ringingActive from prompt 1.
No silent deactivation.
```

## 5. Skip zero-length jumps

```
src/patterns/point_optimizer.cpp, emitBlankJump(): when two consecutive
segments share a vertex — the normal case for wireframe chains —
min_blank_samples points are emitted anyway, even though dist ~ 0.

Fix: early return when dist is below a threshold (e.g. 4 DAC units, as a
constant with justification). The budget in optimize() may only be
underestimated by this, never overestimated — so maxBlankJumpPts() from
prompt 3 remains unchanged as the upper bound.
```

---

# Wave 2 — Call-site chain (P0)

Both prompts touch the same files and lines (preset_patterns.cpp, text_renderer.cpp). Strictly one after the other, not in parallel.

## 6. Blank jumps between sub-shapes are teleports

```
Many presets call optimizer::optimize(&seg, 1, o+n, m-n, cfg) in a loop
(src/patterns/preset_patterns.cpp lines 1104, 1125, 1213, 1227, 1299, 1314,
1805, 2355; src/patterns/text_renderer.cpp line 228 per glyph). Since out
points to o+n, n is internally 0, and emitBlankJump() takes the n==0
fallback: blank_samples points directly at the target, without a ramp from
the real last position. Pillar 2 (distance-proportional + smoothstep) and
Pillar 3 therefore only take effect within a single optimize() call; for
multi-call presets every jump is a hard position jump.

Fix: extend OptimizerConfig with `bool hasPrevPos = false; float prevX = 0,
prevY = 0;`. emitBlankJump() uses this start position when n==0 && hasPrevPos.
All multi-call call sites set it from o[n-1] (only when n > 0).
Default false -> byte-identical behavior for all single-call callers.
No behavior change when n > 0.
```

## 7. max_pts_per_frame is not a frame budget

```
In src/patterns/point_optimizer.cpp, optimize() computes effective_cap =
min(max_out, max_pts_per_frame) PER CALL. For multi-call presets and for
text (one optimize() per glyph), each call may write up to max_pts_per_frame;
the real upper bound is only PATTERN_POINTS_MAX (2048). The documented
flicker guarantee does not hold for a large share of the presets.

Fix: extend OptimizerConfig with `uint16_t frameBudgetRemaining = 0;`
(0 = unused, old behavior). When > 0,
effective_cap = min(max_out, frameBudgetRemaining). optimize() still returns
only the point count; the callers subtract it themselves.
Convert multi-call callers in preset_patterns.cpp and text_renderer.cpp:
initialize the budget once from gOptimizerConfig.max_pts_per_frame and
decrement it across the loop — the same call sites that prompt 6 just
extended with hasPrevPos.
Analyze and list all call sites first, before you patch.
```

---

# Wave 3 — Independent corrections (P1)

No dependencies among each other, any order, one session each.

## 8. Missing cross-field validation

```
In src/net/web_ui.cpp, min/max pairs are clamped independently, in both
paths: applyOptimizerOverrides() (from line 117) and POST /api/optimizer-live
(from line 1053).

Consequences:
- min_blank_samples > blank_samples is settable. Stage 1 in optimize()
  checks `blank_samples > min_blank_samples` -> never runs, the budget can
  no longer be reduced.
- min_corner_pts > max_corner_pts is settable. cornerPointCount() thereby
  becomes monotonically decreasing: sharp corners get FEWER points than
  soft ones.

Fix: after applying all fields in both paths, call a shared helper
normalizeOptimizerConfig(OptimizerLiveConfig&) that enforces
min_blank_samples <= blank_samples and min_corner_pts <= max_corner_pts
(correcting the min value downward in each case) and reports the corrected
values back in `applied`. Apply the same helper also in backup_manager.cpp
(import path, line ~314) and community_presets.cpp, so imported profiles
cannot bypass it.
```

## 9. applyTransform: capacity and leak

```
src/patterns/point_optimizer.cpp, applyTransform() / scratch block from
line 495.

1. kMaxXfVerts = 512; the comment claims "largest caller today declares
   PathVertex[64]". Reality: paint::generate() passes up to
   PAINT_STROKES_MAX(12) * PAINT_VERTS_PER_STROKE(96) = 1152 vertices ->
   with rotation active, strokes are silently dropped via `break`.
   calib cam_spiral passes 512 vertices, i.e. exactly zero headroom.
   Fix: raise kMaxXfVerts to 1280 (PSRAM, ~30 KB), correct the comment with
   the real caller numbers, and on the `break` due to exhausted scratch,
   emit a one-time LOG_W instead of silently dropping.

2. Leak: if only s_xf_segs fails, s_xf_verts is freed and nulled while
   s_xf_segs stays allocated. The next call only checks `if (!s_xf_verts)`
   and reallocates both -> leak per call.
   Fix: on failure free and null both.

3. free() instead of heap_caps_free() for heap_caps_malloc memory. Align it,
   and extend the host build shim (lines 7-16) accordingly.
```

## 10. applyPpsScaling incomplete

```
optimizer::applyPpsScaling() in src/patterns/point_optimizer.h scales only
pts_per_1000_units, max_step_units, max_accel_units. Missing:
- resample_spacing_units (spacing in units/point) -> must be *= r
  Current consequence: with resample_enabled=true, PPS scaling has ZERO
  effect on density, because edgeInteriorCount() then ignores
  pts_per_1000_units.
- blank_pts_per_1000_units (density per output tick) -> must be *= 1/r

Fix: add both, extend the header comment (model block) accordingly.
Then check whether web_ui.cpp line 663 (opt_eff_* output) should also expose
the two new effective values — if so, add them.
```

## 11. Accel clamp measures the wrong quantity

```
src/patterns/point_optimizer.cpp, clampScannerLimits() Pass 2 (lines
656-668): three problems.

1. `(mag - prevMag) > max_accel_units` compares magnitudes instead of
   vectors. Acceleration is ||v_i - v_(i-1)||. A 180-degree direction
   reversal at constant magnitude yields mag - prevMag = 0 and is not
   clamped, even though that is the load case with maximum torque — i.e.
   exactly the sharp corners the clamp is meant for.
2. prevMag is also set to mag after inserting a midpoint; correct would be
   mag/2, since the step was halved. Consequence: subsequent checks too lax.
3. Deceleration (mag - prevMag < -limit) is not handled, even though it
   loads symmetrically in mechanical terms.

Fix: store step vectors (dx, dy) instead of magnitudes, switch the criterion
to ||v_i - v_(i-1)|| > max_accel_units (covers 2 and 3 automatically), set
prevStep after an insertion to the actually emitted last sub-step.

max_accel_units keeps its unit (units/sample^2) and thus its existing WebUI
value range, but the semantics become stricter — note this in the commit
message, since hardware-tuned values must be recalibrated.
```

---

# Wave 4 — Clarify semantics

Must precede the refactor in Wave 5: all three decide which fields even belong in the new SegmentPlan struct.

## 12. min_segment_pts is ineffective

```
In src/patterns/point_optimizer.cpp, the min_segment_pts floor sits in the
RETURN VALUE of planSegment() (line 372). optimize() discards this return
value (line 733) and only sums the out parameters cp/ip, which are not
floored. The parameter is therefore fully ineffective — yet it is wired
into: WebUI slider, NVS ("opt_minsp", main.cpp line 138 / web_ui.cpp
line 426), backup_manager.cpp, community_presets.cpp validation, and all 8
optimizer profiles. The documented "serif fix" in text_renderer.cpp
lines 50-54 (min_segment_pts >= 3) is therefore also a no-op.

Decide with justification between two options and implement one:
(a) Repair: make the floor effective in optimize() by having planSegment()
    apply it to interior_total (not corner_total), and enforce a
    corresponding lower bound for ipts in emitSegment(), so plan and emit
    agree.
(b) Remove: strike the parameter from OptimizerConfig, OptimizerLiveConfig,
    WebUI, NVS, backup, and the community preset schema, with a migration
    path for existing saved profiles (unknown key is ignored).

Output the recommendation with justification first, then patch.
```

## 13. speedT is provably dead

```
src/patterns/point_optimizer.cpp, cornerSeverity() lines 242-261.
speedT = (stepLen - nominalStep)/nominalStep with
stepLen = inLen/(edgeInteriorCount(inLen)+1).
In non-resample mode, edgeInteriorCount = round(inLen/nominal), so
speedT > 0 requires u > round(u)+1 — mathematically impossible.
Verified numerically over 200k samples: max speedT = 0.0.
In resample mode, speedT reaches at most 0.5 (at u ~ 1.5).

The extensively documented wireframe-strut fix therefore does not take
effect at all in default mode; what actually works is solely the
`return 1.0f` rule for open endpoints (line 228).

Task: either switch speedT to a criterion that actually triggers in
non-resample mode (e.g. compare the incoming edge length against nominalStep
instead of the derived stepLen), or remove speedT and reduce the comment
block lines 195-223 to what the code actually does. Analyze first which
variant really improves the wireframe struts, and justify.
```

## 14. PathVertex::lift effective only at vertex 0

```
src/patterns/point_optimizer.cpp, emitSegment() lines 399-403: lift is
evaluated only via `first_point_overall && k == 0 && va.lift`, i.e.
exclusively for vertex 0 of the segment. The header comment in
point_optimizer.h promises pen-up mid-path ("after a pen-up in a text
glyph") — that does not exist.

Decide and implement:
(a) Implement the semantics: on va.lift of a non-zero vertex, insert a real
    emitBlankJump() to that vertex; planSegment() must budget these
    additional blank runs.
(b) Correct the header comment to the real semantics ("only the first vertex
    of a segment is consulted").
Check first whether an existing caller sets mid-path lift (grep across
src/patterns/ and src/net/). If not, (b) is sufficient.
```

---

# Wave 5 — Performance & structure

Strict order. 15 creates the cache, 16 restructures byte-identically, 17 puts the behavior change on top, 18 cleans up the callers.

## 15. cornerSeverity computed 4-6x per vertex

```
src/patterns/point_optimizer.cpp: cornerSeverity() is recomputed multiple
times per vertex — planSegment() (N+1 calls via cornerPtsAtVertex),
possibly a second plan pass in Stage 1.5 (another N+1), emitSegment() 3x per
edge (cornerPtsAtVertex(a) + easeIn + easeOut). Each call costs 1x acosf,
3x sqrtf and 1x edgeInteriorCount(). For the 512-vertex spiral
(calib_patterns cam_spiral) that is roughly 3000 acosf and 9000 sqrtf per
frame, roughly 3-6 ms on the S3 at 240 MHz — 10-18% CPU at 30 fps, for a
result that does not change from frame to frame.

Fix: precompute severity[] and edgeLength[] once per segment into a lazy
PSRAM scratch (heap_caps_malloc(MALLOC_CAP_SPIRAM) + memreg::track, same
pattern as s_clamp_scratch), sized by kMaxXfVerts. planSegment(),
emitSegment() and Stage 1.5 then only read from the cache. On a Stage 1.5
change of min/max_corner_pts, only cornerPointCount() has to be re-evaluated,
not cornerSeverity().

Report a before/after measurement via the existing cpu_monitor.
Byte-identical output is mandatory — verify against the host build.
```

## 16. Decouple plan/emit duplication

```
Refactor, behavior unchanged (verify byte-identical output).
src/patterns/point_optimizer.cpp: planSegment() and emitSegment() run two
separate, manually kept-in-sync edge loops. The "must match" invariant lives
only in comments (lines 442-450, 352-358) — exactly the class of bug from
which the Stage-1/Stage-2 ordering and the closed-path double dwell already
arose.

Fix: introduce a SegmentPlan struct that planSegment() fills (per vertex:
cornerPts; per edge: interiorPts, easeIn, easeOut, length) and that
emitSegment() consumes exclusively — emitSegment() then computes nothing
itself. The plan lives in the PSRAM scratch from prompt 15. This guarantees
the match structurally instead of by comment.

Fold the decisions from prompt 12 (min_segment_pts) and 14 (lift) into the
struct design.
Then: carry over the Stats fields from prompt 1 that were previously filled
in emitSegment() to the new structure.

Minor version bump (refactor across many call sites).
```

## 17. Re-plan after Stage 2

```
src/patterns/point_optimizer.cpp, optimize() Stage 2 (lines 878-900): the
scaling factor is a one-time estimate. edgeInteriorCount() then rounds per
edge with lroundf, and these roundings accumulate (documented in the comment
at line 920: 480-vertex circle, 1464 instead of 1300 points, 12.6%
overshoot). This is caught only by hard truncation at effective_cap in
emitAllSegments() — i.e. truncation precisely where Stage 1.5 is supposed to
prevent it.

Requires prompt 15 and 16: with cached lengths and the SegmentPlan, an
additional plan pass costs almost nothing.

Fix: extract planTotal(segments, count, cfg) -> uint32_t as a pure function,
and replace Stage 2 with a bisection over a global density scalar (3-4
iterations, fixed upper bound) that hits effective_cap from below instead of
undershooting or overshooting it. Stop once planTotal is within 2% below the
cap.

Document the side effect: the budget is thereby used better, patterns become
denser at the same max_pts_per_frame.
Fill Stats.stage2Scale and Stats.truncated from prompt 1 accordingly.
```

## 18. Merge liveOptimizerConfig duplication

```
There are 5 nearly identical liveOptimizerConfig() copies:
src/patterns/preset_patterns.cpp line 71, paint_patterns.cpp line 14,
calib_patterns.cpp line 535, text_renderer.cpp line 25 (as
textOptimizerConfig), net/helios_net.cpp line 63.

They have already drifted apart:
- text_renderer.cpp: does not set resample_enabled/resample_spacing_units
  at all.
- helios_net.cpp: does not set galvo_kpps (uses the struct default 30).
- Only preset_patterns.cpp applies the OPT_DENSITY modulator binding.

Fix: a function optimizer::configFromLive(const OptimizerLiveConfig&,
uint16_t ratedKpps, uint16_t outputKpps) in point_optimizer.h that maps all
fields and calls applyPpsScaling(). The 5 call sites reduce to this call plus
their respective specializations (text floors, modulator binding, Helios
subset). The new fields from prompt 6 (hasPrevPos/prevX/prevY) and 7
(frameBudgetRemaining) deliberately remain caller responsibility and do NOT
belong in configFromLive().
Explicitly assess the existing behavior differences: which are intentional,
which are drift? List the result before you patch.

Minor version bump.
```

---

# Wave 6 — Tuning & features

Only on a stable, instrumented base. Order free, but 21 (defaults) sensibly last, because 19/20 change the budget situation.

## 19. Brightness gradient on straight lines

```
Analysis task with a subsequent fix proposal.
src/patterns/point_optimizer.cpp, cornerSeverity() line 228: open endpoints
always return severity = 1.0. For a 2-vertex segment (line()), easeIn =
easeOut = 1.0, so shapeEdgeT(t) = smoothstep(t): the points sit densely at
both ends and thinly in the middle. For a long single line this is a visible
brightness gradient (middle darker). Additionally, each line costs
2 * max_corner_pts = 16 points before any interior point is produced at all
— substantial for row/grid presets (preset_patterns.cpp lines 911, 972).

Task: distinguish between "free end" (real path endpoint, needs dwell to
decelerate, but no velocity easing across the whole edge) and "shared vertex"
(wireframe strut meets face loop). Propose whether easeIn/easeOut should be
decoupled from cornerSeverity — dwell count and spacing shaping are
physically two different things. The SegmentPlan struct from prompt 16
already separates both fields.
Analyze and justify first, then patch.
```

## 20. Optimize jump order (TSP)

```
New feature in src/patterns/point_optimizer.cpp, emitAllSegments():
segments are currently processed in input order. For wireframes, text and
paint, however, the jump order is freely choosable.

Implement a nearest-neighbour pass over the segment start points
(O(S^2), S < 64, negligible) that additionally checks, for open segments,
whether the segment end is closer than the start — in which case the segment
is traversed backwards. Closed segments keep their vertex order (color
gradients along the edges are preserved) but may rotate their start vertex.

Gate via a new cfg field `bool reorderSegments` (default false ->
byte-identical). The saved jump distance is directly recovered point budget
and less ringing excitation; expected reduction of total jump distance
30-50% for wireframe/text. Verify the reduction via Stats.jumpDistanceTotal
from prompt 1.

Add a WebUI toggle in the optimizer tab, per profile (do not forget the NVS
key + backup_manager + community_presets schema).
```

## 21. Pillar-2 defaults make blanking constant

```
Analysis task, no blind patch. Run only once prompts 2-5 and 20 have landed
— they change the budget situation.

blank_pts_per_1000_units=8 and blank_samples=16 (OPT_DEFAULT_* in
include/config.h) yield a proportional range of only 750-2000 DAC units:
count = dist/1000 * 8, clamped to [min_blank_samples=6, 16]. Every jump over
2000 units — practically all in a +-32767 coordinate space — is clamped to
16. Pillar 2 is thereby effectively a constant.

Task: compute sensible defaults from the actual jump distances of the preset
classes (Wireframe, MultiObject, Text) so that the proportional range covers
the real distance range. Use the jumpDistanceTotal/jumpCount measured via
/api/optimizer-stats instead of estimates. Take into account that
blank_samples is simultaneously the budget per jump, that Stage 1 reduces it
further, and that the ZV shaper (prompt 4) demands a lower bound. Propose
values per optimizer profile (OPT_PROFILE_COUNT, index 0-7), not global.
Present the calculation before you patch.
```

## 22. ILDA optimizer Option B (open roadmap item)

```
src/ilda/ilda_player.cpp does not call optimizer:: at all. The velocity
clamp for ILDA runs instead in pattern_engine.cpp lines 1278-1286 via
clampScannerLimits() with a minimal clampCfg.

Task: evaluate Option B — additionally run ILDA frames through the blanking
stages (Pillar 2/3) by detecting contiguous blank runs in the frame and
replacing them with emitBlankTo(), while lit points remain untouched.

Constraint from the existing comment: resample and corner dwell deliberately
stay off, because ILDA timing (flicker, strobes) is encoded by the author.
Blanking, however, is not affected by that, as long as the total point count
of the blank run is preserved.

Deliver a feasibility analysis with risks first (point-count preservation,
color changes within blank runs, frames without blanking), then an
implementation proposal behind a cfg gate. Do not patch yet.
```

---

# Wave 7 — Cleanup

Last, so the comment patch does not collide with the structural changes.

## 23. Stale comments and doc drift

```
Pure comment/consistency patch in src/patterns/point_optimizer.h,
point_optimizer.cpp and include/config.h. No behavior change.

1. point_optimizer.h, max_pts_per_frame: comment "FLICKER BUDGET:
   45000/750 = 60 Hz" — the default is OPT_DEFAULT_MAX_PTS_PER_FRAME = 1010.
   Update to the real calculation, and reference the new
   frameBudgetRemaining semantics from prompt 7.
2. point_optimizer.h, OptimizerConfig::galvo_kpps = 30: the only field
   without an OPT_DEFAULT_* macro. Add OPT_DEFAULT_GALVO_KPPS in config.h and
   reference it.
3. OptimizerLiveConfig is described as "mirrors" of OptimizerConfig, but
   knows neither galvo_kpps nor transform. Make the comment precise.
4. point_optimizer.cpp: move the historical comment blocks in optimize()
   ("THIS WAS THE ACTUAL BUG behind the still-no-lines report", lines
   763-792, and the Stage-1.5 derivation lines 826-847) to docs/ and reduce
   them in the code to 2-3 lines each that describe WHAT the stage does, not
   which bug triggered it.
5. Update the pipeline comment in the header to the actual order after
   Wave 5.
```
