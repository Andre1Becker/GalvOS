/**
 * test_weld_path.cpp -- host-side tests for weld_path.h's arc-length path
 * construction (GalvOS Text-Welding feature, see weld_patterns.h/.cpp).
 *
 * weld_path.h is the piece of the Welding effect that can run on the host:
 * it is pure geometry (no Arduino/PSRAM/millis), factored out of
 * weld_patterns.cpp specifically so both the Paint-canvas source and the new
 * Text-Mode source (weld::generateText(), which feeds it
 * textrender::glyphOutlinePaths()'s glyph sub-paths) share ONE path-building
 * implementation instead of two -- see weld_patterns.h's header comment.
 *
 * weld_patterns.cpp itself (wall-clock head motion, sparks, PSRAM scratch,
 * gPaint/gTextConfig globals) pulls in ESPAsyncWebServer/Arduino-only
 * dependencies this native test environment deliberately excludes (see
 * platformio.ini's [env:native] comment) -- so, same as test_upload_guard.cpp,
 * only the host-portable half is unit-tested here. What is NOT covered by
 * this suite:
 *   - textrender::generate() (Text Mode's own, non-Welding animated
 *     renderer) -- lives in text_renderer.cpp, tied to gOptimizerConfig/
 *     gProjection globals only main.cpp defines. "Normal text without
 *     Welding" (regression scenario 1) stays manually/on-device verified.
 *   - the wall-clock torch head / spark motion itself (weld_patterns.cpp's
 *     renderTorch()) -- tuning documented as "browser-preview approved" in
 *     that file, unchanged by this feature.
 *   - multi-line text -- GalvOS's font renderer has no line-break glyph and
 *     never did (see text_renderer.cpp/.h); "multiple lines" (part of
 *     regression scenario 5) is not applicable by design, single-line only.
 * What IS covered: the arc-length path builder + sampler + lift-boundary
 * detector that both Welding sources go through, exercised directly
 * (test_weldPath_*) and through synthetic glyph-shaped input run through the
 * REAL point_optimizer (test_weldText_*) -- covering "short text", "longer
 * text", "no invalid points", "no connecting lines between separate text
 * parts", and "combination with the Point Optimizer" from the regression
 * list.
 *
 * Runs in the same native binary as test_contract.cpp (one main(), see that
 * file) -- `pio test -e native`.
 */

#include <unity.h>

#include <math.h>
#include <string.h>

#include "config.h"
#include "patterns/point_optimizer.h"
#include "patterns/weld_path.h"

using optimizer::OptimizerConfig;
using optimizer::PathSegment;
using optimizer::PathVertex;

// ── weld::buildArcLengthPath() ────────────────────────────────────────────

void test_weldPath_singleOpenStroke(void) {
    static const float xs[3] = {0.f, 100.f, 100.f};
    static const float ys[3] = {0.f, 0.f, 100.f};
    weld::SourceStroke strokes[1] = {{xs, ys, 3, false}};

    weld::PathNode nodes[16];
    float liftS[4];
    size_t liftCount = 0;
    float pathLen = 0.f;
    size_t nc = weld::buildArcLengthPath(strokes, 1, nodes, 16, liftS, 4, liftCount, pathLen);

    TEST_ASSERT_EQUAL_UINT32(3u, (uint32_t)nc);
    TEST_ASSERT_EQUAL_UINT32(1u, (uint32_t)liftCount);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.f, liftS[0]);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 200.f, pathLen);   // 100 + 100 units of travel
    TEST_ASSERT_TRUE(nodes[0].lift);
    TEST_ASSERT_FALSE(nodes[1].lift);
    TEST_ASSERT_FALSE(nodes[2].lift);
}

void test_weldPath_shortStrokeSkipped(void) {
    // A stroke with count<2 (a degenerate single-point glyph fragment) has
    // no length to draw and must be skipped, not crash or emit a garbage
    // zero-length node.
    static const float x0[1] = {5.f};
    static const float y0[1] = {5.f};
    static const float xs[2] = {0.f, 50.f};
    static const float ys[2] = {0.f, 0.f};
    weld::SourceStroke strokes[2] = {
        {x0, y0, 1, false},   // degenerate -- skipped
        {xs, ys, 2, false},
    };

    weld::PathNode nodes[16];
    float liftS[4];
    size_t liftCount = 0;
    float pathLen = 0.f;
    size_t nc = weld::buildArcLengthPath(strokes, 2, nodes, 16, liftS, 4, liftCount, pathLen);

    TEST_ASSERT_EQUAL_UINT32(2u, (uint32_t)nc);
    TEST_ASSERT_EQUAL_UINT32(1u, (uint32_t)liftCount);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 50.f, pathLen);
}

