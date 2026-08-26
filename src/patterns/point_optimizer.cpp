#include "point_optimizer.h"
#include "warpGrid.h"
#include "brightnessField.h"
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
Stats gFrameStatsSnapshot;

namespace {
    uint32_t sPlanned   = 0;   // attempted writes (emit stage + clamp inserts)
    uint32_t sTruncated = 0;   // attempted writes dropped at the cap
    bool     sRinging   = false;   // ZV shaper was active on at least one jump
}

void Stats::reset() {
    emittedLit = 0; emittedBlank = 0; truncated = 0; plannedTotal = 0;
    jumpCount = 0; jumpDistanceTotal = 0.0f; calls = 0;
    stage2Scale = 1.0f;
    stage1Triggered = false; stage1BlankSamples = 0; stage1BlankClamped = false;
    stage15Triggered = false; ringingActive = false;
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
    // Same "worst call wins" rule as stage2Scale above: 0 means the call did
    // not run Stage 1 at all, so it must not pull the frame's value down.
    if (call.stage1BlankSamples > 0 &&
        (stage1BlankSamples == 0 || call.stage1BlankSamples < stage1BlankSamples))
        stage1BlankSamples = call.stage1BlankSamples;
    stage1BlankClamped |= call.stage1BlankClamped;
    stage15Triggered |= call.stage15Triggered;
    ringingActive    |= call.ringingActive;
}

// Publish before clearing: gFrameStats at this point is the PREVIOUS frame's
// fully-accumulated total (nothing writes it between the last frame's final
// add() and this call, since both run sequentially on Core 1), so the
// snapshot assignment always sees a complete record -- never the zero this
// function is about to write. See gFrameStatsSnapshot's doc comment.
void resetFrameStats() { gFrameStatsSnapshot = gFrameStats; gFrameStats.reset(); }

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

