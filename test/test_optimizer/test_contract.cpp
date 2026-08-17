/**
 * test_contract.cpp -- the Optimizer Contract.
 *
 * Host-side regression tests for the eight invariants listed in
 * docs/optimizer-refactor/CONTRACT.md. Written before any refactor wave and
 * committed RED: several of these fail against the current optimizer, and
 * that failure IS the specification. Each wave turns its own invariants
 * green. Never weaken a test to make it pass.
 *
 * Test names mirror CONTRACT.md's invariant names one-for-one so the
 * checklist in STATE.md can be ticked straight from the runner output.
 *
 * Equivalence class is stated per test:
 *   CLASS-IDENTICAL  flag OFF / unchanged single-call callers -- output must
 *                    stay bit-identical (+-1 DAC unit only for float
 *                    reordering introduced by caching).
 *   CLASS-INVARIANT  flag ON / deliberately changed callers -- no baseline
 *                    diff, judged against the invariants alone.
 *
 * No hardware-dependent assertions live here. Real ringing and real galvo
 * response are validated on the bench, not on the host.
 */

#include <unity.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "alloc_probe.h"
#include "config.h"
#include "contract_features.h"
#include "fixtures.h"
#include "patterns/point_optimizer.h"
#include "warpGrid.h"
#include "brightnessField.h"
#include "inverseFilter.h"
#include "test_preset_matrix.h"

using optimizer::OptimizerConfig;
using optimizer::PathSegment;
using optimizer::PathVertex;

namespace {

// One frame buffer, sized like the firmware's. Static rather than
// stack-local: PATTERN_POINTS_MAX LaserPoints is 16 KB per buffer.
LaserPoint gFrame[PATTERN_POINTS_MAX];
LaserPoint gFrameB[PATTERN_POINTS_MAX];

// Rounding slack. Positions round through lroundf() at several stages, so an
// exact comparison would fail on ULP noise rather than on behavior.
constexpr float kDacTolerance   = 1.0f;
constexpr float kMotionTolerance = 2.0f;

char gMsg[256];

inline float dist(const LaserPoint& a, const LaserPoint& b) {
    float dx = (float)b.x - (float)a.x;
    float dy = (float)b.y - (float)a.y;
    return sqrtf(dx * dx + dy * dy);
}

// Field-by-field (not a whole-struct assignment -- InverseFilterConfig::
// enabled is volatile like WarpConfig/BrightnessConfig's own enabled flag,
// same convention those structs' tests already follow).
void resetInverseFilterConfig() {
    gInverseFilter.enabled  = false;
    gInverseFilter.regAlpha = 0.35f;
    gInverseFilter.x = InverseFilterAxisModel();
    gInverseFilter.y = InverseFilterAxisModel();
}

}  // namespace

void setUp(void) {}
void tearDown(void) {}

// ── 6. allocFreeSymmetric ───────────────────────────────────────────────
//
// CLASS-INVARIANT. [P9, P15]
//
// Runs FIRST on purpose. The optimizer's transform scratch is a lazily
// allocated static: once any earlier test has populated it, the allocation
// path under test is never entered again in this process.
//
// Two halves, in this order:
//   (a) exhausted heap -- when the first scratch allocation fails and the
//       second succeeds, applyTransform() must not strand the second block.
//       It currently does (it frees and clears only s_xf_verts), so every
//       further call overwrites a live pointer: one leaked block per frame.
//       Must run before (b): once the scratch is successfully allocated the
//       out-of-memory branch is unreachable for the rest of the process.
//   (b) steady state -- after a warm-up frame, 100 further frames must not
//       grow the live allocation count. Persistent scratch buffers are fine;
//       scratch that is re-allocated per frame is not.
void test_allocFreeSymmetric(void) {
    OptimizerConfig cfg = fx::baseCfg();
    cfg.transform          = optimizer::makeTransform(0.37f, 250.0f, -180.0f);
    cfg.vel_clamp_enabled  = true;
    cfg.accel_clamp_enabled = true;

    size_t segCount = 0;
    const PathSegment* segs = fx::closedSquare(segCount);

    // Probe sanity: without the --wrap link flags every count below is 0 and
    // the test would report a meaningless pass.
    allocProbe::reset();
    allocProbe::arm(true);
    void* probe = malloc(32);
    free(probe);
    allocProbe::arm(false);
    TEST_ASSERT_TRUE_MESSAGE(allocProbe::active(),
        "allocation probe never reached -- add -Wl,--wrap=malloc -Wl,--wrap=free");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1u, (uint32_t)allocProbe::total(),
        "allocation probe did not count a plain malloc()");
    TEST_ASSERT_EQUAL_INT32_MESSAGE(0, (int32_t)allocProbe::live(),
        "allocation probe did not count a plain free()");

    // (a) first scratch allocation fails, second succeeds, 100 frames.
    //
    // Pre-warm the clamp-scratch singleton (s_clamp_scratch) OUTSIDE the
    // armed window, via an identity-transform call: cfg has vel/accel clamp
    // on, so clampScannerLimits() lazily allocates its own persistent PSRAM
    // block the first time ANY call reaches it, same as the transform
    // scratch under test here -- but identity bypasses applyTransform
    // entirely (optimize()'s `if (!cfg.transform.isIdentity())` guard), so
    // this warms clamp scratch without touching transform scratch. Without
    // this, frame 0 of the armed loop below allocates clamp scratch for the
    // first time ever in this process -- a legitimate one-time cache, not a
    // leak -- and it would never free, permanently confounding starvedLive
    // with +1 unrelated to the applyTransform bug under test.
    OptimizerConfig warmClampCfg = cfg;
    warmClampCfg.transform = optimizer::AffineTransform();  // identity
    optimizer::optimize(segs, segCount, gFrame, PATTERN_POINTS_MAX, warmClampCfg);

    allocProbe::reset();
    allocProbe::arm(true);
    for (int frame = 0; frame < 100; frame++) {
        allocProbe::failNext(1);
        optimizer::optimize(segs, segCount, gFrame, PATTERN_POINTS_MAX, cfg);
    }
    allocProbe::arm(false);
    long starvedLive = allocProbe::live();

    // (b) steady state across 100 frames, heap healthy again.
    allocProbe::reset();
    optimizer::optimize(segs, segCount, gFrame, PATTERN_POINTS_MAX, cfg);  // warm-up
    allocProbe::arm(true);
    for (int frame = 0; frame < 100; frame++) {
        optimizer::optimize(segs, segCount, gFrame, PATTERN_POINTS_MAX, cfg);
    }
    allocProbe::arm(false);
    long steadyLive = allocProbe::live();
    allocProbe::reset();

    snprintf(gMsg, sizeof(gMsg),
             "out-of-memory scratch path leaks: %ld live allocations after 100 frames",
             starvedLive);
    TEST_ASSERT_EQUAL_INT32_MESSAGE(0, (int32_t)starvedLive, gMsg);

    snprintf(gMsg, sizeof(gMsg),
             "scratch re-allocated per frame: %ld live allocations after 100 frames",
             steadyLive);
    TEST_ASSERT_EQUAL_INT32_MESSAGE(0, (int32_t)steadyLive, gMsg);
}

