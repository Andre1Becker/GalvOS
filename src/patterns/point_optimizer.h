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
// Defaults sourced from OPT_DEFAULT_* macros in config.h -- single source
    // of truth. Both OptimizerConfig (here) and OptimizerLiveConfig (config.h)
    // reference the same macros so they stay in sync automatically.
    float    corner_angle_deg   = OPT_DEFAULT_CORNER_ANGLE_DEG;   // exterior angle below which a
                                                                    // vertex is NOT a "sharp corner"
    uint8_t  min_corner_pts     = OPT_DEFAULT_MIN_CORNER_PTS;     // points placed at the softest corners
    uint8_t  max_corner_pts     = OPT_DEFAULT_MAX_CORNER_PTS;     // points placed at the sharpest (180°) corners
    float    pts_per_1000_units = OPT_DEFAULT_PTS_PER_1000_UNITS; // interior straight-segment density
    uint8_t  min_segment_pts    = OPT_DEFAULT_MIN_SEGMENT_PTS;    // floor per edge (>=2 = start+end)
    uint8_t  blank_samples      = OPT_DEFAULT_BLANK_SAMPLES;      // blank-jump length ceiling (Pillar 2
                                                                    // makes this a max, not a constant)
    uint16_t max_pts_per_frame  = OPT_DEFAULT_MAX_PTS_PER_FRAME;  // FLICKER BUDGET: 45000/750 = 60 Hz.
                                                                    // Tune via WebUI slider.
    uint8_t  min_blank_samples  = OPT_DEFAULT_MIN_BLANK_SAMPLES;  // floor for blank_samples when budget
                                                                    // clamp shrinks blanking (not just
                                                                    // interior density). Pillar 2 interim.
    uint8_t  stage1_blank_target = OPT_DEFAULT_STAGE1_BLANK_TARGET; // Stage 1 reduces blank_samples to
                                                                    // this value before falling back to
                                                                    // min_blank_samples as last resort.
    float    blank_pts_per_1000_units = OPT_DEFAULT_BLANK_PTS_PER_1000_UNITS;
                                                                    // PILLAR 2: distance-proportional blank
                                                                    // density. emitBlankJump() clamps to
                                                                    // [min_blank_samples, blank_samples].
                                                                    // Smoothstep ease-in/out applied.
    uint8_t  min_interior_pts_per_segment = OPT_DEFAULT_MIN_INTERIOR_PTS_PER_SEG;
                                                                    // Interior pts reserved per segment
                                                                    // before blank budget is computed.
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

// Emits a distance-proportional, smoothstep-eased blank jump from the
// current galvo position (last point in out[0..n-1]) to (x1, y1).
// Writes only blank points (laser OFF). Used by patterns that manage
// their own point emission (e.g. Starfield single-dot dwell).
// If n==0 (no previous position known), emits cfg.blank_samples ticks
// at (x1,y1) as a conservative settle.
void emitBlankTo(LaserPoint* out, size_t& n, size_t max,
                 float x1, float y1, const OptimizerConfig& cfg);

}  // namespace optimizer