// Squared Euclidean distance -- every use below only compares distances
// against each other, so the sqrtf() is skipped.
static inline float distSq(float x0, float y0, float x1, float y1) {
    float dx = x1 - x0, dy = y1 - y0;
    return dx * dx + dy * dy;
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

// Upper bound on INPUT geometry -- vertices as the caller hands them in,
// before any corner/interior fill. Two per-call scratch buffers are sized by
// it: the transform stage's vertex copy (s_xf_verts, see the "Scratch sizing"
// note above applyTransform() for where 1280 comes from) and the geometry
// cache below. Declared here rather than next to either because both need it.
// Both degrade gracefully past the bound -- the transform drops trailing
// segments and logs, the cache falls back to computing.
static constexpr size_t kMaxXfVerts = 1280;
static constexpr size_t kMaxXfSegs  = 64;

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

// Requested sample count for a jump of this length, before the ZV tail rule
// (planBlankJump) turns it into an emitted total. Split out of emitBlankJump()
// because Stage 2's blank-jump plan prices the same jumps without emitting
// them -- one definition, the same rule for both, in the spirit of
// walkSegment().
static inline int blankCountForDistance(float dist, const OptimizerConfig& cfg) {
    if (dist < 0.0f) dist = 0.0f;
    int count = (int)lroundf((dist / 1000.0f) * cfg.blank_pts_per_1000_units);
    if (count < (int)cfg.min_blank_samples) count = cfg.min_blank_samples;
    if (count > (int)cfg.blank_samples)     count = cfg.blank_samples;
    return count;
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
// A1*u[i] + A2*u[i-shift]) before being handed to `emitPoint` -- actively
// cancelling galvo ringing at the landing point instead of only waiting it
// out. Shared by emitBlankJump() (Pillar 2/3, `plan.total` derived from
// distance) and reshapeBlankRun() (P22, `plan.total` fixed by the caller) --
// the shaping math must not fork into two copies. Callers choose what
// happens to each point: emitBlankJump() appends via emit() (budget
// accounting), reshapeBlankRun() overwrites an existing slice in place (no
// accounting -- it runs outside optimize() entirely).
template <typename Emit>
static void buildBlankTrajectory(float x0, float y0, float x1, float y1,
                                  const BlankJumpPlan& plan, const ZvShaper& shaper,
                                  Emit emitPoint) {
    float ux[kMaxBlankPts], uy[kMaxBlankPts];
    for (int i = 0; i < plan.total; i++) {
        if (i < plan.move) {
            float t = smoothstep((float)(i + 1) / (float)plan.move);
            ux[i] = x0 + (x1 - x0) * t;
            uy[i] = y0 + (y1 - y0) * t;
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
        emitPoint(i, sx, sy);
    }
}

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

    int           count  = blankCountForDistance(dist, cfg);
    ZvShaper      shaper = computeZvShaper(cfg);
    BlankJumpPlan plan   = planBlankJump(count, shaper, cfg);
    if (plan.shaped) sRinging = true;

    buildBlankTrajectory(x0, y0, x1, y1, plan, shaper,
        [&](int /*i*/, float sx, float sy) {
            emit(out, n, max, sx, sy, 0, 0, 0, 1);
        });
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

// ── P22: ILDA blank-run reshaping ────────────────────────────────────────
//
// ILDA frames are pre-rendered LaserPoint streams read straight from a .ild
// file -- they never go through optimize(), so Pillar 2/3's smoothstep-ease
// and ZV-shape trajectory never reaches them. reshapeBlankRun() re-times an
// already-blank-flagged run of points IN PLACE, reusing that exact math via
// buildBlankTrajectory() above, WITHOUT changing the run's own point count:
// a different count changes how long the jump is actually on screen relative
// to how the .ild file's author timed it (see DECISIONS.md Session Q/P22).
//
// Endpoints are the run's OWN recorded coordinates -- out[i0] and out[i1-1]
// -- never a neighboring frame's position: ILDA keeps no cross-frame position
// state (unlike optimize()'s hasPrevPos, which exists only for multiple
// optimize() calls building one PRESET frame inside a single render pass).
//
// No-op (out[i0..i1) is left exactly as it was) if count < 2 -- nothing to
// ramp between two points, or a single point can't express a trajectory at
// all -- or count > kMaxBlankPts, the trajectory scratch's stack budget (see
// buildBlankTrajectory()'s ux/uy arrays). Whether real ILDA content ever
// produces a run that long is unmeasured -- see STATE.md's open item.
void reshapeBlankRun(LaserPoint* out, size_t i0, size_t i1, const OptimizerConfig& cfg) {
    if (i1 < i0) return;
    int count = (int)(i1 - i0);
    if (count < 2 || count > kMaxBlankPts) return;

    float x0 = (float)out[i0].x,     y0 = (float)out[i0].y;
    float x1 = (float)out[i1 - 1].x, y1 = (float)out[i1 - 1].y;

    ZvShaper      shaper = computeZvShaper(cfg);
    BlankJumpPlan plan;
    plan.total = count;
    if (shaper.A2 > 0.0f && count >= shaper.min_jump_pts) {
        // planBlankJump() reacts to a too-short jump by EXTENDING plan.total
        // (up to kMaxBlankPts) -- not available here, since the caller's
        // count is fixed. So instead the settle tail is grown to exactly
        // what the endpoint rule needs (shift_pts+1 parked ticks): with
        // count >= min_jump_pts (== shift_pts+2, computeZvShaper()'s own
        // floor) that always leaves >=1 tick for the ramp. This is the same
        // "report inactive rather than half-apply" rule P4 established for
        // Pillar 3 -- reused via the `count >= min_jump_pts` gate below,
        // not reinvented.
        plan.move   = count - (shaper.shift_pts + 1);
        plan.shaped = true;
    } else {
        plan.move   = count - blankSettlePts(count, cfg);
        plan.shaped = false;
    }

    buildBlankTrajectory(x0, y0, x1, y1, plan, shaper,
        [&](int i, float sx, float sy) {
            sx = std::max(-32767.f, std::min(32767.f, sx));
            sy = std::max(-32767.f, std::min(32767.f, sy));
            out[i0 + (size_t)i] = LaserPoint((int16_t)sx, (int16_t)sy, 0, 0, 0, 1);
        });
}

// Scans out[0..n) for contiguous blank==1 runs and reshapes each one via
// reshapeBlankRun(). One function, not inlined at the call site -- matches
// this file's "optimizer owns blanking, callers only call it" boundary
// (emitBlankTo()'s own doc comment: "used by patterns that manage their own
// point emission").
void reshapeBlankRuns(LaserPoint* out, size_t n, const OptimizerConfig& cfg) {
    size_t i = 0;
    while (i < n) {
        if (!out[i].blank) { i++; continue; }
        size_t j = i;
        while (j < n && out[j].blank) j++;
        reshapeBlankRun(out, i, j, cfg);
        i = j;
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

// True at vertex i if segment `seg` has no neighbor on at least one side --
// the open end of a non-closed path. Shared by cornerSeverity() (dwell) and
// SegmentPlan::easeSeverity() (edge shaping, below), which give this case two
// DIFFERENT answers -- see the P19 note on cornerSeverity() for why.
static inline bool isOpenEndpoint(const PathSegment& seg, size_t i) {
    if (seg.closed) return false;
    return i == 0 || i + 1 >= seg.count;
}

// Corner DWELL severity in [0,1], purely geometric: the exterior angle at
// vertex i, mapped to 0 at/below cfg.corner_angle_deg and to 1 at a full 180
// deg reversal -- the sharpest case this optimizer handles. Feeds
// SegmentPlan::cornerPts() -- how many stationary points to place at the
// vertex so the mirror can decelerate, settle, and reaccelerate.
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
// An open-path endpoint (isOpenEndpoint() true) returns max severity (1.0),
// not 0. A genuinely free end (line()/text-stroke pen-up) just gets a few
// extra dwell samples sitting still -- harmless. But wf()/buildWfChains()
// (Cube/Pyramid/Tetrahedron) splits a polyhedron into closed face-loops plus
// open struts that *share a vertex* with those loops: from the strut's own
// PathSegment the shared vertex looks like a free end, so it used to get
// min_corner_pts regardless of the real angle there -- under-dwelling
// exactly where a strut meets a face, visible as a small gap at that corner.
// Octahedron has no open chains (all-closed decomposition) and never showed
// the gap, confirming this path. Treating unknown-neighbor endpoints as
// worst-case sharp instead of softest fixes the shared-vertex case and is a
// no-op risk for true free ends -- for DWELL COUNT. It is the wrong answer
// for edge SHAPING; see SegmentPlan::easeSeverity() (P19).
static float cornerSeverity(const PathSegment& seg, const OptimizerConfig& cfg,
                             size_t i) {
    if (isOpenEndpoint(seg, i)) return 1.0f;

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

// Raw interior turn angle at vertex i (radians, 0..PI) -- the discrete
// curvature proxy for curvature-adaptive resampling (P11b), i.e. the second
// difference of the vertex sequence. Unlike cornerSeverity(), an open-path
// endpoint returns 0 (SMOOTH), not 1.0: a curve fed as an open polyline has no
// real corner at its free ends, and the P11b rule is that missing corner
// metadata degrades to smooth (density there falls back to the base spacing).
// Interior vertices return the plain exterior angle between adjacent edges.
static float vertexTurnAngle(const PathSegment& seg, size_t i) {
    if (seg.count < 3) return 0.0f;
    if (isOpenEndpoint(seg, i)) return 0.0f;
    size_t prev = (i == 0) ? seg.count - 1 : i - 1;
    size_t next = (i + 1) % seg.count;
    return exteriorAngle(seg.vertices[prev].x, seg.vertices[prev].y,
                          seg.vertices[i].x,    seg.vertices[i].y,
                          seg.vertices[next].x, seg.vertices[next].y);
}

// Number of points to place at a corner, scaled by severity between
// cfg.min_corner_pts (severity 0) and cfg.max_corner_pts (severity 1).
static uint8_t cornerPointCount(float severity, const OptimizerConfig& cfg) {
    float pts = cfg.min_corner_pts +
                severity * (cfg.max_corner_pts - cfg.min_corner_pts);
    return (uint8_t)lroundf(pts);
}

// ── Per-call geometry cache ──────────────────────────────────────────────
//
// The two per-vertex/per-edge quantities the pipeline keeps asking for --
// corner severity and edge length -- are pure functions of the (already
// transformed) geometry plus cfg.corner_angle_deg, and NOTHING inside
// optimize() changes either input. Stage 1 rewrites blank_samples, Stage 1.5
// rewrites min/max_corner_pts, Stage 2 rewrites pts_per_1000_units or
// resample_spacing_units; none of those is read by cornerSeverity() or by an
// edge length. (Only true since P13 dropped severity's arrival-speed term,
// which went through edgeInteriorCount() and therefore through the density
// stages -- see DECISIONS.md.)
//
// Uncached, every vertex is evaluated four to six times per frame: once in the
// plan pass, again in Stage 1.5's re-plan, and three times in the emit pass
// (its own corner dwell, plus easeIn/easeOut on each of the two adjacent
// edges). Each evaluation costs one acosf and three sqrtf. On a 512-vertex
// spiral (calib_patterns' cam_spiral) that is thousands of transcendentals per
// frame for numbers that never move.
//
// So both are computed once per optimize() call into a lazy PSRAM scratch,
// then read back through SegmentPlan below. This is a pure memo, never a
// source of truth: every accessor there falls back to computing when the
// scratch is absent (no PSRAM) or too small for the input, so the emitted
// stream is identical either way.
namespace {
    float* s_geom_sev = nullptr;   // corner severity, one per vertex
    float* s_geom_len = nullptr;   // edge length, one per edge
    float* s_geom_turn = nullptr;  // raw turn angle (P11b), one per vertex
    size_t s_geom_sev_n = 0;       // entries valid for the CURRENT call
    size_t s_geom_len_n = 0;
}

// Edges a segment contributes: `count` closed (wrap-around edge included),
// `count - 1` open, none below two vertices. The single definition -- the
// memo is filled by it, PlanCursor slices by it, and walkSegment() iterates
// by it, so no consumer can disagree about where one segment's edges end.
static inline size_t segEdgeCount(const PathSegment& seg) {
    if (seg.count < 2) return 0;
    return seg.closed ? seg.count : (seg.count - 1);
}

// Fills the scratch for one optimize() call, allocating it on first use (lazy
// PSRAM singleton, same pattern as s_clamp_scratch). Input larger than the
// scratch stops the fill instead of truncating anything: the segments that fit
// stay cached, the rest fall back to computing.
static void buildGeomCache(const PathSegment* segments, size_t segment_count,
                            const OptimizerConfig& cfg) {
    s_geom_sev_n = 0;
    s_geom_len_n = 0;
    if (!s_geom_sev) {
        s_geom_sev = (float*)heap_caps_malloc(sizeof(float) * kMaxXfVerts,
                                              MALLOC_CAP_SPIRAM);
        s_geom_len = (float*)heap_caps_malloc(sizeof(float) * kMaxXfVerts,
                                              MALLOC_CAP_SPIRAM);
        s_geom_turn = (float*)heap_caps_malloc(sizeof(float) * kMaxXfVerts,
                                               MALLOC_CAP_SPIRAM);
        if (!s_geom_sev || !s_geom_len || !s_geom_turn) {
            // No PSRAM -> compute on the fly forever. Free+null ALL, same
            // reasoning as applyTransform()'s failure path: leaving one live
            // means the next call's `if (!s_geom_sev)` guard reallocates over
            // it, one leaked block per frame.
            heap_caps_free(s_geom_sev);
            heap_caps_free(s_geom_len);
            heap_caps_free(s_geom_turn);
            s_geom_sev = nullptr;
            s_geom_len = nullptr;
            s_geom_turn = nullptr;
            return;
        }
        memreg::track("Optimizer Geom Cache", sizeof(float) * kMaxXfVerts * 3, true);
    }

    size_t v = 0, e = 0;
    for (size_t s = 0; s < segment_count; s++) {
        const PathSegment& seg = segments[s];
        size_t ec = segEdgeCount(seg);
        if (v + seg.count > kMaxXfVerts || e + ec > kMaxXfVerts) break;
        for (size_t i = 0; i < seg.count; i++) {
            s_geom_sev[v + i]  = cornerSeverity(seg, cfg, i);
            s_geom_turn[v + i] = vertexTurnAngle(seg, i);
        }
        for (size_t k = 0; k < ec; k++) {
            size_t a = k, b = (k + 1) % seg.count;
            float dx = seg.vertices[b].x - seg.vertices[a].x;
            float dy = seg.vertices[b].y - seg.vertices[a].y;
            s_geom_len[e + k] = sqrtf(dx * dx + dy * dy);
        }
        v += seg.count;
        e += ec;
    }
    s_geom_sev_n = v;
    s_geom_len_n = e;
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
// Below this turn angle (radians) an edge endpoint is treated as straight, so
// curvature-adaptive resampling leaves it on the plain constant-spacing path
// -- keeps a straight run byte-identical to the non-curvature resample result.
static constexpr float kCurvatureEps = 1e-3f;

// Curvature-adaptive spacing (P11b): scale the base resample spacing DOWN by
// the local turn angle so bends get denser sampling, clamped to
// [min_spacing_units, max_spacing_units]. Callers only invoke this once curv
// exceeds kCurvatureEps, so a straight edge never reaches here and stays on
// edgeInteriorCount()'s plain path.
static float curvatureSpacing(float baseSpacing, float curv,
                              const OptimizerConfig& cfg) {
    float sp = baseSpacing / (1.0f + cfg.curvature_gain * curv);
    float lo = cfg.min_spacing_units, hi = cfg.max_spacing_units;
    if (hi < lo) hi = lo;   // defensive: inverted bracket collapses to the floor
    if (sp < lo) sp = lo;
    if (sp > hi) sp = hi;
    return sp;
}

// spacingOverride > 0 replaces resample_spacing_units for this one edge
// (curvature-adaptive path). 0 = use the config's own spacing, unchanged.
static uint16_t edgeInteriorCount(float length, const OptimizerConfig& cfg,
                                  float spacingOverride = 0.0f) {
    float raw;
    if (spacingOverride > 0.01f) {
        raw = (length / spacingOverride) - 1.0f;
    } else if (cfg.resample_enabled && cfg.resample_spacing_units > 0.01f) {
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

// ── Segment plan ─────────────────────────────────────────────────────────
//
// Everything a segment's output is made of: how many dwell points sit at each
// vertex, how many interior points each edge carries, how long that edge is,
// and the easing pair that shapes the spacing along it.
//
// The budget pass (planSegment) and the write pass (emitSegment) both see the
// segment only through this type, and both walk it through walkSegment()
// below. They used to be two separately-written loops whose agreement was
// asserted by a pair of comments -- the arrangement the closed-path double
// dwell and the Stage-1/Stage-2 ordering bug both came out of. What a segment
// contains is now defined once, for both passes, or for neither.
//
// Per-vertex severity and per-edge length are read from the per-call memo
// (buildGeomCache() above) when it holds this segment, and computed here when
// it does not -- so a board without PSRAM, or geometry larger than the
// scratch, costs speed and nothing else.
//
// The two derived counts are deliberately NOT snapshotted into the scratch
// next to them. `cfg` points at optimize()'s live config, which Stage 1.5
// (corner counts) and Stage 2 (interior density) rewrite AFTER the first plan
// pass; a snapshot would have to be invalidated by hand at each of those
// writes -- the same hand-maintained invariant this struct exists to retire,
// one level down. Read through the accessors, the plan cannot go stale, and
// each of the two formulas costs one lroundf.
//
// There is no per-segment point floor (P12 removed min_segment_pts: a floor is
// budget Stage 2 cannot give back) and no lift or mid-path-jump field (P14:
// lift only recolors the first dwell point at vertex 0 and costs nothing to
// plan -- see PathVertex in the header).
struct SegmentPlan {
    const PathSegment*     seg = nullptr;
    const OptimizerConfig* cfg = nullptr;
    const float*           sev = nullptr;   // per vertex, memo slice or null
    const float*           len = nullptr;   // per edge,   memo slice or null
    const float*           turn = nullptr;  // per vertex, raw turn angle (P11b)

    size_t edgeCount() const { return segEdgeCount(*seg); }

    float severity(size_t i) const {
        return sev ? sev[i] : cornerSeverity(*seg, *cfg, i);
    }

    // Raw turn angle at vertex i (radians) -- the curvature-adaptive resample
    // input. Memo slice when cached, computed otherwise (same fallback rule as
    // severity()).
    float turnAngle(size_t i) const {
        return turn ? turn[i] : vertexTurnAngle(*seg, i);
    }

    float edgeLength(size_t e) const {
        if (len) return len[e];
        size_t a = e, b = (e + 1) % seg->count;
        float dx = seg->vertices[b].x - seg->vertices[a].x;
        float dy = seg->vertices[b].y - seg->vertices[a].y;
        return sqrtf(dx * dx + dy * dy);
    }

    // Dwell points at vertex i, scaled by its corner severity.
    uint8_t cornerPts(size_t i) const {
        return cornerPointCount(severity(i), *cfg);
    }

    // Interior points along edge e, excluding both endpoints -- those are the
    // dwell points of the vertices the edge runs between. When curvature-
    // adaptive resampling is on (P11b, needs resample mode active), the edge's
    // effective spacing is reduced by its own local curvature -- the larger of
    // the two endpoint turn angles -- so a bend densifies while a straight run
    // (turn <= kCurvatureEps at both ends) collapses to the plain resample
    // count. Both plan and emit passes read this one accessor, so their point
    // counts agree by construction.
    uint16_t interiorPts(size_t e) const {
        float spacingOverride = 0.0f;
        if (cfg->curvature_resample_enabled && cfg->resample_enabled &&
            cfg->resample_spacing_units > 0.01f) {
            size_t a = e, b = (e + 1) % seg->count;
            float curv = std::max(turnAngle(a), turnAngle(b));
            if (curv > kCurvatureEps)
                spacingOverride = curvatureSpacing(cfg->resample_spacing_units, curv, *cfg);
        }
        return edgeInteriorCount(edgeLength(e), *cfg, spacingOverride);
    }

    // Edge-shaping severity (P19) -- deliberately NOT the same value as
    // severity()/cornerPts() at an open endpoint. cornerSeverity() returns 1.0
    // there so the vertex gets a full stop-dwell (P14's wireframe-strut fix);
    // that is a statement about how long the beam sits still, not about how
    // its neighboring edge should be shaped. A "free end" (line()/text-stroke
    // pen-up) has no real corner to ease toward -- the dwell alone handles the
    // stop -- so warping the edge's interior spacing there was pure
    // side-effect: on a 2-vertex open segment (line()) BOTH ends read
    // severity 1.0, so shapeEdgeT() reduces to smoothstep() end to end, i.e.
    // dense-thin-dense along a perfectly straight line with nothing sharp
    // anywhere -- a visible brightness gradient (dim middle) on every long
    // straight segment (row/grid presets, DNA-helix rungs, wireframe struts).
    // A "shared vertex" (wf()/buildWfChains() strut meeting a face loop)
    // looks identical from inside this segment -- there is no cross-segment
    // angle available to shape toward correctly either -- so treating it the
    // same way (no bias) is the safe default rather than a second guess.
    // Interior vertices are unaffected: isOpenEndpoint() is false there, so
    // easeSeverity() falls through to the same angle-based value cornerPts()
    // uses, unchanged.
    float easeSeverity(size_t i) const {
        return isOpenEndpoint(*seg, i) ? 0.0f : severity(i);
    }

    // Spacing within an edge is eased toward whichever of its two ends has the
    // more severe corner (see shapeEdgeT()), so the pair is a property of the
    // edge, not a decision the emit pass makes.
    float easeIn(size_t e)  const { return easeSeverity(e); }
    float easeOut(size_t e) const { return easeSeverity((e + 1) % seg->count); }
};

// Walks the memo in the order it was built, handing each segment its plan. A
// running cursor rather than a per-segment base array because segment_count is
// unbounded while the memo is not, and every consumer (the plan pass, Stage
// 1.5's re-plan, and emitAllSegments) iterates segments front to back exactly
// once.
//
// next() must be called for EVERY segment, including the count==0 ones
// consumers skip, or the following segments read the wrong slice.
struct PlanCursor {
    size_t vbase = 0, ebase = 0;

    SegmentPlan next(const PathSegment& seg, const OptimizerConfig& cfg) {
        SegmentPlan p;
        p.seg = &seg;
        p.cfg = &cfg;
        size_t ec = segEdgeCount(seg);
        if (s_geom_sev && vbase + seg.count <= s_geom_sev_n &&
            ebase + ec <= s_geom_len_n) {
            p.sev = s_geom_sev + vbase;
            p.len = s_geom_len + ebase;
            // s_geom_turn is filled in lockstep with s_geom_sev over the same
            // vertex range, so the same bound gates it.
            if (s_geom_turn) p.turn = s_geom_turn + vbase;
        }
        vbase += seg.count;
        ebase += ec;
        return p;
    }
};

// The shape of one segment's output, defined once and walked by both passes:
// planSegment() counts what it yields, emitSegment() writes it. Neither can
// decide anything the other does not see.
//
// Order, per edge: the dwell at the edge's START vertex, then that edge's
// interior points. Each vertex therefore gets its dwell exactly once, on the
// one edge it starts, rather than once per adjacent edge -- and the run closes
// with a trailing dwell (below).
//
// onCorner(vertexIdx, pts, liftEligible) -- liftEligible marks the very first
// point the segment emits, the only place PathVertex::lift is consulted.
// onInterior(edgeIdx, pts).
template <typename CornerFn, typename InteriorFn>
static void walkSegment(const SegmentPlan& p, CornerFn onCorner,
                         InteriorFn onInterior) {
    const PathSegment& seg = *p.seg;
    if (seg.count == 0) return;
    if (seg.count == 1) {
        // Single-vertex fast path: exactly one lit point, independent of
        // min_corner_pts, and lift is not consulted (P14 -- documented on
        // PathVertex; no caller builds a one-vertex segment).
        onCorner((size_t)0, (uint8_t)1, false);
        return;
    }

    size_t edge_count = p.edgeCount();
    for (size_t e = 0; e < edge_count; e++) {
        onCorner(e, p.cornerPts(e), e == 0);
        onInterior(e, p.interiorPts(e));
    }

    // Trailing dwell, at the last vertex of an open path or a SECOND time at
    // vertex 0 of a closed one.
    //
    // Open: the final vertex starts no edge, so nothing above has dwelled on
    // it. Emitting a single point there instead of its full planned dwell
    // starved the far end of every open line (line() calls, open PathSegment
    // strokes) -- most visible where that dwell is a large share of the lit
    // points, e.g. DNA-helix rungs near the strand crossings, which looked cut
    // short and dim rather than fully drawn.
    //
    // Closed: the wrap-around edge's interior points approach vertex 0 but
    // never land on it, and a single closing point was not enough for the
    // galvo to settle there on short/fast edges (residual gap, worse at small
    // Size). A second full dwell -- same count as its frame-start one -- gives
    // the beam time to arrive before the trailing closing blank cuts it.
    size_t tail = seg.closed ? 0 : seg.count - 1;
    onCorner(tail, p.cornerPts(tail), false);
}

// Budget pass: what this segment costs, without writing any output. Reports
// the corner-point and interior-point sub-totals separately (either pointer
// may be nullptr) so the caller can scale only the interior (length-
// proportional) portion when the budget is exceeded -- corner points are
// capped by max_corner_pts already and matter most for tracking accuracy, so
// they are treated as fixed overhead rather than scaled down.
//
// The return value is exactly out_corner_pts + out_interior_pts. Do not
// re-introduce a per-segment point floor here: a floor cannot be scaled by
// Stage 2, so on a many-edge shape it costs budget that emitAllSegments()
// then has to take out of the shape itself (truncation mid-draw). The one
// case a floor would nominally guard -- a single long lit step once Stage 2
// has crushed interior density -- belongs to max_step_units /
// clampScannerLimits(), which measures actual DAC units per tick on the
// emitted stream and stays inside effective_cap.
static uint16_t planSegment(const SegmentPlan& p,
                             uint16_t* out_corner_pts = nullptr,
                             uint16_t* out_interior_pts = nullptr) {
    uint32_t corner_total = 0, interior_total = 0;
    walkSegment(p,
        [&](size_t, uint8_t pts, bool) { corner_total   += pts; },
        [&](size_t, uint16_t pts)      { interior_total += pts; });

    if (out_corner_pts)   *out_corner_pts   = (corner_total > 0xFFFF) ? 0xFFFF : (uint16_t)corner_total;
    if (out_interior_pts) *out_interior_pts = (interior_total > 0xFFFF) ? 0xFFFF : (uint16_t)interior_total;

    uint32_t total = corner_total + interior_total;
    if (total > 0xFFFF) total = 0xFFFF;
    return (uint16_t)total;
}

// Write pass: the same walk, emitting instead of counting. Every count it
// uses comes from the plan; what it adds is the geometry between them --
// where each interior point lands, its interpolated color, and the jitter
// offset.
static void emitSegment(const SegmentPlan& p,
                         LaserPoint* out, size_t& n, size_t max) {
    const PathSegment&     seg = *p.seg;
    const OptimizerConfig& cfg = *p.cfg;

    walkSegment(p,
        [&](size_t i, uint8_t pts, bool liftEligible) {
            const PathVertex& v = seg.vertices[i];
            for (uint8_t k = 0; k < pts; k++) {
                emit(out, n, max, v.x, v.y, v.r, v.g, v.b,
                     (liftEligible && k == 0 && v.lift) ? 1 : 0);
            }
        },
        [&](size_t e, uint16_t ipts) {
            const PathVertex& va = seg.vertices[e];
            const PathVertex& vb = seg.vertices[(e + 1) % seg.count];
            float dx  = vb.x - va.x, dy = vb.y - va.y;
            float len = p.edgeLength(e);
            float easeIn  = p.easeIn(e);
            float easeOut = p.easeOut(e);

            for (uint16_t k = 1; k <= ipts; k++) {
                float tLin = (float)k / (ipts + 1);
                float t = shapeEdgeT(tLin, easeIn, easeOut);
                float px = va.x + dx * t, py = va.y + dy * t;

                // Jitter (Phase 4): perpendicular offset, interior points only
                // -- corner points stay exact so the shape's vertices remain
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
        });
}

// Plan pass over the whole input: the totals every budget stage reasons about,
// split into corner (fixed overhead) and interior (what Stage 2 scales). Runs
// at most twice per optimize() call -- once for the budget check, once more if
// Stage 1.5 rewrites the corner counts -- over the same plans emitAllSegments()
// later walks.
static void planAllSegments(const PathSegment* segments, size_t segment_count,
                             const OptimizerConfig& cfg,
                             uint32_t& corner_total, uint32_t& interior_total) {
    corner_total   = 0;
    interior_total = 0;
    PlanCursor cur;
    for (size_t s = 0; s < segment_count; s++) {
        uint16_t cp = 0, ip = 0;
        planSegment(cur.next(segments[s], cfg), &cp, &ip);
        corner_total   += cp;
        interior_total += ip;
    }
}

// Points emitAllSegments() will write for the segments themselves at this cfg,
// corner and interior together. Pure: no output, no counter, no cfg change --
// which is what lets Stage 2 evaluate it repeatedly while searching for a
// density (a discarded attempt must not register as planned or truncated).
//
// Blank jumps are deliberately NOT included -- planBlankTotal() below prices
// those, and no density field scales them, so keeping them out is what makes
// this a function of density alone.
static uint32_t planTotal(const PathSegment* segments, size_t segment_count,
                           const OptimizerConfig& cfg) {
    uint32_t corner_total = 0, interior_total = 0;
    planAllSegments(segments, segment_count, cfg, corner_total, interior_total);
    return corner_total + interior_total;
}

// Distance slack the blank-jump plan below prices around, in DAC units.
//
// A real jump measures from out[n-1], which emit() has already clamped and
// cast to int16, so it can sit up to a unit per axis (~1.42 diagonally) from
// the float vertex the plan reads. 3 units covers that with margin, at a cost
// of at most one sample per jump.
static constexpr float kJumpPredictSlack = 3.0f;

// Emitted length of one planned jump, as an upper bound.
//
// Two things stop this from being a single evaluation. planBlankJump()'s total
// is NOT monotone in count -- the shaper switches off where its tail no longer
// fits kMaxBlankPts, so a longer requested jump can emit fewer points (see
// DECISIONS.md, Session C / P3) -- and the measured distance carries the
// rounding slack above. So the whole count range the slack spans is evaluated
// and the largest total wins. 0 only when the jump is provably skipped, i.e.
// short enough that even the slack keeps it under kMinJumpDistUnits.
static int planJumpPts(float x0, float y0, float x1, float y1,
                        const ZvShaper& sh, const OptimizerConfig& cfg) {
    float dx = x1 - x0, dy = y1 - y0;
    float dist = sqrtf(dx * dx + dy * dy);
    if (dist + kJumpPredictSlack < kMinJumpDistUnits) return 0;

    int lo = blankCountForDistance(dist - kJumpPredictSlack, cfg);
    int hi = blankCountForDistance(dist + kJumpPredictSlack, cfg);
    int peak = 0;
    for (int c = lo; c <= hi; c++)
        peak = std::max(peak, planBlankJump(c, sh, cfg).total);
    return peak;
}

// Points this frame's blank jumps cost -- planned per jump instead of reserved
// at worst case.
//
// maxBlankJumpPts() answers "how long can any jump get", which is exactly what
// Stages 1 and 1.5 need: they decide whether the FIXED overhead fits at all
// and must not gamble on the jumps being short. Stage 2 asks a different
// question -- how much budget is left for density -- and there the worst case
// is budget left unspent. A single closed shape reserves two full-length jumps
// for one leading jump plus a closing one of literally zero length (P5); with
// the ZV shaper on that is 184 of 1010 points reserved against 6 spent, and
// the interior density pays for all of it.
//
// Every jump's length is fixed before Stage 2 runs, so it can be planned:
//   - a jump starts at the previous segment's LAST emitted point, which
//     walkSegment() always closes on the tail vertex (vertex 0 closed, the
//     final vertex open) -- no density field moves it;
//   - the first one starts at cfg.prevX/prevY, or is emitBlankRun()'s fixed
//     blank_samples settle when the caller supplied no position;
//   - the closing one returns to segments[0].vertices[0], on the same
//     condition emitAllSegments() applies.
// Start positions are clamped exactly as emit() clamps them, so geometry
// outside the DAC range is priced where it will really land.
//
// Returns false when a segment's tail vertex would get no dwell point at all
// (min_corner_pts == 0 -- reachable only by a caller bypassing the 1..20 clamp
// every config write path applies). The next jump would then start from an
// interior point, i.e. from a density-dependent position, and the caller must
// fall back to the worst-case reserve rather than trust this.
static bool planBlankTotal(const PathSegment* segments, size_t segment_count,
                            const OptimizerConfig& cfg, uint32_t& out_total) {
    ZvShaper sh = computeZvShaper(cfg);
    uint32_t total = 0;
    bool     have  = cfg.hasPrevPos;      // is a start position known?
    float    px = cfg.prevX, py = cfg.prevY;
    bool     any = false;

    PlanCursor cur;
    for (size_t s = 0; s < segment_count; s++) {
        const PathSegment& seg = segments[s];
        SegmentPlan plan = cur.next(seg, cfg);   // advance for every segment
        if (seg.count == 0) continue;

        if (have) {
            total += (uint32_t)planJumpPts(px, py, seg.vertices[0].x,
                                            seg.vertices[0].y, sh, cfg);
        } else {
            total += cfg.blank_samples;          // emitBlankRun fallback
        }

        size_t tail = (seg.count == 1) ? 0 : (seg.closed ? 0 : seg.count - 1);
        if (seg.count > 1 && plan.cornerPts(tail) == 0) return false;
        px = std::max(-32767.0f, std::min(32767.0f, seg.vertices[tail].x));
        py = std::max(-32767.0f, std::min(32767.0f, seg.vertices[tail].y));
        have = true;
        any  = true;
    }

    if (any && segments[0].count > 0) {
        total += (uint32_t)planJumpPts(px, py, segments[0].vertices[0].x,
                                        segments[0].vertices[0].y, sh, cfg);
    }
    out_total = total;
    return true;
}

// ── Transform stage (Phase 1) ────────────────────────────────────────────
//
// Pipeline order: Primitive -> [Transform] -> Warp -> Segment Reorder ->
// Resample -> Corner Dwell -> Jitter -> Blanking -> Velocity Clamp ->
// Acceleration Clamp -> DAC. This is the Transform stage: every input vertex
// is pushed through cfg.transform before any scanner-dependent processing
// (corner detection, length-proportional resampling, blank jumps) sees it.
// Corner severity and edge lengths are therefore computed in the transformed
// frame, which is what a downstream resample/velocity stage needs (a rotated
// square still has 90 deg corners; a scaled path has correspondingly scaled
// edge lengths).
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
    // kMaxXfVerts / kMaxXfSegs are declared at the top of the file -- the
    // geometry cache is sized by the same input bound.
    //
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

// ── Warp stage (Prompt 7a) ────────────────────────────────────────────────
//
// Pipeline order: Primitive -> Transform -> [Warp] -> Segment Reorder ->
// Resample -> Corner Dwell -> Jitter -> Blanking -> Velocity Clamp ->
// Acceleration Clamp -> DAC. Runs after Transform (so warp corrects the
// already rotated/moved geometry, matching where the camera actually sees
// it) and before Resample/Corner Dwell/Segment Reorder (so length-
// proportional spacing, corner severity, and tour-distance all measure the
// WARPED shape -- a warp that stretches one region should get proportionally
// more resample points there, not the pre-warp count).
//
// Same lazy-PSRAM-scratch-copy convention as applyTransform() just above,
// deliberately not merged with it: a call with both a non-identity transform
// AND an active warp needs two independent copies (transform's output is
// warp's input), and warp::isIdentity() is the overwhelmingly common case
// (feature off) where this whole stage is skipped for free.
namespace {
    PathVertex*  s_warp_verts = nullptr;
    PathSegment* s_warp_segs  = nullptr;
}

static size_t applyWarp(const PathSegment* segments, size_t segment_count,
                         const PathSegment** out_segments) {
    if (!s_warp_verts) {
        s_warp_verts = (PathVertex*)heap_caps_malloc(
            kMaxXfVerts * sizeof(PathVertex), MALLOC_CAP_SPIRAM);
        s_warp_segs = (PathSegment*)heap_caps_malloc(
            kMaxXfSegs * sizeof(PathSegment), MALLOC_CAP_SPIRAM);
        if (!s_warp_verts || !s_warp_segs) {
            // No PSRAM -> pass the input through unwarped, never crash. Same
            // free+null-both reasoning as applyTransform()'s identical guard.
            heap_caps_free(s_warp_verts);
            heap_caps_free(s_warp_segs);
            s_warp_verts = nullptr;
            s_warp_segs  = nullptr;
            *out_segments = segments;
            return segment_count;
        }
        memreg::track("Optimizer Warp Scratch",
                      kMaxXfVerts * sizeof(PathVertex) +
                      kMaxXfSegs * sizeof(PathSegment), true);
    }
    size_t seg_out = 0;
    size_t vtx_out = 0;
    for (size_t s = 0; s < segment_count && seg_out < kMaxXfSegs; s++) {
        const PathSegment& src = segments[s];
        if (src.count == 0) {
            s_warp_segs[seg_out] = PathSegment(nullptr, 0, src.closed);
            seg_out++;
            continue;
        }
        if (vtx_out + src.count > kMaxXfVerts) {
            // Same degrade-not-overflow behavior as applyTransform() -- one-
            // time log, trailing segments dropped rather than corrupting
            // adjacent scratch.
            static bool warned = false;
            if (!warned) {
                warned = true;
                LOG_W(logbuf::CAT_GALVO,
                      "applyWarp: scratch exhausted at %u/%u verts, "
                      "%u segments dropped", (unsigned)vtx_out,
                      (unsigned)kMaxXfVerts,
                      (unsigned)(segment_count - s));
            }
            break;
        }

        PathVertex* dst = &s_warp_verts[vtx_out];
        for (size_t i = 0; i < src.count; i++) {
            const PathVertex& v = src.vertices[i];
            float nx = v.x, ny = v.y;
            warp::apply(nx, ny);
            dst[i] = PathVertex(nx, ny, v.r, v.g, v.b, v.lift);
        }
        s_warp_segs[seg_out] = PathSegment(dst, src.count, src.closed);
        vtx_out += src.count;
        seg_out++;
    }
    *out_segments = s_warp_segs;
    return seg_out;
}

// ── Segment reorder (P20) ────────────────────────────────────────────────
//
// Patterns that hand optimize() several disconnected segments in one call
// (wireframes, text glyph strokes, paint) currently visit them in whatever
// order the caller built them in, jumping start-to-start regardless of how
// far apart that leaves consecutive segments. For a caller where the visit
// order is free to choose (nothing about the geometry requires segment K to
// be drawn before segment K+1), a shorter jump is directly recovered point
// budget (blank_pts_per_1000_units means a shorter jump costs fewer points)
// and less ringing excitation (a shorter jump needs a shorter ZV shaper
// tail, see planJumpPts()).
//
// Two phases, kept separate because they answer different questions:
//
//   Phase A -- which segment is next, and (for an open one) which end to
//   enter it from. A nearest-neighbour heuristic (not an exact TSP solve)
//   over each segment's own start point, plus its end point when the
//   segment is open (entering it there means traversing it backwards).
//   O(segment_count^2) -- negligible next to the emit pass that follows,
//   bounded by kMaxXfSegs (64) same as applyTransform()'s scratch. Ties
//   (and the "no known previous position" first step) resolve to the
//   lowest original segment index, so the result is deterministic.
//
//   Phase B -- once the tour order is fixed, a CLOSED segment may rotate
//   which of its own vertices serves as the entry/exit point (its vertex
//   order past that point is untouched, so a color gradient along its
//   edges still runs the same direction, just starting/ending at a
//   different vertex). This is an O(vertex count) nearest-vertex search
//   per closed segment using the ACTUAL incoming point Phase A computed,
//   not part of the O(S^2) tour search above.
//
// Reversing an open segment or rotating a closed one both need a reordered
// COPY of its vertices (PathSegment::vertices is a plain pointer into the
// caller's own array) -- built into a lazy PSRAM scratch, same pattern as
// applyTransform()'s s_xf_verts/s_xf_segs just above, and separate from it
// so a call with both a non-identity transform AND reordering enabled isn't
// reading and overwriting the same buffer.
//
// Gated by cfg.reorder_segments (default false -> byte-identical, segments
// visited in input order). When cfg.reorder_2opt is also set, the greedy NN
// tour is refined by a bounded 2-opt pass (Phase A.5) before the scratch copy
// is built. Degrades gracefully past kMaxXfSegs/kMaxXfVerts,
// same convention as applyTransform() -- input too large for the scratch is
// passed through unreordered rather than overflowing.
namespace {
    PathVertex*  s_reorder_verts = nullptr;
    PathSegment* s_reorder_segs  = nullptr;
}

// ── 2-opt refinement (P11a) ──────────────────────────────────────────────
// Above this many valid segments, 2-opt is skipped and the greedy NN tour
// stands -- an O(n^2)-per-pass sweep runs every frame while enabled, so the
// cap bounds its cost on Core 1. Passes stop early once a full sweep finds no
// improvement; the cap is a hard ceiling on top of that. Epsilon is in DAC
// units squared-free (real distance), just large enough that float rounding
// can't make an accepted move oscillate.
static constexpr size_t kReorder2optMaxSegs  = 32;
static constexpr int    kReorder2optMaxPasses = 6;
static constexpr float  kReorder2optEps       = 0.5f;

// Total straight-line blank-jump length emitAllSegments() will travel for a
// tour: the beam-off hops from the previous galvo position (if known) into
// each segment's entry point in visit order, PLUS the closing blank back to
// the first emitted segment's entry (emitAllSegments() always appends it, so
// 2-opt must count it or it would trade a shorter path for a longer return).
// Each open segment enters/exits per its rev flag; closed segments always
// enter at vertex 0 (matching Phase A's cost model and Phase B's later
// rotation). Real Euclidean distance -- summing squared distances would rank a
// different tour, so 2-opt must not reuse distSq() bare here.
static float tourJumpCost(const PathSegment* segments, const size_t* order,
                          const bool* rev, size_t validCount,
                          const OptimizerConfig& cfg) {
    if (validCount == 0) return 0.0f;
    float total = 0.0f;
    bool  have  = cfg.hasPrevPos;
    float cx = cfg.prevX, cy = cfg.prevY;
    for (size_t step = 0; step < validCount; step++) {
        const PathSegment& s = segments[order[step]];
        float inx = rev[step] ? s.vertices[s.count - 1].x : s.vertices[0].x;
        float iny = rev[step] ? s.vertices[s.count - 1].y : s.vertices[0].y;
        if (have) total += sqrtf(distSq(cx, cy, inx, iny));
        if (rev[step]) { cx = s.vertices[0].x;             cy = s.vertices[0].y; }
        else           { cx = s.vertices[s.count - 1].x;   cy = s.vertices[s.count - 1].y; }
        have = true;
    }
    // Closing blank: last exit (cx,cy) back to the first segment's entry.
    const PathSegment& first = segments[order[0]];
    float e0x = rev[0] ? first.vertices[first.count - 1].x : first.vertices[0].x;
    float e0y = rev[0] ? first.vertices[first.count - 1].y : first.vertices[0].y;
    total += sqrtf(distSq(cx, cy, e0x, e0y));
    return total;
}

// Reverse the tour's visit order over [lo..hi] and flip the traversal
// direction of every OPEN segment in that block, so the beam still runs
// continuously through the reversed run. Closed segments keep rev=false (they
// have no reversible entry in this model). Its own inverse: applied twice it
// restores the original order and flags, which is what lets the caller revert
// a rejected 2-opt move by simply calling it again.
static void reverseTourBlock(const PathSegment* segments, size_t* order,
                             bool* rev, size_t lo, size_t hi) {
    size_t a = lo, b = hi;
    while (a < b) {
        size_t to = order[a]; order[a] = order[b]; order[b] = to;
        bool   tr = rev[a];   rev[a]   = rev[b];   rev[b]   = tr;
        a++; b--;
    }
    for (size_t k = lo; k <= hi; k++) {
        const PathSegment& s = segments[order[k]];
        if (!s.closed && s.count > 1) rev[k] = !rev[k];
    }
}

static size_t reorderSegments(const PathSegment* segments, size_t segment_count,
                               const OptimizerConfig& cfg,
                               const PathSegment** out_segments) {
    if (segment_count < 2 || segment_count > kMaxXfSegs) {
        *out_segments = segments;
        return segment_count;
    }

    // Only non-empty segments have a position to reason about -- an empty
    // one (a caller's unused fixed-array slot) costs nothing to visit in
    // any order, so it is excluded from the tour and appended back verbatim
    // afterward.
    size_t validIdx[kMaxXfSegs];
    size_t validCount = 0;
    size_t totalVerts = 0;
    for (size_t i = 0; i < segment_count; i++) {
        if (segments[i].count == 0) continue;
        validIdx[validCount++] = i;
        totalVerts += segments[i].count;
    }
    if (validCount < 2 || totalVerts > kMaxXfVerts) {
        *out_segments = segments;
        return segment_count;
    }

    if (!s_reorder_verts) {
        s_reorder_verts = (PathVertex*)heap_caps_malloc(
            kMaxXfVerts * sizeof(PathVertex), MALLOC_CAP_SPIRAM);
        s_reorder_segs = (PathSegment*)heap_caps_malloc(
            kMaxXfSegs * sizeof(PathSegment), MALLOC_CAP_SPIRAM);
        if (!s_reorder_verts || !s_reorder_segs) {
            // No PSRAM -> pass the input through unreordered, never crash.
            // Free+null BOTH pointers -- same reasoning as applyTransform()'s
            // identical guard just above: leaving one allocated-but-live
            // strands it for the rest of the process.
            heap_caps_free(s_reorder_verts);
            heap_caps_free(s_reorder_segs);
            s_reorder_verts = nullptr;
            s_reorder_segs  = nullptr;
            *out_segments = segments;
            return segment_count;
        }
        memreg::track("Optimizer Reorder Scratch",
                      kMaxXfVerts * sizeof(PathVertex) +
                      kMaxXfSegs * sizeof(PathSegment), true);
    }

    // ---- Phase A: nearest-neighbour tour over the valid segments' ----
    // ---- start/end points.                                         ----
    size_t order[kMaxXfSegs];
    bool   rev[kMaxXfSegs];
    float  entryX[kMaxXfSegs], entryY[kMaxXfSegs];
    bool   used[kMaxXfSegs] = {false};

    float curX = cfg.prevX, curY = cfg.prevY;
    bool  haveCur = cfg.hasPrevPos;

    for (size_t step = 0; step < validCount; step++) {
        size_t bestSlot = (size_t)-1;
        bool   bestRev  = false;
        float  bestD    = 0.0f;

        if (!haveCur) {
            // No known incoming position: anchor the tour at the first
            // (original-order) valid segment, entered forward -- the same
            // starting point emitAllSegments() already uses today.
            bestSlot = 0;
        } else {
            for (size_t slot = 0; slot < validCount; slot++) {
                if (used[slot]) continue;
                const PathSegment& s = segments[validIdx[slot]];
                float d = distSq(curX, curY, s.vertices[0].x, s.vertices[0].y);
                bool  r = false;
                if (!s.closed && s.count > 1) {
                    float de = distSq(curX, curY,
                                       s.vertices[s.count - 1].x,
                                       s.vertices[s.count - 1].y);
                    if (de < d) { d = de; r = true; }
                }
                if (bestSlot == (size_t)-1 || d < bestD) {
                    bestSlot = slot; bestD = d; bestRev = r;
                }
            }
        }

        const PathSegment& chosen = segments[validIdx[bestSlot]];
        order[step]  = validIdx[bestSlot];
        rev[step]    = bestRev;
        entryX[step] = haveCur ? curX : chosen.vertices[0].x;
        entryY[step] = haveCur ? curY : chosen.vertices[0].y;
        used[bestSlot] = true;

        // Exit point: the far end from wherever this segment was entered.
        if (bestRev) { curX = chosen.vertices[0].x; curY = chosen.vertices[0].y; }
        else         { curX = chosen.vertices[chosen.count - 1].x;
                        curY = chosen.vertices[chosen.count - 1].y; }
        haveCur = true;
    }

    // ---- Phase A.5: optional 2-opt refinement of the tour (P11a). ----
    // Take the greedy NN tour above and try reversing every contiguous block
    // of the visit order, keeping a reversal only when it strictly shortens
    // the total blank-jump path. Bounded segment count + pass count keep the
    // per-frame cost in check; strict-improvement acceptance keeps it
    // deterministic and guarantees the result is never worse than NN.
    if (cfg.reorder_2opt && validCount >= 3 && validCount <= kReorder2optMaxSegs) {
        float best = tourJumpCost(segments, order, rev, validCount, cfg);
        bool  improved = true;
        int   passes = 0;
        while (improved && passes++ < kReorder2optMaxPasses) {
            improved = false;
            for (size_t i = 0; i < validCount; i++) {
                for (size_t j = i + 1; j < validCount; j++) {
                    reverseTourBlock(segments, order, rev, i, j);
                    float c = tourJumpCost(segments, order, rev, validCount, cfg);
                    if (c + kReorder2optEps < best) {
                        best = c; improved = true;
                    } else {
                        reverseTourBlock(segments, order, rev, i, j);  // revert
                    }
                }
            }
        }

        // 2-opt has rewritten order[]/rev[], so the per-step entry points
        // Phase A recorded no longer match -- recompute them from the final
        // tour before Phase B (which rotates closed segments toward their
        // actual incoming point) reads them.
        bool  have = cfg.hasPrevPos;
        float cx = cfg.prevX, cy = cfg.prevY;
        for (size_t step = 0; step < validCount; step++) {
            const PathSegment& s = segments[order[step]];
            if (have) { entryX[step] = cx;              entryY[step] = cy; }
            else      { entryX[step] = s.vertices[0].x; entryY[step] = s.vertices[0].y; }
            if (rev[step]) { cx = s.vertices[0].x;           cy = s.vertices[0].y; }
            else           { cx = s.vertices[s.count - 1].x; cy = s.vertices[s.count - 1].y; }
            have = true;
        }
    }

    // ---- Phase B: closed segments may rotate their start vertex. ----
    size_t rotate[kMaxXfSegs] = {0};
    for (size_t step = 0; step < validCount; step++) {
        const PathSegment& seg = segments[order[step]];
        if (!seg.closed || seg.count < 2) continue;
        size_t best  = 0;
        float  bestD = distSq(entryX[step], entryY[step],
                              seg.vertices[0].x, seg.vertices[0].y);
        for (size_t v = 1; v < seg.count; v++) {
            float d = distSq(entryX[step], entryY[step],
                             seg.vertices[v].x, seg.vertices[v].y);
            if (d < bestD) { bestD = d; best = v; }
        }
        rotate[step] = best;
    }

    // ---- Build the reordered scratch copy. ----
    size_t vtx_out = 0, seg_out = 0;
    for (size_t step = 0; step < validCount; step++) {
        const PathSegment& seg = segments[order[step]];
        PathVertex* dst = &s_reorder_verts[vtx_out];
        if (seg.closed && rotate[step] != 0) {
            for (size_t i = 0; i < seg.count; i++)
                dst[i] = seg.vertices[(rotate[step] + i) % seg.count];
        } else if (rev[step] && seg.count > 1) {
            for (size_t i = 0; i < seg.count; i++)
                dst[i] = seg.vertices[seg.count - 1 - i];
        } else {
            for (size_t i = 0; i < seg.count; i++)
                dst[i] = seg.vertices[i];
        }
        s_reorder_segs[seg_out] = PathSegment(dst, seg.count, seg.closed);
        vtx_out += seg.count;
        seg_out++;
    }
    // Empty segments carry no position and cost nothing wherever they sit --
    // appended after the reordered real ones, in their original relative
    // order.
    for (size_t i = 0; i < segment_count; i++) {
        if (segments[i].count != 0) continue;
        s_reorder_segs[seg_out++] = segments[i];
    }

    *out_segments = s_reorder_segs;
    return seg_out;
}

// ── Brightness compensation (Prompt 7c) ──────────────────────────────────
//
// Pipeline order: ... -> Blanking -> [Brightness] -> Velocity Clamp ->
// Acceleration Clamp -> DAC. Runs on the already-emitted point stream, after
// Blanking and before the Velocity/Acceleration clamp: blanked points are
// skipped outright (their RGB doesn't reach the laser regardless), and
// running before the clamp means any intermediate points it inserts inherit
// already-gain-corrected RGB rather than being missed by a stage that ran
// before the clamp added them. Output-stage only -- see brightnessField.h's
// note on why this must never be written back into a pattern's own color
// definition (0/255 default channel rule) or confused with col_override.
static void applyBrightnessField(LaserPoint* out, size_t n) {
    if (brightness::isIdentity()) return;
    for (size_t i = 0; i < n; i++) {
        if (out[i].blank) continue;
        uint8_t g = brightness::gain(out[i].x, out[i].y);
        out[i].r = (uint8_t)(((uint16_t)out[i].r * g) / 255);
        out[i].g = (uint8_t)(((uint16_t)out[i].g * g) / 255);
        out[i].b = (uint8_t)(((uint16_t)out[i].b * g) / 255);
    }
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
    PlanCursor cur;
    for (size_t s = 0; s < segment_count; s++) {
        const PathSegment& seg = segments[s];
        // Advanced for every segment, skipped ones included -- see PlanCursor.
        SegmentPlan plan = cur.next(seg, cfg);
        if (seg.count == 0) continue;

        // Blank jump to this segment's first vertex -- distance-
        // proportional + eased (Pillar 2), see emitBlankJump().
        emitBlankJump(out, n, max_out, seg.vertices[0].x, seg.vertices[0].y, cfg);

        emitSegment(plan, out, n, max_out);
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

// ── Stage 2 density search ───────────────────────────────────────────────
//
// Bounds and stopping rule for the bisection in optimize() below.

// Plan passes the search may spend, not bisection halvings: the first probe is
// the closed-form estimate, the rest halve the bracket around it. A hard
// ceiling because this runs on the pattern task -- a plan pass is arithmetic
// over the geometry memo (no transcendentals since P15), so six of them still
// cost a fraction of the emit pass that follows.
static constexpr int kStage2MaxProbes = 6;

// Stop as soon as the plan sits this close under its allowance. 2% of a frame
// budget is a couple of dozen points at most -- below one edge's worth on any
// shape dense enough to reach Stage 2, so refining further buys nothing the
// galvo can show.
static constexpr float kStage2FitFraction = 0.98f;

// Density floors, one per mode -- what a scale of 0 resolves to. They keep the
// sparsest density a real, drawable one instead of an empty frame.
static constexpr float kMinPtsPer1000Units = 0.1f;    // points per 1000 units
static constexpr float kMinDensityScale    = 0.02f;   // resample spacing divisor

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

    // Stage 0.4 -- Warp (Prompt 7a). Camera closed-loop keystone correction.
    // Runs BEFORE Segment Reorder (Stage 0.5) deliberately: reorder's
    // nearest-neighbour tour minimizes blank-jump distance, which must be
    // measured on the geometry the galvo will actually traverse -- i.e.
    // post-warp, not pre-warp. isIdentity() (disabled, or grid numerically
    // equals the identity grid) is the fast path: skip the whole scratch-copy
    // stage, output stays byte-identical to the pre-warp optimizer.
    if (!warp::isIdentity()) {
        const PathSegment* warped_segments = nullptr;
        segment_count = applyWarp(segments, segment_count, &warped_segments);
        segments = warped_segments;
        if (segment_count == 0) return finishStats(st, out, 0);
    }

    // Stage 0.5 -- Segment reorder (P20). Nearest-neighbour tour over the
    // (already-transformed) segments' start/end points to shorten total
    // blank-jump distance. Purely a visitation-order change -- no geometry is
    // added or removed -- so it must run before the geometry cache and every
    // budget/plan/emit stage below, all of which walk `segments` in whatever
    // order it holds at that point. Gated behind cfg.reorder_segments
    // (default false -> byte-identical); CLASS-INVARIANT when on, see
    // CONTRACT.md.
    if (cfg.reorder_segments) {
        const PathSegment* ro_segments = nullptr;
        size_t ro_count = reorderSegments(segments, segment_count, cfg, &ro_segments);
        segments = ro_segments;
        segment_count = ro_count;
    }

    // Geometry cache (not a pipeline stage -- it emits nothing and changes
    // nothing). Corner severity and edge length are fixed for the rest of this
    // call, since no later stage touches an input of either, so they are
    // computed once here instead of four to six times per vertex across plan /
    // re-plan / emit. Must run AFTER the transform above: severity and length
    // are properties of the transformed geometry.
    buildGeomCache(segments, segment_count, cfg);

    // Budget check: plan at the requested density first, tracking corner
    // and interior sub-totals separately (corner points are fixed
    // overhead -- capped by max_corner_pts, not scaled down; only
    // interior/length-proportional density is reduced to fit budget).
    uint32_t corner_total = 0, interior_total = 0;
    planAllSegments(segments, segment_count, cfg, corner_total, interior_total);
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

    // Stage 1 (MUST run before Stage 2 below): shrinks blank_samples toward
    // stage1_blank_target (falling back further to min_blank_samples) when
    // fixed overhead -- corners + blank jumps at the configured
    // blank_samples -- leaves too little of the budget for interior
    // density. The ordering is load-bearing, not cosmetic -- see
    // docs/05-optimizer.md's Stage 5 "Budget Interaction" section.
    uint32_t fixed_overhead_at_default_blank = corner_total + blank_overhead;
    uint32_t min_interior_reserve = (uint32_t)cfg.min_interior_pts_per_segment * segment_count;
    bool cap_exceeded   = fixed_overhead_at_default_blank > effective_cap;
    bool reserve_too_low = (effective_cap >= fixed_overhead_at_default_blank) &&
        ((effective_cap - fixed_overhead_at_default_blank) < min_interior_reserve);

    if ((cap_exceeded || reserve_too_low) && cfg.blank_samples > cfg.min_blank_samples) {
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
        // budget? If so, degrade to the LARGEST blank_samples that does fit,
        // NOT straight to the hard floor. Taking the floor here handed a user
        // who raised stage1_blank_target past the frame budget the exact
        // opposite of what they asked for -- measured on cam_segments at
        // max_pts_per_frame=150 (n=3, identical emitted counts): targets
        // 8/14/20 gave ~8.8/14.8/20.8 blank points per jump as intended, but
        // target 30 silently reverted to the same ~8.8 as target 8, with no
        // signal anywhere. Same per-jump term throughout --
        // maxBlankJumpPts() is the single source.
        //
        // maxBlankJumpPts() is non-decreasing in blank_samples (unshaped its
        // result IS blank_samples; shaped it is move(count)+shift+1 with
        // move() non-decreasing), so the values that fit form a prefix of
        // [min_blank_samples, target] and a bisection finds the largest one
        // in ~7 probes instead of a linear walk in this per-call path.
        auto overheadAt = [&](int blank) -> uint32_t {
            const uint8_t saved = cfg.blank_samples;
            cfg.blank_samples   = (uint8_t)blank;
            uint32_t o = corner_total +
                         (uint32_t)maxBlankJumpPts(cfg) * (segment_count + 1);
            cfg.blank_samples = saved;
            return o;
        };
        // "Fits" here is this stage's own trigger, negated: overhead PLUS
        // min_interior_reserve must be within the cap. The cap alone is not
        // enough -- it can leave the interior nothing but the density floor
        // (kMinPtsPer1000Units), which Stage 2 cannot scale below, so the plan
        // overshoots and the frame truncates. The whole point of shrinking
        // blank_samples here is to buy interior room; a fallback that spends
        // the last point back on blank jumps defeats it. The old code used the
        // looser cap-only test for the target and then jumped to the floor,
        // which is also why the settled value used to be non-monotonic in the
        // target across that boundary.
        const uint32_t stage1_fit_budget =
            (effective_cap > min_interior_reserve)
                ? (uint32_t)(effective_cap - min_interior_reserve) : 0u;
        if (overheadAt((int)target) > stage1_fit_budget) {
            int lo   = (int)cfg.min_blank_samples;
            int hi   = (int)target;
            int best = lo;   // even the floor may not fit -- then it is the
                             // floor, exactly as before this change
            while (lo <= hi) {
                int mid = lo + (hi - lo) / 2;
                if (overheadAt(mid) <= stage1_fit_budget) { best = mid; lo = mid + 1; }
                else                                     { hi  = mid - 1; }
            }
            cfg.blank_samples      = (uint8_t)best;
            st.stage1BlankClamped  = true;
        }
        st.stage1BlankSamples = cfg.blank_samples;
        blank_overhead = (uint32_t)maxBlankJumpPts(cfg) * (segment_count + 1);
        needed = planned_total + blank_overhead;
    }

    // Stage 1.5: when corner dwell ALONE still exceeds what's left after
    // blank overhead, scale min_corner_pts/max_corner_pts down together
    // (floor 1 pt/vertex) and re-plan, trading corner sharpness for the one
    // thing that must never be sacrificed: a closed path actually closing.
    // See docs/05-optimizer.md's Stage 4 "Corner dwell vs. the frame
    // budget" section for the failure this prevents and why it's needed.
    if (corner_total + blank_overhead > effective_cap && corner_total > 0) {
        st.stage15Triggered = true;
        float available_for_corners = (float)effective_cap - (float)blank_overhead;
        if (available_for_corners < 0.0f) available_for_corners = 0.0f;
        float corner_scale = available_for_corners / (float)corner_total;

        uint8_t new_min = (uint8_t)std::max(1.0f, floorf(cfg.min_corner_pts * corner_scale));
        uint8_t new_max = (uint8_t)std::max((float)new_min, floorf(cfg.max_corner_pts * corner_scale));
        cfg.min_corner_pts = new_min;
        cfg.max_corner_pts = new_max;

        // Re-plan reuses the same cache: only min/max_corner_pts changed, and
        // cornerPointCount() re-reads those -- the severities they scale do
        // not depend on them.
        planAllSegments(segments, segment_count, cfg, corner_total, interior_total);
        planned_total = corner_total + interior_total;
        needed = planned_total + blank_overhead;
    }

    // Stage 2: reduce interior (length-proportional) density until the plan
    // fits the budget -- by SEARCHING for the density, not by estimating it
    // once.
    //
    // The closed-form estimate (available_for_interior / interior_total) would
    // be exact if an edge could carry a fractional point. edgeInteriorCount()
    // instead lroundf()s every edge on its own, and those roundings accumulate
    // in whichever direction the edge lengths happen to fall. Measured on a
    // 480-vertex circle at cap 1300: the plan said 1300, the emit pass wrote
    // 1464 -- 12.6% over, caught only by emitAllSegments()'s hard cap, i.e. by
    // truncation, in exactly the place Stage 1.5 exists to prevent it (a closed
    // path that stops before it closes). The same rounding runs the other way
    // just as often, leaving a large share of the frame's budget unspent.
    //
    // planTotal() is precisely what the emit pass will write for the segments
    // (P16: one walkSegment definition for both), so bisecting it against the
    // cap settles both directions at once. The search keeps the largest density
    // whose plan still fits, so the frame lands AT the budget from below rather
    // than near it from either side.
    //
    // SIDE EFFECT, and the point of the exercise: at an unchanged
    // max_pts_per_frame, budget-bound patterns get denser -- the leftover the
    // one-shot estimate used to give away is now spent on interior points.
    // Frames that were not budget-bound (this branch does not run) are
    // untouched.
    //
    // Bisection is valid because both density fields enter edgeInteriorCount()
    // through a product with the edge length: every edge's count is
    // non-decreasing in the scale, so their sum is too. Corner counts do not
    // move at all under a density change (no density field reaches
    // cornerPointCount()), which is why the search can leave them inside the
    // target instead of subtracting them out -- it makes no assumption either
    // way, it just measures.
    //
    // Where all edges share a length, they also share their rounding boundary
    // and the plan steps by edge_count at a single scale. The search then
    // settles below that step (the largest plan that fits), which can leave the
    // budget visibly underspent -- deliberately: a complete shape drawn coarsely
    // beats a denser one truncated mid-path.
    //
    // The probes write nothing and touch neither sPlanned nor sTruncated, so a
    // discarded attempt is not a truncation and invariant 3 keeps counting only
    // what the emit pass really attempted.
    if (needed > effective_cap && interior_total > 0) {
        // Resample mode: edgeInteriorCount() ignores pts_per_1000_units
        // entirely and derives its count from resample_spacing_units instead
        // (points = length/spacing), so density is scaled there by *growing*
        // the spacing -- count goes as 1/spacing, which reaches the same target
        // the pts_per_1000_units branch reaches for the non-resample case.
        const bool  resample    = cfg.resample_enabled && cfg.resample_spacing_units > 0.01f;
        const float basePpu     = cfg.pts_per_1000_units;
        const float baseSpacing = cfg.resample_spacing_units;

        // What the segments may spend: the cap, less what the blank jumps
        // between them will really cost. Stages 1 and 1.5 above reserve the
        // worst case (blank_overhead) because they decide whether the fixed
        // overhead fits at all; here the worst case would simply hand the
        // difference back unspent, and on a single closed shape that
        // difference is most of the reserve -- see planBlankTotal(), which
        // bounds each jump instead of assuming the longest one.
        uint32_t blank_planned = blank_overhead;
        uint32_t exact_blank   = 0;
        if (planBlankTotal(segments, segment_count, cfg, exact_blank))
            blank_planned = std::min(blank_planned, exact_blank);

        uint32_t plan_cap = (effective_cap > blank_planned)
                                 ? (uint32_t)(effective_cap - blank_planned) : 0u;

        auto applyDensity = [&](float s) {
            if (resample)
                cfg.resample_spacing_units = baseSpacing / std::max(s, kMinDensityScale);
            else
                cfg.pts_per_1000_units = std::max(kMinPtsPer1000Units, basePpu * s);
        };

        // Bracket. 1.0 is the requested density, which this branch's condition
        // has just shown does not fit. 0.0 resolves to the floors above -- the
        // sparsest density that still draws something, and the answer when
        // nothing fits at all (corner points alone over budget, Stage 1.5
        // having already done what it could).
        float lo = 0.0f, hi = 1.0f, best = 0.0f;

        // First probe: the closed-form estimate this stage used to apply
        // outright. It is wrong only by the accumulated per-edge rounding, so
        // it starts the search inside the answer's neighbourhood instead of
        // spending two halvings getting there.
        float available_for_interior =
            (float)plan_cap - (float)corner_total;
        if (available_for_interior < 0.0f) available_for_interior = 0.0f;
        float probe = available_for_interior / (float)interior_total;
        if (!(probe > 0.0f)) probe = 0.0f;      // also catches NaN
        if (probe > 1.0f)    probe = 1.0f;

        for (int i = 0; i < kStage2MaxProbes && plan_cap > 0; i++) {
            applyDensity(probe);
            uint32_t total = planTotal(segments, segment_count, cfg);
            if (total <= plan_cap) {
                lo = best = probe;
                // The requested density fits after all -- the trigger above
                // measures against the worst-case blank reserve, which is the
                // larger of the two. Nothing to scale, and nothing above 1.0
                // to search toward.
                if (probe >= 1.0f) break;
                if ((float)total >= kStage2FitFraction * (float)plan_cap) break;
            } else {
                hi = probe;
            }
            probe = 0.5f * (lo + hi);
        }

        applyDensity(best);
        // Report the factor really applied, floors included -- a scale of 0.01
        // that the density floor turned into 0.4 would otherwise read as a far
        // tighter squeeze than actually happened.
        st.stage2Scale = resample
            ? (baseSpacing / cfg.resample_spacing_units)
            : ((basePpu > 1e-4f) ? (cfg.pts_per_1000_units / basePpu) : best);
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
    //   - Brightness (Prompt 7c): active. applyBrightnessField(), called
    //     below, right after this emit pass and before the clamp.
    //   - Velocity Clamp / Acceleration Clamp (Phase 4): a post-pass over the
    //     emitted out[0..n-1] that inserts intermediate points where the
    //     per-tick position (velocity) or its delta (acceleration) exceeds the
    //     galvo limit. Implemented in clampScannerLimits(), called below.
    // Emit bounded by effective_cap, NOT max_out: max_pts_per_frame is a hard
    // guarantee, and emitAllSegments() already stops writing at its max_out
    // argument, so the cap holds by construction rather than by trusting the
    // plan.
    //
    // Since Stage 2 searches the plan against that same cap instead of
    // estimating it once, the two normally agree and this bound is not the
    // thing doing the work. It still is for the cases no stage can plan away:
    // corner points alone over budget after Stage 1.5's 1-point-per-vertex
    // floor, and clampScannerLimits() below inserting into an already-full
    // frame. Truncation there is counted (Stats::truncated), never silent.
    size_t n = emitAllSegments(segments, segment_count, cfg, out, effective_cap);

    // Brightness compensation (Prompt 7c). No-op (byte-identical) unless
    // gBrightness.enabled and the grid is non-identity -- see
    // applyBrightnessField() above. Runs after Blanking, before the
    // Velocity/Acceleration clamp -- see that function's own doc comment for
    // why the ordering relative to the clamp matters. Scales RGB only, never
    // touches x/y, so it does not affect the clamp's own geometry.
    applyBrightnessField(out, n);

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

// ── Curve ingestion (P11b) ───────────────────────────────────────────────
// Lit runs above this count fall back to "not handled" (return 0) -- a
// dot-cloud curve (phyllotaxis) would otherwise relaminate into hundreds of
// one-vertex segments, each carrying a blank jump, which is neither a good
// fit for curvature resampling nor worth the budget churn. Continuous curves
// produce one lit run; only the dot-based ones approach this.
static constexpr size_t kStreamMaxSegs = 96;
namespace {
    PathVertex*  s_stream_verts = nullptr;
    PathSegment* s_stream_segs  = nullptr;
}

size_t optimizeStream(const LaserPoint* in, size_t n_in,
                      LaserPoint* out, size_t max_out,
                      const OptimizerConfig& cfg) {
    if (!in || !out || n_in == 0) return 0;
    if (n_in > kMaxXfVerts) return 0;   // more vertices than the scratch holds

    if (!s_stream_verts) {
        s_stream_verts = (PathVertex*)heap_caps_malloc(
            kMaxXfVerts * sizeof(PathVertex), MALLOC_CAP_SPIRAM);
        s_stream_segs = (PathSegment*)heap_caps_malloc(
            kStreamMaxSegs * sizeof(PathSegment), MALLOC_CAP_SPIRAM);
        if (!s_stream_verts || !s_stream_segs) {
            // No PSRAM -> not handled; caller keeps its own stream. Free+null
            // BOTH, same reasoning as the other lazy scratch singletons.
            heap_caps_free(s_stream_verts);
            heap_caps_free(s_stream_segs);
            s_stream_verts = nullptr;
            s_stream_segs  = nullptr;
            return 0;
        }
        memreg::track("Optimizer Stream Scratch",
                      kMaxXfVerts * sizeof(PathVertex) +
                      kStreamMaxSegs * sizeof(PathSegment), true);
    }

    // Split the stream into contiguous lit runs -- one open PathSegment each,
    // blank points dropped (optimize() re-adds its own blank jumps). Every
    // segment's vertices point into s_stream_verts, which is fully populated
    // before optimize() runs, so `in` and `out` may safely alias.
    size_t vtx = 0, nseg = 0, runStart = 0;
    bool   inRun = false;
    for (size_t i = 0; i < n_in; i++) {
        if (!in[i].blank) {
            if (!inRun) { inRun = true; runStart = vtx; }
            s_stream_verts[vtx++] = PathVertex((float)in[i].x, (float)in[i].y,
                                               in[i].r, in[i].g, in[i].b, false);
        } else if (inRun) {
            if (nseg >= kStreamMaxSegs) return 0;
            s_stream_segs[nseg++] = PathSegment(s_stream_verts + runStart,
                                                vtx - runStart, /*closed=*/false);
            inRun = false;
        }
    }
    if (inRun) {
        if (nseg >= kStreamMaxSegs) return 0;
        s_stream_segs[nseg++] = PathSegment(s_stream_verts + runStart,
                                            vtx - runStart, /*closed=*/false);
    }
    if (nseg == 0) return 0;   // stream was all-blank -> not handled

    return optimize(s_stream_segs, nseg, out, max_out, cfg);
}

}  // namespace optimizer