// ── 1. budgetNeverExceeded ──────────────────────────────────────────────
//
// CLASS-INVARIANT. [P7]
//
// The flicker budget is a property of the FRAME, not of one optimize() call.
// A preset that draws three primitives calls optimize() three times into the
// same buffer; each call today sees the full max_pts_per_frame, so the frame
// can land at three times its budget. Nothing in the current signature lets a
// caller say how much of the budget is already spent -- that is what P7's
// frameBudgetRemaining adds.
void test_budgetNeverExceeded(void) {
    OptimizerConfig base = fx::baseCfg();
    base.max_pts_per_frame = 600;

    size_t frameTotal = 0;
    for (size_t call = 0; call < fx::kPresetCallCount; call++) {
        size_t segCount = 0;
        const PathSegment* segs = fx::presetCall(call, segCount);

        // What a converted multi-call preset does per sub-shape: the frame's
        // running point count is the only state, and frameContext() turns it
        // into the remaining budget plus the previous galvo position.
        OptimizerConfig cfg = base;
        if (!optimizer::frameContext(cfg, gFrame, frameTotal)) break;

        size_t n = optimizer::optimize(segs, segCount,
                                       gFrame + frameTotal,
                                       PATTERN_POINTS_MAX - frameTotal, cfg);

        snprintf(gMsg, sizeof(gMsg),
                 "call %u alone emitted %u > max_pts_per_frame %u",
                 (unsigned)call, (unsigned)n, (unsigned)base.max_pts_per_frame);
        TEST_ASSERT_TRUE_MESSAGE(n <= base.max_pts_per_frame, gMsg);

        frameTotal += n;
    }

    snprintf(gMsg, sizeof(gMsg),
             "frame of %u calls emitted %u > max_pts_per_frame %u",
             (unsigned)fx::kPresetCallCount, (unsigned)frameTotal,
             (unsigned)base.max_pts_per_frame);
    TEST_ASSERT_TRUE_MESSAGE(frameTotal <= base.max_pts_per_frame, gMsg);
}

// ── 2. blankJumpEndsAtTarget ────────────────────────────────────────────
//
// CLASS-INVARIANT. [P2, P4]
//
// A blank jump exists to put the mirror on (x1,y1) before the beam comes
// back on. Its last emitted point must therefore BE (x1,y1). Swept across
// ring_freq_hz 50..1000 Hz, blank_samples 8..100, shaper off and on: the ZV
// shaper convolves the trajectory with a delayed copy of itself and emits the
// result unmodified, so the final sample is a blend of the target and a
// mid-move position -- the jump lands short by however far the beam still had
// to travel over the shift window.
void test_blankJumpEndsAtTarget(void) {
    const float ringFreqs[]    = { 50.0f, 200.0f, 500.0f, 1000.0f };
    const uint8_t blankCounts[] = { 8, 16, 40, 100 };
    const float targetX = 18000.0f, targetY = 12000.0f;

    for (int shaper = 0; shaper <= 1; shaper++) {
        for (size_t f = 0; f < sizeof(ringFreqs) / sizeof(ringFreqs[0]); f++) {
            for (size_t b = 0; b < sizeof(blankCounts) / sizeof(blankCounts[0]); b++) {
                OptimizerConfig cfg = fx::baseCfg();
                cfg.ringing_comp_enabled = (shaper != 0);
                cfg.ring_freq_hz         = ringFreqs[f];
                cfg.blank_samples        = blankCounts[b];

                // Known starting position, far enough away that the jump is
                // distance-limited rather than clamped to a single sample.
                size_t n = 0;
                gFrame[n++] = LaserPoint(-20000, -15000, 255, 255, 255, 0);

                optimizer::emitBlankTo(gFrame, n, PATTERN_POINTS_MAX,
                                       targetX, targetY, cfg);

                TEST_ASSERT_TRUE_MESSAGE(n > 1, "blank jump emitted no points");

                float ex = fabsf((float)gFrame[n - 1].x - targetX);
                float ey = fabsf((float)gFrame[n - 1].y - targetY);
                snprintf(gMsg, sizeof(gMsg),
                         "shaper=%d ring=%.0fHz blank_samples=%u: jump ended at "
                         "(%d,%d), off target by (%.1f,%.1f) DAC units",
                         shaper, (double)ringFreqs[f], (unsigned)blankCounts[b],
                         (int)gFrame[n - 1].x, (int)gFrame[n - 1].y,
                         (double)ex, (double)ey);
                TEST_ASSERT_TRUE_MESSAGE(ex <= kDacTolerance && ey <= kDacTolerance, gMsg);
            }
        }
    }
}

// ── 2b. blankJumpEndsAtTarget -- dense ZV sweep [P2] ────────────────────
//
// CLASS-INVARIANT. P2's own regression case, finer than invariant 2's
// four-by-four grid: every 25 Hz from 50 to 1000 Hz, every blank_samples from
// 8 to 100, three damping ratios and both jump directions. The shaper's
// impulse delay is round(Td/2 / point_period), so it steps through every
// integer shift from ~300 down to ~15 across that frequency range -- the cases
// where shift lands exactly on the settle-tail boundary are the ones a coarse
// grid walks straight past.
//
// The rule under test: whether or not the trajectory ends up shaped, the LAST
// emitted blank point is the target. A shaped run that cannot fit its tail
// inside kMaxBlankPts must fall back to the unshaped trajectory, never to a
// partially shaped one that lands short.
void test_blankJumpEndsAtTarget_zvSweep(void) {
    struct Jump { float x0, y0, x1, y1; };
    const Jump jumps[] = {
        { -20000.0f, -15000.0f,  18000.0f,  12000.0f },   // long diagonal
        {  16000.0f,  14000.0f, -16000.0f, -14000.0f },   // the same, reversed
        {   -400.0f,    250.0f,    380.0f,   -260.0f },   // short, count-floored
    };
    const float dampings[] = { 0.02f, 0.15f, 0.60f };

    for (size_t j = 0; j < sizeof(jumps) / sizeof(jumps[0]); j++) {
        for (size_t d = 0; d < sizeof(dampings) / sizeof(dampings[0]); d++) {
            for (int freq = 50; freq <= 1000; freq += 25) {
                for (int blank = 8; blank <= 100; blank++) {
                    OptimizerConfig cfg = fx::baseCfg();
                    cfg.ringing_comp_enabled = true;
                    cfg.ring_freq_hz         = (float)freq;
                    cfg.ring_damping_ratio   = dampings[d];
                    cfg.blank_samples        = (uint8_t)blank;

                    size_t n = 0;
                    gFrame[n++] = LaserPoint((int16_t)jumps[j].x0,
                                             (int16_t)jumps[j].y0,
                                             255, 255, 255, 0);
                    optimizer::emitBlankTo(gFrame, n, PATTERN_POINTS_MAX,
                                           jumps[j].x1, jumps[j].y1, cfg);

                    snprintf(gMsg, sizeof(gMsg),
                             "ring=%dHz zeta=%.2f blank_samples=%d: jump emitted "
                             "no points", freq, (double)dampings[d], blank);
                    TEST_ASSERT_TRUE_MESSAGE(n > 1, gMsg);

                    float ex = fabsf((float)gFrame[n - 1].x - jumps[j].x1);
                    float ey = fabsf((float)gFrame[n - 1].y - jumps[j].y1);
                    snprintf(gMsg, sizeof(gMsg),
                             "ring=%dHz zeta=%.2f blank_samples=%d: %u pts, ended "
                             "at (%d,%d), off target by (%.1f,%.1f) DAC units",
                             freq, (double)dampings[d], blank, (unsigned)(n - 1),
                             (int)gFrame[n - 1].x, (int)gFrame[n - 1].y,
                             (double)ex, (double)ey);
                    TEST_ASSERT_TRUE_MESSAGE(
                        ex <= kDacTolerance && ey <= kDacTolerance, gMsg);
                }
            }
        }
    }
}

