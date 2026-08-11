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

// One vertex in a path. `lift` is consulted ONLY on vertices[0] of a segment,
// and only to turn that vertex's FIRST corner-dwell point blank instead of
// lit: one dark sample sitting on the landing position before the beam comes
// on. It replaces a dwell point rather than adding one, so it costs no budget.
//
// It does NOT create the move to the vertex. emitAllSegments() blank-jumps to
// every segment's vertices[0] unconditionally (emitBlankJump(), Pillar 2),
// with or without lift. It is also not read on vertices[1..count-1] at all,
// nor on a count==1 segment (emitSegment()'s single-vertex fast path always
// emits lit; no caller builds one).
//
// A disconnected sub-path -- a pen-up inside a text glyph, an isolated
// wireframe edge -- is therefore expressed by splitting the geometry into
// separate PathSegments, which is what text_renderer.cpp's glyph walker does
// at every PU stroke, not by a mid-path lift flag.
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

    // Point Distribution Modifier -- Jitter (Phase 4). Deterministic
    // perpendicular-to-edge offset on interior points only, applied at emit
    // time in emitSegment() -- see config.h's OPT_DEFAULT_JITTER_* comment
    // for why this needs no changes to the planning stages. Off by default
    // -> byte-identical to the pre-jitter optimizer.
    bool     jitter_enabled       = OPT_DEFAULT_JITTER_ENABLED;
    float    jitter_amount_units  = OPT_DEFAULT_JITTER_AMOUNT_UNITS; // max perpendicular offset (DAC units)

    // ── Frame context for multi-call callers ─────────────────────────────
    //
    // A preset that renders each of its sub-shapes with its own optimize()
    // call writes into `out = o + n`, so from inside optimize() the buffer
    // always looks empty. Two things are invisible to it as a result, and
    // these fields hand them in. Left at their defaults, every single-call
    // caller produces byte-identical output to before.
    //
    // hasPrevPos / prevX / prevY -- where the galvo already is when the call
    // starts. Without it emitBlankJump() takes its n==0 fallback and parks
    // blank_samples ticks ON the target instead of ramping to it, i.e. a
    // teleport: Pillars 2 and 3 only ever applied WITHIN a single call.
    bool     hasPrevPos           = false;
    float    prevX                = 0.0f;
    float    prevY                = 0.0f;

    // frameBudgetRemaining -- points the FRAME still has left, as opposed to
    // max_pts_per_frame, which optimize() applies per CALL. Without it N
    // calls each plan a whole frame's worth and the only real ceiling left
    // is max_out (PATTERN_POINTS_MAX). 0 = not tracked, i.e. the per-call
    // behavior. optimize() returns only the count it wrote; the caller owns
    // the subtraction -- see frameContext().
    uint16_t frameBudgetRemaining = 0;
};

// Fills in the frame context above from the caller's own running state, so a
// multi-call preset converts one call site with one line instead of four.
// `o` is the frame buffer base and `n` the number of points already written
// into it this frame -- exactly the pair such callers already carry around to
// build their `optimize(&seg, 1, o + n, m - n, cfg)`.
//
// The budget is derived from n rather than carried in a separate counter that
// each loop would have to decrement itself: n IS the frame's spend so far, it
// cannot drift out of sync, and it also charges points written outside the
// optimizer (raw dwell dots, seam bridges) against the same flicker budget --
// which is what the budget is a statement about.
//
// Returns false when the budget is exhausted; the caller must then stop
// emitting rather than call optimize() anyway. Passing frameBudgetRemaining=0
// would read as "not tracked" and hand that call a full frame all over again.
// At n == 0 the budget is the full max_pts_per_frame and hasPrevPos stays
// false, so the first call of a frame is unchanged.
inline bool frameContext(OptimizerConfig& cfg, const LaserPoint* o, size_t n) {
    if (n >= (size_t)cfg.max_pts_per_frame) return false;
    cfg.frameBudgetRemaining = (uint16_t)((size_t)cfg.max_pts_per_frame - n);
    if (n > 0 && o != nullptr) {
        cfg.hasPrevPos = true;
        cfg.prevX      = (float)o[n - 1].x;
        cfg.prevY      = (float)o[n - 1].y;
    }
    return true;
}

