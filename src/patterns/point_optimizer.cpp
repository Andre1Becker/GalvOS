#include "point_optimizer.h"
#include <math.h>
#include <algorithm>
#if defined(ESP_PLATFORM) || defined(ARDUINO)
#include <esp_heap_caps.h>
#include "util/mem_registry.h"
#include "util/log_buffer.h"
#else
// Host-side unit test build (g++ + cfg_stub.h): no ESP heap API. Fall back to
// plain malloc/free so clampScannerLimits() is testable off-target. MALLOC_CAP_*
// is ignored here. mem_registry.cpp isn't part of this build, so stub track()
// out too. log_buffer.cpp needs Arduino.h/esp_log.h to link, so LOG_W is a
// no-op here rather than pulling that in.
#include <cstdlib>
#define MALLOC_CAP_SPIRAM 0
static inline void* heap_caps_malloc(size_t sz, uint32_t) { return malloc(sz); }
static inline void  heap_caps_free(void* p) { free(p); }
namespace memreg { static inline void track(const char*, size_t, bool) {} }
namespace logbuf { enum LogCat : uint8_t { CAT_GALVO = 4 }; }
#define LOG_W(cat, ...) ((void)0)
#endif

namespace optimizer {

static constexpr float PI_F  = 3.14159265358979323846f;
static constexpr float TAU_F = 2.0f * PI_F;

// ── telemetry state ──────────────────────────────────────────────────────
//
// Counters live for the duration of one optimize() call and are folded into
// gLastStats / gFrameStats when it returns (see finishStats()). Only the two
// values that cannot be recovered from the finished buffer are tracked here:
// how many points the pipeline WANTED to write, and how many of those the
// budget cap swallowed. Everything else is measured off the output.

Stats gLastStats;
Stats gFrameStats;

namespace {
    uint32_t sPlanned   = 0;   // attempted writes (emit stage + clamp inserts)
    uint32_t sTruncated = 0;   // attempted writes dropped at the cap
    bool     sRinging   = false;   // ZV shaper was active on at least one jump
}

void Stats::reset() {
    emittedLit = 0; emittedBlank = 0; truncated = 0; plannedTotal = 0;
    jumpCount = 0; jumpDistanceTotal = 0.0f; calls = 0;
    stage2Scale = 1.0f;
    stage1Triggered = false; stage15Triggered = false; ringingActive = false;
}

void Stats::add(const Stats& call) {
    emittedLit        += call.emittedLit;
    emittedBlank      += call.emittedBlank;
    truncated         += call.truncated;
    plannedTotal      += call.plannedTotal;
    jumpCount         += call.jumpCount;
    jumpDistanceTotal += call.jumpDistanceTotal;
    calls             += call.calls;
    // Worst (most aggressive) squeeze any call in the frame needed -- an
    // average would hide the one call that was actually starved.
    if (call.calls > 0 && call.stage2Scale < stage2Scale) stage2Scale = call.stage2Scale;
    stage1Triggered  |= call.stage1Triggered;
    stage15Triggered |= call.stage15Triggered;
    ringingActive    |= call.ringingActive;
}

void resetFrameStats() { gFrameStats.reset(); }

// ── internal helpers ─────────────────────────────────────────────────────

static inline void emit(LaserPoint* out, size_t& n, size_t max,
                         float x, float y, uint8_t r, uint8_t g, uint8_t b,
                         uint8_t blank) {
    sPlanned++;
    if (n >= max) { sTruncated++; return; }
    out[n] = LaserPoint(
        (int16_t)std::max(-32767.f, std::min(32767.f, x)),
        (int16_t)std::max(-32767.f, std::min(32767.f, y)),
        r, g, b, blank);
    n++;
}

// No `n < max` in the loop condition: emit() guards the write itself, and
// running to `count` is what lets it count the points the cap swallowed
// instead of leaving them unaccounted. Same applies to emitBlankJump()'s
// emit loop below. Both are bounded (<=255 / <=kMaxBlankPts).
static inline void emitBlankRun(LaserPoint* out, size_t& n, size_t max,
                                 float x, float y, uint8_t count) {
    for (uint8_t k = 0; k < count; k++)
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

// Point Distribution Modifier -- Jitter (Phase 4, see plans/generic-roaming-
// dahl.md). Deterministic integer hash -> [-1..1], same family as
// modulator_engine.cpp's hashNoise() / spatial_noise.cpp's hash2() but a
// separate copy here on purpose (self-contained .cpp, no new cross-module
// dependency for a purely-cosmetic optimizer knob). Keyed by (edge index,
// point-in-edge index) so the same shape wobbles identically every frame --
// no flicker/shimmer -- while different edges/points get different offsets.
static inline float jitterHash(uint32_t edgeIdx, uint32_t ptIdx) {
    uint32_t h = edgeIdx * 2654435761u + ptIdx * 2246822519u + 0x9E3779B1u;
    h ^= h >> 15; h *= 0x85EBCA6Bu;
    h ^= h >> 13; h *= 0xC2B2AE35u;
    h ^= h >> 16;
    return ((float)h / 4294967295.0f) * 2.0f - 1.0f;
}

// Longest blank jump, in emitted points, this optimizer will ever build.
// Sized above the WebUI's blank_samples clamp (<=100, see web_ui.cpp POST
// /api/optimizer-live) with headroom for the ZV shaper's tail extension in
// planBlankJump(). emitBlankJump() holds two float[kMaxBlankPts] scratch
// arrays on the stack, so this is also a ~1 KB stack budget on the pattern
// task -- do not grow it casually.
static constexpr int kMaxBlankPts = 128;

// Below this jump distance, emitBlankJump() emits nothing at all.
//
// Two consecutive segments that share a vertex -- the normal case for
// wireframe chains, and for the closing blank of a closed path, which
// returns to the point the beam is already sitting on -- otherwise pay a
// full min_blank_samples run for a move of no length: budget spent, a
// laser-off gap punched into geometry that is actually continuous, and the
// mirror commanded nowhere.
//
// 4 DAC units is the threshold because the DAC8562's integral non-linearity
// is specified at +-4 LSB: a commanded move that small is not distinguishable
// from converter error at the galvo, so there is no motion to blank for.
// (For scale: 4 of the 65536 codes spanning +-32767, i.e. 0.006% of full
// scale, ~1 mdeg of optical deflection.)
static constexpr float kMinJumpDistUnits = 4.0f;

// PILLAR 3: Zero-Vibration (ZV) two-impulse input shaper. Cancels galvo
// ringing by convolving the commanded trajectory with two impulses -- A1
// at t=0, A2 at t=Td/2 -- sized so the plant's response to the first
// impulse is destructively cancelled by its response to the second. See
// design doc Section 5.3 for the derivation. With cfg.ringing_comp_enabled
// == false (the default) this reduces to A1=1, A2=0: shaped output is
// then byte-identical to the pre-Pillar-3 trajectory.
struct ZvShaper {
    float A1 = 1.0f, A2 = 0.0f;
    int   shift_pts    = 0;   // point-count delay of the second impulse
    int   min_jump_pts = 0;   // shortest jump, in emitted points, that can
                               // carry this shaper: one move tick plus the
                               // shift_pts+1 already-parked ticks the endpoint
                               // rule needs (see planBlankJump()). 0 when the
                               // shaper is disabled.
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
    s.min_jump_pts = s.shift_pts + 2;
    return s;
}

// Settle ticks carved from the tail of a `count`-point jump -- the ticks that
// sit ON the target instead of ramping toward it. Capped at count/2 so a short
// jump always keeps enough move ticks to decelerate smoothly: without this,
// short jumps get settle==count and move==0, forcing an instantaneous position
// jump that causes overshoot.
static inline int blankSettlePts(int count, const OptimizerConfig& cfg) {
    int settle = (int)cfg.min_blank_samples;
    if (settle > count / 2) settle = count / 2;
    if (settle < 1) settle = 1;
    return settle;
}

// Resolved shape of one blank jump. Single source of truth for the emit path
// (emitBlankJump) and the budget reserve (maxBlankJumpPts) alike.
struct BlankJumpPlan {
    int  total  = 0;   // points emitted
    int  move   = 0;   // leading ticks that ramp; ticks [move,total) sit on the target
    bool shaped = false;
};

// PILLAR 3 endpoint rule. The shaper emits
//     shaped[i] = A1*u[i] + A2*u[i-shift]
// so the LAST sample only lands on the target when u[total-1-shift] is already
// parked there -- i.e. when the settled tail is at least shift_pts+1 ticks
// long. Convolving a trajectory that is merely `count` long instead blends the
// target with a mid-move position and the jump ends short: measured at 200 Hz /
// zeta 0.15 / 30 kpps (shift_pts 76, A1 0.617 / A2 0.383) the last blank point
// sat at 68% of the distance, with the next LIT point being the corner dwell at
// the real vertex -- a bright step exactly where the blank jump was supposed to
// hide the move.
//
// So the trajectory is EXTENDED with further parked ticks until the rule holds,
// bounded by kMaxBlankPts. If it does not fit even then, the shaper is switched
// off for this jump rather than applied partially: an unshaped jump lands on
// target, a partially shaped one does not.
static BlankJumpPlan planBlankJump(int count, const ZvShaper& sh,
                                    const OptimizerConfig& cfg) {
    BlankJumpPlan p;
    if (count < 1)             count = 1;
    if (count > kMaxBlankPts)  count = kMaxBlankPts;
    p.move  = count - blankSettlePts(count, cfg);
    p.total = count;
    if (sh.A2 > 0.0f) {
        int need = p.move + sh.shift_pts + 1;
        if (need <= kMaxBlankPts) {
            if (need > p.total) p.total = need;
            p.shaped = true;
        }
    }
    return p;
}

// Pillar 2: distance-proportional + eased blank jump from (x0,y0) to
// (x1,y1). Sample count scales with jump distance using the same "points
// per 1000 units" convention as interior density (cfg.blank_pts_per_1000_units),
// clamped to [min_blank_samples, blank_samples] -- short jumps (e.g.
// between adjacent wireframe vertices) get fewer samples, long diagonal
// jumps get more, instead of every jump paying the same fixed cost
// regardless of distance.
// The start position is the last point already in the buffer, or -- when this
// call writes into the middle of a frame someone else started (out = o + n, so
// n is 0 here) -- cfg.prevX/prevY, which the caller supplies via
// frameContext(). Only with neither does it fall back to a simple
// stay-at-target run (old emitBlankRun behavior), since there is then genuinely
// no position to ramp from.
//
// PILLAR 3: the resulting move+settle trajectory is buffered locally,
// then re-shaped with the ZV impulse response above (shaped[i] =
// A1*u[i] + A2*u[i-shift]) before being emitted -- actively cancelling
// galvo ringing at the landing point instead of only waiting it out.
// planBlankJump() extends the parked tail so the shaped run still ENDS on
// (x1,y1), and falls back to the unshaped trajectory when the shaper is
// disabled or its tail cannot fit inside kMaxBlankPts.
//
// A jump shorter than kMinJumpDistUnits emits nothing -- see that constant.
static void emitBlankJump(LaserPoint* out, size_t& n, size_t max,
                           float x1, float y1, const OptimizerConfig& cfg) {
    float x0, y0;
    if (n > 0) {
        x0 = out[n - 1].x;
        y0 = out[n - 1].y;
    } else if (cfg.hasPrevPos) {
        x0 = cfg.prevX;
        y0 = cfg.prevY;
    } else {
        emitBlankRun(out, n, max, x1, y1, cfg.blank_samples);
        return;
    }
    float dx = x1 - x0, dy = y1 - y0;
    float dist = sqrtf(dx * dx + dy * dy);
    if (dist < kMinJumpDistUnits) return;   // shared vertex -> nothing to jump

    int count = (int)lroundf((dist / 1000.0f) * cfg.blank_pts_per_1000_units);
    if (count < (int)cfg.min_blank_samples) count = cfg.min_blank_samples;
    if (count > (int)cfg.blank_samples)     count = cfg.blank_samples;

    ZvShaper      shaper = computeZvShaper(cfg);
    BlankJumpPlan plan   = planBlankJump(count, shaper, cfg);
    if (plan.shaped) sRinging = true;

    float ux[kMaxBlankPts], uy[kMaxBlankPts];
    for (int i = 0; i < plan.total; i++) {
        if (i < plan.move) {
            float t = smoothstep((float)(i + 1) / (float)plan.move);
            ux[i] = x0 + dx * t;
            uy[i] = y0 + dy * t;
        } else {
            ux[i] = x1;
            uy[i] = y1;
        }
    }

    for (int i = 0; i < plan.total; i++) {
        float sx = ux[i], sy = uy[i];
        if (plan.shaped) {
            int j = i - shaper.shift_pts;
            float px = (j >= 0) ? ux[j] : x0;
            float py = (j >= 0) ? uy[j] : y0;
            sx = shaper.A1 * ux[i] + shaper.A2 * px;
            sy = shaper.A1 * uy[i] + shaper.A2 * py;
        }
        emit(out, n, max, sx, sy, 0, 0, 0, 1);
    }
}

// Longest run of points a single emitBlankJump() can produce with this config
// -- i.e. exactly what optimize() has to reserve per jump.
//
// A jump's length is distance-driven (blank_pts_per_1000_units) and clamped to
// [min_blank_samples, blank_samples], then possibly extended by the ZV tail
// rule, so the emitted total is the maximum of planBlankJump(count).total over
// count in [lo, hi]:
//   - unshaped, the total IS count, maximal at hi (which is also exactly what
//     the n==0 fallback emitBlankRun() writes);
//   - shaped, the total is move(count)+shift_pts+1. move() is non-decreasing in
//     count and grows by at most 1 per step, so the reachable shaped totals are
//     contiguous and the largest one still under kMaxBlankPts is simply
//     min(need(hi), kMaxBlankPts) -- provided need(lo) fits at all, since
//     otherwise no length in the range can be shaped.
// Distance can only make a real jump SHORTER than this (and a near-zero jump is
// skipped outright, see kMinJumpDistUnits), so this bounds every jump above.
static int maxBlankJumpPts(const OptimizerConfig& cfg) {
    int lo = (int)cfg.min_blank_samples;
    if (lo < 1) lo = 1;
    int hi = (int)cfg.blank_samples;
    if (hi < lo) hi = lo;

    int peak = hi;                                  // unshaped branch
    ZvShaper sh = computeZvShaper(cfg);
    if (sh.A2 > 0.0f) {
        int clo = std::min(lo, kMaxBlankPts);
        int chi = std::min(hi, kMaxBlankPts);
        int need_lo = clo - blankSettlePts(clo, cfg) + sh.shift_pts + 1;
        int need_hi = chi - blankSettlePts(chi, cfg) + sh.shift_pts + 1;
        if (need_lo <= kMaxBlankPts)
            peak = std::max(peak, std::min(need_hi, kMaxBlankPts));
    }
    return peak;
}

RingingStatus ringingStatus(const OptimizerConfig& cfg) {
    RingingStatus rs;
    ZvShaper sh   = computeZvShaper(cfg);
    rs.shift_pts    = sh.shift_pts;
    rs.min_jump_pts = sh.min_jump_pts;

    // Judged at the LONGEST jump this config builds: need() is non-decreasing
    // in jump length, so if the tail fits there it fits at every shorter jump
    // too -- and the longest jump is both the hardest case and the one whose
    // ringing most needs cancelling.
    int hi = std::max((int)cfg.blank_samples, (int)cfg.min_blank_samples);
    rs.active = (sh.A2 > 0.0f) && planBlankJump(hi, sh, cfg).shaped;
    return rs;
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

// Corner severity in [0,1], purely geometric: the exterior angle at vertex i,
// mapped to 0 at/below cfg.corner_angle_deg and to 1 at a full 180 deg
// reversal -- the sharpest case this optimizer handles.
//
// Severity deliberately does NOT model how fast the beam arrives at the
// vertex. Point density already keeps the per-point step at or below the
// nominal step by construction (see edgeInteriorCount()), and the one thing
// that can still produce an oversized arrival step -- Stage 2 crushing
// interior density -- is bounded on the emitted stream in real DAC units by
// clampScannerLimits(), whose acceleration pass limits deceleration into
// every corner dwell. A planning-time speed proxy here would only duplicate
// that, at the cost of corner budget Stage 2 cannot give back.
//
// An open-path endpoint (no neighbor on one side) returns max severity
// (1.0), not 0. A genuinely free end (line()/text-stroke pen-up) just
// gets a few extra dwell samples sitting still -- harmless. But
// wf()/buildWfChains() (Cube/Pyramid/Tetrahedron) splits a polyhedron
// into closed face-loops plus open struts that *share a vertex* with
// those loops: from the strut's own PathSegment the shared vertex looks
// like a free end, so it used to get min_corner_pts regardless of the
// real angle there -- under-dwelling exactly where a strut meets a face,
// visible as a small gap at that corner. Octahedron has no open chains
// (all-closed decomposition) and never showed the gap, confirming this
// path. Treating unknown-neighbor endpoints as worst-case sharp instead
// of softest fixes the shared-vertex case and is a no-op risk for true
// free ends.
static float cornerSeverity(const PathSegment& seg, const OptimizerConfig& cfg,
                             size_t i) {
    bool hasIncoming = seg.closed || i > 0;
    bool hasOutgoing = seg.closed || i + 1 < seg.count;
    if (!hasIncoming || !hasOutgoing) return 1.0f;

    size_t prev = (i == 0) ? seg.count - 1 : i - 1;
    size_t next = (i + 1) % seg.count;

    float angle = exteriorAngle(seg.vertices[prev].x, seg.vertices[prev].y,
                                 seg.vertices[i].x,    seg.vertices[i].y,
                                 seg.vertices[next].x, seg.vertices[next].y);
    float angle_deg = angle * (180.0f / PI_F);
    float span = 180.0f - cfg.corner_angle_deg;
    float angleT = (angle_deg <= cfg.corner_angle_deg) ? 0.0f :
                   (span > 0.01f ? (angle_deg - cfg.corner_angle_deg) / span : 1.0f);
    return std::max(0.0f, std::min(1.0f, angleT));
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
//
// The return value is exactly out_corner_pts + out_interior_pts. Do not
// re-introduce a per-segment point floor here: a floor cannot be scaled by
// Stage 2, so on a many-edge shape it costs budget that emitAllSegments()
// then has to take out of the shape itself (truncation mid-draw). The one
// case a floor would nominally guard -- a single long lit step once Stage 2
// has crushed interior density -- belongs to max_step_units /
// clampScannerLimits(), which measures actual DAC units per tick on the
// emitted stream and stays inside effective_cap.
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
            float px = va.x + dx * t, py = va.y + dy * t;

            // Jitter (Phase 4): perpendicular offset, interior points only --
            // corner points stay exact so the shape's vertices remain
            // recognizable. Applied here, after planning/severity/resample
            // have already fixed point counts, so it's a pure emit-time
            // perturbation (see config.h's OPT_DEFAULT_JITTER_* comment).
            if (cfg.jitter_enabled && cfg.jitter_amount_units > 0.01f && len > 1e-3f) {
                float nx = -dy / len, ny = dx / len;   // unit perpendicular
                float j = jitterHash((uint32_t)e, (uint32_t)k) * cfg.jitter_amount_units;
                px += nx * j;
                py += ny * j;
            }

            float rf = va.r + (float)(vb.r - va.r) * t;
            float gf = va.g + (float)(vb.g - va.g) * t;
            float bf = va.b + (float)(vb.b - va.b) * t;
            emit(out, n, max, px, py,
                 (uint8_t)lroundf(rf), (uint8_t)lroundf(gf), (uint8_t)lroundf(bf), 0);
        }
    }

    // Final vertex of an open path needs its own corner point(s) --
    // closed paths already covered the last vertex as the "a" of the
    // wrap-around edge above. Must match planSegment(), which budgets
    // cornerPtsAtVertex() (up to max_corner_pts, since an open endpoint
    // is always worst-case severity) for this vertex too -- emitting
    // only one point here starved the far end of every open line
    // (line() calls, open PathSegment strokes) of its planned dwell,
    // most visible on short segments where that endpoint dwell is a
    // large share of the total lit points (e.g. DNA-helix rungs near
    // the strand crossings): the line looked cut short/dim instead of
    // fully drawn.
    if (!seg.closed) {
        const PathVertex& vlast = seg.vertices[seg.count - 1];
        uint8_t cpts = cornerPtsAtVertex(seg, cfg, seg.count - 1);
        for (uint8_t k = 0; k < cpts; k++) {
            emit(out, n, max, vlast.x, vlast.y, vlast.r, vlast.g, vlast.b, 0);
        }
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
// Jitter -> Blanking -> Velocity Clamp -> Acceleration Clamp -> DAC. This is the
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
// smaller than the emitted point count. The largest caller by TOTAL vertices
// is paint::generate() -- PAINT_STROKES_MAX(12) * PAINT_VERTS_PER_STROKE(96)
// = 1152 vertices across up to 12 segments; the largest by a SINGLE segment
// is calib_patterns.cpp's cam_spiral (512 vertices, 1 segment). text_renderer
// declares PathVertex[64]/PathSegment[16] per call, smaller than either.
// kMaxXfVerts previously sat at exactly 512 -- zero headroom for cam_spiral
// and well under paint's 1152, so a non-identity transform (e.g. rotation)
// silently dropped trailing paint strokes via the `break` below. Sized to
// 1280 for real margin over both; applyTransform bounds-checks against both
// caps, so an over-large caller (still practically unreachable) degrades by
// dropping trailing segments rather than overflowing, and now logs once
// instead of doing that silently.
namespace {
    constexpr size_t kMaxXfVerts = 1280;
    constexpr size_t kMaxXfSegs  = 64;
    // Lazy PSRAM (~16.5 KB total) -- was static DRAM .bss. Fully overwritten
    // before use on every applyTransform() call. Uses the same
    // heap_caps_malloc path as s_clamp_scratch so the host-side unit-test
    // build (plain-malloc shim above) keeps working.
    PathVertex*  s_xf_verts = nullptr;
    PathSegment* s_xf_segs  = nullptr;
}

// Fills s_xf_segs / s_xf_verts with transformed copies of the input and points
// out_segments at them. Returns the segment count actually written (segments
// beyond the scratch capacity are dropped -- practically unreachable, real
// callers pass well under kMaxXfSegs segments). Only called for non-identity
// transforms; the identity case bypasses this entirely.
static size_t applyTransform(const PathSegment* segments, size_t segment_count,
                              const AffineTransform& xf,
                              const PathSegment** out_segments) {
    if (!s_xf_verts) {
        s_xf_verts = (PathVertex*)heap_caps_malloc(
            kMaxXfVerts * sizeof(PathVertex), MALLOC_CAP_SPIRAM);
        s_xf_segs = (PathSegment*)heap_caps_malloc(
            kMaxXfSegs * sizeof(PathSegment), MALLOC_CAP_SPIRAM);
        if (!s_xf_verts || !s_xf_segs) {
            // No PSRAM -> pass the input through untransformed, never crash.
            // Free+null BOTH pointers, not just s_xf_verts: leaving the other
            // one allocated-but-live means the next call's `if (!s_xf_verts)`
            // guard skips it and reallocates on top -- a leak of one block
            // per call for the rest of the process.
            heap_caps_free(s_xf_verts);
            heap_caps_free(s_xf_segs);
            s_xf_verts = nullptr;
            s_xf_segs  = nullptr;
            *out_segments = segments;
            return segment_count;
        }
        memreg::track("Optimizer XForm Scratch",
                      kMaxXfVerts * sizeof(PathVertex) +
                      kMaxXfSegs * sizeof(PathSegment), true);
    }
    size_t seg_out = 0;
    size_t vtx_out = 0;
    for (size_t s = 0; s < segment_count && seg_out < kMaxXfSegs; s++) {
        const PathSegment& src = segments[s];
        if (src.count == 0) {
            s_xf_segs[seg_out] = PathSegment(nullptr, 0, src.closed);
            seg_out++;
            continue;
        }
        if (vtx_out + src.count > kMaxXfVerts) {
            // Scratch exhausted -- trailing segments dropped. One-time log
            // (not per-frame spam at 25 fps) so an over-large caller shows up
            // instead of silently losing geometry, same failure mode the
            // 512-vertex cap above used to hit on every cam_spiral call.
            static bool warned = false;
            if (!warned) {
                warned = true;
                LOG_W(logbuf::CAT_GALVO,
                      "applyTransform: scratch exhausted at %u/%u verts, "
                      "%u segments dropped", (unsigned)vtx_out,
                      (unsigned)kMaxXfVerts,
                      (unsigned)(segment_count - s));
            }
            break;
        }

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
//   2. Acceleration: the per-tick velocity CHANGE is capped at
//      cfg.max_accel_units, measured vectorially as ||v[i] - v[i-1]|| -- not
//      the scalar magnitude difference |v[i]|-|v[i-1]|, which only ever
//      catches speeding up. A mirror decelerating into a corner dwell (full
//      cruise speed one tick, zero the next) loads the same as one
//      accelerating out of it, and a direction reversal at constant speed
//      is invisible to a magnitude-only comparison entirely.
//
//      Deceleration cannot always be fixed by subdividing the offending
//      step itself: a corner dwell's own step is zero-length (repeated
//      points at the vertex), so bisecting IT is a no-op -- the ramp has to
//      live in the segment BEFORE the dwell. This pass therefore peeks one
//      step ahead before committing a point, and picks the smallest
//      power-of-two subdivision of the CURRENT step that keeps both the
//      arrival transition (prevStep -> first sub-step) and the departure
//      transition (last sub-step -> next step) within max_accel_units --
//      iterated, not a single fixed bisection (one midpoint halves the
//      violation but does not by itself guarantee the bound).
//
//      After an insertion, the velocity actually carried forward into the
//      next comparison is the LAST emitted sub-step, not the original
//      unsplit step -- a halved step delivers half the velocity change.
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

size_t clampScannerLimits(LaserPoint* out, size_t n,
                          const OptimizerConfig& cfg, size_t max_out) {
    if (n < 2) return n;
    const bool doVel   = cfg.vel_clamp_enabled   && cfg.max_step_units  > 0.5f;
    const bool doAccel = cfg.accel_clamp_enabled && cfg.max_accel_units > 0.5f;
    if (!doVel && !doAccel) return n;   // gated off -> byte-identical passthrough

    if (!s_clamp_scratch) {
        s_clamp_scratch = (LaserPoint*)heap_caps_malloc(
            sizeof(LaserPoint) * PATTERN_POINTS_MAX, MALLOC_CAP_SPIRAM);
        if (!s_clamp_scratch) return n;   // no PSRAM -> skip clamp, never crash
        memreg::track("Point Optimizer Scratch", sizeof(LaserPoint) * PATTERN_POINTS_MAX, true);
    }
    LaserPoint* buf = s_clamp_scratch;
    const size_t cap = std::min(max_out, (size_t)PATTERN_POINTS_MAX);

    // ---- Pass 1: velocity (position-delta) clamp ----
    //
    // Telemetry note: points carried over from out[] were already counted as
    // planned by the emit stage, so only the interpolated INSERTIONS add to
    // sPlanned here. A carried-over point that no longer fits the cap is a
    // drop like any other, and is counted as one.
    size_t m = 0;
    if (m < cap) buf[m++] = out[0]; else sTruncated++;
    for (size_t i = 1; i < n; i++) {
        const LaserPoint& a = out[i - 1];
        const LaserPoint& b = out[i];
        bool blankStep = a.blank || b.blank;
        if (doVel && !blankStep) {
            float dx = (float)b.x - a.x, dy = (float)b.y - a.y;
            float dist = sqrtf(dx * dx + dy * dy);
            if (dist > cfg.max_step_units) {
                uint32_t sub  = (uint32_t)ceilf(dist / cfg.max_step_units);
                uint32_t want = sub - 1;
                // Written in one bounded batch rather than per-point-with-a-
                // cap-test, so an exhausted budget costs O(0) instead of one
                // loop iteration per point it cannot write.
                size_t   room = (m < cap) ? (cap - m) : 0;
                uint32_t wrote = (uint32_t)std::min((size_t)want, room);
                for (uint32_t k = 1; k <= wrote; k++)
                    lerpPoint(a, b, (float)k / sub, buf[m++]);
                sPlanned   += want;
                sTruncated += want - wrote;
            }
        }
        if (m < cap) buf[m++] = b; else sTruncated++;
    }

    // ---- Pass 2: acceleration (velocity-delta) clamp ----
    // Operates on the velocity-clamped stream in buf[0..m-1], writing the
    // final stream back into out[]. If the velocity pass already filled the
    // buffer there is no room to insert accel points -> copy back and return.
    if (!doAccel || m >= cap) {
        for (size_t i = 0; i < m; i++) out[i] = buf[i];
        return m;
    }

    // Doubling cap on the per-step subdivision search below. 1024 sub-steps
    // is already far beyond anything a real corner needs (the search exits
    // as soon as both transitions fit); it exists only so a pathological
    // config (e.g. max_step_units set far above 2*max_accel_units, leaving
    // no achievable subdivision) cannot spin forever -- the loop gives up
    // and emits the best it found, same graceful-degradation contract as
    // the rest of this pass.
    constexpr uint32_t kMaxAccelSubdiv = 1024;

    size_t o = 0;
    if (o < cap) out[o++] = buf[0]; else sTruncated++;
    float prevDx = 0.f, prevDy = 0.f;   // velocity carried INTO buf[0]: none
    for (size_t i = 1; i < m; i++) {
        const LaserPoint& a = buf[i - 1];
        const LaserPoint& b = buf[i];
        bool blankStep = a.blank || b.blank;
        float dx = (float)b.x - a.x, dy = (float)b.y - a.y;

        if (blankStep) {
            // Exempt, same as pass 1 -- a blank jump is its own intentional
            // reposition, already shaped by Pillars 2/3. Still tracked as
            // the carried-forward velocity (matches this pass's pre-P11
            // behaviour) since the accel test itself exempts any triple
            // touching a blank point, so what it carries across one is moot.
            if (o < cap) out[o++] = b; else sTruncated++;
            prevDx = dx; prevDy = dy;
            continue;
        }

        // One-step lookahead: what velocity must this segment hand off to?
        // Absent (end of stream, or the next step is itself blank/exempt),
        // there is no departure constraint -- only the arrival one applies.
        bool  havePeek = (i + 1 < m) && !buf[i + 1].blank;
        float nextDx = 0.f, nextDy = 0.f;
        if (havePeek) {
            nextDx = (float)buf[i + 1].x - b.x;
            nextDy = (float)buf[i + 1].y - b.y;
        }

        uint32_t sub = 1;
        for (; sub < kMaxAccelSubdiv; sub *= 2) {
            float sdx = dx / sub, sdy = dy / sub;
            float adx = sdx - prevDx, ady = sdy - prevDy;
            bool arrivalOk = sqrtf(adx * adx + ady * ady) <= cfg.max_accel_units;
            bool departureOk = true;
            if (havePeek) {
                float ddx = nextDx - sdx, ddy = nextDy - sdy;
                departureOk = sqrtf(ddx * ddx + ddy * ddy) <= cfg.max_accel_units;
            }
            if (arrivalOk && departureOk) break;
        }

        if (sub > 1) {
            uint32_t want  = sub - 1;
            size_t   room  = (o < cap) ? (cap - o) : 0;
            uint32_t wrote = (uint32_t)std::min((size_t)want, room);
            for (uint32_t k = 1; k <= wrote; k++) {
                LaserPoint p;
                lerpPoint(a, b, (float)k / sub, p);
                if (o < cap) out[o++] = p; else sTruncated++;
            }
            sPlanned   += want;
            sTruncated += want - wrote;
        }

        if (o < cap) out[o++] = b; else sTruncated++;

        // Velocity actually delivered into b is the LAST sub-step, not the
        // unsplit (dx,dy) -- a subdivided step hands off less speed.
        prevDx = dx / sub;
        prevDy = dy / sub;
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

// Walks the finished output and fills in everything the emitted stream can be
// asked about directly: lit/blank split, blank-run count, and the distance
// travelled with the beam off. A blank run's first step counts too -- the
// galvo is already moving off the last lit point when the beam goes out.
static void measureEmitted(const LaserPoint* out, size_t n, Stats& st) {
    bool inRun = false;
    for (size_t k = 0; k < n; k++) {
        if (out[k].blank) {
            st.emittedBlank++;
            if (!inRun) { st.jumpCount++; inRun = true; }
            if (k > 0) {
                float dx = (float)out[k].x - (float)out[k - 1].x;
                float dy = (float)out[k].y - (float)out[k - 1].y;
                st.jumpDistanceTotal += sqrtf(dx * dx + dy * dy);
            }
        } else {
            st.emittedLit++;
            inRun = false;
        }
    }
}

// Closes out one optimize() call: measures the emitted stream, folds in the
// planned/truncated counters, and publishes to gLastStats + gFrameStats.
static size_t finishStats(Stats& st, const LaserPoint* out, size_t n) {
    measureEmitted(out, n, st);
    st.plannedTotal  = sPlanned;
    st.truncated     = sTruncated;
    st.ringingActive = sRinging;
    st.calls         = 1;
    gLastStats = st;
    gFrameStats.add(st);
    return n;
}

size_t optimize(const PathSegment* segments, size_t segment_count,
                 LaserPoint* out, size_t max_out,
                 const OptimizerConfig& cfg_in) {
    Stats st;
    sPlanned = 0; sTruncated = 0; sRinging = false;

    if (segment_count == 0 || max_out == 0) return finishStats(st, out, 0);

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
        if (segment_count == 0) return finishStats(st, out, 0);
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
    //
    // Per-jump cost is maxBlankJumpPts() -- the one place that knows how long
    // a jump can actually get, including the ZV shaper's tail extension. This
    // used to read (blank_samples + min_blank_samples), which described a
    // settle run added ON TOP of the move; emitBlankJump() has always carved
    // its settle ticks OUT of the same count instead, so the old term
    // over-reserved min_blank_samples per jump on every frame and pushed
    // Stages 1/2 into squeezing shapes that in fact fit.
    uint32_t blank_overhead = (uint32_t)maxBlankJumpPts(cfg) * (segment_count + 1);
    uint32_t needed = planned_total + blank_overhead;

    // Effective cap = the tighter of two independent limits:
    //  - max_out: hard buffer-capacity ceiling (never write past the
    //    caller's array; PATTERN_POINTS_MAX-derived)
    //  - max_pts_per_frame: flicker-budget ceiling (frame rate at 15kpps
    //    must stay above the eye's flicker-fusion threshold -- this is
    //    almost always the tighter constraint in practice, e.g. a single
    //    ngon can fit easily within max_out=2048 while still flickering)
    //
    // max_pts_per_frame is a PER-CALL cap, which is the whole budget only when
    // the frame is one call. A caller that renders its sub-shapes one call each
    // passes the frame's actual remainder in frameBudgetRemaining instead (see
    // frameContext() in the header); it is derived from max_pts_per_frame, so
    // it replaces rather than joins it.
    uint16_t frame_cap = (cfg.frameBudgetRemaining > 0) ? cfg.frameBudgetRemaining
                                                        : cfg.max_pts_per_frame;
    size_t effective_cap = std::min(max_out, (size_t)frame_cap);

    // Stage 1 (MUST run before Stage 2 below): shrink blank_samples FIRST,
    // before touching interior density. Stage 1 triggers in two cases:
    //  (a) fixed overhead (corners + blanking at the configured
    //      blank_samples) alone exceeds the cap -- e.g. a 30-edge
    //      dodecahedron, whose blank_overhead is hundreds of points on its
    //      own, more still once the ZV tail extension is in play.
    //  (b) fixed overhead fits the cap, but leaves less than
    //      min_interior_pts_per_segment reserved per segment -- e.g. a
    //      6-edge tetrahedron fits at 292/310, but only 18 points (3/edge)
    //      remain for interior density, too sparse to read as a line
    //      rather than a dotted/broken edge.
    //
    // Running this BEFORE the interior-density clamp (Stage 2) is
    // required: Stage 2 computes available_for_interior using
    // blank_overhead, and if that still reflects the un-reduced
    // blank_samples, available_for_interior is driven to ~0 for any shape
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
        st.stage1Triggered = true;
        // Re-check: does the target still leave the fixed overhead over
        // budget? If so, fall back further toward the hard floor. Same
        // per-jump term as above -- maxBlankJumpPts() is the single source.
        uint32_t retry_overhead =
            corner_total + (uint32_t)maxBlankJumpPts(cfg) * (segment_count + 1);
        if (retry_overhead > effective_cap) {
            cfg.blank_samples = cfg.min_blank_samples;
        }
        blank_overhead = (uint32_t)maxBlankJumpPts(cfg) * (segment_count + 1);
        needed = planned_total + blank_overhead;
    }

    // Stage 1.5: corner dwell is fixed overhead -- Stage 2 below only ever
    // scales interior (length-proportional) density, never corner_total.
    // That's fine as long as corner_total + blank_overhead fits under
    // effective_cap; Stage 1 above already drove blank_samples to its
    // floor trying to arrange that. But if corner_total ALONE (many
    // vertices, e.g. a dense sampled curve or a many-sided polygon) still
    // exceeds what's left after the floor blank overhead, nothing upstream
    // can save it: emitAllSegments()'s hard per-point cap (see emit()) then
    // truncates mid-shape once spending runs out mid-corner-loop. For a
    // CLOSED path that always sacrifices whatever is written last -- the
    // final edge, the closing dwell at vertex 0 -- i.e. the loop silently
    // stops short of reconnecting, a real gap the eye reads as "not
    // closed" (as opposed to merely a coarser corner). Observed on
    // many-vertex closed shapes (Octagon and up, dense Lissajous/rose
    // curves) once max_pts_per_frame is tuned low enough that even
    // min_corner_pts per vertex doesn't fit.
    //
    // Scale min_corner_pts/max_corner_pts down together (floor 1 pt/vertex
    // -- the minimum needed to actually visit every vertex) so corner_total
    // itself shrinks to fit, then re-plan with the new corner budget. This
    // trades corner sharpness/dwell for the one thing that must never be
    // sacrificed: the shape actually closing.
    if (corner_total + blank_overhead > effective_cap && corner_total > 0) {
        st.stage15Triggered = true;
        float available_for_corners = (float)effective_cap - (float)blank_overhead;
        if (available_for_corners < 0.0f) available_for_corners = 0.0f;
        float corner_scale = available_for_corners / (float)corner_total;

        uint8_t new_min = (uint8_t)std::max(1.0f, floorf(cfg.min_corner_pts * corner_scale));
        uint8_t new_max = (uint8_t)std::max((float)new_min, floorf(cfg.max_corner_pts * corner_scale));
        cfg.min_corner_pts = new_min;
        cfg.max_corner_pts = new_max;

        corner_total = 0; interior_total = 0;
        for (size_t s = 0; s < segment_count; s++) {
            uint16_t cp = 0, ip = 0;
            planSegment(segments[s], cfg, &cp, &ip);
            corner_total += cp;
            interior_total += ip;
        }
        planned_total = corner_total + interior_total;
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
        if (cfg.resample_enabled && cfg.resample_spacing_units > 0.01f) {
            // Resample mode: edgeInteriorCount() ignores pts_per_1000_units
            // entirely and derives its count from resample_spacing_units
            // instead (points = length/spacing). Scaling pts_per_1000_units
            // below this branch is a no-op against the actual emission path
            // -- the fixed-spacing point count never shrinks, so total
            // output keeps growing with pattern size until it silently
            // overruns max_out (buffer truncation) instead of respecting
            // max_pts_per_frame. Shrink the count by *growing* the spacing
            // instead -- count scales as 1/spacing, so this reaches the
            // same effective_cap target the ppu branch reaches for the
            // non-resample case.
            float safe_scale = std::max(scale, 0.02f);
            cfg.resample_spacing_units = cfg.resample_spacing_units / safe_scale;
            st.stage2Scale = safe_scale;
        } else {
            float before = cfg.pts_per_1000_units;
            cfg.pts_per_1000_units = std::max(0.1f, cfg.pts_per_1000_units * scale);
            // Report the factor that was really applied, floor included --
            // a scale of 0.01 that the 0.1 ppu floor turned into 0.4 would
            // otherwise read as a far tighter squeeze than actually happened.
            st.stage2Scale = (before > 1e-4f) ? (cfg.pts_per_1000_units / before) : scale;
        }
    }

    // Emit stage: walk segments, blank-jumping between them, writing corner +
    // interior points per segment. This is where the remaining scanner-protection
    // stages of the Phase-1 pipeline will hook in:
    //   - Resample (Phase 2): active. edgeInteriorCount() switches to
    //     constant spacing (points = length / resample_spacing_units) when
    //     cfg.resample_enabled; feeds planSegment / emitSegment.
    //   - Corner Dwell: already active (cornerPtsAtVertex / emitSegment).
    //   - Jitter (Phase 4): active. Deterministic perpendicular offset on
    //     interior points, applied inline in emitSegment()'s interior loop.
    //   - Blanking: already active (emitBlankJump, Pillars 2/3).
    //   - Velocity Clamp / Acceleration Clamp (Phase 4): a post-pass over the
    //     emitted out[0..n-1] that inserts intermediate points where the
    //     per-tick position (velocity) or its delta (acceleration) exceeds the
    //     galvo limit. Implemented in clampScannerLimits(), called below.
    // Emit bounded by effective_cap, NOT max_out. Stage 2 computes a single
    // global scale factor, but edgeInteriorCount() then lroundf()s each edge
    // independently -- with many edges those round-ups accumulate and the
    // emitted total can exceed the plan, and therefore max_pts_per_frame.
    // Measured on a 480-vertex circle at cap=1300: 1464 points, 12.6% over.
    // The overshoot grows with edge count, so it stayed invisible on the
    // low-vertex shapes (ngon/star, ~24 vertices) that were migrated first and
    // only appeared once dense sampled curves started using the optimizer.
    //
    // Passing effective_cap here makes max_pts_per_frame the hard guarantee it
    // is documented to be: emitAllSegments() already stops writing at its
    // max_out argument, so the cap is enforced by construction rather than by
    // hoping the plan was exact.
    size_t n = emitAllSegments(segments, segment_count, cfg, out, effective_cap);

    // Final scanner-protection stage. No-op (byte-identical) unless
    // cfg.vel_clamp_enabled / cfg.accel_clamp_enabled are set -- see
    // clampScannerLimits(). Runs on the fully emitted stream so it sees the
    // true per-tick motion; may grow n, bounded by the same effective cap.
    n = clampScannerLimits(out, n, cfg, effective_cap);
    return finishStats(st, out, n);
}

void emitBlankTo(LaserPoint* out, size_t& n, size_t max,
                 float x1, float y1, const OptimizerConfig& cfg) {
    emitBlankJump(out, n, max, x1, y1, cfg);
}

}  // namespace optimizer