// ── 2c. zero-length jumps are skipped [P5] ──────────────────────────────
//
// CLASS-INVARIANT. Two segments that share a vertex -- every wireframe chain --
// used to pay a full min_blank_samples blank run for a move of no length:
// budget spent, and a laser-off gap punched into geometry that is continuous.
// Below kMinJumpDistUnits (4 DAC units) the jump must emit nothing at all.
//
// The bound is one-sided on purpose: skipping only ever leaves the frame
// SHORTER than the reserve optimize() computed, so maxBlankJumpPts() stays a
// valid upper bound (invariant 1 is unaffected).
void test_zeroLengthJumpSkipped(void) {
    const float offsets[] = { 0.0f, 1.0f, 3.0f };   // all under the 4-unit floor

    for (int shaper = 0; shaper <= 1; shaper++) {
        for (size_t o = 0; o < sizeof(offsets) / sizeof(offsets[0]); o++) {
            OptimizerConfig cfg = fx::baseCfg();
            cfg.ringing_comp_enabled = (shaper != 0);

            size_t n = 0;
            gFrame[n++] = LaserPoint(5000, -3000, 255, 255, 255, 0);
            optimizer::emitBlankTo(gFrame, n, PATTERN_POINTS_MAX,
                                   5000.0f + offsets[o], -3000.0f, cfg);

            snprintf(gMsg, sizeof(gMsg),
                     "shaper=%d offset=%.0f: emitted %u blank points for a jump "
                     "of no length", shaper, (double)offsets[o], (unsigned)(n - 1));
            TEST_ASSERT_EQUAL_UINT32_MESSAGE(1u, (uint32_t)n, gMsg);
        }
    }

    // Just above the floor the jump must still happen -- the skip is a
    // threshold, not a general suppression of short jumps.
    OptimizerConfig cfg = fx::baseCfg();
    size_t n = 0;
    gFrame[n++] = LaserPoint(5000, -3000, 255, 255, 255, 0);
    optimizer::emitBlankTo(gFrame, n, PATTERN_POINTS_MAX, 5040.0f, -3000.0f, cfg);
    TEST_ASSERT_TRUE_MESSAGE(n > 1, "a 40-unit jump was skipped as zero-length");
    TEST_ASSERT_TRUE_MESSAGE(fabsf((float)gFrame[n - 1].x - 5040.0f) <= kDacTolerance,
                             "short jump did not land on its target");
}

// ── 2d. ringing compensation never fails silently [P4] ──────────────────
//
// CLASS-INVARIANT. Pillar 3 used to require shift_pts < the jump's own point
// count, so at factory settings (200 Hz / 30 kpps -> shift_pts 76, against a
// default blank_samples of 16) it could never engage -- and said so nowhere.
// Two halves:
//   (a) at factory settings with the box ticked, compensation must actually
//       run: ringingStatus() says active, and a rendered frame reports it.
//   (b) where it genuinely cannot run -- a ring period so long that the
//       impulse delay outgrows any jump the optimizer builds -- that must be
//       REPORTED as inactive, not quietly ignored, and the jump must fall back
//       to the unshaped trajectory (covered by the sweep above).
void test_ringingCompNotSilentlyInactive(void) {
#if GALVOS_OPT_HAS_STATS
    OptimizerConfig cfg = fx::baseCfg();
    cfg.ringing_comp_enabled = true;

    optimizer::RingingStatus rs = optimizer::ringingStatus(cfg);
    snprintf(gMsg, sizeof(gMsg),
             "factory settings (%.0f Hz, %u kpps, blank_samples %u): shaper "
             "reports inactive, shift_pts %d, min_jump_pts %d",
             (double)cfg.ring_freq_hz, (unsigned)cfg.galvo_kpps,
             (unsigned)cfg.blank_samples, rs.shift_pts, rs.min_jump_pts);
    TEST_ASSERT_TRUE_MESSAGE(rs.active, gMsg);
    TEST_ASSERT_TRUE_MESSAGE(rs.shift_pts > 0, "active shaper reports no impulse delay");

    size_t segCount = 0;
    const PathSegment* segs = fx::wireframeChain(segCount);
    optimizer::optimize(segs, segCount, gFrame, PATTERN_POINTS_MAX, cfg);
    TEST_ASSERT_TRUE_MESSAGE(optimizer::gLastStats.ringingActive,
        "ringingStatus() says active but no jump in the frame was shaped");

    // Disabled -> reported inactive, no shaping, no impulse delay claimed.
    cfg.ringing_comp_enabled = false;
    rs = optimizer::ringingStatus(cfg);
    TEST_ASSERT_FALSE_MESSAGE(rs.active, "shaper reports active while disabled");
    optimizer::optimize(segs, segCount, gFrame, PATTERN_POINTS_MAX, cfg);
    TEST_ASSERT_FALSE_MESSAGE(optimizer::gLastStats.ringingActive,
                              "a jump was shaped while the shaper is disabled");

    // Out of reach -> honestly reported as inactive rather than half-applied.
    cfg.ringing_comp_enabled = true;
    cfg.ring_freq_hz         = 50.0f;
    rs = optimizer::ringingStatus(cfg);
    snprintf(gMsg, sizeof(gMsg),
             "50 Hz needs a %d-point impulse delay -- must be reported inactive",
             rs.shift_pts);
    TEST_ASSERT_FALSE_MESSAGE(rs.active, gMsg);
    optimizer::optimize(segs, segCount, gFrame, PATTERN_POINTS_MAX, cfg);
    TEST_ASSERT_FALSE_MESSAGE(optimizer::gLastStats.ringingActive,
        "a jump was shaped although the impulse delay does not fit");
#else
    TEST_IGNORE_MESSAGE("needs P1 telemetry (gLastStats) -- see contract_features.h");
#endif
}

// ── 3. noSilentPointLoss ────────────────────────────────────────────────
//
// CLASS-INVARIANT. [P1, P17]
//
// Everything the optimizer planned must be accounted for: emitted lit points
// plus emitted blank points plus points dropped by the budget must equal the
// planned total. A frame that quietly loses geometry to an emit()-level cap
// is the failure mode this catches. Needs P1's telemetry to be observable at
// all -- see contract_features.h.
void test_noSilentPointLoss(void) {
#if GALVOS_OPT_HAS_STATS
    OptimizerConfig cfg = fx::baseCfg();
    cfg.max_pts_per_frame = 700;   // deliberately below what circle480 plans

    const fx::Fixture* fixtures = fx::all();
    for (size_t i = 0; i < fx::kFixtureCount; i++) {
        size_t n = optimizer::optimize(fixtures[i].segs, fixtures[i].count,
                                       gFrame, PATTERN_POINTS_MAX, cfg);

        const optimizer::Stats& st = optimizer::gLastStats;

        size_t lit = 0, blank = 0;
        for (size_t k = 0; k < n; k++) {
            if (gFrame[k].blank) blank++; else lit++;
        }

        snprintf(gMsg, sizeof(gMsg),
                 "%s: gLastStats lit/blank %u/%u vs buffer %u/%u",
                 fixtures[i].name, (unsigned)st.emittedLit,
                 (unsigned)st.emittedBlank, (unsigned)lit, (unsigned)blank);
        TEST_ASSERT_TRUE_MESSAGE(st.emittedLit == lit && st.emittedBlank == blank, gMsg);

        snprintf(gMsg, sizeof(gMsg),
                 "%s: %u emitted + %u truncated != %u planned",
                 fixtures[i].name, (unsigned)n, (unsigned)st.truncated,
                 (unsigned)st.plannedTotal);
        TEST_ASSERT_TRUE_MESSAGE(n + st.truncated == st.plannedTotal, gMsg);
    }
#else
    TEST_FAIL_MESSAGE(
        "optimizer telemetry (gLastStats) not implemented -- P1 owns this invariant");
#endif
}

// ── 3b. stage2PlansWithinBudget ─────────────────────────────────────────
//
// CLASS-INVARIANT. [P17] Sits alongside invariant 3: same accounting, read
// from the other end -- nothing may be truncated that a density choice could
// have prevented.
//
// Stage 2 picks ONE global density scalar and edgeInteriorCount() then rounds
// every edge on its own, so a single closed-form estimate lands wherever the
// accumulated rounding puts it. Overshoot is caught only by emitAllSegments()'s
// hard cap -- i.e. by truncation, which on a closed path means it stops before
// it closes, the exact failure Stage 1.5 exists to prevent.
//
// So: unless Stage 1.5 had to floor the corner counts -- the one case where
// the fixed overhead does not fit at ANY density and truncation is beyond the
// optimizer's reach -- a frame must not truncate at all.
//
// Clamps stay off: clampScannerLimits() inserts points after the plan and may
// legitimately reach the cap on its own (invariant 4 owns that).
void test_stage2PlansWithinBudget(void) {
#if GALVOS_OPT_HAS_STATS
    const uint16_t caps[] = { 2048, 1300, 1010, 900, 700, 500, 300, 120 };
    const fx::Fixture* fixtures = fx::all();

    for (int zv = 0; zv <= 1; zv++) {
        for (size_t c = 0; c < sizeof(caps) / sizeof(caps[0]); c++) {
            for (size_t i = 0; i < fx::kFixtureCount; i++) {
                OptimizerConfig cfg = fx::baseCfg();
                cfg.max_pts_per_frame    = caps[c];
                cfg.ringing_comp_enabled = (zv != 0);

                size_t n = optimizer::optimize(fixtures[i].segs,
                                               fixtures[i].count,
                                               gFrame, PATTERN_POINTS_MAX, cfg);
                const optimizer::Stats& st = optimizer::gLastStats;

                snprintf(gMsg, sizeof(gMsg),
                         "%s cap %u zv=%d: emitted %u points",
                         fixtures[i].name, (unsigned)caps[c], zv, (unsigned)n);
                TEST_ASSERT_TRUE_MESSAGE(n <= caps[c], gMsg);

                // Corner points alone over budget -- no density can fix that.
                if (st.stage15Triggered) continue;

                snprintf(gMsg, sizeof(gMsg),
                         "%s cap %u zv=%d: %u points truncated although the "
                         "corner budget fit -- the plan overshot the cap",
                         fixtures[i].name, (unsigned)caps[c], zv,
                         (unsigned)st.truncated);
                TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, st.truncated, gMsg);
            }
        }
    }
