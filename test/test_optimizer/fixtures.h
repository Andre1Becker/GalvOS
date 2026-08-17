#pragma once
/**
 * fixtures.h -- the fixed input set the Optimizer Contract is measured
 * against (see docs/optimizer-refactor/CONTRACT.md, "Fixed inputs").
 *
 * Five shapes, chosen to cover the structurally distinct cases the optimizer
 * treats differently:
 *   openLine        one open polyline    -- both endpoints are free ends
 *   closedSquare    one closed loop      -- wrap-around edge + closing dwell
 *   wireframeChain  closed loop + struts -- vertices shared across segments
 *   presetCall(i)   three optimize()     -- one frame built from several calls
 *                   calls in sequence       (the multi-call preset stand-in)
 *   circle480       480 closed vertices  -- corner-dominated, budget-bound
 *
 * Everything here is fixed geometry with fixed colors: invariant 7
 * (deterministicOutput) requires byte-identical input on every run. The
 * optimizer takes no frame index of its own -- modulator/noise-driven values
 * (OPT_DENSITY and friends) reach it through OptimizerConfig -- so pinning
 * baseCfg() is what pins "the frame" here.
 */

#include "config.h"
#include "patterns/point_optimizer.h"

namespace fx {

using optimizer::OptimizerConfig;
using optimizer::PathSegment;
using optimizer::PathVertex;

constexpr float kTau = 6.28318530717958647692f;

// The one config every test starts from: stock OPT_DEFAULT_* values, i.e.
// what a device runs with out of the box. Individual tests override only the
// fields they are actually exercising, so a failure points at one knob.
inline OptimizerConfig baseCfg() {
    return OptimizerConfig();
}

// --- one open line -------------------------------------------------------

inline const PathSegment* openLine(size_t& count) {
    static const PathVertex v[2] = {
        PathVertex(-14000.0f, -9000.0f, 255, 255, 255),
        PathVertex( 13000.0f,  8000.0f, 255, 255, 255),
    };
    static const PathSegment s[1] = { PathSegment(v, 2, false) };
    count = 1;
    return s;
}

// --- one closed square ---------------------------------------------------

inline const PathSegment* closedSquare(size_t& count) {
    static const PathVertex v[4] = {
        PathVertex(-10000.0f, -10000.0f, 255, 0, 0),
        PathVertex( 10000.0f, -10000.0f, 255, 0, 0),
        PathVertex( 10000.0f,  10000.0f, 255, 0, 0),
        PathVertex(-10000.0f,  10000.0f, 255, 0, 0),
    };
    static const PathSegment s[1] = { PathSegment(v, 4, true) };
    count = 1;
    return s;
}

// --- one closed hexagon (second call of the multi-call preset) -----------

inline const PathSegment* hexagon(size_t& count) {
    static PathVertex  v[6];
    static PathSegment s[1];
    static bool built = false;
    if (!built) {
        for (int i = 0; i < 6; i++) {
            float a = (float)i * (kTau / 6.0f);
            v[i] = PathVertex(cosf(a) * 11000.0f, sinf(a) * 11000.0f, 0, 0, 255);
        }
        s[0]  = PathSegment(v, 6, true);
        built = true;
    }
    count = 1;
    return s;
}

// --- closed face plus open struts sharing its vertices -------------------
//
// The shared-vertex case cornerSeverity() documents: strutA/strutB each start
// on a vertex the closed face also owns, so from the strut's own PathSegment
// that vertex looks like a free end while geometrically it is a real corner.

inline const PathSegment* wireframeChain(size_t& count) {
    static const PathVertex face[4] = {
        PathVertex(-8000.0f, -8000.0f, 0, 255, 0),
        PathVertex( 8000.0f, -8000.0f, 0, 255, 0),
        PathVertex( 8000.0f,  8000.0f, 0, 255, 0),
        PathVertex(-8000.0f,  8000.0f, 0, 255, 0),
    };
    static const PathVertex strutA[2] = {
        PathVertex(  8000.0f,  -8000.0f, 0, 255, 0),
        PathVertex( 15000.0f, -15000.0f, 0, 255, 0),
    };
    static const PathVertex strutB[2] = {
        PathVertex( -8000.0f,   8000.0f, 0, 255, 0),
        PathVertex(-15000.0f,  15000.0f, 0, 255, 0),
    };
    static const PathSegment s[3] = {
        PathSegment(face,   4, true),
        PathSegment(strutA, 2, false),
        PathSegment(strutB, 2, false),
    };
    count = 3;
    return s;
}

// --- 480-vertex closed circle -------------------------------------------

inline const PathSegment* circle480(size_t& count) {
    static PathVertex  v[480];
    static PathSegment s[1];
    static bool built = false;
    if (!built) {
        for (int i = 0; i < 480; i++) {
            float a = (float)i * (kTau / 480.0f);
            v[i] = PathVertex(cosf(a) * 15000.0f, sinf(a) * 15000.0f, 255, 255, 0);
        }
        s[0]  = PathSegment(v, 480, true);
        built = true;
    }
    count = 1;
    return s;
}

// --- multi-call preset stand-in ------------------------------------------
//
// One displayed frame assembled from three separate optimize() calls writing
// into the same output buffer -- the shape every real preset that draws more
// than one primitive has (preset_patterns.cpp). Invariants 1 and 3 are
// measured per FRAME, so they must be measured across all three.

constexpr size_t kPresetCallCount = 3;

inline const PathSegment* presetCall(size_t idx, size_t& count) {
    switch (idx) {
        case 0:  return closedSquare(count);
        case 1:  return hexagon(count);
        default: return wireframeChain(count);
    }
}

// --- iteration helper ----------------------------------------------------

struct Fixture {
    const char*        name;
    const PathSegment* segs;
    size_t             count;
};

constexpr size_t kFixtureCount = 4;

// Single-call fixtures, for the invariants that hold per optimize() call.
inline const Fixture* all() {
    static Fixture f[kFixtureCount];
    static bool built = false;
    if (!built) {
        size_t c = 0;
        f[0].name = "openLine";       f[0].segs = openLine(c);       f[0].count = c;
        f[1].name = "closedSquare";   f[1].segs = closedSquare(c);   f[1].count = c;
        f[2].name = "wireframeChain"; f[2].segs = wireframeChain(c); f[2].count = c;
        f[3].name = "circle480";      f[3].segs = circle480(c);      f[3].count = c;
        built = true;
    }
    return f;
}

// ── Preset-derived fixtures (test_preset_matrix.cpp) ─────────────────────
//
// Real geometry lifted from src/patterns/preset_patterns.cpp, reproduced
// here so the Easy/Medium/Hard regression matrix exercises the SAME vertex
// construction a live preset emits, not a stand-in shape. Each builder cites
// its source function; keep the two in sync if that function's geometry ever
// changes.
//
// ph=0, sp=0 everywhere -> presetAang()==0 (no rotation) -- same determinism
// reason baseCfg()/the fixtures above are pinned to fixed input. sz is fixed
// at kPresetSize, a representative "large" size-slider value: the STRUCTURE
// these fixtures exist to cover (segment/corner counts, direction changes)
// does not depend on which size is chosen, only the absolute scale does.
// Colors are placeholders where the real preset reads them from runtime
// state (ngon()'s gLivePreset.col_r/g/b) -- color never enters the
// optimizer's geometry decisions, only position does.

constexpr float   kPresetSC   = 18000.0f;   // mirrors preset_patterns.cpp's SC
constexpr uint8_t kPresetSize = 180;        // representative size-slider value (0..255)

inline float presetSsc(uint8_t sz) { return 0.25f + (sz / 255.0f) * 1.1765f; }

enum class Difficulty { Easy, Medium, Hard };

struct PresetFixture {
    const char*        name;         // exactly as PRESETS[] in preset_patterns.cpp
    const char*        profile;      // OPT_PROFILE family the preset actually runs under
    Difficulty          difficulty;
    const PathSegment*  segs;
    size_t              count;
};

// --- Square (p01 -> ngonSegmented() -> ngon(), category "Geometry") ------
// Closed 4-vertex ngon, radius SC*ssc(sz)*.9f.
inline const PathSegment* presetSquare(size_t& count) {
    static PathVertex  v[4];
    static PathSegment s[1];
    static bool built = false;
    if (!built) {
        float sc = kPresetSC * presetSsc(kPresetSize) * 0.9f;
        for (int i = 0; i < 4; i++) {
            float a = kTau * i / 4.0f;
            v[i] = PathVertex(cosf(a) * sc, sinf(a) * sc, 255, 255, 255);
        }
        s[0] = PathSegment(v, 4, true);
        built = true;
    }
    count = 1;
    return s;
}

// --- Cross + (p10, category "Lines") --------------------------------------
// Two independent open 2-vertex lines (horizontal, vertical), each its own
// PathSegment -- mirrors p10's two separate line() calls.
inline const PathSegment* presetCrossPlus(size_t& count) {
    static PathVertex  v[4];
    static PathSegment s[2];
    static bool built = false;
    if (!built) {
        float sc = kPresetSC * presetSsc(kPresetSize) * 0.9f;
        v[0] = PathVertex(-sc,  0.0f, 255,   0,   0, true);   // lift: blank-jump to start
        v[1] = PathVertex( sc,  0.0f, 255,   0,   0, false);
        v[2] = PathVertex( 0.0f, -sc,   0, 255,   0, true);
        v[3] = PathVertex( 0.0f,  sc,   0, 255,   0, false);
        s[0] = PathSegment(v,     2, false);
        s[1] = PathSegment(v + 2, 2, false);
        built = true;
    }
    count = 2;
    return s;
}

// --- Grid 3x3 (p12, category "Lines") -------------------------------------
// 4 vertical + 4 horizontal open 2-vertex lines -- 8 segments total.
inline const PathSegment* presetGrid3x3(size_t& count) {
    static PathVertex  v[16];
    static PathSegment s[8];
    static bool built = false;
    if (!built) {
        float sc = kPresetSC * presetSsc(kPresetSize) * 0.9f;
        float st = sc * 2.0f / 3.0f;
        int idx = 0;
        for (int i = 0; i <= 3; i++, idx++) {
            float x = -sc + i * st;
            v[idx * 2]     = PathVertex(x, -sc, 0, 255, 255, true);
            v[idx * 2 + 1] = PathVertex(x,  sc, 0, 255, 255, false);
            s[idx] = PathSegment(v + idx * 2, 2, false);
        }
        for (int i = 0; i <= 3; i++, idx++) {
            float y = -sc + i * st;
            v[idx * 2]     = PathVertex(-sc, y, 0, 255, 255, true);
            v[idx * 2 + 1] = PathVertex( sc, y, 0, 255, 255, false);
            s[idx] = PathSegment(v + idx * 2, 2, false);
        }
        built = true;
    }
    count = 8;
    return s;
}

// --- Double Spiral (p21, category "Spirals") ------------------------------
// Two open 150-vertex Archimedean arms (3 turns each), 180deg apart.
inline const PathSegment* presetDoubleSpiral(size_t& count) {
    static PathVertex  v[300];
    static PathSegment s[2];
    static bool built = false;
    if (!built) {
        float sc = kPresetSC * presetSsc(kPresetSize) * 0.9f;
        const int N = 150;
        for (int i = 0; i < N; i++) {
            float t = i / (float)N, a = t * kTau * 3.0f, r = t * sc;
            v[i] = PathVertex(cosf(a) * r, sinf(a) * r, 255, 80, 0, i == 0);
        }
        for (int i = 0; i < N; i++) {
            float t = i / (float)N, a = t * kTau * 3.0f + (float)M_PI, r = t * sc;
            v[N + i] = PathVertex(cosf(a) * r, sinf(a) * r, 0, 80, 255, i == 0);
        }
        s[0] = PathSegment(v,     N, false);
        s[1] = PathSegment(v + N, N, false);
        built = true;
    }
    count = 2;
    return s;
}

// --- Nested Squares (p57, category "Complex") -----------------------------
// 6 closed 4-vertex loops, shrinking radius, incrementally rotated.
inline const PathSegment* presetNestedSquares(size_t& count) {
    static PathVertex  v[24];
    static PathSegment s[6];
    static bool built = false;
    if (!built) {
        float sc = kPresetSC * presetSsc(kPresetSize) * 0.9f;
        for (int l = 0; l < 6; l++) {
            float sVal = sc * (6 - l) / 6.0f;
            float rot  = l * ((float)M_PI / (4.0f * 6.0f));
            float h    = l / 6.0f;
            uint8_t r = (uint8_t)(fabsf(sinf(h * (float)M_PI)) * 255);
            uint8_t g = (uint8_t)(fabsf(sinf(h * (float)M_PI + 2.094f)) * 255);
            uint8_t b = (uint8_t)(fabsf(sinf(h * (float)M_PI + 4.189f)) * 255);
            for (int i = 0; i < 4; i++) {
                float a = kTau * i / 4.0f + rot;
                v[l * 4 + i] = PathVertex(cosf(a) * sVal, sinf(a) * sVal, r, g, b);
            }
            s[l] = PathSegment(v + l * 4, 4, true);
        }
        built = true;
    }
    count = 6;
    return s;
}

// --- Hibiscus (p86, category "Party") -------------------------------------
// 5 petals (closed 62-vertex loops) + 1 centre ring (closed 12-vertex loop).
inline const PathSegment* presetHibiscus(size_t& count) {
    constexpr int NP = 5, NT = 31, PETAL = NT * 2, NC = 12;
    static PathVertex  v[NP * PETAL + NC];
    static PathSegment s[NP + 1];
    static bool built = false;
    if (!built) {
        float sc = kPresetSC * presetSsc(kPresetSize) * 0.9f;
        for (int p = 0; p < NP; p++) {
            float base = kTau * p / (float)NP;
            PathVertex* vp = v + p * PETAL;
            for (int i = 0; i < NT; i++) {
                float t = i / (float)(NT - 1), spread = sinf(t * (float)M_PI);
                float a = base + spread * 0.4f, r = 0.15f + (0.65f - 0.15f) * t;
                vp[i] = PathVertex(cosf(a) * r * sc, sinf(a) * r * sc,
                                    255, (uint8_t)(50 + t * 100), (uint8_t)(100 - t * 100), i == 0);
            }
            for (int i = 0; i < NT; i++) {
                float t = (NT - 1 - i) / (float)(NT - 1), spread = sinf(t * (float)M_PI);
                float a = base - spread * 0.4f, r = 0.15f + (0.65f - 0.15f) * t;
                vp[NT + i] = PathVertex(cosf(a) * r * sc, sinf(a) * r * sc,
                                          255, (uint8_t)(50 + t * 100), 0, false);
            }
            s[p] = PathSegment(vp, PETAL, true);
        }
        PathVertex* vc = v + NP * PETAL;
        for (int i = 0; i < NC; i++) {
            float a = kTau * i / (float)NC;
            vc[i] = PathVertex(cosf(a) * 0.12f * sc, sinf(a) * 0.12f * sc, 255, 255, 0, i == 0);
        }
        s[NP] = PathSegment(vc, NC, true);
        built = true;
    }
    count = NP + 1;
    return s;
}

constexpr size_t kPresetFixtureCount = 6;

// Easy: low dynamic load, simple geometry. Medium: multiple direction
// changes / multiple objects. Hard: high point/segment counts, many
// direction changes. Grouping mirrors the task's own difficulty definition.
inline const PresetFixture* presetFixtures() {
    static PresetFixture f[kPresetFixtureCount];
    static bool built = false;
    if (!built) {
        size_t c;
        c = 0; f[0].name = "Square";        f[0].profile = "Vector";      f[0].difficulty = Difficulty::Easy;
                f[0].segs = presetSquare(c);        f[0].count = c;
        c = 0; f[1].name = "Cross +";        f[1].profile = "Vector";      f[1].difficulty = Difficulty::Medium;
                f[1].segs = presetCrossPlus(c);      f[1].count = c;
        c = 0; f[2].name = "Double Spiral";  f[2].profile = "MultiObject"; f[2].difficulty = Difficulty::Medium;
                f[2].segs = presetDoubleSpiral(c);   f[2].count = c;
        c = 0; f[3].name = "Nested Squares"; f[3].profile = "MultiObject"; f[3].difficulty = Difficulty::Medium;
                f[3].segs = presetNestedSquares(c);  f[3].count = c;
        c = 0; f[4].name = "Grid 3x3";       f[4].profile = "Vector";      f[4].difficulty = Difficulty::Hard;
                f[4].segs = presetGrid3x3(c);        f[4].count = c;
        c = 0; f[5].name = "Hibiscus";       f[5].profile = "MultiObject"; f[5].difficulty = Difficulty::Hard;
                f[5].segs = presetHibiscus(c);       f[5].count = c;
        built = true;
    }
    return f;
}

}  // namespace fx
