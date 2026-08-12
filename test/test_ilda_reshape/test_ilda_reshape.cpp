/**
 * test_ilda_reshape.cpp -- P22 ILDA blank-run reshaping.
 *
 * optimizer::reshapeBlankRun()/reshapeBlankRuns() operate on an
 * already-rendered LaserPoint stream (ILDA playback never calls
 * optimize()), so none of the 8 CONTRACT.md invariants apply here -- this
 * is a separate suite, same "no hardware-dependent assertions" rule as
 * test_contract.cpp.
 *
 * See docs/optimizer-refactor/DECISIONS.md, Session Q/P22, for the design
 * this suite verifies against.
 */

#include <unity.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "patterns/point_optimizer.h"

using optimizer::OptimizerConfig;

namespace {

// Mirrors point_optimizer.cpp's own (file-local) kMaxBlankPts -- the public
// contract is "no-op above this", not the exact constant, but the suite
// needs a concrete number to build an oversized fixture with.
constexpr int   kMaxBlankPts   = 128;
constexpr float kDacTolerance  = 1.0f;

LaserPoint gFrame[1024];
LaserPoint gRef[1024];
char gMsg[256];

LaserPoint lit(int16_t x, int16_t y, uint8_t r, uint8_t g, uint8_t b) {
    return LaserPoint(x, y, r, g, b, 0);
}
LaserPoint blankPt(int16_t x, int16_t y) {
    return LaserPoint(x, y, 0, 0, 0, 1);
}

// Naive (non-eased) blank run from (x0,y0) to (x1,y1), `count` points -- what
// a raw ILDA-authored jump might look like before reshaping. Only the first
// and last slot are read by reshapeBlankRun() as the anchor coordinates, but
// filling the whole run with real, distinct values makes a bug that reads
// the wrong slot range observable.
void fillBlankRun(LaserPoint* dst, int count, float x0, float y0, float x1, float y1) {
    for (int i = 0; i < count; i++) {
        float t = (count > 1) ? (float)i / (float)(count - 1) : 1.0f;
        dst[i] = blankPt((int16_t)(x0 + (x1 - x0) * t), (int16_t)(y0 + (y1 - y0) * t));
    }
}

float dist(const LaserPoint& a, const LaserPoint& b) {
    float dx = (float)b.x - (float)a.x, dy = (float)b.y - (float)a.y;
    return sqrtf(dx * dx + dy * dy);
}

// Factory ZV coefficients (200 Hz / zeta 0.15 / 30 kpps -- shift_pts 76,
// min_jump_pts 78), same as OPT_DEFAULT_*. Only the fields
// pattern_engine.cpp's reshapeCfg actually sets are non-default here.
OptimizerConfig factoryCfg(bool ringingEnabled) {
    OptimizerConfig cfg;
    cfg.ringing_comp_enabled = ringingEnabled;
    cfg.galvo_kpps           = 30;
    return cfg;
}

// Higher ring frequency -> much shorter impulse delay (shift_pts ~15 at
// 1000 Hz/30 kpps instead of 76 at 200 Hz), so a modest-length run (30 pts)
// clears min_jump_pts and can actually be shaped -- needed to exercise the
// shaped branch without a >=78-point fixture.
OptimizerConfig fastRingCfg(bool ringingEnabled) {
    OptimizerConfig cfg = factoryCfg(ringingEnabled);
    cfg.ring_freq_hz = 1000.0f;
    return cfg;
}

}  // namespace

void setUp(void) {}
void tearDown(void) {}