#else
    TEST_IGNORE_MESSAGE("needs P1 telemetry (gLastStats) -- see contract_features.h");
#endif
}

// ── 4. velocityAccelLimitsHold ──────────────────────────────────────────
//
// CLASS-INVARIANT. [P11]
//
// The hardware-protecting invariant. With both clamps enabled:
//   velocity     -- no lit-to-lit step longer than max_step_units
//   acceleration -- no lit-to-lit velocity CHANGE ||v_i - v_(i-1)|| longer
//                   than max_accel_units
//
// The acceleration half is vectorial by definition: a mirror decelerating
// from full speed into a corner dwell, or reversing direction at constant
// speed, is subject to exactly the same physical limit as one speeding up.
// The current pass compares scalar step magnitudes (mag - prevMag) and so
// only ever limits speeding up -- every corner dwell in every fixture drops
// the velocity to zero in one sample, unlimited.
//
// Interior density is lowered from the stock value so the velocity clamp has
// real subdivision work to do while the frame still fits the buffer. At stock
// density the subdivided straight edges alone fill PATTERN_POINTS_MAX, the
// stream is truncated before the first corner, and the acceleration pass never
// even runs -- the measurement would be vacuous rather than green.
void test_velocityAccelLimitsHold(void) {
    OptimizerConfig cfg     = fx::baseCfg();
    cfg.max_pts_per_frame   = PATTERN_POINTS_MAX;
    cfg.pts_per_1000_units  = 1.5f;
    cfg.vel_clamp_enabled   = true;
    cfg.max_step_units      = 200.0f;
    cfg.accel_clamp_enabled = true;
    cfg.max_accel_units     = 100.0f;

    const fx::Fixture* fixtures = fx::all();
    for (size_t i = 0; i < fx::kFixtureCount; i++) {
        size_t n = optimizer::optimize(fixtures[i].segs, fixtures[i].count,
                                       gFrame, PATTERN_POINTS_MAX, cfg);
        TEST_ASSERT_TRUE_MESSAGE(n > 2, "fixture emitted too few points to measure");

        for (size_t k = 1; k < n; k++) {
            if (gFrame[k - 1].blank || gFrame[k].blank) continue;   // blank runs are exempt
            float step = dist(gFrame[k - 1], gFrame[k]);
            snprintf(gMsg, sizeof(gMsg),
                     "%s: step %u->%u is %.1f > max_step_units %.1f",
                     fixtures[i].name, (unsigned)(k - 1), (unsigned)k,
                     (double)step, (double)cfg.max_step_units);
            TEST_ASSERT_TRUE_MESSAGE(step <= cfg.max_step_units + kMotionTolerance, gMsg);
        }

        for (size_t k = 2; k < n; k++) {
            if (gFrame[k - 2].blank || gFrame[k - 1].blank || gFrame[k].blank) continue;
            float vx0 = (float)gFrame[k - 1].x - (float)gFrame[k - 2].x;
            float vy0 = (float)gFrame[k - 1].y - (float)gFrame[k - 2].y;
            float vx1 = (float)gFrame[k].x     - (float)gFrame[k - 1].x;
            float vy1 = (float)gFrame[k].y     - (float)gFrame[k - 1].y;
            float dvx = vx1 - vx0, dvy = vy1 - vy0;
            float accel = sqrtf(dvx * dvx + dvy * dvy);
            snprintf(gMsg, sizeof(gMsg),
                     "%s: |dv| at %u is %.1f > max_accel_units %.1f "
                     "(v0=%.0f,%.0f v1=%.0f,%.0f)",
                     fixtures[i].name, (unsigned)k, (double)accel,
                     (double)cfg.max_accel_units,
                     (double)vx0, (double)vy0, (double)vx1, (double)vy1);
            TEST_ASSERT_TRUE_MESSAGE(accel <= cfg.max_accel_units + kMotionTolerance, gMsg);
        }
    }
}

// ── 5. dacRangeValid ────────────────────────────────────────────────────
//
// CLASS-IDENTICAL. Expected green already; if it ever goes red, that is a
// find, not a spec.
//
// Every emitted coordinate must survive the DAC encoding galvo_out.cpp
// performs: code = coordinate + 0x8000 as a uint16. -32768 would wrap the
// code to 0x0000 rather than clamping, so the emit path must keep positions
// inside +-32767. Driven with an 8x scale-up transform so the geometry is
// pushed well past the DAC range on purpose.
void test_dacRangeValid(void) {
    OptimizerConfig cfg = fx::baseCfg();
    cfg.max_pts_per_frame = PATTERN_POINTS_MAX;
    cfg.transform = optimizer::AffineTransform(8.0f, 0.0f, 4000.0f,
                                               0.0f, 8.0f, -4000.0f);

    const fx::Fixture* fixtures = fx::all();
    for (size_t i = 0; i < fx::kFixtureCount; i++) {
        size_t n = optimizer::optimize(fixtures[i].segs, fixtures[i].count,
                                       gFrame, PATTERN_POINTS_MAX, cfg);
        for (size_t k = 0; k < n; k++) {
            int32_t codeX = (int32_t)gFrame[k].x + 0x8000;
            int32_t codeY = (int32_t)gFrame[k].y + 0x8000;
            snprintf(gMsg, sizeof(gMsg),
                     "%s: point %u at (%d,%d) encodes to (%d,%d), outside 0..65535",
                     fixtures[i].name, (unsigned)k,
                     (int)gFrame[k].x, (int)gFrame[k].y, (int)codeX, (int)codeY);
            TEST_ASSERT_TRUE_MESSAGE(codeX >= 0 && codeX <= 0xFFFF &&
                                     codeY >= 0 && codeY <= 0xFFFF, gMsg);
        }
    }
}