void test_weldPath_closedStrokeWrapsToStart(void) {
    // A closed stroke (e.g. an 'O'/'D' glyph outline) repeats vertex 0 as a
    // trailing node -- the path must include the closing edge's length and
    // that trailing node must NOT itself count as a new lift.
    static const float xs[4] = {0.f, 100.f, 100.f, 0.f};
    static const float ys[4] = {0.f, 0.f, 100.f, 100.f};
    weld::SourceStroke strokes[1] = {{xs, ys, 4, true}};

    weld::PathNode nodes[16];
    float liftS[4];
    size_t liftCount = 0;
    float pathLen = 0.f;
    size_t nc = weld::buildArcLengthPath(strokes, 1, nodes, 16, liftS, 4, liftCount, pathLen);

    TEST_ASSERT_EQUAL_UINT32(5u, (uint32_t)nc);     // 4 vertices + 1 closing node
    TEST_ASSERT_EQUAL_UINT32(1u, (uint32_t)liftCount);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 400.f, pathLen); // perimeter of a 100x100 square
    TEST_ASSERT_FALSE(nodes[4].lift);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.f, nodes[4].x);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.f, nodes[4].y);
}

void test_weldPath_emptyInputReturnsZero(void) {
    weld::PathNode nodes[16];
    float liftS[4];
    size_t liftCount = 123;   // must be reset to 0, not left stale
    float pathLen = -1.f;
    size_t nc = weld::buildArcLengthPath(nullptr, 0, nodes, 16, liftS, 4, liftCount, pathLen);
    TEST_ASSERT_EQUAL_UINT32(0u, (uint32_t)nc);
    TEST_ASSERT_EQUAL_UINT32(0u, (uint32_t)liftCount);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.f, pathLen);
}

void test_weldPath_nodeCapacityClamped(void) {
    // A "longer text" stand-in: one long stroke whose node count exceeds the
    // caller's buffer -- must clamp instead of overflowing nodesOut[].
    static float xs[50], ys[50];
    for (int i = 0; i < 50; i++) { xs[i] = (float)i * 10.f; ys[i] = 0.f; }
    weld::SourceStroke strokes[1] = {{xs, ys, 50, false}};

    weld::PathNode nodes[10];   // deliberately smaller than the 50-vertex stroke
    float liftS[4];
    size_t liftCount = 0;
    float pathLen = 0.f;
    size_t nc = weld::buildArcLengthPath(strokes, 1, nodes, 10, liftS, 4, liftCount, pathLen);
    TEST_ASSERT_TRUE(nc <= 10u);
    TEST_ASSERT_EQUAL_UINT32(1u, (uint32_t)liftCount);
}

void test_weldPath_liftCapacityClamped(void) {
    // More separate strokes ("glyphs") than the lift-table capacity -- the
    // path itself must still build correctly, only the boundary DETECTION
    // for lifts beyond capacity is lost (documented in the header).
    static const float xs[2] = {0.f, 10.f};
    static const float ys[2] = {0.f, 0.f};
    weld::SourceStroke strokes[5] = {
        {xs, ys, 2, false}, {xs, ys, 2, false}, {xs, ys, 2, false},
        {xs, ys, 2, false}, {xs, ys, 2, false},
    };

    weld::PathNode nodes[32];
    float liftS[3];   // smaller than the 5 strokes
    size_t liftCount = 0;
    float pathLen = 0.f;
    size_t nc = weld::buildArcLengthPath(strokes, 5, nodes, 32, liftS, 3, liftCount, pathLen);
    TEST_ASSERT_EQUAL_UINT32(10u, (uint32_t)nc);     // all 5 strokes still built
    TEST_ASSERT_EQUAL_UINT32(3u, (uint32_t)liftCount); // capped at capacity
}

// ── weld::sampleAt() ───────────────────────────────────────────────────────