// ── (a)+(b): frame point count and each run's own length are preserved ──
//
// A multi-run frame -- lit/blank/lit/blank/lit/... with varying run lengths,
// including one long enough to shape (1000 Hz cfg) -- must come out of
// reshapeBlankRuns() with the exact same total point count and the exact
// same sequence of run boundaries; only what is WRITTEN inside each blank
// run may change.
void test_pointCountAndRunLengthsPreserved(void) {
    size_t n = 0;
    gFrame[n++] = lit(-20000, -15000, 10, 20, 30);                          // A
    fillBlankRun(gFrame + n, 30, -5000, -3000, 18000, 12000); n += 30;      // R1 (30)
    gFrame[n++] = lit(18000, 12000, 40, 50, 60);                            // B
    fillBlankRun(gFrame + n, 1, 18000, 12000, 18010, 12010); n += 1;        // R2 (1)
    gFrame[n++] = lit(18010, 12010, 70, 80, 90);                            // C
    fillBlankRun(gFrame + n, 5, 18010, 12010, -9000, 6000); n += 5;         // R3 (5)
    gFrame[n++] = lit(-9000, 6000, 100, 110, 120);                          // D

    // Record the run-length sequence (lengths of contiguous blank==1 runs,
    // in order) before reshaping.
    auto runLengths = [](LaserPoint* buf, size_t total, int* out, int maxRuns) -> int {
        int nRuns = 0;
        size_t i = 0;
        while (i < total) {
            if (!buf[i].blank) { i++; continue; }
            size_t j = i;
            while (j < total && buf[j].blank) j++;
            if (nRuns < maxRuns) out[nRuns++] = (int)(j - i);
            i = j;
        }
        return nRuns;
    };

    int before[8], after[8];
    int nBefore = runLengths(gFrame, n, before, 8);

    optimizer::reshapeBlankRuns(gFrame, n, fastRingCfg(true));

    int nAfter = runLengths(gFrame, n, after, 8);

    TEST_ASSERT_EQUAL_INT_MESSAGE(nBefore, nAfter, "number of blank runs changed");
    for (int i = 0; i < nBefore; i++) {
        snprintf(gMsg, sizeof(gMsg), "run %d: length %d before, %d after", i, before[i], after[i]);
        TEST_ASSERT_EQUAL_INT_MESSAGE(before[i], after[i], gMsg);
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(3, nBefore, "fixture itself is wrong -- expected 3 runs");
}

// ── (c): every LIT point is byte-identical; only interior blank indices ──
// may change (boundary lit points untouched).
void test_litPointsByteIdentical(void) {
    size_t n = 0;
    gFrame[n++] = lit(-20000, -15000, 10, 20, 30);
    fillBlankRun(gFrame + n, 30, -5000, -3000, 18000, 12000); n += 30;
    gFrame[n++] = lit(18000, 12000, 40, 50, 60);
    fillBlankRun(gFrame + n, 1, 18000, 12000, 18010, 12010); n += 1;
    gFrame[n++] = lit(18010, 12010, 70, 80, 90);
    fillBlankRun(gFrame + n, 5, 18010, 12010, -9000, 6000); n += 5;
    gFrame[n++] = lit(-9000, 6000, 100, 110, 120);

    memcpy(gRef, gFrame, n * sizeof(LaserPoint));
    optimizer::reshapeBlankRuns(gFrame, n, fastRingCfg(true));

    for (size_t i = 0; i < n; i++) {
        if (gRef[i].blank) continue;   // only LIT points are asserted here
        snprintf(gMsg, sizeof(gMsg), "lit point at index %u changed", (unsigned)i);
        TEST_ASSERT_EQUAL_INT_MESSAGE(
            0, memcmp(&gRef[i], &gFrame[i], sizeof(LaserPoint)), gMsg);
    }
}

// ── (d): the last point of every reshaped run lands on its original ──────
// target, within +-1 DAC unit -- CONTRACT.md's own blankJumpEndsAtTarget
// tolerance. Swept across run lengths that fall on both sides of
// min_jump_pts, and shaper on/off.
void test_reshapedRunEndsAtTarget(void) {
    struct Case { int count; bool ringing; float freq; };
    const Case cases[] = {
        {   2, false, 200.0f },
        {   5, false, 200.0f },
        {  30, false, 200.0f },
        {  30,  true, 200.0f },   // too short to shape at 200 Hz -- plain fallback
        {  30,  true, 1000.0f },  // shapeable at 1000 Hz (min_jump_pts ~17)
        { 100,  true, 200.0f },   // shapeable at 200 Hz (min_jump_pts 78)
        { 128,  true, 200.0f },   // exactly kMaxBlankPts
    };

    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        const Case& tc = cases[c];
        size_t n = 0;
        gFrame[n++] = lit(-20000, -15000, 1, 2, 3);
        fillBlankRun(gFrame + n, tc.count, -5000.0f, -3000.0f, 18000.0f, 12000.0f);
        size_t i0 = n, i1 = n + (size_t)tc.count;
        n += (size_t)tc.count;
        gFrame[n++] = lit(18000, 12000, 4, 5, 6);

        LaserPoint originalTarget = gFrame[i1 - 1];

        OptimizerConfig cfg = factoryCfg(tc.ringing);
        cfg.ring_freq_hz = tc.freq;
        optimizer::reshapeBlankRun(gFrame, i0, i1, cfg);

        float ex = fabsf((float)gFrame[i1 - 1].x - (float)originalTarget.x);
        float ey = fabsf((float)gFrame[i1 - 1].y - (float)originalTarget.y);
        snprintf(gMsg, sizeof(gMsg),
                 "count=%d ringing=%d freq=%.0f: last point off target by (%.1f,%.1f)",
                 tc.count, tc.ringing, (double)tc.freq, (double)ex, (double)ey);
        TEST_ASSERT_TRUE_MESSAGE(ex <= kDacTolerance && ey <= kDacTolerance, gMsg);
    }
}