// ── Telemetry ────────────────────────────────────────────────────────────
//
// What one optimize() call actually produced, as opposed to what it planned.
// The optimizer's failure modes are all silent by construction -- a truncated
// shape, a ZV shaper that never activates, a frame using a third of its point
// budget -- and none of them are visible from the emitted geometry alone.
//
// emittedLit/emittedBlank/jumpCount/jumpDistanceTotal are MEASURED from the
// final output buffer after every stage has run (including the velocity /
// acceleration clamp, which inserts points of its own), so they describe the
// stream the galvo is actually handed, not an intermediate plan.
//
// plannedTotal and truncated close the accounting loop instead:
//   plannedTotal = every point the pipeline attempted to write -- the emit
//                  stage plus the clamp stage's interpolated insertions.
//   truncated    = attempted writes dropped because the point budget
//                  (effective_cap) was already full.
// so `emittedLit + emittedBlank + truncated == plannedTotal` holds for every
// call. A point that vanishes anywhere without passing one of those two
// counters breaks the identity -- which is the point of tracking both
// (CONTRACT.md invariant 3, noSilentPointLoss).
//
// The host Contract build switches its gated assertions on with
// -D GALVOS_OPT_HAS_STATS=1 (see [env:native] in platformio.ini); it cannot
// come from this header, because test_contract.cpp includes
// contract_features.h -- which defaults the gate to 0 -- before this file.
struct Stats {
    uint32_t emittedLit;          // lit points in the final buffer
    uint32_t emittedBlank;        // blanked points in the final buffer
    uint32_t truncated;           // attempted writes dropped at the budget cap
    uint32_t plannedTotal;        // emitted + truncated, counted independently
    uint32_t jumpCount;           // blank runs (one per blank jump)
    float    jumpDistanceTotal;   // DAC units travelled with the beam off
    uint32_t calls;               // optimize() calls folded into this record
    float    stage2Scale;         // interior-density factor Stage 2 applied
                                   // (1.0 = Stage 2 did not trigger), floors
                                   // included -- i.e. what was really used
    bool     stage1Triggered;     // blank_samples was reduced to fit budget
    bool     stage15Triggered;    // corner point counts were scaled to fit
    bool     ringingActive;       // ZV shaper actually shaped at least one jump

    Stats() { reset(); }
    void reset();
    void add(const Stats& call);   // frame accumulation, see gFrameStats
};

// PILLAR 3 status for a config, derivable without rendering a frame.
//
// The ZV shaper can silently do nothing: the delay between its two impulses is
// a physical time (half the damped ring period) converted into output points,
// so at a low ring_freq_hz and/or a high galvo_kpps it needs a longer blank
// jump than the optimizer will ever build (kMaxBlankPts) and there is nothing
// to shape with. At 200 Hz / 30 kpps that delay is already 76 points. Rather
// than leaving the user with a ticked "Ringing Compensation" box and no effect,
// this is published as opt_eff_ringing_active / opt_eff_ring_shift_pts on
// /api/config, next to the other opt_eff_* derived values.
//
// Stats::ringingActive is the runtime counterpart: it says a jump really WAS
// shaped while rendering the last frame.
struct RingingStatus {
    bool active;         // shaper runs on the longest jump this config builds
    int  shift_pts;      // second-impulse delay, in output points
    int  min_jump_pts;   // shortest jump, in points, that can carry the shaper
};
RingingStatus ringingStatus(const OptimizerConfig& cfg);

// Stats of the most recent optimize() call. Overwritten per call, so a caller
// that renders one shape can read exactly what that shape cost.
extern Stats gLastStats;

// Stats accumulated over the current frame: every optimize() call adds itself
// here, and pattern_engine clears it at frame start via resetFrameStats() --
// same publish-per-frame lifecycle as gLiveTransform above. This is the record
// the point budget is a property of: a preset that draws three primitives
// calls optimize() three times, and only the sum is the frame.
extern Stats gFrameStats;

// Clears gFrameStats. Called once per frame by pattern_engine, before any
// generate() runs.
void resetFrameStats();

