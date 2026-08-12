#include "weld_patterns.h"
#include "paint_patterns.h"
#include "point_optimizer.h"
#include "mutex.h"
#include "util/ps_scratch.h"
#include "util/mem_registry.h"
#include <Arduino.h>
#include <math.h>
#include <string.h>

namespace weld {

// ── Fixed algorithm constants (browser-preview approved -- do not re-tune) ────
static constexpr int   TRAIL_VERTS       = 24;      // afterglow polyline samples (tail -> head)
static constexpr int   TRAIL_SEGS_MAX    = 6;       // trail sub-paths after lift/off-path splits
static constexpr float SPARK_GRAVITY     = 55000.f; // u/s^2, pulls toward -Y (canvas maps +Y up)
static constexpr float SPARK_DRAG        = 1.8f;    // 1/s exponential velocity decay
static constexpr float SPARK_SPEED_MIN   = 8000.f;  // u/s spawn speed floor
static constexpr float SPARK_SPEED_SPAN  = 16000.f; // u/s random span on top
static constexpr float SPARK_CONE_RAD    = 2.2f;    // half-angle around back-travel axis
static constexpr float SPARK_STREAK_S    = 0.035f;  // streak length = velocity * this
static constexpr float FRAME_DT_MAX      = 0.1f;    // dt clamp after stalls / mode entry
static constexpr int   HEAD_DWELL_PTS    = 3;       // extra head-vertex repeats (hot spot)
static constexpr float COORD_LIMIT       = 32000.f; // sparks leaving this range die

// One flattened, arc-length-parameterized path node. `lift` marks a stroke's
// first node; the gap between strokes costs zero arc length.
struct PathNode { float x, y, s; bool lift; };

// One spark particle (ballistic, drag + gravity).
struct Spark { float x, y, vx, vy, life, lifeMax; };

struct Sample { bool valid; float x, y; };

// ── Persistent effect state (pattern task only -- single-threaded) ───────────
static float    sHeadPos = 0.f;     // arc-length position of the torch head
static int      sPingDir = 1;       // ping-pong travel sign (+1 / -1)
static bool     sSeekEnd = false;   // one-shot: start from the far end
static bool     sHasTime = false;   // millis() baseline captured yet
static uint32_t sLastMs  = 0;
static Spark    sSparks[WELD_SPARK_COUNT_MAX];

// Small xorshift32 PRNG (deterministic, no rand()).
static uint32_t sRng = 0x1234567u;
static inline uint32_t xorshift32() {
    uint32_t x = sRng; x ^= x << 13; x ^= x >> 17; x ^= x << 5; sRng = x; return x;
}
static inline float randf() { return (float)(xorshift32() >> 8) * (1.0f / 16777216.0f); } // [0,1)
static inline float rand2() { return randf() * 2.0f; }                                     // [0,2)

// Binary search for the highest node with node.s <= s, then linear interpolation
// on that edge. Invalid outside [0, pathLen] so the trail fades at open ends
// instead of wrapping across a jump.
static Sample sampleAt(const PathNode* nodes, int count, float pathLen, float s) {
    Sample r = {false, 0.f, 0.f};
    if (count < 2 || s < 0.f || s > pathLen) return r;
    int lo = 0, hi = count - 1, idx = 0;
    while (lo <= hi) { int mid = (lo + hi) / 2; if (nodes[mid].s <= s) { idx = mid; lo = mid + 1; } else hi = mid - 1; }
    if (idx >= count - 1) { r.valid = true; r.x = nodes[count - 1].x; r.y = nodes[count - 1].y; return r; }
    const PathNode& a = nodes[idx];
    const PathNode& b = nodes[idx + 1];
    float ds = b.s - a.s;
    float t = (ds > 1e-6f) ? (s - a.s) / ds : 0.f;
    r.valid = true;
    r.x = a.x + (b.x - a.x) * t;
    r.y = a.y + (b.y - a.y) * t;
    return r;
}

// True if any lift node's arc-length sits strictly between sa and sb (i.e. the
// trail window crosses a stroke boundary / blank jump).
static bool crossesLift(const float* liftS, int liftCount, float sa, float sb) {
    float lo = sa < sb ? sa : sb, hi = sa < sb ? sb : sa;
    for (int i = 0; i < liftCount; i++) { float ls = liftS[i]; if (ls > lo && ls < hi) return true; }
    return false;
}

static void spawnSpark(Spark& sp, float hx, float hy, float tx, float ty) {
    // Cone around the BACK-travel axis (-tx,-ty).
    float bx = -tx, by = -ty;
    if (bx == 0.f && by == 0.f) { bx = 0.f; by = 1.f; }   // no travel dir -> straight up
    float ang = (rand2() - 1.f) * SPARK_CONE_RAD;
    float ca = cosf(ang), sa = sinf(ang);
    float dx = bx * ca - by * sa;
    float dy = bx * sa + by * ca;
    float speed = SPARK_SPEED_MIN + randf() * SPARK_SPEED_SPAN;
    sp.x = hx; sp.y = hy;
    sp.vx = dx * speed; sp.vy = dy * speed;
    sp.lifeMax = (gWeld.spark_life_ms * 0.001f) * (0.6f + randf() * 0.7f);
    if (sp.lifeMax < 0.001f) sp.lifeMax = 0.001f;
    sp.life = sp.lifeMax;
}

void reset() {
    sHeadPos = 0.f;
    sPingDir = 1;
    sSeekEnd = false;
    sHasTime = false;
    for (int i = 0; i < WELD_SPARK_COUNT_MAX; i++) sSparks[i].life = 0.f;
}

void seekEnd() { sSeekEnd = true; }

size_t generate(LaserPoint* out, size_t maxPts) {
    // PSRAM scratch: canvas snapshot (~9 KB) + flattened path buffer (~18 KB).
    // Rebuilt every frame -- the canvas can change at any time. generate() runs
    // only on the pattern task, so one shared buffer set is race-free.
    static const int MAXNODES = PAINT_STROKES_MAX * (PAINT_VERTS_PER_STROKE + 1);
    static PaintConfig* snapBuf = nullptr;
    static PathNode*    nodes   = nullptr;
    if (!psScratch(snapBuf, 1) || !psScratch(nodes, MAXNODES)) return 0;
    static bool tracked = false;
    if (!tracked) {
        tracked = true;
        memreg::track("Weld Scratch", sizeof(PaintConfig) + MAXNODES * sizeof(PathNode), true);
    }

    // Thread-safe snapshot (same write-tear rule as paint::generate()).
    PaintConfig& snap = *snapBuf;
    { LOCK_PAINT(); memcpy(snapBuf, &gPaint, sizeof(PaintConfig)); }
    if (snap.stroke_count == 0) return 0;

    // ── Build the arc-length path ────────────────────────────────────────────
    int   nc = 0;
    float s  = 0.f;
    float liftS[PAINT_STROKES_MAX];
    int   liftCount = 0;
    for (uint8_t st = 0; st < snap.stroke_count && st < PAINT_STROKES_MAX; st++) {
        const PaintStroke& ps = snap.strokes[st];
        if (ps.count < 2) continue;
        uint16_t n = ps.count > PAINT_VERTS_PER_STROKE ? PAINT_VERTS_PER_STROKE : ps.count;
        for (uint16_t i = 0; i < n; i++) {
            if (i > 0) { float dx = ps.x[i] - ps.x[i - 1]; float dy = ps.y[i] - ps.y[i - 1]; s += sqrtf(dx * dx + dy * dy); }
            nodes[nc].x = ps.x[i]; nodes[nc].y = ps.y[i]; nodes[nc].s = s; nodes[nc].lift = (i == 0);
            if (i == 0 && liftCount < PAINT_STROKES_MAX) liftS[liftCount++] = s;
            nc++;
        }
        if (ps.closed) {  // repeat vertex 0 as the closing node
            float dx = ps.x[0] - ps.x[n - 1]; float dy = ps.y[0] - ps.y[n - 1]; s += sqrtf(dx * dx + dy * dy);
            nodes[nc].x = ps.x[0]; nodes[nc].y = ps.y[0]; nodes[nc].s = s; nodes[nc].lift = false; nc++;
        }
    }
    if (nc < 2) return 0;
    float pathLen = nodes[nc - 1].s;
    if (pathLen <= 1.f) return 0;

    if (sSeekEnd) { sHeadPos = pathLen; sSeekEnd = false; }   // consumed after pathLen is known

    // ── Wall-clock head motion ───────────────────────────────────────────────
    uint32_t now = millis();
    float dt;
    if (!sHasTime) { dt = 0.f; sHasTime = true; }
    else { float d = (float)(now - sLastMs) * 0.001f; dt = d > FRAME_DT_MAX ? FRAME_DT_MAX : (d < 0.f ? 0.f : d); }
    sLastMs = now;

    uint8_t mode = gWeld.direction;
    int dirSign;
    if (mode == WELD_DIR_FORWARD)      dirSign = 1;
    else if (mode == WELD_DIR_REVERSE) dirSign = -1;
    else                               dirSign = (sPingDir >= 0) ? 1 : -1;   // ping-pong

    float speed = (float)gWeld.speed_units;
    float glow  = (float)gWeld.glow_units;
    if (glow < 1.f) glow = 1.f;
    sHeadPos += speed * (float)dirSign * dt;

    if (mode == WELD_DIR_PINGPONG) {
        if (sHeadPos > pathLen)      { sHeadPos = pathLen; sPingDir = -1; dirSign = -1; }
        else if (sHeadPos < 0.f)     { sHeadPos = 0.f;     sPingDir =  1; dirSign =  1; }
    } else {
        // Wrap only after running one glow length past the end, so the
        // afterglow drains off the path before the head restarts.
        if (dirSign > 0 && sHeadPos >  pathLen + glow) sHeadPos -= pathLen + glow;
        else if (dirSign < 0 && sHeadPos < -glow)      sHeadPos += pathLen + glow;
    }

    bool onPath = (sHeadPos >= 0.f && sHeadPos <= pathLen);

    // Travel direction from two samples a short arc-length apart.
    Sample sh = sampleAt(nodes, nc, pathLen, sHeadPos);
    Sample sb = sampleAt(nodes, nc, pathLen, sHeadPos - (float)dirSign * 200.f);
    float tx = 0.f, ty = 0.f;
    if (sh.valid && sb.valid) {
        float dx = sh.x - sb.x, dy = sh.y - sb.y;
        float m = sqrtf(dx * dx + dy * dy);
        if (m > 1e-3f) { tx = dx / m; ty = dy / m; }
    }

    // ── Sparks ───────────────────────────────────────────────────────────────
    float decay = 1.f - SPARK_DRAG * dt;
    if (decay < 0.f) decay = 0.f;
    for (int i = 0; i < WELD_SPARK_COUNT_MAX; i++) {
        Spark& sp = sSparks[i];
        if (i >= gWeld.spark_count) { sp.life = 0.f; continue; }  // force-kill above slider
        if (sp.life > 0.f) {
            sp.vx *= decay;
            sp.vy = sp.vy * decay - SPARK_GRAVITY * dt;
            sp.x += sp.vx * dt; sp.y += sp.vy * dt;
            sp.life -= dt;
            if (fabsf(sp.x) > COORD_LIMIT || fabsf(sp.y) > COORD_LIMIT) sp.life = 0.f;
        }
        if (sp.life <= 0.f && onPath && sh.valid) spawnSpark(sp, sh.x, sh.y, tx, ty);
    }

    // ── Assemble output geometry ─────────────────────────────────────────────
    optimizer::PathVertex tverts[TRAIL_VERTS + HEAD_DWELL_PTS];
    optimizer::PathVertex sverts[WELD_SPARK_COUNT_MAX * 2];
    optimizer::PathSegment segs[TRAIL_SEGS_MAX + WELD_SPARK_COUNT_MAX];
    int segCount = 0;

    // Trail: sample head-first, emit tail-first (draw order ends on the head).
    float  sampS[TRAIL_VERTS];
    Sample samp[TRAIL_VERTS];
    for (int k = 0; k < TRAIL_VERTS; k++) {
        float u  = (float)k / (float)(TRAIL_VERTS - 1);
        float sp = sHeadPos - (float)dirSign * u * glow;
        sampS[k] = sp;
        samp[k]  = sampleAt(nodes, nc, pathLen, sp);
    }

    int  vw = 0, runStart = 0, runLen = 0, prevValidK = -1;
    bool headWritten = false;
    for (int k = TRAIL_VERTS - 1; k >= 0; k--) {
        bool valid = samp[k].valid;
        bool cross = valid && prevValidK >= 0 && crossesLift(liftS, liftCount, sampS[prevValidK], sampS[k]);
        if (!valid || cross) {                                  // split -> flush prior run
            if (runLen >= 2 && segCount < TRAIL_SEGS_MAX)
                segs[segCount++] = optimizer::PathSegment(&tverts[runStart], runLen, false);
            runStart = vw; runLen = 0;
        }
        if (!valid) { prevValidK = -1; continue; }
        float u   = (float)k / (float)(TRAIL_VERTS - 1);
        float inv = 1.f - u;
        optimizer::PathVertex& v = tverts[vw];
        v.x = samp[k].x; v.y = samp[k].y;
        v.r = (uint8_t)((gWeld.head_r * inv + gWeld.glow_r * u) * inv);
        v.g = (uint8_t)((gWeld.head_g * inv + gWeld.glow_g * u) * inv);
        v.b = (uint8_t)((gWeld.head_b * inv + gWeld.glow_b * u) * inv);
        v.lift = false;
        prevValidK = k;
        runLen++; vw++;
        if (k == 0) headWritten = true;
    }
    // Head-dwell hot spot: repeat the head vertex at the end of the last run.
    if (headWritten && runLen >= 1) {
        for (int d = 0; d < HEAD_DWELL_PTS && vw < TRAIL_VERTS + HEAD_DWELL_PTS; d++) {
            tverts[vw] = tverts[runStart + runLen - 1];
            vw++; runLen++;
        }
    }
    if (runLen >= 2 && segCount < TRAIL_SEGS_MAX)
        segs[segCount++] = optimizer::PathSegment(&tverts[runStart], runLen, false);

    // Spark streaks: 2-vertex, first vertex lifted.
    int sv = 0;
    for (int i = 0; i < WELD_SPARK_COUNT_MAX && i < gWeld.spark_count; i++) {
        Spark& sp = sSparks[i];
        if (sp.life <= 0.f) continue;
        if (segCount >= TRAIL_SEGS_MAX + WELD_SPARK_COUNT_MAX) break;
        float frac = sp.life / sp.lifeMax;
        if (frac > 1.f) frac = 1.f; else if (frac < 0.f) frac = 0.f;
        float ageT = 1.f - frac;
        uint8_t r = (uint8_t)((gWeld.spark_r * (1.f - ageT) + gWeld.glow_r * ageT) * frac);
        uint8_t g = (uint8_t)((gWeld.spark_g * (1.f - ageT) + gWeld.glow_g * ageT) * frac);
        uint8_t b = (uint8_t)((gWeld.spark_b * (1.f - ageT) + gWeld.glow_b * ageT) * frac);
        float x0 = sp.x - sp.vx * SPARK_STREAK_S;
        float y0 = sp.y - sp.vy * SPARK_STREAK_S;
        sverts[sv]     = optimizer::PathVertex(x0, y0, r, g, b, true);
        sverts[sv + 1] = optimizer::PathVertex(sp.x, sp.y, r, g, b, false);
        segs[segCount++] = optimizer::PathSegment(&sverts[sv], 2, false);
        sv += 2;
    }

    if (segCount == 0) return 0;
    optimizer::OptimizerConfig cfg = paint::liveOptimizerConfig();
    return optimizer::optimize(segs, segCount, out, maxPts, cfg);
}

} // namespace weld
