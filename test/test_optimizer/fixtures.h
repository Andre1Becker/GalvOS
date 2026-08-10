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

}  // namespace fx