// ── positive check: reshaping actually rewrites the run ──────────────────
//
// Every assertion above (point count, run length, lit points, endpoint) is
// satisfied trivially by a no-op -- a naive linear blank run, by
// construction, already starts and ends on the right coordinates. This test
// closes that gap: a run built as a raw LINEAR ramp must come out DIFFERENT
// from reshapeBlankRun() (which applies the smoothstep ease), proving the
// primitive actually re-times the run instead of leaving it untouched.
void test_reshapedRunDiffersFromRawLinearRamp(void) {
    const int count = 30;
    size_t n = 0;
    gFrame[n++] = lit(-20000, -15000, 1, 2, 3);
    size_t i0 = n;
    fillBlankRun(gFrame + n, count, -5000.0f, -3000.0f, 18000.0f, 12000.0f);
    size_t i1 = n + (size_t)count;
    n = i1;
    gFrame[n++] = lit(18000, 12000, 4, 5, 6);

    memcpy(gRef, gFrame, n * sizeof(LaserPoint));

    optimizer::reshapeBlankRun(gFrame, i0, i1, factoryCfg(false));   // plain smoothstep ease

    bool anyDiff = false;
    for (size_t i = i0; i < i1; i++) {
        if (memcmp(&gFrame[i], &gRef[i], sizeof(LaserPoint)) != 0) { anyDiff = true; break; }
    }
    TEST_ASSERT_TRUE_MESSAGE(anyDiff,
        "reshaped run is byte-identical to the raw linear ramp -- reshapeBlankRun() no-op'd");
}

// ── positive check: the ZV shaper actually engages when it should ────────
//
// Complements test_shortRunFallsBackNotPartial (which proves the fallback
// path is taken when shaping does NOT fit) with the other direction: when a
// run IS long enough (count >= min_jump_pts), enabling ringing_comp_enabled
// must change the output relative to leaving it disabled -- otherwise the
// shaper could be permanently inert (e.g. always falling back) without any
// test here noticing.
void test_shapedRunDiffersFromUnshaped(void) {
    const int count = 100;   // >= min_jump_pts (78) at the factory 200 Hz
    auto build = [&](LaserPoint* buf, size_t& n) {
        n = 0;
        buf[n++] = lit(-20000, -15000, 1, 2, 3);
        fillBlankRun(buf + n, count, -5000.0f, -3000.0f, 18000.0f, 12000.0f);
        n += (size_t)count;
        buf[n++] = lit(18000, 12000, 4, 5, 6);
    };

    size_t n1 = 0, n2 = 0;
    build(gFrame, n1);
    build(gRef,   n2);

    optimizer::reshapeBlankRuns(gFrame, n1, factoryCfg(true));    // shaper ON, long enough
    optimizer::reshapeBlankRuns(gRef,   n2, factoryCfg(false));   // shaper OFF

    bool differs = memcmp(gFrame, gRef, n1 * sizeof(LaserPoint)) != 0;
    TEST_ASSERT_TRUE_MESSAGE(differs,
        "a run long enough to shape produced identical output with the shaper on vs off");
}

