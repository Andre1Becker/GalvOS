# Optimizer Contract — Invariants

Host-side regression tests. Written FIRST (Session A) and committed RED.
Several fail against 13d6b8f — that failure is the specification. Each wave
turns its invariants green. A test flips to green only when its prompt lands;
never weaken a test to make it pass.

Test location: test/optimizer/ (host build, no hardware).
Fixed inputs: seeded modulator/noise, fixed frame index, a small fixture set
of segments (one open line, one closed square, one wireframe chain with shared
vertices, one multi-call preset stand-in, one 480-vertex circle).

## Two output-equivalence classes

Every behavioral assertion belongs to exactly one:

- **CLASS-IDENTICAL** — flag OFF and unchanged single-call callers. Output
  bit-identical, tolerance +-1 DAC unit / +-1 point only for float-reordering
  from caching (P15/P16). Anything larger is a behavior change and must be
  justified.
- **CLASS-INVARIANT** — flag ON / deliberately changed callers (P6 hasPrevPos,
  P7 frameBudgetRemaining, P20 reorderSegments). NO baseline diff. Judged only
  against the invariants below.

Each test states its class in a comment.

## Invariants

1. **budgetNeverExceeded** — across all optimize() calls that build one frame,
   emitted total <= frameBudgetRemaining (or, pre-P7, <= PATTERN_POINTS_MAX and
   flagged). Measured per frame, not per call. [P7]

2. **blankJumpEndsAtTarget** — the last emitted point of every blank jump lies
   on (x1,y1) within +-1 DAC unit, over ring_freq_hz 50..1000 Hz and
   blank_samples 8..100, shaper on and off. [P2, P4]

3. **noSilentPointLoss** — emittedLit + emittedBlank + truncated == the
   accounted planned total for the frame. Nothing vanishes unaccounted. [P1, P17]

4. **velocityAccelLimitsHold** — no emitted step > max_step_units; no emitted
   acceleration ||v_i - v_(i-1)|| > max_accel_units after P11. Load-bearing:
   this is the hardware-protecting invariant. Holds for every fixture. [P11]

5. **dacRangeValid** — all emitted x/y within int16 DAC range; code = coord +
   0x8000 never wraps.

6. **allocFreeSymmetric** — with a counting allocator shim, the live allocation
   count returns to its pre-frame baseline after N=100 frames. Catches the
   applyTransform leak and any scratch mismanagement. [P9, P15]

7. **deterministicOutput** — identical config + identical input + identical
   frame index yields byte-identical output across repeated runs. Requires the
   seed/frame index to be fixed in the fixture. [all]

8. **statsConsistent** — gLastStats fields (emittedLit/Blank, jumpCount,
   jumpDistanceTotal, truncated) agree with what the emit path actually wrote.
   [P1]

## Notes

- Tests 1, 2, 3, 4, 8 are expected RED at 13d6b8f.
- Test 6 is RED for the applyTransform path (P9).
- Tests 5, 7 should be GREEN already; if not, that is a find — record it.
- Do not add hardware-dependent assertions here. Physical validation (real
  ringing, real galvo response) stays out of the host contract.