// ── Prompt 7a: warp grid corners stay in range (I2 extension) ───────────
//
// CONTRACT.md's I2 explicitly calls out "corners of the warp grid" as a
// case Prompt 7 must cover: an aggressive, non-identity grid pushing
// control points out to the REST API's own validation bound (+-1.5, see
// web_ui.cpp's /api/warp/set) must still leave every emitted point's DAC
// code inside 0..65535. This exercises warpGrid.cpp::apply()'s own clamp,
// not the pre-existing optimizer clamp test_dacRangeValid above already
// covers (which runs with warp untouched/identity).
void test_warpGridCornersInRange(void) {
    // Full-scale square: corners sit exactly at the +-32767 extremes the
    // warp stage's [-1..1] normalization is built around.
    static const PathVertex v[4] = {
        PathVertex(-32767.0f, -32767.0f, 255, 255, 255),
        PathVertex( 32767.0f, -32767.0f, 255, 255, 255),
        PathVertex( 32767.0f,  32767.0f, 255, 255, 255),
        PathVertex(-32767.0f,  32767.0f, 255, 255, 255),
    };
    PathSegment seg(v, 4, true);

    gWarp.enabled  = true;
    gWarp.gridSize = 2;
    gWarp.points[0][0][0] = -1.5f; gWarp.points[0][0][1] = -1.5f;
    gWarp.points[0][1][0] =  1.5f; gWarp.points[0][1][1] = -1.5f;
    gWarp.points[1][0][0] = -1.5f; gWarp.points[1][0][1] =  1.5f;
    gWarp.points[1][1][0] =  1.5f; gWarp.points[1][1][1] =  1.5f;
    warp::refresh();

    OptimizerConfig cfg   = fx::baseCfg();
    cfg.max_pts_per_frame = PATTERN_POINTS_MAX;

    size_t n = optimizer::optimize(&seg, 1, gFrame, PATTERN_POINTS_MAX, cfg);
    TEST_ASSERT_TRUE_MESSAGE(n > 0, "warp corner test produced no points");
    for (size_t k = 0; k < n; k++) {
        int32_t codeX = (int32_t)gFrame[k].x + 0x8000;
        int32_t codeY = (int32_t)gFrame[k].y + 0x8000;
        snprintf(gMsg, sizeof(gMsg),
                 "warpGridCorners: point %u at (%d,%d) encodes to (%d,%d), outside 0..65535",
                 (unsigned)k, (int)gFrame[k].x, (int)gFrame[k].y, (int)codeX, (int)codeY);
        TEST_ASSERT_TRUE_MESSAGE(codeX >= 0 && codeX <= 0xFFFF &&
                                 codeY >= 0 && codeY <= 0xFFFF, gMsg);
    }

    // Restore the shared-process default (identity, disabled) so later
    // tests in this binary see pre-7a behavior.
    gWarp.enabled = false;
    gWarp.resetIdentity();
    warp::refresh();
}

// ── Prompt 7c: brightness field multiplies RGB, leaves geometry untouched ──
//
// Not itself one of CONTRACT.md's numbered invariants -- applyBrightnessField()
// never touches x/y or point count, so the I1/I2-style clamp risk warp's own
// test covers does not apply here the same way. Pins down the new per-point
// gain multiply directly instead: a 2x2 grid with one corner attenuated and
// the opposite corner left at identity (255), checked against the segment's
// own start/end vertices (corner dwell emits the literal vertex position, so
// their pattern-space coordinates -- and therefore which grid corner's gain
// applies -- are known exactly, unlike an interior/resampled point).
void test_brightnessFieldAppliesGain(void) {
    static const PathVertex v[2] = {
        PathVertex(-32767.0f, -32767.0f, 200, 100, 50),
        PathVertex( 32767.0f,  32767.0f, 200, 100, 50),
    };
    PathSegment seg(v, 2, false);

    gBrightness.enabled    = true;
    gBrightness.gridSize   = 2;
    gBrightness.gain[0][0] = 128;   // corner (-1,-1) -> pattern (-32767,-32767)
    gBrightness.gain[0][1] = 255;
    gBrightness.gain[1][0] = 255;
    gBrightness.gain[1][1] = 255;   // corner (1,1) -> pattern (32767,32767): identity
    brightness::refresh();

    OptimizerConfig cfg   = fx::baseCfg();
    cfg.max_pts_per_frame = PATTERN_POINTS_MAX;

    size_t n = optimizer::optimize(&seg, 1, gFrame, PATTERN_POINTS_MAX, cfg);
    TEST_ASSERT_TRUE_MESSAGE(n >= 2, "brightness test produced too few points");

    size_t first = 0;
    while (first < n && gFrame[first].blank) first++;
    TEST_ASSERT_TRUE_MESSAGE(first < n, "no lit point found");
    snprintf(gMsg, sizeof(gMsg), "attenuated corner: got rgb(%u,%u,%u)",
             gFrame[first].r, gFrame[first].g, gFrame[first].b);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE((200u * 128u) / 255u, (uint32_t)gFrame[first].r, gMsg);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE((100u * 128u) / 255u, (uint32_t)gFrame[first].g, gMsg);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(( 50u * 128u) / 255u, (uint32_t)gFrame[first].b, gMsg);

    size_t last = n - 1;
    while (last > first && gFrame[last].blank) last--;
    snprintf(gMsg, sizeof(gMsg), "identity corner: got rgb(%u,%u,%u)",
             gFrame[last].r, gFrame[last].g, gFrame[last].b);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(200u, (uint32_t)gFrame[last].r, gMsg);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(100u, (uint32_t)gFrame[last].g, gMsg);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE( 50u, (uint32_t)gFrame[last].b, gMsg);

    gBrightness.enabled = false;
    gBrightness.resetIdentity();
    brightness::refresh();
}

// ── Prompt 12b: inverse filter is a no-op when inactive ────────────────────
//
// Not one of CONTRACT.md's numbered invariants -- pins down the "falls back
// to Pillar 3 behavior when disabled" requirement structurally: apply() must
// be an exact pass-through, both when the feature is off and when an axis
// has no measured model (wnHz<=0), independent of whatever regAlpha/zeta
// happen to be sitting in config.
void test_inverseFilterPassthroughWhenInactive(void) {
    resetInverseFilterConfig();  // enabled=false, both axes unmeasured
    invfilter::refresh(30000);
    TEST_ASSERT_FALSE_MESSAGE(invfilter::isActive(), "disabled+unmeasured must report inactive");

    float x = 12345.0f, y = -6789.0f;
    invfilter::apply(x, y);
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(12345.0f, x, "disabled filter altered x");
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(-6789.0f, y, "disabled filter altered y");

    // Enabled, but no axis measured yet -- still must pass through.
    gInverseFilter.enabled = true;
    invfilter::refresh(30000);
    x = 500.0f; y = -250.0f;
    invfilter::apply(x, y);
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(500.0f, x, "enabled-but-unmeasured filter altered x");
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(-250.0f, y, "enabled-but-unmeasured filter altered y");

    resetInverseFilterConfig();
    invfilter::refresh(30000);
}

// ── Prompt 12b: inverse filter has unity DC gain ────────────────────────────
//
// H_inv(0) = wn^2 / (wn^2 * 1) = 1 analytically, for ANY wn>0/zeta/regAlpha>0
// -- the regularization only shapes the ROLLOFF, not the DC term. Since the
// bilinear transform maps s=0 to z=1 exactly, the discrete filter's settled
// response to a constant input must converge to that same input, regardless
// of which (wn, zeta, regAlpha) triple is under test. A model-independent
// invariant, so it doubles as a design-error trap: get the numerator/
// denominator derivation wrong (see inverseFilter.cpp's header comment) and
// this fails for every parameter combination, not just edge cases.
void test_inverseFilterDcGainUnity(void) {
    const float sampleRate = 30000.0f;
    const float wnCases[]    = { 50.0f, 210.0f, 800.0f };
    const float zetaCases[]  = { 0.0f, 0.15f, 0.6f };
    const float alphaCases[] = { 0.1f, 0.35f, 1.0f };

    for (float wn : wnCases) {
        for (float zeta : zetaCases) {
            for (float alpha : alphaCases) {
                resetInverseFilterConfig();
                gInverseFilter.enabled  = true;
                gInverseFilter.regAlpha = alpha;
                gInverseFilter.x.wnHz   = wn;
                gInverseFilter.x.zeta   = zeta;
                invfilter::refresh((uint32_t)sampleRate);
                invfilter::resetState();
                TEST_ASSERT_TRUE_MESSAGE(invfilter::isActive(), "expected filter to be active");

                const float target = 8000.0f;
                float x = target, y = 0.0f;
                // Settle: a 2nd-order filter's step response needs several
                // time constants: run for many periods of the SLOWEST
                // relevant rate (wn) to be sure it has converged.
                for (int i = 0; i < 4000; i++) { x = target; invfilter::apply(x, y); }

                snprintf(gMsg, sizeof(gMsg),
                         "DC gain != 1 for wn=%.0fHz zeta=%.2f alpha=%.2f: settled=%.2f (target=%.2f)",
                         wn, zeta, alpha, x, target);
                TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1.0f, target, x, gMsg);
            }
        }
    }

    resetInverseFilterConfig();
    invfilter::refresh((uint32_t)sampleRate);
}

