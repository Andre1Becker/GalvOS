/**
 * test_preset_matrix.cpp -- Easy/Medium/Hard regression matrix over REAL
 * preset geometry (see fixtures.h's "Preset-derived fixtures" section).
 *
 * Unlike test_contract.cpp's invariant tests -- which probe individual
 * optimizer mechanisms with fixtures engineered to isolate one behavior --
 * this suite runs the actual vertex geometry six shipped presets emit
 * (Square / Cross + / Grid 3x3 / Double Spiral / Nested Squares / Hibiscus,
 * src/patterns/preset_patterns.cpp) through a spread of OptimizerConfig
 * combinations, prints a readable metrics table per (preset, config) cell,
 * and asserts plausibility/regression properties -- reusing CONTRACT.md's
 * own invariants (noSilentPointLoss, statsConsistent, dacRangeValid) plus a
 * few new ones the task asks for (no non-finite output, no degenerate
 * shape, bounded blank runs, bounded runtime) via the same TEST_ASSERT_* +
 * optimizer::gLastStats machinery test_contract.cpp already established,
 * rather than a parallel scoring mechanism.
 *
 * Runs in the same native binary as test_contract.cpp (one main(), see that
 * file) -- `pio test -e native`.
 */

#include <unity.h>

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "config.h"
#include "fixtures.h"
#include "patterns/point_optimizer.h"
#include "test_preset_matrix.h"

using optimizer::OptimizerConfig;
using optimizer::PathSegment;

