#pragma once
#include "config.h"

/**
 * point_optimizer.h -- GalvOS v5 Point Optimizer (Pillar 1: adaptive density)
 *
 * Sits between pattern generation and galvo::pushFrame(). Patterns describe
 * geometry as PathSegments (vertex lists with corner metadata); optimize()
 * performs corner-aware, length-proportional point sampling and writes the
 * final LaserPoint[] output.
 *
 * Blanking between sub-paths still uses a fixed sample count
 * (OptimizerConfig::blank_samples) -- distance-proportional / eased
 * blanking is Pillar 2 (not implemented yet, see design doc Section 5).
 *
 * Scope: works for discrete-vertex geometry (polygons, stars, wireframes,
 * text-glyph strokes). NOT used for curve_patterns.cpp -- continuous
 * parametric curves have no discrete corners; see design doc Section 9.2.
 */

namespace optimizer {

// One vertex in a path. lift=true means: blank-jump TO this vertex
// (it starts a new disconnected sub-path, e.g. after a pen-up in a
// text glyph, or the first vertex of an isolated wireframe edge).
//
// NOTE: explicit constructors, not default member initializers --
// PlatformIO/Arduino-ESP32 builds under -std=gnu++11, where a struct
// with a default member initializer is no longer an aggregate and
// brace-init (`PathVertex v{...}` or `v = {...}`) stops compiling
// ("no match for operator=" / "no matching constructor"). Explicit
// constructors work under every standard.
struct PathVertex {
    float   x, y;
    uint8_t r, g, b;
    bool    lift;

    PathVertex() : x(0), y(0), r(0), g(0), b(0), lift(false) {}
    PathVertex(float x, float y, uint8_t r, uint8_t g, uint8_t b, bool lift = false)
        : x(x), y(y), r(r), g(g), b(b), lift(lift) {}
};

// One path = a sequence of vertices connected by straight segments.
// closed=true adds an implicit edge from vertices[count-1] back to
// vertices[0] (e.g. ngon, star). closed=false is an open polyline
// (e.g. one wireframe edge, one text-glyph stroke run).
//
// Same C++11/aggregate note as PathVertex above -- explicit constructor.
struct PathSegment {
    const PathVertex* vertices;
    size_t            count;
    bool              closed;

    // Default constructor (count=0 -> optimize() skips this segment, see
    // the `if (seg.count == 0) continue;` guard) -- needed so callers
    // with a variable/upper-bounded number of segments can declare a
    // fixed-size PathSegment array and fill only the first N entries
    // (e.g. wf()'s wireframe edges: declare PathSegment segs[64], use
    // only segs[0..edge_count-1]).
    PathSegment() : vertices(nullptr), count(0), closed(false) {}
    PathSegment(const PathVertex* vertices, size_t count, bool closed = false)
        : vertices(vertices), count(count), closed(closed) {}
};

// Runtime-tunable parameters (mirrors gOptimizerConfig in config.h --
// passed explicitly here rather than read as a global so the function
// stays testable / has no hidden state).
struct OptimizerConfig {
    float    corner_angle_deg   = 25.0f;  // exterior angle below which a
                                           // vertex is NOT a "sharp corner"
    uint8_t  min_corner_pts     = 1;      // points placed at the softest corners
    uint8_t  max_corner_pts     = 6;      // points placed at the sharpest (180°) corners
    float    pts_per_1000_units = 4.0f;   // interior straight-segment density
    uint8_t  min_segment_pts    = 2;      // floor per edge (>=2 = start+end)
    uint8_t  blank_samples      = 40;     // fixed blank-jump length (Pillar 2 will
                                           // make this a max instead of a constant)
    uint16_t max_pts_per_frame  = 310;    // FLICKER BUDGET, separate from max_out
                                           // (buffer capacity). At 15kpps, n points
                                           // per frame -> 15000/n Hz frame rate.
                                           // Measured on hardware: >=310 pts/frame
                                           // visibly strobes (square=460pts flickers,
                                           // star4=176pts does not). 280 gives ~10%
                                           // margin below the observed 310pt threshold
                                           // (15000/280 ~= 53Hz). Tune via WebUI slider
                                           // if a given setup needs more/less margin.
uint8_t  min_blank_samples  = 8;      // floor for blank_samples when the budget
                                           // clamp needs to shrink blanking itself,
                                           // not just interior density (relevant for
                                           // many-short-edges shapes like wireframes,
                                           // where blank overhead -- not interior
                                           // density -- dominates the point count;
                                           // e.g. 30-edge dodecahedron: 31 blank runs
                                           // x 40 samples = 1240 pts before a single
                                           // visible point is drawn). The original
                                           // v4.5.29 fix found 1 sample insufficient
                                           // for large jumps -- 8 is a conservative
                                           // floor, not yet hardware-validated at the
                                           // low end. This is an interim measure;
                                           // proper distance-proportional + eased
                                           // blanking is Pillar 2 (see design doc).
    uint8_t  min_interior_pts_per_segment = 6;
                                           // Minimum interior points to RESERVE per
                                           // segment before computing the blank
                                           // budget. Without this, blank_samples
                                           // only shrinks once the OVERALL cap is
                                           // exceeded -- a 6-edge tetrahedron at the
                                           // default 40 blank samples uses only
                                           // 292/310 of the cap, so the cap-based
                                           // trigger never fires, yet the 18 points
                                           // left over for interior density (3/edge)
                                           // is still too sparse to read as a line,
                                           // not a dotted/broken edge. This forces
                                           // blank_samples to give up budget earlier
                                           // so each edge gets a usable minimum.
};

// Runs Pillar-1 density optimization across all given segments and writes
// LaserPoint output (including blank jumps between segments/sub-paths).
// Returns the number of points written (<= max_out).
//
// If the planned point count would exceed max_out, pts_per_1000_units is
// scaled down uniformly once and the pass is repeated, so output never
// silently truncates mid-shape the way ap()'s "if (n>=mx) return" can.
size_t optimize(const PathSegment* segments, size_t segment_count,
                 LaserPoint* out, size_t max_out,
                 const OptimizerConfig& cfg);

}  // namespace optimizer