void test_weldPath_sampleOutOfRangeInvalid(void) {
    static const float xs[2] = {0.f, 100.f};
    static const float ys[2] = {0.f, 0.f};
    weld::SourceStroke strokes[1] = {{xs, ys, 2, false}};
    weld::PathNode nodes[8];
    float liftS[2]; size_t liftCount = 0; float pathLen = 0.f;
    weld::buildArcLengthPath(strokes, 1, nodes, 8, liftS, 2, liftCount, pathLen);

    weld::Sample below = weld::sampleAt(nodes, 2, pathLen, -1.f);
    weld::Sample above = weld::sampleAt(nodes, 2, pathLen, pathLen + 1.f);
    TEST_ASSERT_FALSE(below.valid);
    TEST_ASSERT_FALSE(above.valid);
}

void test_weldPath_sampleInterpolatesOnSegment(void) {
    static const float xs[2] = {0.f, 100.f};
    static const float ys[2] = {0.f, 0.f};
    weld::SourceStroke strokes[1] = {{xs, ys, 2, false}};
    weld::PathNode nodes[8];
    float liftS[2]; size_t liftCount = 0; float pathLen = 0.f;
    weld::buildArcLengthPath(strokes, 1, nodes, 8, liftS, 2, liftCount, pathLen);

    weld::Sample mid = weld::sampleAt(nodes, 2, pathLen, 50.f);
    TEST_ASSERT_TRUE(mid.valid);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 50.f, mid.x);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.f, mid.y);
}

// ── weld::crossesLift() ────────────────────────────────────────────────────

void test_weldPath_crossesLiftDetectsGapBetweenStrokes(void) {
    // Two separate strokes ("letters") with a physical gap between them --
    // this is exactly what stops the torch trail from drawing a straight
    // connecting line across the gap (regression: "no visible connecting
    // lines between separate text parts").
    static const float xsA[2] = {0.f, 50.f};
    static const float ysA[2] = {0.f, 0.f};
    static const float xsB[2] = {200.f, 250.f};   // gap: x in [50,200] is empty
    static const float ysB[2] = {0.f, 0.f};
    weld::SourceStroke strokes[2] = {{xsA, ysA, 2, false}, {xsB, ysB, 2, false}};

    weld::PathNode nodes[16];
    float liftS[4]; size_t liftCount = 0; float pathLen = 0.f;
    weld::buildArcLengthPath(strokes, 2, nodes, 16, liftS, 4, liftCount, pathLen);

    // Arc length is continuous across the gap (buildArcLengthPath does not
    // add a jump cost for it -- see its header comment): stroke A ends at
    // s=50, stroke B's lift sits at s=50 too (its first node reuses that
    // running arc length), and B ends at s=100.
    TEST_ASSERT_EQUAL_UINT32(2u, (uint32_t)liftCount);
    TEST_ASSERT_TRUE(weld::crossesLift(liftS, liftCount, 10.f, 90.f));   // window spans the boundary
    TEST_ASSERT_FALSE(weld::crossesLift(liftS, liftCount, 10.f, 40.f)); // stays inside stroke A
}

void test_weldPath_crossesLiftFalseWithinOneStroke(void) {
    static const float lift[1] = {50.f};
    TEST_ASSERT_FALSE(weld::crossesLift(lift, 1, 10.f, 20.f));
    TEST_ASSERT_FALSE(weld::crossesLift(lift, 1, 60.f, 90.f));
    // Order-independence: sa > sb must still detect the same boundary.
    TEST_ASSERT_TRUE(weld::crossesLift(lift, 1, 90.f, 10.f));
}

// ── Text-Welding regression scenarios (glyph-shaped multi-stroke input) ────

// Builds a small set of glyph-like open strokes (mimics
// textrender::glyphOutlinePaths()'s per-character sub-paths: a handful of
// short pen-lift segments, physically separated left-to-right) and returns
// the flattened path.
static size_t buildGlyphLikePath(size_t glyphCount, weld::PathNode* nodes, size_t maxNodes,
                                  float* liftS, size_t maxLifts, size_t& liftCount, float& pathLen) {
    static float xs[8][2];
    static float ys[8][2];
    weld::SourceStroke strokes[8];
    size_t n = glyphCount < 8 ? glyphCount : 8;
    for (size_t i = 0; i < n; i++) {
        float gx = (float)i * 40.f;   // glyph cell advance, matches text_renderer.cpp's spacing
        xs[i][0] = gx; xs[i][1] = gx + 20.f;
        ys[i][0] = -35.f; ys[i][1] = 35.f;   // font glyphs span y in [-7,7]*scale
        strokes[i] = {xs[i], ys[i], 2, false};
    }
    return weld::buildArcLengthPath(strokes, n, nodes, maxNodes, liftS, maxLifts, liftCount, pathLen);
}

