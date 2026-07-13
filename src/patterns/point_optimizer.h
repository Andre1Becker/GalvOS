#pragma once
#include "config.h"

/**
* point_optimizer.h -- GalvOS v5 Point Optimizer
 *   Pillar 1: adaptive corner/interior point density (done)
 *   Pillar 2: distance-proportional, eased blank-jump sampling (done)
 *   Pillar 3: active ringing compensation via ZV input shaping, applied
 *             to blank-jump moves (this revision). Corner-dwell shaping
 *             is deferred, see design doc Section 5.3.
 *
 * Sits between pattern generation and galvo::pushFrame(). Patterns describe
 * geometry as PathSegments (vertex lists with corner metadata); optimize()
 * performs corner-aware, length-proportional point sampling and writes the
 * final LaserPoint[] output.
 *
 * Pillar 3 needs cfg.ring_freq_hz / cfg.ring_damping_ratio measured on real
 * hardware (step-response capture on a scope). cfg.ringing_comp_enabled
 * defaults to false so unmeasured defaults can't make ringing worse.
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

// Affine 2x3 transform applied to every input vertex before the corner /
// resample / blanking stages run (pipeline stage: Primitive -> Transform ->
// Resample -> Corner Dwell -> Blanking -> ...). Row-major:
//   x' = a*x + b*y + tx
//   y' = c*x + d*y + ty
// The default is the identity ({1,0,0, 0,1,0}), so callers that do not set a
// transform get byte-identical output to the pre-transform-stage optimizer.
// A full 2x3 (not 2x2) is used so translation composes into the same matrix
// as rotation/scale/shear -- Phase 3 builds these from the live rotation /
// move controls (pattern_engine publishes optimizer::gLiveTransform per frame)
// instead of the old post-optimizer inline pass. Non-affine effects (Y/X
// perspective tilt, DMX wave warp) remain post-optimizer point passes.
struct AffineTransform {
    float a, b, tx;   // first row  (x' = a*x + b*y + tx)
    float c, d, ty;   // second row (y' = c*x + d*y + ty)

    AffineTransform() : a(1), b(0), tx(0), c(0), d(1), ty(0) {}
    AffineTransform(float a, float b, float tx, float c, float d, float ty)
        : a(a), b(b), tx(tx), c(c), d(d), ty(ty) {}

    bool isIdentity() const {
        return a == 1.0f && b == 0.0f && tx == 0.0f &&
               c == 0.0f && d == 1.0f && ty == 0.0f;
    }

    // Apply to a point (in-place-friendly: reads inputs before writing).
    void apply(float xin, float yin, float& xout, float& yout) const {
        xout = a * xin + b * yin + tx;
        yout = c * xin + d * yin + ty;
    }
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
    bool     resample_enabled    = OPT_DEFAULT_RESAMPLE_ENABLED;    // RESAMPLE STAGE (Phase 2): when true,
                                                                    // edgeInteriorCount() uses constant
                                                                    // spacing (length / resample_spacing_units)
                                                                    // instead of pts_per_1000_units. false =
                                                                    // byte-identical to pre-resample output.
    float    resample_spacing_units = OPT_DEFAULT_RESAMPLE_SPACING_UNITS; // Target distance between interior
                                                                    // points when resample_enabled. Smaller =
                                                                    // denser. Corner dwell runs on top of this
                                                                    // (see pipeline: Resample -> Corner Dwell).
    float    blank_pts_per_1000_units = OPT_DEFAULT_BLANK_PTS_PER_1000_UNITS;
                                                                    // PILLAR 2: distance-proportional blank
                                                                    // density. emitBlankJump() clamps to
                                                                    // [min_blank_samples, blank_samples].
                                                                    // Smoothstep ease-in/out applied.
    uint8_t  min_interior_pts_per_segment = OPT_DEFAULT_MIN_INTERIOR_PTS_PER_SEG;
                                                                    // Interior pts reserved per segment
                                                                    // before blank budget is computed.
    bool     ringing_comp_enabled = OPT_DEFAULT_RINGING_COMP_ENABLED; // PILLAR 3: enables the ZV shaper
                                                                    // in emitBlankJump(). false = shaper
                                                                    // reduces to A1=1/A2=0, byte-identical
                                                                    // to pre-Pillar-3 output.
    float    ring_freq_hz         = OPT_DEFAULT_RING_FREQ_HZ;      // Measured galvo mechanical resonance (Hz).
    float    ring_damping_ratio   = OPT_DEFAULT_RING_DAMPING_RATIO; // Measured damping ratio zeta (0..~0.9).
    uint16_t galvo_kpps           = 30;    // Mirrors gProjection.galvo_kpps -- passed explicitly rather
                                            // than read as a global (same rule as the rest of this
                                            // struct, see file header) so the ZV shaper can convert a
                                            // physical time (half the ring period) into a point count.

    // Transform stage (Phase 1). Applied to every input vertex before
    // corner/resample/blanking. Identity by default -> output is unchanged
    // for callers that leave this alone. Phase 3 populates it from the live
    // rotation/move controls via optimizer::gLiveTransform, retiring the
    // post-optimizer inline Z-rotation formerly in pattern_engine.cpp.
    AffineTransform transform;

    // Velocity / Acceleration clamp (Phase 4). Post-pass over the emitted
    // lit point stream (see clampScannerLimits()). Both off by default ->
    // output byte-identical to the pre-clamp optimizer. Tuned on hardware.
    bool     vel_clamp_enabled    = OPT_DEFAULT_VEL_CLAMP_ENABLED;   // subdivide over-long lit steps
    float    max_step_units       = OPT_DEFAULT_MAX_STEP_UNITS;      // max per-tick position delta (units/sample)
    bool     accel_clamp_enabled  = OPT_DEFAULT_ACCEL_CLAMP_ENABLED; // limit per-tick step-magnitude growth
    float    max_accel_units      = OPT_DEFAULT_MAX_ACCEL_UNITS;     // max per-tick velocity delta (units/sample^2)
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

// Builds a 2x3 affine from an in-plane rotation (radians, CCW) plus a
// post-rotation translation (DAC units). Composition order matches the
// legacy inline pass it replaces: rotate about the origin first, then
// translate -- x' = R*x + t. angle==0 && tx==ty==0 yields the identity,
// so a caller with no active rotation/move produces byte-identical output.
inline AffineTransform makeTransform(float angle_rad, float tx, float ty) {
    float ca = cosf(angle_rad), sa = sinf(angle_rad);
    return AffineTransform(ca, -sa, tx,
                           sa,  ca, ty);
}

// Live transform published by the pattern engine once per frame (under
// mtx::state) BEFORE the active generate() call runs, and copied into
// OptimizerConfig::transform by each path's liveOptimizerConfig(). Holds
// the affine part (in-plane Z rotation + translation) of the live controls
// / DMX so the optimizer sees rotated, moved geometry before corner
// detection and resampling. Non-affine effects (Y/X perspective tilt, DMX
// wave warp, auto-scale collapse) stay as post-optimizer point passes.
// Defaults to identity -> no behavioural change until the engine writes it.
extern AffineTransform gLiveTransform;

// Emits a distance-proportional, smoothstep-eased blank jump from the
// current galvo position (last point in out[0..n-1]) to (x1, y1).
// Writes only blank points (laser OFF). Used by patterns that manage
// their own point emission (e.g. Starfield single-dot dwell).
// If n==0 (no previous position known), emits cfg.blank_samples ticks
// at (x1,y1) as a conservative settle.
void emitBlankTo(LaserPoint* out, size_t& n, size_t max,
                 float x1, float y1, const OptimizerConfig& cfg);

}  // namespace optimizer