// ── Prompt 12b: inverse filter stays bounded under pathological input ──────
//
// The filter's poles sit at a fixed double root s=-wn/regAlpha (left-half-
// plane for any regAlpha>0, wn>0) BY CONSTRUCTION -- the regularization, not
// the measured zeta, determines stability (see inverseFilter.cpp's header
// comment). Drives an extreme (near-Nyquist wn, high zeta, small regAlpha)
// configuration with a worst-case alternating (bang-bang) input for many
// iterations and asserts the output never diverges -- this is the property
// pattern_engine.cpp's own defense-in-depth clamp exists as a backstop for,
// not a substitute for the filter itself being sound.
void test_inverseFilterStableUnderPathologicalCoeffs(void) {
    const float sampleRate = 30000.0f;
    resetInverseFilterConfig();
    gInverseFilter.enabled    = true;
    gInverseFilter.regAlpha   = 0.05f;                 // smallest value the design floors to
    gInverseFilter.x.wnHz     = sampleRate * 0.45f;     // close to Nyquist
    gInverseFilter.x.zeta     = 0.9f;                    // clamp ceiling
    gInverseFilter.y.wnHz     = sampleRate * 0.45f;
    gInverseFilter.y.zeta     = 0.0f;                    // undamped extreme, other end
    invfilter::refresh((uint32_t)sampleRate);
    invfilter::resetState();
    TEST_ASSERT_TRUE_MESSAGE(invfilter::isActive(), "expected filter to be active");

    const float amp = 32767.0f;
    for (int i = 0; i < 5000; i++) {
        float x = (i % 2 == 0) ? amp : -amp;   // worst-case bang-bang excitation
        float y = (i % 3 == 0) ? amp : -amp;
        invfilter::apply(x, y);
        snprintf(gMsg, sizeof(gMsg), "inverse filter diverged at iter %d: x=%.1f y=%.1f", i, x, y);
        // Generous bound (not the DC-gain-1 tightness above) -- this test is
        // about boundedness, not accuracy: a stable-but-ringy filter can
        // legitimately overshoot a hard step several times over.
        TEST_ASSERT_TRUE_MESSAGE(isfinite(x) && fabsf(x) < amp * 20.0f, gMsg);
        TEST_ASSERT_TRUE_MESSAGE(isfinite(y) && fabsf(y) < amp * 20.0f, gMsg);
    }

    resetInverseFilterConfig();
    invfilter::refresh((uint32_t)sampleRate);
}

// ── 7. deterministicOutput ──────────────────────────────────────────────
//
// CLASS-IDENTICAL. Expected green already; if it ever goes red, that is a
// find, not a spec.
//
// Identical config plus identical input must give byte-identical output, run
// after run. Every optional stage is switched ON here -- transform, jitter,
// resample, ZV shaper, both clamps -- because a stage that carries state
// between calls (a scratch buffer read before it is written, a cached corner
// severity keyed on the wrong thing) only shows up once it has been run at
// least once before.
void test_deterministicOutput(void) {
    OptimizerConfig cfg      = fx::baseCfg();
    cfg.max_pts_per_frame    = PATTERN_POINTS_MAX;
    cfg.transform            = optimizer::makeTransform(0.61f, -900.0f, 640.0f);
    cfg.jitter_enabled       = true;
    cfg.resample_enabled     = true;
    cfg.ringing_comp_enabled = true;
    cfg.vel_clamp_enabled    = true;
    cfg.accel_clamp_enabled  = true;
    cfg.reorder_segments     = true;

    const fx::Fixture* fixtures = fx::all();
    for (size_t i = 0; i < fx::kFixtureCount; i++) {
        memset(gFrame, 0, sizeof(gFrame));
        memset(gFrameB, 0, sizeof(gFrameB));

        size_t n1 = optimizer::optimize(fixtures[i].segs, fixtures[i].count,
                                        gFrame, PATTERN_POINTS_MAX, cfg);
        size_t n2 = optimizer::optimize(fixtures[i].segs, fixtures[i].count,
                                        gFrameB, PATTERN_POINTS_MAX, cfg);

        snprintf(gMsg, sizeof(gMsg), "%s: point count differs between runs (%u vs %u)",
                 fixtures[i].name, (unsigned)n1, (unsigned)n2);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE((uint32_t)n1, (uint32_t)n2, gMsg);

        snprintf(gMsg, sizeof(gMsg), "%s: output differs between identical runs",
                 fixtures[i].name);
        TEST_ASSERT_EQUAL_INT_MESSAGE(
            0, memcmp(gFrame, gFrameB, n1 * sizeof(LaserPoint)), gMsg);
    }
}

// ── 8. statsConsistent ──────────────────────────────────────────────────
//
// CLASS-INVARIANT. [P1]
//
// Telemetry that disagrees with the emit path is worse than no telemetry: it
// makes every later wave's measurements wrong. jumpCount must match the blank
// runs actually written, jumpDistanceTotal the distance actually travelled
// with the beam off.
void test_statsConsistent(void) {
#if GALVOS_OPT_HAS_STATS
    OptimizerConfig cfg   = fx::baseCfg();
    cfg.max_pts_per_frame = PATTERN_POINTS_MAX;

    size_t segCount = 0;
    const PathSegment* segs = fx::wireframeChain(segCount);
    size_t n = optimizer::optimize(segs, segCount, gFrame, PATTERN_POINTS_MAX, cfg);

    const optimizer::Stats& st = optimizer::gLastStats;

    size_t lit = 0, blank = 0, runs = 0;
    float  jumpDist = 0.0f;
    bool   inRun = false;
    for (size_t k = 0; k < n; k++) {
        if (gFrame[k].blank) {
            blank++;
            if (!inRun) { runs++; inRun = true; }
            if (k > 0) jumpDist += dist(gFrame[k - 1], gFrame[k]);
        } else {
            lit++;
            inRun = false;
        }
    }

    TEST_ASSERT_EQUAL_UINT32_MESSAGE((uint32_t)lit, (uint32_t)st.emittedLit,
                                     "gLastStats.emittedLit disagrees with the buffer");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE((uint32_t)blank, (uint32_t)st.emittedBlank,
                                     "gLastStats.emittedBlank disagrees with the buffer");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE((uint32_t)runs, (uint32_t)st.jumpCount,
                                     "gLastStats.jumpCount disagrees with the buffer");

    float tolerance = jumpDist * 0.01f + 1.0f;
    snprintf(gMsg, sizeof(gMsg),
             "gLastStats.jumpDistanceTotal %.1f vs measured %.1f",
             (double)st.jumpDistanceTotal, (double)jumpDist);
    TEST_ASSERT_TRUE_MESSAGE(
        fabsf(st.jumpDistanceTotal - jumpDist) <= tolerance, gMsg);
#else
    TEST_FAIL_MESSAGE(
        "optimizer telemetry (gLastStats) not implemented -- P1 owns this invariant");
#endif
}