void test_weldText_shortStringSingleGlyph(void) {
    weld::PathNode nodes[16];
    float liftS[4]; size_t liftCount = 0; float pathLen = 0.f;
    size_t nc = buildGlyphLikePath(1, nodes, 16, liftS, 4, liftCount, pathLen);
    TEST_ASSERT_EQUAL_UINT32(2u, (uint32_t)nc);
    TEST_ASSERT_EQUAL_UINT32(1u, (uint32_t)liftCount);
    TEST_ASSERT_TRUE(pathLen > 0.f);
}

void test_weldText_longerStringManyGlyphsNoConnectingLine(void) {
    weld::PathNode nodes[64];
    float liftS[8]; size_t liftCount = 0; float pathLen = 0.f;
    size_t nc = buildGlyphLikePath(8, nodes, 64, liftS, 8, liftCount, pathLen);
    TEST_ASSERT_EQUAL_UINT32(16u, (uint32_t)nc);   // 8 glyphs x 2 verts
    TEST_ASSERT_EQUAL_UINT32(8u, (uint32_t)liftCount);
    // A trail window spanning two glyph cells must see a lift crossing --
    // the shared renderer (weld_patterns.cpp's renderTorch) uses exactly
    // this check to split the afterglow polyline instead of drawing one
    // continuous line across every glyph in the string.
    TEST_ASSERT_TRUE(weld::crossesLift(liftS, liftCount, 5.f, 100.f));
}

void test_weldText_optimizerProducesNoInvalidPointsAndBlanksBetweenGlyphs(void) {
    // End-to-end: flatten glyph-like strokes, sample a handful of points
    // per stroke (mirroring renderTorch()'s trail sampling), feed the
    // resulting per-glyph segments through the REAL point_optimizer, and
    // check the output the same way test_dacRangeValid does for the rest of
    // the Contract suite -- combination with the Point Optimizer, no
    // invalid points, correct blanking between separate glyphs.
    weld::PathNode nodes[64];
    float liftS[8]; size_t liftCount = 0; float pathLen = 0.f;
    size_t nc = buildGlyphLikePath(4, nodes, 64, liftS, 8, liftCount, pathLen);
    TEST_ASSERT_TRUE(nc > 0u);

    PathVertex verts[4][2];
    PathSegment segs[4];
    for (size_t g = 0; g < 4; g++) {
        // Each glyph is its own segment -- the Welding renderer never
        // stitches separate strokes into one segment (that is precisely
        // what would draw a connecting line between them).
        weld::Sample a = weld::sampleAt(nodes, nc, pathLen, nodes[g * 2].s);
        weld::Sample b = weld::sampleAt(nodes, nc, pathLen, nodes[g * 2 + 1].s);
        TEST_ASSERT_TRUE(a.valid);
        TEST_ASSERT_TRUE(b.valid);
        verts[g][0] = PathVertex(a.x, a.y, 255, 255, 255, true);
        verts[g][1] = PathVertex(b.x, b.y, 255, 255, 255, false);
        segs[g] = PathSegment(verts[g], 2, false);
    }

    OptimizerConfig cfg = OptimizerConfig();
    cfg.max_pts_per_frame = PATTERN_POINTS_MAX;
    LaserPoint out[PATTERN_POINTS_MAX];
    size_t n = optimizer::optimize(segs, 4, out, PATTERN_POINTS_MAX, cfg);
    TEST_ASSERT_TRUE(n > 0u);

    bool sawBlank = false;
    for (size_t k = 0; k < n; k++) {
        int32_t codeX = (int32_t)out[k].x + 0x8000;
        int32_t codeY = (int32_t)out[k].y + 0x8000;
        TEST_ASSERT_TRUE_MESSAGE(codeX >= 0 && codeX <= 0xFFFF && codeY >= 0 && codeY <= 0xFFFF,
                                 "Text-Weld point out of valid DAC range");
        if (out[k].blank) sawBlank = true;
    }
    // Four physically separated glyphs, none of them touching -- the
    // optimizer must have inserted at least one blank jump between them
    // rather than drawing one unbroken lit path across the whole string.
    TEST_ASSERT_TRUE_MESSAGE(sawBlank, "expected a blanked jump between separate glyphs");
}
