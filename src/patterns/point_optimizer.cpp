#include "point_optimizer.h"
#include <math.h>
#include <algorithm>
#if defined(ESP_PLATFORM) || defined(ARDUINO)
#include <esp_heap_caps.h>
#else
// Host-side unit test build (g++ + cfg_stub.h): no ESP heap API. Fall back to
// plain malloc so clampScannerLimits() is testable off-target. MALLOC_CAP_*
// is ignored here.
#include <cstdlib>
#define MALLOC_CAP_SPIRAM 0
static inline void* heap_caps_malloc(size_t sz, uint32_t) { return malloc(sz); }
#endif

namespace optimizer {

static constexpr float PI_F  = 3.14159265358979323846f;
static constexpr float TAU_F = 2.0f * PI_F;

// ── internal helpers ─────────────────────────────────────────────────────

static inline void emit(LaserPoint* out, size_t& n, size_t max,
                         float x, float y, uint8_t r, uint8_t g, uint8_t b,
                         uint8_t blank) {
    if (n >= max) return;
    out[n] = LaserPoint(
        (int16_t)std::max(-32767.f, std::min(32767.f, x)),
        (int16_t)std::max(-32767.f, std::min(32767.f, y)),
        r, g, b, blank);
    n++;
}

static inline void emitBlankRun(LaserPoint* out, size_t& n, size_t max,
                                 float x, float y, uint8_t count) {
    for (uint8_t k = 0; k < count && n < max; k++)
        emit(out, n, max, x, y, 0, 0, 0, 1);
}

// smoothstep ease-in/ease-out: 3t^2 - 2t^3. At t=0 and t=1, the derivative
// is 0 -- the galvo starts and stops the jump gently instead of being
// commanded to an instantaneous velocity change at both ends, which is
// what produces overshoot/undershoot at the landing point (visible as
// edges that don't quite meet at a shared vertex) and curved-looking
// straight segments (the servo is still settling from the jump while the
// first few "interior" points of the next edge are being drawn).
static inline float smoothstep(float t) {
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return t * t * (3.0f - 2.0f * t);
}

// PILLAR 3: Zero-Vibration (ZV) two-impulse input shaper. Cancels galvo
// ringing by convolving the commanded trajectory with two impulses -- A1
// at t=0, A2 at t=Td/2 -- sized so the plant's response to the first
// impulse is destructively cancelled by its response to the second. See
// design doc Section 5.3 for the derivation. With cfg.ringing_comp_enabled
// == false (the default) this reduces to A1=1, A2=0: shaped output is
// then byte-identical to the pre-Pillar-3 trajectory.
struct ZvShaper {
    float A1 = 1.0f, A2 = 0.0f;
    int   shift_pts = 0;   // point-count delay of the second impulse
};

static ZvShaper computeZvShaper(const OptimizerConfig& cfg) {
    ZvShaper s;
    if (!cfg.ringing_comp_enabled || cfg.ring_freq_hz <= 1.0f || cfg.galvo_kpps == 0)
        return s;

    float zeta  = std::max(0.0f, std::min(0.9f, cfg.ring_damping_ratio));
    float wn    = TAU_F * cfg.ring_freq_hz;                // undamped natural freq, rad/s
    float wd_f  = sqrtf(std::max(1.0f - zeta * zeta, 1e-6f));
    float K     = expf(-zeta * PI_F / wd_f);               // amplitude ratio between the two impulses

    s.A1 = 1.0f / (1.0f + K);
    s.A2 = K    / (1.0f + K);

    float td_half_s       = PI_F / (wn * wd_f);            // half the damped oscillation period
    float point_period_s  = 1.0f / ((float)cfg.galvo_kpps * 1000.0f);
    s.shift_pts = (int)lroundf(td_half_s / point_period_s);
    if (s.shift_pts < 1) s.shift_pts = 1;
    return s;
}

// Pillar 2: distance-proportional + eased blank jump from (x0,y0) to
// (x1,y1). Sample count scales with jump distance using the same "points
// per 1000 units" convention as interior density (cfg.blank_pts_per_1000_units),
// clamped to [min_blank_samples, blank_samples] -- short jumps (e.g.
// between adjacent wireframe vertices) get fewer samples, long diagonal
// jumps get more, instead of every jump paying the same fixed cost
// regardless of distance.
// Falls back to a simple stay-at-target run (old emitBlankRun behavior)
// when there is no prior point to jump from (n==0).
//
// PILLAR 3: the resulting move+settle trajectory is buffered locally,
// then re-shaped with the ZV impulse response above (shaped[i] =
// A1*u[i] + A2*u[i-shift]) before being emitted -- actively cancelling
// galvo ringing at the landing point instead of only waiting it out.
// Falls back to the unshaped trajectory when disabled, or when the
// jump's point budget is too small to fit the required shift.
static void emitBlankJump(LaserPoint* out, size_t& n, size_t max,
                           float x1, float y1, const OptimizerConfig& cfg) {
    if (n == 0) {
        emitBlankRun(out, n, max, x1, y1, cfg.blank_samples);
        return;
    }
    float x0 = out[n - 1].x, y0 = out[n - 1].y;
    float dx = x1 - x0, dy = y1 - y0;
    float dist = sqrtf(dx * dx + dy * dy);

    int count = (int)lroundf((dist / 1000.0f) * cfg.blank_pts_per_1000_units);
    if (count < (int)cfg.min_blank_samples) count = cfg.min_blank_samples;
    if (count > (int)cfg.blank_samples)     count = cfg.blank_samples;

    // Settle ticks are carved from the end of count (no budget increase).
    // Cap at count/2 so there are always enough move ticks for the galvo
    // to decelerate smoothly -- without this, short jumps get settle=count
    // and move=0, forcing an instantaneous position jump that causes overshoot.
    int settle = (int)cfg.min_blank_samples;
    if (settle > count / 2) settle = count / 2;
    if (settle < 1) settle = 1;
    int move = count - settle;

    // kMaxBlankPts headroom above the WebUI's blank_samples clamp (<=100,
    // see web_ui.cpp POST /api/optimizer-live) -- count can never exceed
    // cfg.blank_samples, so 128 always covers it with margin.
    static constexpr int kMaxBlankPts = 128;
    float ux[kMaxBlankPts], uy[kMaxBlankPts];
    int total = std::min(count, kMaxBlankPts);
    for (int i = 0; i < total; i++) {
        if (i < move) {
            float t = smoothstep((float)(i + 1) / (float)move);
            ux[i] = x0 + dx * t;
            uy[i] = y0 + dy * t;
        } else {
            ux[i] = x1;
            uy[i] = y1;
        }
    }

    ZvShaper shaper = computeZvShaper(cfg);
    bool shape_active = shaper.A2 > 0.0f && shaper.shift_pts < total;

    for (int i = 0; i < total && n < max; i++) {
        float sx = ux[i], sy = uy[i];
        if (shape_active) {
            int j = i - shaper.shift_pts;
            float px = (j >= 0) ? ux[j] : x0;
            float py = (j >= 0) ? uy[j] : y0;
            sx = shaper.A1 * ux[i] + shaper.A2 * px;
            sy = shaper.A1 * uy[i] + shaper.A2 * py;
        }
        emit(out, n, max, sx, sy, 0, 0, 0, 1);
    }
}

// Exterior angle (0..PI) between the incoming edge (prev->cur) and the
// outgoing edge (cur->next), measured at "cur". 0 = straight through
// (collinear, dot=+1, acos=0), PI = full reversal (dot=-1, acos=PI).
// Degenerate (zero-length) edges return 0 (treated as soft/no extra
// density) rather than producing NaN.
static float exteriorAngle(float pxx, float pxy, float cxx, float cxy,
                            float nxx, float nxy) {
    float ix = cxx - pxx, iy = cxy - pxy;   // incoming direction
    float ox = nxx - cxx, oy = nxy - cxy;   // outgoing direction
    float ilen = sqrtf(ix * ix + iy * iy);
    float olen = sqrtf(ox * ox + oy * oy);
    if (ilen < 1e-6f || olen < 1e-6f) return 0.0f;
    float dot = (ix * ox + iy * oy) / (ilen * olen);
    dot = std::max(-1.0f, std::min(1.0f, dot));
    return acosf(dot);   // collinear (same direction) -> 0; full reversal -> PI
}

// Forward decl: edgeInteriorCount() (defined below) is needed by
// cornerSeverity() to estimate the incoming edge's approach speed.
static uint16_t edgeInteriorCount(float length, const OptimizerConfig& cfg);

// Corner severity in [0,1]: 0 = soft/straight (or an open-path endpoint
// with no neighbor on one side), 1 = the sharpest case this optimizer
// handles. Two independent contributors are blended with max() so
// either one alone can drive dwell up to the sharpest setting:
//
//  - angleT:  geometric severity from the exterior angle (as before).
//  - speedT:  severity from how fast the beam is
//    actually moving when it arrives. A short edge below the
//    edgeInteriorCount() floor (too few interior points for its
//    length) gets snapped to the vertex in one oversized step instead
//    of decelerating into it -- angle alone can't see this, since a
//    short sharp edge and a long sharp edge produce the same angle but
//    very different arrival speeds. speedT compares the incoming
//    edge's actual per-point step length against the nominal step
//    length the current pts_per_1000_units density targets: 0 at/below
//    nominal, ramping to 1 at 2x nominal or more.
static float cornerSeverity(const PathSegment& seg, const OptimizerConfig& cfg,
                             size_t i) {
    bool hasIncoming = seg.closed || i > 0;
    bool hasOutgoing = seg.closed || i + 1 < seg.count;
    if (!hasIncoming || !hasOutgoing) return 0.0f;

    size_t prev = (i == 0) ? seg.count - 1 : i - 1;
    size_t next = (i + 1) % seg.count;

    float angle = exteriorAngle(seg.vertices[prev].x, seg.vertices[prev].y,
                                 seg.vertices[i].x,    seg.vertices[i].y,
                                 seg.vertices[next].x, seg.vertices[next].y);
    float angle_deg = angle * (180.0f / PI_F);
    float span = 180.0f - cfg.corner_angle_deg;
    float angleT = (angle_deg <= cfg.corner_angle_deg) ? 0.0f :
                   (span > 0.01f ? (angle_deg - cfg.corner_angle_deg) / span : 1.0f);
    angleT = std::max(0.0f, std::min(1.0f, angleT));

    float dxp = seg.vertices[i].x - seg.vertices[prev].x;
    float dyp = seg.vertices[i].y - seg.vertices[prev].y;
    float inLen = sqrtf(dxp * dxp + dyp * dyp);
    uint16_t inPts = edgeInteriorCount(inLen, cfg);
    float stepLen = inLen / (float)(inPts + 1);
    // Nominal per-point step the active density targets -- must match the
    // mode edgeInteriorCount() used above, or speedT compares against the
    // wrong reference. Resample: spacing is the nominal step directly.
    float nominalStep;
    if (cfg.resample_enabled && cfg.resample_spacing_units > 0.01f) {
        nominalStep = cfg.resample_spacing_units;
    } else {
        nominalStep = (cfg.pts_per_1000_units > 0.01f)
                          ? 1000.0f / cfg.pts_per_1000_units : 0.0f;
    }
    float speedT = 0.0f;
    if (nominalStep > 0.01f) {
        speedT = (stepLen - nominalStep) / nominalStep;
        speedT = std::max(0.0f, std::min(1.0f, speedT));
    }

    return std::max(angleT, speedT);
}

// Number of points to place at a corner, scaled by severity between
// cfg.min_corner_pts (severity 0) and cfg.max_corner_pts (severity 1).
static uint8_t cornerPointCount(float severity, const OptimizerConfig& cfg) {
    float pts = cfg.min_corner_pts +
                severity * (cfg.max_corner_pts - cfg.min_corner_pts);
    return (uint8_t)lroundf(pts);
}

// Corner dwell point count at vertex i of a segment. Shared by
// planSegment(), emitSegment(), and the closed-path second dwell at
// vertex 0 so all three agree on the same count.
static uint8_t cornerPtsAtVertex(const PathSegment& seg,
                                  const OptimizerConfig& cfg, size_t i) {
    return cornerPointCount(cornerSeverity(seg, cfg, i), cfg);
}

// Reshapes a uniformly-spaced edge parameter (0..1) into a
// velocity-eased one. Endpoints (t=0, t=1) stay fixed, so point COUNT
// and coverage are unchanged -- only spacing within the edge shifts,
// denser near whichever end has higher corner severity.
//
// blend(t) interpolates linearly from easeIn (at t=0) to easeOut (at
// t=1); the result is one continuous formula across the whole edge,
// not two pieces stitched at the midpoint. A stitched version
// (smoothstep on [0,0.5] with easeIn, on [0.5,1] with easeOut) matches
// VALUE at the stitch but not SLOPE whenever easeIn != easeOut (the
// normal case) -- that derivative jump is a velocity kink at every
// edge midpoint, exciting galvo ringing on every pattern. This form
// has no seam, so no kink exists at any severity combination.
static float shapeEdgeT(float t, float easeIn, float easeOut) {
    float blend = easeIn + (easeOut - easeIn) * t;
    return t + blend * (smoothstep(t) - t);
}

// RESAMPLE STAGE (Phase 2): interior (non-endpoint) sample count for one
// straight edge of the given length, before the corner points at either end.
//
// Two modes, selected by cfg.resample_enabled:
//   - Resample OFF (default): length-proportional density via
//     pts_per_1000_units. Byte-identical to the pre-resample optimizer.
//   - Resample ON: constant spacing -- points = length / resample_spacing_units.
//     Point spacing is then absolute and length-independent (a short and a
//     long edge get the same points-per-unit), which is what keeps galvo
//     velocity uniform across a shape instead of scaling with edge length.
static uint16_t edgeInteriorCount(float length, const OptimizerConfig& cfg) {
    float raw;
    if (cfg.resample_enabled && cfg.resample_spacing_units > 0.01f) {
        // -1: length/spacing counts the point intervals; interior points are
        // the divisions strictly between the two endpoints (which are corner
        // points), so one fewer than the interval count.
        raw = (length / cfg.resample_spacing_units) - 1.0f;
    } else {
        raw = (length / 1000.0f) * cfg.pts_per_1000_units;
    }
    int n = (int)lroundf(raw);
    if (n < 0) n = 0;
    return (uint16_t)n;
}

struct PlannedSegment {
    const PathSegment* seg;
    uint16_t total_pts;   // corner + interior points for this segment,
                           // NOT including the blank jump leading into it
};

// First pass: compute how many points each segment would need at the
// given pts_per_1000_units scale, without writing any output. Optionally
// reports the corner-point and interior-point sub-totals separately
// (out_corner_pts / out_interior_pts may be nullptr) so the caller can
// scale only the interior (length-proportional) portion when the overall
// budget is exceeded -- corner points are capped by max_corner_pts
// already and matter most for tracking accuracy, so they're treated as
// fixed overhead rather than scaled down.
static uint16_t planSegment(const PathSegment& seg, const OptimizerConfig& cfg,
                             uint16_t* out_corner_pts = nullptr,
                             uint16_t* out_interior_pts = nullptr) {
    if (seg.count == 0) { if(out_corner_pts)*out_corner_pts=0; if(out_interior_pts)*out_interior_pts=0; return 0; }
    if (seg.count == 1) { if(out_corner_pts)*out_corner_pts=1; if(out_interior_pts)*out_interior_pts=0; return 1; }

    size_t edge_count = seg.closed ? seg.count : (seg.count - 1);
    uint32_t corner_total = 0, interior_total = 0;

    for (size_t i = 0; i < seg.count; i++) {
        corner_total += cornerPtsAtVertex(seg, cfg, i);
    }

    if (seg.closed) {
        // Reserve budget for vertex 0's second corner dwell, emitted at
        // the end of emitSegment() for closed paths -- same size as its
        // frame-start dwell, fixed overhead, not scaled with interior
        // density.
        corner_total += cornerPtsAtVertex(seg, cfg, 0);
    }

    for (size_t e = 0; e < edge_count; e++) {
        size_t a = e, b = (e + 1) % seg.count;
        float dx = seg.vertices[b].x - seg.vertices[a].x;
        float dy = seg.vertices[b].y - seg.vertices[a].y;
        float len = sqrtf(dx * dx + dy * dy);
        interior_total += edgeInteriorCount(len, cfg);
    }

    if (out_corner_pts)   *out_corner_pts   = (corner_total > 0xFFFF) ? 0xFFFF : (uint16_t)corner_total;
    if (out_interior_pts) *out_interior_pts = (interior_total > 0xFFFF) ? 0xFFFF : (uint16_t)interior_total;

    uint32_t total = corner_total + interior_total;
    if (total < cfg.min_segment_pts) total = cfg.min_segment_pts;
    if (total > 0xFFFF) total = 0xFFFF;
    return (uint16_t)total;
}

// Second pass: actually write corner + interior points for one segment.
static void emitSegment(const PathSegment& seg, const OptimizerConfig& cfg,
                         LaserPoint* out, size_t& n, size_t max) {
    if (seg.count == 0) return;
    if (seg.count == 1) {
        const PathVertex& v = seg.vertices[0];
        emit(out, n, max, v.x, v.y, v.r, v.g, v.b, 0);
        return;
    }

    size_t edge_count = seg.closed ? seg.count : (seg.count - 1);

    for (size_t e = 0; e < edge_count; e++) {
        size_t a = e, b = (e + 1) % seg.count;
        const PathVertex& va = seg.vertices[a];
        const PathVertex& vb = seg.vertices[b];

        // Corner point(s) at the start vertex of this edge (vertex "a").
        // Only emitted once per vertex -- i.e. on the edge where it is
        // the *start* -- so each corner appears exactly once in the
        // output, not once per adjacent edge.
        uint8_t cpts = cornerPtsAtVertex(seg, cfg, a);
        bool first_point_overall = (e == 0);
        for (uint8_t k = 0; k < cpts; k++) {
            emit(out, n, max, va.x, va.y, va.r, va.g, va.b,
                 (first_point_overall && k == 0 && va.lift) ? 1 : 0);
        }

        // Interior points along the edge (excludes both endpoints --
        // endpoints are corner points of vertex a / vertex b).
        float dx = vb.x - va.x, dy = vb.y - va.y;
        float len = sqrtf(dx * dx + dy * dy);
        uint16_t ipts = edgeInteriorCount(len, cfg);
        // Ease speed into/out of whichever corner is
        // more severe at each end, see shapeEdgeT().
        float easeIn  = cornerSeverity(seg, cfg, a);
        float easeOut = cornerSeverity(seg, cfg, b);

        for (uint16_t k = 1; k <= ipts; k++) {
            float tLin = (float)k / (ipts + 1);
            float t = shapeEdgeT(tLin, easeIn, easeOut);
            float rf = va.r + (float)(vb.r - va.r) * t;
            float gf = va.g + (float)(vb.g - va.g) * t;
            float bf = va.b + (float)(vb.b - va.b) * t;
            emit(out, n, max, va.x + dx * t, va.y + dy * t,
                 (uint8_t)lroundf(rf), (uint8_t)lroundf(gf), (uint8_t)lroundf(bf), 0);
        }
    }

    // Final vertex of an open path needs its own corner point(s) --
    // closed paths already covered the last vertex as the "a" of the
    // wrap-around edge above.
    if (!seg.closed) {
        const PathVertex& vlast = seg.vertices[seg.count - 1];
        emit(out, n, max, vlast.x, vlast.y, vlast.r, vlast.g, vlast.b, 0);
    } else {
        // Closed path: the wrap-around edge's interior points approach
        // vertex 0 but never land on it. A single closing point wasn't
        // enough for the galvo to actually settle there on short/fast
        // edges (residual gap, worse at small Size). Give vertex 0 a
        // second full corner dwell here -- same point count as its
        // frame-start dwell -- so the beam has time to arrive before
        // the trailing closing blank turns the laser off.
        const PathVertex& v0 = seg.vertices[0];
        uint8_t cpts = cornerPtsAtVertex(seg, cfg, 0);
        for (uint8_t k = 0; k < cpts; k++) {
            emit(out, n, max, v0.x, v0.y, v0.r, v0.g, v0.b, 0);
        }
    }
}

// ── Transform stage (Phase 1) ────────────────────────────────────────────
//
// Pipeline order: Primitive -> [Transform] -> Resample -> Corner Dwell ->
// Blanking -> Velocity Clamp -> Acceleration Clamp -> DAC. This is the
// Transform stage: every input vertex is pushed through cfg.transform before
// any scanner-dependent processing (corner detection, length-proportional
// resampling, blank jumps) sees it. Corner severity and edge lengths are
// therefore computed in the transformed frame, which is what a downstream
// resample/velocity stage needs (a rotated square still has 90 deg corners;
// a scaled path has correspondingly scaled edge lengths).
//
// Identity fast-path: when cfg.transform is the identity matrix (the default),
// the original segment pointers are used unchanged -- no copy, no arithmetic,
// byte-identical output to the pre-transform-stage optimizer. Only a non-
// identity matrix triggers the copy into the scratch buffers below.
//
// Scratch sizing: input geometry (vertices before interior/corner fill) is far
// smaller than the emitted point count. The largest caller today declares
// PathVertex[64] / PathSegment[16] (text_renderer.cpp); these scratch sizes
// leave an 8x/4x margin while keeping the static DRAM cost small (~8 KB total).
// applyTransform bounds-checks against both, so an over-large caller degrades
// by dropping trailing segments rather than overflowing.
namespace {
    constexpr size_t kMaxXfVerts = 512;
    constexpr size_t kMaxXfSegs  = 64;
    PathVertex  s_xf_verts[kMaxXfVerts];
    PathSegment s_xf_segs[kMaxXfSegs];
}

// Fills s_xf_segs / s_xf_verts with transformed copies of the input and points
// out_segments at them. Returns the segment count actually written (segments
// beyond the scratch capacity are dropped -- practically unreachable, real
// callers pass well under kMaxXfSegs segments). Only called for non-identity
// transforms; the identity case bypasses this entirely.
static size_t applyTransform(const PathSegment* segments, size_t segment_count,
                              const AffineTransform& xf,
                              const PathSegment** out_segments) {
    size_t seg_out = 0;
    size_t vtx_out = 0;
    for (size_t s = 0; s < segment_count && seg_out < kMaxXfSegs; s++) {
        const PathSegment& src = segments[s];
        if (src.count == 0) {
            s_xf_segs[seg_out] = PathSegment(nullptr, 0, src.closed);
            seg_out++;
            continue;
        }
        if (vtx_out + src.count > kMaxXfVerts) break;  // scratch exhausted

        PathVertex* dst = &s_xf_verts[vtx_out];
        for (size_t i = 0; i < src.count; i++) {
            const PathVertex& v = src.vertices[i];
            float nx, ny;
            xf.apply(v.x, v.y, nx, ny);
            dst[i] = PathVertex(nx, ny, v.r, v.g, v.b, v.lift);
        }
        s_xf_segs[seg_out] = PathSegment(dst, src.count, src.closed);
        vtx_out += src.count;
        seg_out++;
    }
    *out_segments = s_xf_segs;
    return seg_out;
}

// ── Velocity / Acceleration clamp (Phase 4) ──────────────────────────────
//
// Scanner-protection post-pass over the already-emitted lit point stream.
// Runs last in optimize(), after every geometry/density stage, because it
// reasons about the final per-tick motion the galvo will actually be
// commanded to perform -- something only the fully emitted stream exposes.
//
// Two independent, cfg-gated limits, applied in physical order (velocity
// before acceleration -- a velocity subdivision changes the step sizes the
// acceleration pass then sees):
//
//   1. Velocity: no lit-to-lit step may exceed cfg.max_step_units. An
//      over-long step of length L is split into ceil(L/max_step) equal
//      sub-steps by linear interpolation of BOTH position and color, so a
//      colour gradient along the step is preserved. This bounds how far the
//      mirror is asked to travel in one sample, i.e. its peak velocity.
//
//   2. Acceleration: the per-tick growth of step magnitude is capped at
//      cfg.max_accel_units. Where |v[i]| - |v[i-1]| exceeds the limit (the
//      beam is being asked to speed up too hard, typically ramping out of a
//      corner), an interpolated midpoint is inserted to split the velocity
//      jump into two smaller increments. A single forward pass is sufficient
//      in practice; it is not iterated to a fixed point (bounded cost, and
//      the dominant hard-accel case is the isolated corner exit).
//
// Blank runs are EXEMPT from both. A blank jump is an intentional fast
// reposition with the beam off, already shaped by Pillars 2/3 (eased,
// distance-proportional); subdividing it here would fight that and waste
// budget. A step is treated as blank-exempt if either endpoint is blanked.
//
// Bounded output: the pass writes into a PSRAM scratch buffer and stops
// inserting once it reaches the cap, then copies the result back over out[].
// An over-budget frame degrades to partial clamping rather than overflowing
// -- the same graceful-degradation contract as the rest of the optimizer.
// Returns the new point count.
static LaserPoint* s_clamp_scratch = nullptr;   // lazy PSRAM, PATTERN_POINTS_MAX

static inline void lerpPoint(const LaserPoint& a, const LaserPoint& b,
                             float t, LaserPoint& dst) {
    dst.x = (int16_t)lroundf(a.x + (b.x - a.x) * t);
    dst.y = (int16_t)lroundf(a.y + (b.y - a.y) * t);
    dst.r = (uint8_t)lroundf(a.r + (float)(b.r - a.r) * t);
    dst.g = (uint8_t)lroundf(a.g + (float)(b.g - a.g) * t);
    dst.b = (uint8_t)lroundf(a.b + (float)(b.b - a.b) * t);
    dst.blank = b.blank;   // an inserted point on a lit step is lit
}

static size_t clampScannerLimits(LaserPoint* out, size_t n,
                                 const OptimizerConfig& cfg, size_t max_out) {
    if (n < 2) return n;
    const bool doVel   = cfg.vel_clamp_enabled   && cfg.max_step_units  > 0.5f;
    const bool doAccel = cfg.accel_clamp_enabled && cfg.max_accel_units > 0.5f;
    if (!doVel && !doAccel) return n;   // gated off -> byte-identical passthrough

    if (!s_clamp_scratch) {
        s_clamp_scratch = (LaserPoint*)heap_caps_malloc(
            sizeof(LaserPoint) * PATTERN_POINTS_MAX, MALLOC_CAP_SPIRAM);
        if (!s_clamp_scratch) return n;   // no PSRAM -> skip clamp, never crash
    }
    LaserPoint* buf = s_clamp_scratch;
    const size_t cap = std::min(max_out, (size_t)PATTERN_POINTS_MAX);

    // ---- Pass 1: velocity (position-delta) clamp ----
    size_t m = 0;
    buf[m++] = out[0];
    for (size_t i = 1; i < n && m < cap; i++) {
        const LaserPoint& a = out[i - 1];
        const LaserPoint& b = out[i];
        bool blankStep = a.blank || b.blank;
        if (doVel && !blankStep) {
            float dx = (float)b.x - a.x, dy = (float)b.y - a.y;
            float dist = sqrtf(dx * dx + dy * dy);
            if (dist > cfg.max_step_units) {
                uint32_t sub = (uint32_t)ceilf(dist / cfg.max_step_units);
                for (uint32_t k = 1; k < sub && m < cap; k++) {
                    lerpPoint(a, b, (float)k / sub, buf[m++]);
                }
            }
        }
        if (m < cap) buf[m++] = b;
    }

    // ---- Pass 2: acceleration (velocity-delta) clamp ----
    // Operates on the velocity-clamped stream in buf[0..m-1], writing the
    // final stream back into out[]. If the velocity pass already filled the
    // buffer there is no room to insert accel points -> copy back and return.
    if (!doAccel || m >= cap) {
        for (size_t i = 0; i < m; i++) out[i] = buf[i];
        return m;
    }

    auto stepMag = [](const LaserPoint& p, const LaserPoint& q) -> float {
        float dx = (float)q.x - p.x, dy = (float)q.y - p.y;
        return sqrtf(dx * dx + dy * dy);
    };

    size_t o = 0;
    out[o++] = buf[0];
    float prevMag = 0.f;
    for (size_t i = 1; i < m && o < cap; i++) {
        const LaserPoint& a = buf[i - 1];
        const LaserPoint& b = buf[i];
        bool blankStep = a.blank || b.blank;
        float mag = stepMag(a, b);
        if (!blankStep && (mag - prevMag) > cfg.max_accel_units) {
            LaserPoint mid;
            lerpPoint(a, b, 0.5f, mid);
            if (o < cap) out[o++] = mid;
        }
        if (o < cap) out[o++] = b;
        prevMag = mag;
    }
    return o;
}

// Emit stage: for each non-empty segment, blank-jump to its first vertex and
// write the segment's corner + interior points, then a closing blank back to
// the first point of the first segment so the next frame does not open with a
// lit retrace. Split out of optimize() so the pipeline reads as discrete
// stages (transform / plan / clamp / emit) and Phase 2/3 stages have an
// obvious insertion point. cfg is already fully resolved by optimize()
// (density scaled, blank_samples clamped) before this runs.
static size_t emitAllSegments(const PathSegment* segments, size_t segment_count,
                               const OptimizerConfig& cfg,
                               LaserPoint* out, size_t max_out) {
    size_t n = 0;
    for (size_t s = 0; s < segment_count; s++) {
        const PathSegment& seg = segments[s];
        if (seg.count == 0) continue;

        // Blank jump to this segment's first vertex -- distance-
        // proportional + eased (Pillar 2), see emitBlankJump().
        emitBlankJump(out, n, max_out, seg.vertices[0].x, seg.vertices[0].y, cfg);

        emitSegment(seg, cfg, out, n, max_out);
    }

    // Closing blank: return to the very first point with the laser off,
    // so the next frame doesn't start with a lit retrace -- same purpose
    // as the existing per-pattern closing-blank convention in
    // preset_patterns.cpp (ngon()/star()).
    if (n > 0 && segment_count > 0 && segments[0].count > 0) {
        emitBlankJump(out, n, max_out,
                      segments[0].vertices[0].x, segments[0].vertices[0].y, cfg);
    }
    return n;
}

// ── public entry point ───────────────────────────────────────────────────

size_t optimize(const PathSegment* segments, size_t segment_count,
                 LaserPoint* out, size_t max_out,
                 const OptimizerConfig& cfg_in) {
    if (segment_count == 0 || max_out == 0) return 0;

    OptimizerConfig cfg = cfg_in;

    // Stage 0 -- Transform. Non-identity matrices are applied here, before any
    // scanner-dependent stage sees the geometry (see applyTransform above).
    // Identity (the default) uses the caller's segments unchanged, so output
    // stays byte-identical to the pre-transform-stage optimizer.
    if (!cfg.transform.isIdentity()) {
        const PathSegment* xf_segments = nullptr;
        segment_count = applyTransform(segments, segment_count,
                                       cfg.transform, &xf_segments);
        segments = xf_segments;
        if (segment_count == 0) return 0;
    }

    // Budget check: plan at the requested density first, tracking corner
    // and interior sub-totals separately (corner points are fixed
    // overhead -- capped by max_corner_pts, not scaled down; only
    // interior/length-proportional density is reduced to fit budget).
    uint32_t corner_total = 0, interior_total = 0;
    for (size_t s = 0; s < segment_count; s++) {
        uint16_t cp = 0, ip = 0;
        planSegment(segments[s], cfg, &cp, &ip);
        corner_total += cp;
        interior_total += ip;
    }
    uint32_t planned_total = corner_total + interior_total;
    // Reserve room for inter-segment blank jumps (one per segment) plus
    // the final closing blank back to the first point -- that's
    // (segment_count + 1) blank runs total, not segment_count: each
    // segment gets a leading jump-to-start blank (emitted in the loop
    // below), and there is one additional trailing closing-blank after
    // the loop. Forgetting the "+1" here was the original bug -- it
    // under-reserved by exactly one blank_samples-worth of points,
    // which is why the first cut of this budget fix landed at
    // effective_cap + blank_samples instead of effective_cap.
    // Each blank jump costs up to blank_samples (movement) + min_blank_samples
    // (settle at destination) points. The settle run is fixed at min_blank_samples
    // regardless of Stage 1 reduction -- it's not scaled down because it's
    // needed for galvo settle time, not just laser-off time.
    uint32_t blank_overhead = (uint32_t)(cfg.blank_samples + cfg.min_blank_samples) * (segment_count + 1);
    uint32_t needed = planned_total + blank_overhead;

    // Effective cap = the tighter of two independent limits:
    //  - max_out: hard buffer-capacity ceiling (never write past the
    //    caller's array; PATTERN_POINTS_MAX-derived)
    //  - max_pts_per_frame: flicker-budget ceiling (frame rate at 15kpps
    //    must stay above the eye's flicker-fusion threshold -- this is
    //    almost always the tighter constraint in practice, e.g. a single
    //    ngon can fit easily within max_out=2048 while still flickering)
    size_t effective_cap = std::min(max_out, (size_t)cfg.max_pts_per_frame);

    // Stage 1 (MUST run before Stage 2 below): shrink blank_samples FIRST,
    // before touching interior density. Stage 1 triggers in two cases:
    //  (a) fixed overhead (corners + blanking at the default 40 samples)
    //      alone exceeds the cap -- e.g. 30-edge dodecahedron where
    //      blank_overhead is 1240 pts on its own.
    //  (b) fixed overhead fits the cap, but leaves less than
    //      min_interior_pts_per_segment reserved per segment -- e.g. a
    //      6-edge tetrahedron fits at 292/310, but only 18 points (3/edge)
    //      remain for interior density, too sparse to read as a line
    //      rather than a dotted/broken edge.
    //
    // Running this BEFORE the interior-density clamp (Stage 2) is
    // required: Stage 2 computes available_for_interior using
    // blank_overhead, and if that still reflects the default 40
    // samples/run, available_for_interior is driven to ~0 for any shape
    // with more than a few segments -- collapsing every edge to isolated
    // corner dots with no connecting line. THIS WAS THE ACTUAL BUG behind
    // the "still no lines" report: an earlier patch pass left Stage 2
    // (interior scale) physically ABOVE Stage 1 (blank shrink) in this
    // file, so Stage 2 always ran against the inflated blank_overhead
    // regardless of what Stage 1 later computed. Confirmed against real
    // hardware logs (Cube/Octahedron/Tetrahedron: blank-point counts
    // matched simulation exactly, lit-point counts were 5-8x too low --
    // consistent with Stage 2 having scaled pts_per_1000_units down to
    // its 0.1 floor before Stage 1 ever ran).
    //
    // Interim measure pending Pillar 2 (distance-proportional + eased
    // blanking, see design doc Section 5) -- this just scales the
    // existing fixed-count blanking down uniformly, it does not change
    // its shape.
    uint32_t fixed_overhead_at_default_blank = corner_total + blank_overhead;
    uint32_t min_interior_reserve = (uint32_t)cfg.min_interior_pts_per_segment * segment_count;
    bool cap_exceeded   = fixed_overhead_at_default_blank > effective_cap;
    bool reserve_too_low = (effective_cap >= fixed_overhead_at_default_blank) &&
        ((effective_cap - fixed_overhead_at_default_blank) < min_interior_reserve);

    if ((cap_exceeded || reserve_too_low) && cfg.blank_samples > cfg.min_blank_samples) {
        // Go straight to min_blank_samples rather than solving for an
        // intermediate value that exactly fits corner_total+blank_overhead
        // into effective_cap: that calculation can still leave too little
        // (or zero) budget for interior points. Dropping straight to the
        // floor leaves maximum room for Stage 2 to allocate interior
        // density.
        // Reduce toward stage1_blank_target first -- NOT straight to
        // min_blank_samples. Collapsing blank_samples all the way to the
        // floor leaves emitBlankJump() no range to scale into (every jump
        // gets clamped to the same single value), which silently defeats
        // blank_pts_per_1000_units (distance-proportional scaling has
        // nothing left to scale within). Only fall back to the hard floor
        // if even the target doesn't fit the budget.
        uint8_t target = (cfg.stage1_blank_target >= cfg.min_blank_samples)
                              ? cfg.stage1_blank_target : cfg.min_blank_samples;
        cfg.blank_samples = target;
        // Re-check: does the target still leave the fixed overhead over
        // budget? If so, fall back further toward the hard floor.
        uint32_t retry_overhead = corner_total + (uint32_t)(cfg.blank_samples + cfg.min_blank_samples) * (segment_count + 1);
        if (retry_overhead > effective_cap) {
            cfg.blank_samples = cfg.min_blank_samples;
        }
        blank_overhead = (uint32_t)(cfg.blank_samples + cfg.min_blank_samples) * (segment_count + 1);
        needed = planned_total + blank_overhead;
    }

    // Stage 2: scale interior (length-proportional) density against the
    // now-correct (possibly Stage-1-reduced) blank_overhead. Fixed
    // overhead (corners + blanking) is subtracted first; only the
    // remaining budget is divided among interior points -- this is what
    // makes the scale factor self-consistent. Scaling pts_per_1000_units
    // by (available / planned_total) would still overshoot effective_cap
    // by however many points the unscaled corner_total contributes
    // (corner points don't shrink, so dividing by the *combined* total
    // under-corrects).
    if (needed > effective_cap && interior_total > 0) {
        float available_for_interior =
            (float)effective_cap - (float)blank_overhead - (float)corner_total;
        if (available_for_interior < 0.0f) available_for_interior = 0.0f;
        float scale = available_for_interior / (float)interior_total;
        cfg.pts_per_1000_units = std::max(0.1f, cfg.pts_per_1000_units * scale);
    }

    // Emit stage: walk segments, blank-jumping between them, writing corner +
    // interior points per segment. This is where the remaining scanner-protection
    // stages of the Phase-1 pipeline will hook in:
    //   - Resample (Phase 2): active. edgeInteriorCount() switches to
    //     constant spacing (points = length / resample_spacing_units) when
    //     cfg.resample_enabled; feeds planSegment / emitSegment / cornerSeverity.
    //   - Corner Dwell: already active (cornerPtsAtVertex / emitSegment).
    //   - Blanking: already active (emitBlankJump, Pillars 2/3).
    //   - Velocity Clamp / Acceleration Clamp (Phase 4): a post-pass over the
    //     emitted out[0..n-1] that inserts intermediate points where the
    //     per-tick position (velocity) or its delta (acceleration) exceeds the
    //     galvo limit. Implemented in clampScannerLimits(), called below.
    size_t n = emitAllSegments(segments, segment_count, cfg, out, max_out);

    // Final scanner-protection stage. No-op (byte-identical) unless
    // cfg.vel_clamp_enabled / cfg.accel_clamp_enabled are set -- see
    // clampScannerLimits(). Runs on the fully emitted stream so it sees the
    // true per-tick motion; may grow n up to max_out.
    n = clampScannerLimits(out, n, cfg, max_out);
    return n;
}

void emitBlankTo(LaserPoint* out, size_t& n, size_t max,
                 float x1, float y1, const OptimizerConfig& cfg) {
    emitBlankJump(out, n, max, x1, y1, cfg);
}

}  // namespace optimizer