// ── 8b. reorderSegmentsShortensJumps [P20] ──────────────────────────────
//
// CLASS-INVARIANT. cfg.reorder_segments defaults false, so every test above
// exercises the OFF path; this is the only test that turns it on.
//
// Four short open segments, one per corner of a square, deliberately listed
// in CROSSED order (bottom-left, top-right, top-left, bottom-right) so the
// input-order tour jumps corner-to-diagonal-corner twice, while a
// nearest-neighbour tour walks the perimeter instead. No known previous
// position (cfg.hasPrevPos stays false), so both runs anchor the tour at the
// same first segment -- the only thing that can differ is which order the
// remaining three are visited in and jumpDistanceTotal is measured over.
void test_reorderSegmentsShortensJumps(void) {
#if GALVOS_OPT_HAS_STATS
    static const PathVertex segA[2] = {   // bottom-left
        PathVertex(-15000.0f, -15000.0f, 255, 255, 255),
        PathVertex(-14000.0f, -15000.0f, 255, 255, 255),
    };
    static const PathVertex segB[2] = {   // top-right (diagonal from A)
        PathVertex( 15000.0f,  15000.0f, 255, 255, 255),
        PathVertex( 14000.0f,  15000.0f, 255, 255, 255),
    };
    static const PathVertex segC[2] = {   // top-left
        PathVertex(-15000.0f,  15000.0f, 255, 255, 255),
        PathVertex(-14000.0f,  15000.0f, 255, 255, 255),
    };
    static const PathVertex segD[2] = {   // bottom-right (diagonal from C)
        PathVertex( 15000.0f, -15000.0f, 255, 255, 255),
        PathVertex( 14000.0f, -15000.0f, 255, 255, 255),
    };
    static const PathSegment segs[4] = {
        PathSegment(segA, 2, false),
        PathSegment(segB, 2, false),
        PathSegment(segC, 2, false),
        PathSegment(segD, 2, false),
    };

    OptimizerConfig cfg   = fx::baseCfg();
    cfg.max_pts_per_frame = PATTERN_POINTS_MAX;

    cfg.reorder_segments = false;
    size_t nOff = optimizer::optimize(segs, 4, gFrame, PATTERN_POINTS_MAX, cfg);
    optimizer::Stats stOff = optimizer::gLastStats;

    cfg.reorder_segments = true;
    size_t nOn = optimizer::optimize(segs, 4, gFrameB, PATTERN_POINTS_MAX, cfg);
    optimizer::Stats stOn = optimizer::gLastStats;

    // Same geometry, only reordered/possibly-reversed -- lit point count
    // (dwell + interior) must be unchanged; only the blank jumps differ.
    TEST_ASSERT_EQUAL_UINT32_MESSAGE((uint32_t)stOff.emittedLit,
                                     (uint32_t)stOn.emittedLit,
                                     "reorder changed the lit point count");

    snprintf(gMsg, sizeof(gMsg),
             "reorder OFF jumpDistanceTotal %.0f, ON %.0f -- expected a real "
             "reduction on a deliberately crossed layout",
             (double)stOff.jumpDistanceTotal, (double)stOn.jumpDistanceTotal);
    TEST_ASSERT_TRUE_MESSAGE(
        stOn.jumpDistanceTotal < stOff.jumpDistanceTotal * 0.9f, gMsg);

    // Determinism: reorder must not depend on anything but segs/cfg.
    size_t nOn2 = optimizer::optimize(segs, 4, gFrame, PATTERN_POINTS_MAX, cfg);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE((uint32_t)nOn, (uint32_t)nOn2,
                                     "reorder_segments=true is not deterministic");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        0, memcmp(gFrame, gFrameB, nOn * sizeof(LaserPoint)),
        "reorder_segments=true produced different output on a repeat run");
#else
    TEST_IGNORE_MESSAGE("needs P1 telemetry (gLastStats) -- see contract_features.h");
#endif
}

// ── 8c. reorder2optNeverWorse [P11a] ────────────────────────────────────
//
// CLASS-INVARIANT. cfg.reorder_2opt defaults false; this is the only test
// that turns it on. It layers on top of reorder_segments.
//
// Five point-segments: a thin rectangle (four corners) plus one far outlier
// above it. Greedy nearest-neighbour strands the outlier onto a long final
// hop; 2-opt reverses a block of the tour to fold the outlier back between the
// two near corners it belongs between. Since 2-opt starts from the greedy tour
// and only keeps a reversal that strictly shortens the FULL emitted cycle
// (closing blank included, see tourJumpCost()), the refined tour can never be
// worse than greedy -- and on this deliberately NN-hostile layout it is
// strictly shorter. No previous position, so both runs anchor identically.
void test_reorder2optNeverWorse(void) {
#if GALVOS_OPT_HAS_STATS
    // Each segment is a 2-vertex dot (v0 == v1), so it is a pure point in the
    // tour -- reversal is geometrically a no-op and only the visit order and
    // the blank hops between points matter.
    #define DOT(X, Y) { PathVertex((float)(X), (float)(Y), 255, 255, 255), \
                        PathVertex((float)(X), (float)(Y), 255, 255, 255) }
    static const PathVertex sA[2] = DOT(   0,    0);   // rect BL
    static const PathVertex sB[2] = DOT(9000,    0);   // rect BR
    static const PathVertex sC[2] = DOT(9000, 1800);   // rect TR
    static const PathVertex sD[2] = DOT(   0, 1800);   // rect TL
    static const PathVertex sE[2] = DOT(4500, 15000);  // far outlier above
    #undef DOT
    static const PathSegment segs[5] = {
        PathSegment(sA, 2, false),
        PathSegment(sB, 2, false),
        PathSegment(sC, 2, false),
        PathSegment(sD, 2, false),
        PathSegment(sE, 2, false),
    };

    OptimizerConfig cfg   = fx::baseCfg();
    cfg.max_pts_per_frame = PATTERN_POINTS_MAX;
    cfg.reorder_segments  = true;

    cfg.reorder_2opt = false;
    size_t nNN = optimizer::optimize(segs, 5, gFrame, PATTERN_POINTS_MAX, cfg);
    optimizer::Stats stNN = optimizer::gLastStats;

    cfg.reorder_2opt = true;
    size_t n2 = optimizer::optimize(segs, 5, gFrameB, PATTERN_POINTS_MAX, cfg);
    optimizer::Stats st2 = optimizer::gLastStats;

    TEST_ASSERT_TRUE_MESSAGE(nNN > 0 && n2 > 0, "reorder produced no output");

    // Same geometry -- only the visit order changes, so the lit point count
    // must be identical to the greedy-only run.
    TEST_ASSERT_EQUAL_UINT32_MESSAGE((uint32_t)stNN.emittedLit,
                                     (uint32_t)st2.emittedLit,
                                     "2-opt changed the lit point count");

    // Budget invariant still holds with 2-opt on.
    TEST_ASSERT_TRUE_MESSAGE(n2 <= (size_t)cfg.max_pts_per_frame,
                             "2-opt output exceeded the frame budget");

    // Never worse than greedy, and strictly better on this crossed layout.
    float tol = stNN.jumpDistanceTotal * 0.001f + 1.0f;
    snprintf(gMsg, sizeof(gMsg),
             "2-opt jumpDistanceTotal %.0f must be <= greedy %.0f",
             (double)st2.jumpDistanceTotal, (double)stNN.jumpDistanceTotal);
    TEST_ASSERT_TRUE_MESSAGE(
        st2.jumpDistanceTotal <= stNN.jumpDistanceTotal + tol, gMsg);

    snprintf(gMsg, sizeof(gMsg),
             "2-opt should strictly shorten this NN-hostile layout: greedy %.0f, "
             "2-opt %.0f", (double)stNN.jumpDistanceTotal,
             (double)st2.jumpDistanceTotal);
    TEST_ASSERT_TRUE_MESSAGE(
        st2.jumpDistanceTotal < stNN.jumpDistanceTotal - 1.0f, gMsg);

    // Determinism: identical config + input -> byte-identical output.
    size_t n2b = optimizer::optimize(segs, 5, gFrame, PATTERN_POINTS_MAX, cfg);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE((uint32_t)n2, (uint32_t)n2b,
                                     "reorder_2opt=true is not deterministic");
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        0, memcmp(gFrame, gFrameB, n2 * sizeof(LaserPoint)),
        "reorder_2opt=true produced different output on a repeat run");
#else
    TEST_IGNORE_MESSAGE("needs P1 telemetry (gLastStats) -- see contract_features.h");
#endif
}