// ── (e): a run shorter than min_jump_pts reports the shaper inactive ─────
// rather than emitting a partially-shaped trajectory -- the same "report
// inactive rather than half-apply" rule P4 established for Pillar 3
// (optimize()'s emitBlankJump()), reused here rather than reinvented.
//
// There is no direct Stats/RingingStatus surface on this path (it does not
// go through optimize()), so this is verified structurally: a run too short
// to shape (30 pts at the factory 200 Hz, min_jump_pts 78) must produce
// BYTE-IDENTICAL output whether ringing_comp_enabled is true or false --
// proving the shaper contributed nothing rather than something partial.
void test_shortRunFallsBackNotPartial(void) {
    auto build = [](LaserPoint* buf, size_t& n) {
        n = 0;
        buf[n++] = lit(-20000, -15000, 1, 2, 3);
        fillBlankRun(buf + n, 30, -5000.0f, -3000.0f, 18000.0f, 12000.0f);
        n += 30;
        buf[n++] = lit(18000, 12000, 4, 5, 6);
    };

    size_t n1 = 0, n2 = 0;
    build(gFrame, n1);
    build(gRef,   n2);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE((uint32_t)n1, (uint32_t)n2, "fixture mismatch");

    optimizer::reshapeBlankRuns(gFrame, n1, factoryCfg(true));   // shaper ON, too short
    optimizer::reshapeBlankRuns(gRef,   n2, factoryCfg(false));  // shaper OFF

    TEST_ASSERT_EQUAL_INT_MESSAGE(
        0, memcmp(gFrame, gRef, n1 * sizeof(LaserPoint)),
        "a too-short run was shaped partially instead of falling back to the plain ramp");
}

// ── degenerate cases named in the decision ────────────────────────────────

// count 0: no blank points anywhere in the frame -- reshapeBlankRuns() must
// find nothing and leave the buffer untouched.
void test_degenerateNoBlankRuns(void) {
    size_t n = 0;
    gFrame[n++] = lit(-1000, -1000, 1, 1, 1);
    gFrame[n++] = lit( 1000,  1000, 2, 2, 2);
    gFrame[n++] = lit( 2000, -1000, 3, 3, 3);
    memcpy(gRef, gFrame, n * sizeof(LaserPoint));

    optimizer::reshapeBlankRuns(gFrame, n, fastRingCfg(true));

    TEST_ASSERT_EQUAL_INT_MESSAGE(
        0, memcmp(gFrame, gRef, n * sizeof(LaserPoint)),
        "an all-lit frame was modified");
}

// count 1: a single isolated blank point must pass through unchanged -- it
// cannot express a ramp (matches planBlankJump()'s own count<1->1 floor and
// blankSettlePts()'s count/2 floor; no new special case needed).
void test_degenerateSinglePointRun(void) {
    size_t n = 0;
    gFrame[n++] = lit(-1000, -1000, 1, 1, 1);
    size_t i0 = n;
    gFrame[n++] = blankPt(500, 500);
    size_t i1 = n;
    gFrame[n++] = lit(1000, 1000, 2, 2, 2);
    memcpy(gRef, gFrame, n * sizeof(LaserPoint));

    optimizer::reshapeBlankRun(gFrame, i0, i1, fastRingCfg(true));

    TEST_ASSERT_EQUAL_INT_MESSAGE(
        0, memcmp(gFrame, gRef, n * sizeof(LaserPoint)),
        "a single-point blank run was rewritten instead of passed through");
}