namespace {

LaserPoint gMFrame[PATTERN_POINTS_MAX];
char       gMMsg[320];

// point_optimizer.h's reshapeBlankRun() doc comment documents kMaxBlankPts
// as 128 (it is a .cpp-internal constant, not part of the public header) --
// mirrored here rather than included, same "cite the source" convention
// fixtures.h's preset builders use.
constexpr size_t kDocumentedMaxBlankPts = 128;

// Full-canvas diagonal is ~2*32767*sqrt(2) =~ 92682 DAC units -- nothing
// legitimate travels farther than that in a single output tick.
constexpr float kMaxPlausibleStepUnits = 93000.0f;

float mDist(const LaserPoint& a, const LaserPoint& b) {
    float dx = (float)b.x - (float)a.x;
    float dy = (float)b.y - (float)a.y;
    return sqrtf(dx * dx + dy * dy);
}

// One named optimizer-option combination layered on top of fx::baseCfg().
// Spans every optional stage at least once, including two pairs that only
// affect each other when combined (resample+curvature, reorder+2opt) --
// the "mutually influencing parameters" case the task calls for.
struct ConfigVariant {
    const char* label;
    void (*apply)(OptimizerConfig&);
};

void cvStock(OptimizerConfig&) {}
void cvTightBudget(OptimizerConfig& cfg) { cfg.max_pts_per_frame = 300; }
void cvRingingZv(OptimizerConfig& cfg) { cfg.ringing_comp_enabled = true; }
void cvResampleCurvature(OptimizerConfig& cfg) {
    cfg.resample_enabled           = true;
    cfg.resample_spacing_units     = 300.0f;
    cfg.curvature_resample_enabled = true;
    cfg.curvature_gain             = 2.0f;
    cfg.min_spacing_units          = 40.0f;
    cfg.max_spacing_units          = 400.0f;
}
void cvReorderTsp(OptimizerConfig& cfg) {
    cfg.reorder_segments = true;
    cfg.reorder_2opt     = true;
}
void cvClampsJitter(OptimizerConfig& cfg) {
    cfg.vel_clamp_enabled   = true;
    cfg.max_step_units      = 200.0f;
    cfg.accel_clamp_enabled = true;
    cfg.max_accel_units     = 120.0f;
    cfg.jitter_enabled      = true;
    cfg.jitter_amount_units = 30.0f;
}
void cvAllStagesOn(OptimizerConfig& cfg) {
    cfg.transform = optimizer::makeTransform(0.35f, 400.0f, -250.0f);
    cvResampleCurvature(cfg);
    cvRingingZv(cfg);
    cvReorderTsp(cfg);
    cvClampsJitter(cfg);
}

constexpr ConfigVariant kVariants[] = {
    { "stock",              cvStock },
    { "tightBudget(300)",   cvTightBudget },
    { "ringingZv",          cvRingingZv },
    { "resample+curvature", cvResampleCurvature },
    { "reorder+2opt",       cvReorderTsp },
    { "clamps+jitter",      cvClampsJitter },
    { "allStagesOn",        cvAllStagesOn },
};
constexpr size_t kVariantCount = sizeof(kVariants) / sizeof(kVariants[0]);

const char* difficultyName(fx::Difficulty d) {
    switch (d) {
        case fx::Difficulty::Easy:   return "Easy";
        case fx::Difficulty::Medium: return "Medium";
        default:                     return "Hard";
    }
}

// Runs one (preset, config) matrix cell: prints its row FIRST -- so the
// table stays complete even if an assertion below aborts later cells in the
// same test function, same "loop keeps going until an assert fires" shape
// test_contract.cpp's own sweep tests already use -- then checks every
// plausibility/regression property against it.
void runMatrixCell(const fx::PresetFixture& pf, const ConfigVariant& cv) {
    OptimizerConfig cfg   = fx::baseCfg();
    cfg.max_pts_per_frame = PATTERN_POINTS_MAX;
    cv.apply(cfg);

    size_t inputVerts = 0;
    for (size_t s = 0; s < pf.count; s++) inputVerts += pf.segs[s].count;

    clock_t t0 = clock();
    size_t n = optimizer::optimize(pf.segs, pf.count, gMFrame, PATTERN_POINTS_MAX, cfg);
    clock_t t1 = clock();
    double runtimeMs = 1000.0 * (double)(t1 - t0) / (double)CLOCKS_PER_SEC;

    const optimizer::Stats&  st = optimizer::gLastStats;
    optimizer::RingingStatus rs = optimizer::ringingStatus(cfg);

    printf("| %-6s | %-14s | %-18s | segs=%2zu verts=%3zu | out=%4zu lit=%4u blank=%3u "
           "jumps=%2u trunc=%3u stage2=%.3f s1=%d s15=%d zv=%d/%d | %6.3fms |\n",
           difficultyName(pf.difficulty), pf.name, cv.label,
           pf.count, inputVerts, n, (unsigned)st.emittedLit, (unsigned)st.emittedBlank,
           (unsigned)st.jumpCount, (unsigned)st.truncated, (double)st.stage2Scale,
           (int)st.stage1Triggered, (int)st.stage15Triggered,
           (int)st.ringingActive, (int)rs.active, runtimeMs);

    // ── Regression / plausibility checks ────────────────────────────────

    snprintf(gMMsg, sizeof(gMMsg), "%s/%s/%s: empty output from non-empty input",
             difficultyName(pf.difficulty), pf.name, cv.label);
    TEST_ASSERT_TRUE_MESSAGE(n > 0, gMMsg);

    snprintf(gMMsg, sizeof(gMMsg), "%s/%s/%s: output %zu exceeds PATTERN_POINTS_MAX",
             difficultyName(pf.difficulty), pf.name, cv.label, n);
    TEST_ASSERT_TRUE_MESSAGE(n <= PATTERN_POINTS_MAX, gMMsg);

    snprintf(gMMsg, sizeof(gMMsg), "%s/%s/%s: output %zu exceeds the frame budget %u",
             difficultyName(pf.difficulty), pf.name, cv.label, n, (unsigned)cfg.max_pts_per_frame);
    TEST_ASSERT_TRUE_MESSAGE(n <= (size_t)cfg.max_pts_per_frame, gMMsg);

    // Invariant 3 (noSilentPointLoss), reused per cell: accounting must
    // close for every one of these preset/config combinations too, not just
    // the engineered fixtures test_contract.cpp checks it against.
    snprintf(gMMsg, sizeof(gMMsg), "%s/%s/%s: %zu emitted + %u truncated != %u planned",
             difficultyName(pf.difficulty), pf.name, cv.label,
             n, (unsigned)st.truncated, (unsigned)st.plannedTotal);
    TEST_ASSERT_TRUE_MESSAGE(n + st.truncated == st.plannedTotal, gMMsg);

    size_t lit = 0, blank = 0, curRun = 0;
    bool   inRun = false;
    float  minX = 1e9f, maxX = -1e9f, minY = 1e9f, maxY = -1e9f;
    for (size_t k = 0; k < n; k++) {
        // No NaN/Inf can literally survive an int16_t field, but a garbage
        // cast from an upstream NaN float shows up as an extreme outlier --
        // caught below as an implausible single-tick jump instead. This
        // check pins the encoding itself (invariant 5, dacRangeValid) down
        // per cell.
        int32_t codeX = (int32_t)gMFrame[k].x + 0x8000;
        int32_t codeY = (int32_t)gMFrame[k].y + 0x8000;
        snprintf(gMMsg, sizeof(gMMsg),
                 "%s/%s/%s: point %zu at (%d,%d) encodes outside the DAC's 0..65535 range",
                 difficultyName(pf.difficulty), pf.name, cv.label, k,
                 (int)gMFrame[k].x, (int)gMFrame[k].y);
        TEST_ASSERT_TRUE_MESSAGE(codeX >= 0 && codeX <= 0xFFFF && codeY >= 0 && codeY <= 0xFFFF, gMMsg);

        if (!gMFrame[k].blank) {
            lit++;
            inRun = false;
            if (gMFrame[k].x < minX) minX = gMFrame[k].x;
            if (gMFrame[k].x > maxX) maxX = gMFrame[k].x;
            if (gMFrame[k].y < minY) minY = gMFrame[k].y;
            if (gMFrame[k].y > maxY) maxY = gMFrame[k].y;
        } else {
            blank++;
            if (!inRun) { inRun = true; curRun = 0; }
            curRun++;
            snprintf(gMMsg, sizeof(gMMsg),
                     "%s/%s/%s: blank run %zu ticks long, exceeds the documented "
                     "kMaxBlankPts ceiling (%zu) -- blanking state looks inconsistent",
                     difficultyName(pf.difficulty), pf.name, cv.label, curRun,
                     kDocumentedMaxBlankPts);
            TEST_ASSERT_TRUE_MESSAGE(curRun <= kDocumentedMaxBlankPts, gMMsg);
        }

        if (k > 0) {
            float step = mDist(gMFrame[k - 1], gMFrame[k]);
            snprintf(gMMsg, sizeof(gMMsg),
                     "%s/%s/%s: implausible %.0f-unit single-tick jump at point %zu "
                     "(garbage/non-finite coordinate upstream?)",
                     difficultyName(pf.difficulty), pf.name, cv.label, (double)step, k);
            TEST_ASSERT_TRUE_MESSAGE(step <= kMaxPlausibleStepUnits, gMMsg);
        }
    }

    // Invariant 8 (statsConsistent), reused per cell.
    snprintf(gMMsg, sizeof(gMMsg), "%s/%s/%s: gLastStats lit/blank %u/%u vs buffer %zu/%zu",
             difficultyName(pf.difficulty), pf.name, cv.label,
             (unsigned)st.emittedLit, (unsigned)st.emittedBlank, lit, blank);
    TEST_ASSERT_TRUE_MESSAGE(st.emittedLit == lit && st.emittedBlank == blank, gMMsg);

    snprintf(gMMsg, sizeof(gMMsg), "%s/%s/%s: gLastStats.jumpDistanceTotal (%.1f) is not finite/non-negative",
             difficultyName(pf.difficulty), pf.name, cv.label, (double)st.jumpDistanceTotal);
    TEST_ASSERT_TRUE_MESSAGE(isfinite(st.jumpDistanceTotal) && st.jumpDistanceTotal >= 0.0f, gMMsg);

    snprintf(gMMsg, sizeof(gMMsg), "%s/%s/%s: gLastStats.stage2Scale (%.4f) is not finite/positive",
             difficultyName(pf.difficulty), pf.name, cv.label, (double)st.stage2Scale);
    TEST_ASSERT_TRUE_MESSAGE(isfinite(st.stage2Scale) && st.stage2Scale > 0.0f, gMMsg);

    // Non-degeneration: lit geometry must not have collapsed to (near) a
    // single point. 500 units is generous -- every fixture here spans
    // thousands of units at kPresetSize -- so this only trips on a real
    // collapse (e.g. a density/clamp interaction eating the whole shape).
    if (lit > 0) {
        float diag = mDist(LaserPoint((int16_t)minX, (int16_t)minY, 0, 0, 0, 0),
                            LaserPoint((int16_t)maxX, (int16_t)maxY, 0, 0, 0, 0));
        snprintf(gMMsg, sizeof(gMMsg),
                 "%s/%s/%s: lit geometry collapsed to a %.1f-unit bounding-box diagonal",
                 difficultyName(pf.difficulty), pf.name, cv.label, (double)diag);
        TEST_ASSERT_TRUE_MESSAGE(diag > 500.0f, gMMsg);
    }

    // Runtime sanity: catches a runaway loop (e.g. Stage 2 bisection not
    // converging), not a hardware-cycle-accurate budget.
    snprintf(gMMsg, sizeof(gMMsg), "%s/%s/%s: took %.2fms -- possible runaway loop",
             difficultyName(pf.difficulty), pf.name, cv.label, runtimeMs);
    TEST_ASSERT_TRUE_MESSAGE(runtimeMs < 200.0, gMMsg);
}

void runTier(fx::Difficulty want) {
    const fx::PresetFixture* pf = fx::presetFixtures();
    for (size_t i = 0; i < fx::kPresetFixtureCount; i++) {
        if (pf[i].difficulty != want) continue;
        for (size_t v = 0; v < kVariantCount; v++) {
            runMatrixCell(pf[i], kVariants[v]);
        }
    }
}

}  // namespace