// Runs Pillar-1 density optimization across all given segments and writes
// LaserPoint output (including blank jumps between segments/sub-paths).
// Returns the number of points written (<= max_out). Fills gLastStats and
// adds to gFrameStats.
//
// If the planned point count would exceed the frame budget, interior density
// is searched down (Stage 2 bisects the plan against the cap, landing just
// under it) before anything is written, so output never silently truncates
// mid-shape the way ap()'s "if (n>=mx) return" can -- and what truncation
// remains possible is counted in Stats::truncated.
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

// PPS-derived optimizer scaling (single source of truth, called by every
// path's liveOptimizerConfig()). Scales the point-rate-dependent params --
// interior density (both the legacy and resample-stage forms), blank-jump
// density, and both scanner-protection clamps -- from the ratio of the
// galvo's rated speed to the chosen output rate, leaving GEOMETRY (corner
// counts, angles, segment minimums, blank_samples ceiling) untouched.
//
// Model (r = rated_kpps / output_kpps, the headroom ratio):
//   pts_per_1000_units      *= 1/r  -- density is per output tick, so a lower
//                                      output rate (r>1) needs fewer points
//                                      per unit length, a full-rate output
//                                      (r==1) keeps the tuned value. Mirrors
//                                      the Starfield dwell derivation (points
//                                      scale with the output tick rate for a
//                                      fixed physical target).
//   resample_spacing_units  *= r    -- inverse of the density above (spacing
//                                      is units/point, density is
//                                      points/unit): r>1 means fewer points
//                                      per unit length, i.e. a WIDER target
//                                      spacing. Without this,
//                                      edgeInteriorCount() ignores
//                                      pts_per_1000_units entirely when
//                                      resample_enabled is true, so PPS
//                                      scaling had zero effect on density on
//                                      that path.
//   blank_pts_per_1000_units *= 1/r -- same density-per-output-tick
//                                      reasoning as pts_per_1000_units, just
//                                      for the blank-jump ramp instead of the
//                                      lit interior.
//   max_step_units    *= r          -- units/tick velocity ceiling. Physical
//                                      slew (units/s) tracks the rated speed;
//                                      units/tick = slew / output_rate ∝ r.
//   max_accel_units   *= r*r        -- units/tick^2, so the ratio squared.
//
// At the calibration point (output_kpps == rated_kpps) r==1 -> all five are
// unchanged, so a system run at its rated speed sees the exact tuned values.
// Guards against divide-by-zero / absurd ratios; clamps r to a sane band.
inline void applyPpsScaling(OptimizerConfig& cfg,
                            uint16_t rated_kpps, uint16_t output_kpps) {
    if (rated_kpps == 0 || output_kpps == 0) return;   // nothing sane to derive
    float r = (float)rated_kpps / (float)output_kpps;
    if (r < 0.1f) r = 0.1f;                             // clamp to a sane band
    if (r > 10.0f) r = 10.0f;
    cfg.pts_per_1000_units      *= (1.0f / r);
    cfg.resample_spacing_units  *= r;
    cfg.blank_pts_per_1000_units *= (1.0f / r);
    cfg.max_step_units          *= r;
    cfg.max_accel_units         *= r * r;
}

// Emits a distance-proportional, smoothstep-eased blank jump from the
// current galvo position (last point in out[0..n-1]) to (x1, y1).
// Writes only blank points (laser OFF). Used by patterns that manage
// their own point emission (e.g. Starfield single-dot dwell).
// If n==0 (no previous position known), emits cfg.blank_samples ticks
// at (x1,y1) as a conservative settle.
void emitBlankTo(LaserPoint* out, size_t& n, size_t max,
                 float x1, float y1, const OptimizerConfig& cfg);

// Velocity / Acceleration clamp (Phase 4) post-pass, exposed directly for
// callers that own an already-emitted LaserPoint stream instead of
// PathSegment geometry -- e.g. ILDA playback, whose frames come pre-
// rendered from the .ild file and never go through optimize(). Subdivides
// lit-to-lit steps exceeding cfg.max_step_units / cfg.max_accel_units;
// blank-adjacent steps are exempt (see point_optimizer.cpp). No-op
// (byte-identical, returns n unchanged) unless cfg.vel_clamp_enabled /
// cfg.accel_clamp_enabled are set. Returns the new point count (<= max_out).
size_t clampScannerLimits(LaserPoint* out, size_t n,
                          const OptimizerConfig& cfg, size_t max_out);

}  // namespace optimizer