// ── 8d. curvatureResampleDensifiesBends [P11b] ──────────────────────────
//
// CLASS-INVARIANT. cfg.curvature_resample_enabled defaults false; this is the
// only test that turns it on. It modifies the resample stage, so resample must
// also be on.
//
// A straight run has no turn angle, so curvature-adaptive resampling must
// leave it byte-identical to plain constant-spacing resample. A bent run (a
// right angle) has a real turn at its middle vertex, so its edges must
// densify. Both share the same total path length and base spacing, so the only
// thing that can differ is the curvature response.
void test_curvatureResampleDensifiesBends(void) {
#if GALVOS_OPT_HAS_STATS
    static const PathVertex straight[2] = {
        PathVertex(-8000.0f, 0.0f, 255, 255, 255),
        PathVertex( 8000.0f, 0.0f, 255, 255, 255),
    };
    static const PathVertex bent[3] = {   // right angle at the origin
        PathVertex(-8000.0f,    0.0f, 255, 255, 255),
        PathVertex(    0.0f,    0.0f, 255, 255, 255),
        PathVertex(    0.0f, 8000.0f, 255, 255, 255),
    };
    const PathSegment straightSeg(straight, 2, false);
    const PathSegment bentSeg(bent, 3, false);

    OptimizerConfig cfg      = fx::baseCfg();
    cfg.max_pts_per_frame    = PATTERN_POINTS_MAX;
    cfg.resample_enabled     = true;
    cfg.resample_spacing_units = 400.0f;
    cfg.curvature_gain       = 2.0f;
    cfg.min_spacing_units    = 40.0f;
    cfg.max_spacing_units    = 400.0f;

    // Straight run: curvature OFF vs ON must be byte-identical (turn == 0).
    cfg.curvature_resample_enabled = false;
    size_t sOff = optimizer::optimize(&straightSeg, 1, gFrame, PATTERN_POINTS_MAX, cfg);
    cfg.curvature_resample_enabled = true;
    size_t sOn = optimizer::optimize(&straightSeg, 1, gFrameB, PATTERN_POINTS_MAX, cfg);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE((uint32_t)sOff, (uint32_t)sOn,
        "curvature resample changed a straight run's point count");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, memcmp(gFrame, gFrameB, sOff * sizeof(LaserPoint)),
        "curvature resample altered a straight run (must collapse to base spacing)");

    // Bent run: ON must emit clearly more lit points than OFF.
    cfg.curvature_resample_enabled = false;
    optimizer::optimize(&bentSeg, 1, gFrame, PATTERN_POINTS_MAX, cfg);
    uint32_t bentOff = optimizer::gLastStats.emittedLit;
    cfg.curvature_resample_enabled = true;
    size_t bOnN = optimizer::optimize(&bentSeg, 1, gFrameB, PATTERN_POINTS_MAX, cfg);
    uint32_t bentOn = optimizer::gLastStats.emittedLit;

    TEST_ASSERT_TRUE_MESSAGE(bOnN <= PATTERN_POINTS_MAX,
        "curvature resample exceeded the frame budget");
    snprintf(gMsg, sizeof(gMsg),
        "curvature resample should densify a 90deg bend: off lit %u, on lit %u",
        (unsigned)bentOff, (unsigned)bentOn);
    TEST_ASSERT_TRUE_MESSAGE(bentOn > bentOff + bentOff / 2, gMsg);

    // Determinism.
    size_t bOnN2 = optimizer::optimize(&bentSeg, 1, gFrame, PATTERN_POINTS_MAX, cfg);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE((uint32_t)bOnN, (uint32_t)bOnN2,
        "curvature_resample_enabled=true is not deterministic");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, memcmp(gFrame, gFrameB, bOnN * sizeof(LaserPoint)),
        "curvature_resample_enabled=true produced different output on a repeat run");
#else
    TEST_IGNORE_MESSAGE("needs P1 telemetry (gLastStats) -- see contract_features.h");
#endif
}

// ── 8e. optimizeStreamRelaminates [P11b] ────────────────────────────────
//
// CLASS-INVARIANT. optimizeStream() splits an emitted LaserPoint stream back
// into PathSegments at blank boundaries and runs optimize() on them. Curves
// feed it their sampled polyline (leading blank + lit run). Verifies it
// handles a normal stream, tolerates in/out aliasing (the caller passes the
// same buffer), and returns 0 ("not handled") for an all-blank stream.
void test_optimizeStreamRelaminates(void) {
#if GALVOS_OPT_HAS_STATS
    // Leading blank jump + a 5-point lit polyline, exactly the shape a
    // continuous curve emits.
    LaserPoint stream[6] = {
        LaserPoint(-8000,    0, 0, 0, 0, 1),      // blank jump to start
        LaserPoint(-8000,    0, 255, 255, 255, 0),
        LaserPoint(-4000, 3000, 255, 255, 255, 0),
        LaserPoint(    0,    0, 255, 255, 255, 0),
        LaserPoint( 4000, 3000, 255, 255, 255, 0),
        LaserPoint( 8000,    0, 255, 255, 255, 0),
    };

    OptimizerConfig cfg   = fx::baseCfg();
    cfg.max_pts_per_frame = PATTERN_POINTS_MAX;

    size_t n = optimizer::optimizeStream(stream, 6, gFrame, PATTERN_POINTS_MAX, cfg);
    TEST_ASSERT_TRUE_MESSAGE(n > 0, "optimizeStream did not handle a normal curve stream");
    TEST_ASSERT_TRUE_MESSAGE(n <= PATTERN_POINTS_MAX, "optimizeStream exceeded the budget");
    TEST_ASSERT_TRUE_MESSAGE(optimizer::gLastStats.emittedLit > 0,
        "optimizeStream produced no lit points from a lit input");

    // Aliasing: in == out must produce the same result as the non-aliased run.
    memcpy(gFrameB, stream, sizeof(stream));
    size_t nAlias = optimizer::optimizeStream(gFrameB, 6, gFrameB, PATTERN_POINTS_MAX, cfg);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE((uint32_t)n, (uint32_t)nAlias,
        "optimizeStream point count changed when in and out aliased");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, memcmp(gFrame, gFrameB, n * sizeof(LaserPoint)),
        "optimizeStream output changed when in and out aliased");

    // All-blank stream: nothing to relaminate -> not handled (0).
    LaserPoint allBlank[3] = {
        LaserPoint(0, 0, 0, 0, 0, 1),
        LaserPoint(0, 0, 0, 0, 0, 1),
        LaserPoint(0, 0, 0, 0, 0, 1),
    };
    size_t nb = optimizer::optimizeStream(allBlank, 3, gFrame, PATTERN_POINTS_MAX, cfg);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, (uint32_t)nb,
        "optimizeStream should return 0 for an all-blank stream");
#else
    TEST_IGNORE_MESSAGE("needs P1 telemetry (gLastStats) -- see contract_features.h");
#endif
}

int main(int, char**) {
    UNITY_BEGIN();

    // allocFreeSymmetric first: it is the only test that can observe the
    // lazily allocated transform scratch being created.
    RUN_TEST(test_allocFreeSymmetric);

    RUN_TEST(test_budgetNeverExceeded);
    RUN_TEST(test_blankJumpEndsAtTarget);
    RUN_TEST(test_blankJumpEndsAtTarget_zvSweep);
    RUN_TEST(test_zeroLengthJumpSkipped);
    RUN_TEST(test_ringingCompNotSilentlyInactive);
    RUN_TEST(test_noSilentPointLoss);
    RUN_TEST(test_stage2PlansWithinBudget);
    RUN_TEST(test_velocityAccelLimitsHold);
    RUN_TEST(test_dacRangeValid);
    RUN_TEST(test_warpGridCornersInRange);
    RUN_TEST(test_brightnessFieldAppliesGain);
    RUN_TEST(test_inverseFilterPassthroughWhenInactive);
    RUN_TEST(test_inverseFilterDcGainUnity);
    RUN_TEST(test_inverseFilterStableUnderPathologicalCoeffs);
    RUN_TEST(test_deterministicOutput);
    RUN_TEST(test_statsConsistent);
    RUN_TEST(test_reorderSegmentsShortensJumps);
    RUN_TEST(test_reorder2optNeverWorse);
    RUN_TEST(test_curvatureResampleDensifiesBends);
    RUN_TEST(test_optimizeStreamRelaminates);

    // Easy/Medium/Hard preset regression matrix (test_preset_matrix.cpp) --
    // real preset geometry x a spread of OptimizerConfig combinations, see
    // that file's header comment.
    RUN_TEST(test_presetMatrixEasy);
    RUN_TEST(test_presetMatrixMedium);
    RUN_TEST(test_presetMatrixHard);
    RUN_TEST(test_presetMatrixInvalidParamsDegradeGracefully);

    return UNITY_END();
}