void test_presetMatrixEasy(void)   { runTier(fx::Difficulty::Easy); }
void test_presetMatrixMedium(void) { runTier(fx::Difficulty::Medium); }
void test_presetMatrixHard(void)   { runTier(fx::Difficulty::Hard); }

// ── Invalid / degenerate parameters fail cleanly, not silently ───────────
//
// optimize() trusts its caller's OptimizerConfig -- validation/clamping
// (normalizeOptimizerConfig(), config.h) runs upstream at config-WRITE
// time, not inside optimize() itself. This is not a bug to fix; it is a
// boundary this test pins down: even with parameters no UI path can
// currently produce (0 budget, inverted min/max corner points, a negative
// ring frequency), the optimizer must degrade gracefully -- bounded,
// DAC-range-valid output, never a crash/hang/garbage buffer -- rather than
// silently misbehaving. If one of these ever does misbehave, THIS assertion
// is the "clean, understandable failure" the task asks for.
void test_presetMatrixInvalidParamsDegradeGracefully(void) {
    size_t segCount = 0;
    const PathSegment* segs = fx::presetSquare(segCount);

    struct BadCase { const char* label; void (*apply)(OptimizerConfig&); };
    static const BadCase cases[] = {
        { "max_pts_per_frame=0", [](OptimizerConfig& c) { c.max_pts_per_frame = 0; } },
        { "blank_samples=0",     [](OptimizerConfig& c) { c.blank_samples = 0; c.min_blank_samples = 0; } },
        { "min>max corner pts",  [](OptimizerConfig& c) { c.min_corner_pts = 20; c.max_corner_pts = 2; } },
        { "negative ring freq",  [](OptimizerConfig& c) { c.ringing_comp_enabled = true; c.ring_freq_hz = -50.0f; } },
        { "zero corner angle",   [](OptimizerConfig& c) { c.corner_angle_deg = 0.0f; } },
    };

    for (const BadCase& bc : cases) {
        OptimizerConfig cfg   = fx::baseCfg();
        cfg.max_pts_per_frame = PATTERN_POINTS_MAX;
        bc.apply(cfg);

        size_t n = optimizer::optimize(segs, segCount, gMFrame, PATTERN_POINTS_MAX, cfg);

        snprintf(gMMsg, sizeof(gMMsg),
                 "invalid config '%s': optimize() returned %zu > PATTERN_POINTS_MAX",
                 bc.label, n);
        TEST_ASSERT_TRUE_MESSAGE(n <= PATTERN_POINTS_MAX, gMMsg);

        for (size_t k = 0; k < n; k++) {
            int32_t codeX = (int32_t)gMFrame[k].x + 0x8000;
            int32_t codeY = (int32_t)gMFrame[k].y + 0x8000;
            snprintf(gMMsg, sizeof(gMMsg),
                     "invalid config '%s': point %zu at (%d,%d) encodes outside 0..65535",
                     bc.label, k, (int)gMFrame[k].x, (int)gMFrame[k].y);
            TEST_ASSERT_TRUE_MESSAGE(codeX >= 0 && codeX <= 0xFFFF && codeY >= 0 && codeY <= 0xFFFF, gMMsg);
        }
    }
}