// A run at the very start of the array (no lit point before it) must still
// reshape correctly -- endpoints come from the run's OWN first/last recorded
// point, never a neighboring frame's position (ILDA keeps no such state).
void test_degenerateRunAtArrayStart(void) {
    size_t n = 0;
    fillBlankRun(gFrame + n, 40, -8000.0f, 9000.0f, 7000.0f, -6000.0f);
    size_t i0 = 0, i1 = 40;
    n += 40;
    LaserPoint trailingLit = lit(7000, -6000, 9, 9, 9);
    gFrame[n++] = trailingLit;

    LaserPoint originalTarget = gFrame[i1 - 1];
    optimizer::reshapeBlankRun(gFrame, i0, i1, fastRingCfg(true));

    float ex = fabsf((float)gFrame[i1 - 1].x - (float)originalTarget.x);
    float ey = fabsf((float)gFrame[i1 - 1].y - (float)originalTarget.y);
    snprintf(gMsg, sizeof(gMsg), "leading run off target by (%.1f,%.1f)", (double)ex, (double)ey);
    TEST_ASSERT_TRUE_MESSAGE(ex <= kDacTolerance && ey <= kDacTolerance, gMsg);

    // The trailing lit point must be untouched.
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        0, memcmp(&gFrame[40], &trailingLit, sizeof(LaserPoint)),
        "trailing lit point after a leading run was modified");
}

// A run at the very end of the array (no lit point after it) -- same rule,
// mirrored.
void test_degenerateRunAtArrayEnd(void) {
    size_t n = 0;
    gFrame[n++] = lit(-7000, 6000, 8, 8, 8);
    size_t i0 = n;
    fillBlankRun(gFrame + n, 40, -8000.0f, 9000.0f, 7000.0f, -6000.0f);
    n += 40;
    size_t i1 = n;

    LaserPoint originalLeading = gFrame[0];
    LaserPoint originalTarget  = gFrame[i1 - 1];

    optimizer::reshapeBlankRun(gFrame, i0, i1, fastRingCfg(true));

    TEST_ASSERT_EQUAL_INT_MESSAGE(
        0, memcmp(&gFrame[0], &originalLeading, sizeof(LaserPoint)),
        "leading lit point before a trailing run was modified");

    float ex = fabsf((float)gFrame[i1 - 1].x - (float)originalTarget.x);
    float ey = fabsf((float)gFrame[i1 - 1].y - (float)originalTarget.y);
    snprintf(gMsg, sizeof(gMsg), "trailing run off target by (%.1f,%.1f)", (double)ex, (double)ey);
    TEST_ASSERT_TRUE_MESSAGE(ex <= kDacTolerance && ey <= kDacTolerance, gMsg);
}

// A run longer than kMaxBlankPts must no-op -- leave out[i0..i1) exactly as
// it was, not truncate or overflow the trajectory scratch.
void test_degenerateOversizedRunNoOps(void) {
    size_t n = 0;
    gFrame[n++] = lit(-1000, -1000, 1, 1, 1);
    size_t i0 = n;
    int oversized = kMaxBlankPts + 1;   // 129
    fillBlankRun(gFrame + n, oversized, -5000.0f, -3000.0f, 18000.0f, 12000.0f);
    n += (size_t)oversized;
    size_t i1 = n;
    gFrame[n++] = lit(18000, 12000, 2, 2, 2);

    memcpy(gRef, gFrame, n * sizeof(LaserPoint));

    optimizer::reshapeBlankRun(gFrame, i0, i1, fastRingCfg(true));
    // Also exercise the scanning entry point over the same oversized run.
    optimizer::reshapeBlankRuns(gFrame, n, fastRingCfg(true));

    TEST_ASSERT_EQUAL_INT_MESSAGE(
        0, memcmp(gFrame, gRef, n * sizeof(LaserPoint)),
        "a run longer than kMaxBlankPts was modified instead of left as a no-op");
}

int main(int, char**) {
    UNITY_BEGIN();

    RUN_TEST(test_pointCountAndRunLengthsPreserved);
    RUN_TEST(test_litPointsByteIdentical);
    RUN_TEST(test_reshapedRunEndsAtTarget);
    RUN_TEST(test_reshapedRunDiffersFromRawLinearRamp);
    RUN_TEST(test_shapedRunDiffersFromUnshaped);
    RUN_TEST(test_shortRunFallsBackNotPartial);
    RUN_TEST(test_degenerateNoBlankRuns);
    RUN_TEST(test_degenerateSinglePointRun);
    RUN_TEST(test_degenerateRunAtArrayStart);
    RUN_TEST(test_degenerateRunAtArrayEnd);
    RUN_TEST(test_degenerateOversizedRunNoOps);

    return UNITY_END();
}
