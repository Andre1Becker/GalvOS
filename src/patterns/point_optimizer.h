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
struct PathVertex {
    float   x, y;
    uint8_t r, g, b;
    bool    lift = false;
};

// One path = a sequence of vertices connected by straight segments.
// closed=true adds an implicit edge from vertices[count-1] back to
// vertices[0] (e.g. ngon, star). closed=false is an open polyline
// (e.g. one wireframe edge, one text-glyph stroke run).
struct PathSegment {
    const PathVertex* vertices;
    size_t            count;
    bool              closed = false;
};

// Runtime-tunable parameters (mirrors gOptimizerConfig in config.h --
// passed explicitly here rather than read as a global so the function
// stays testable / has no hidden state).
struct OptimizerConfig {
    float   corner_angle_deg   = 25.0f;  // exterior angle below which a
                                          // vertex is NOT a "sharp corner"
    uint8_t min_corner_pts     = 1;      // points placed at the softest corners
    uint8_t max_corner_pts     = 6;      // points placed at the sharpest (180°) corners
    float   pts_per_1000_units = 4.0f;   // interior straight-segment density
    uint8_t min_segment_pts    = 2;      // floor per edge (>=2 = start+end)
    uint8_t blank_samples      = 40;     // fixed blank-jump length (Pillar 2 will
                                          // make this a max instead of a constant)
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