#include "weld_patterns.h"
#include "weld_path.h"
#include "paint_patterns.h"
#include "text_renderer.h"
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

// One spark particle (ballistic, drag + gravity).
struct Spark { float x, y, vx, vy, life, lifeMax; };

// ── Persistent effect state (pattern task only -- single-threaded) ───────────
// Shared across both path sources (Paint canvas / Text) -- only one of them
// is ever the active render source at a time (Text Mode outranks Paint Mode
// in pattern_engine.cpp), so one travelling-head state is correct, same as
// it already tolerates the Paint canvas' own geometry changing under it
// (edits, undo/redo) without a dedicated reset.
static float    sHeadPos = 0.f;     // arc-length position of the torch head
static int      sPingDir = 1;       // ping-pong travel sign (+1 / -1)
static bool     sSeekEnd = false;   // one-shot: start from the far end
static bool     sHasTime = false;   // millis() baseline captured yet
static uint32_t sLastMs  = 0;
static Spark    sSparks[WELD_SPARK_COUNT_MAX];
static bool     sTextTruncated = false;

// Small xorshift32 PRNG (deterministic, no rand()).
static uint32_t sRng = 0x1234567u;
static inline uint32_t xorshift32() {
    uint32_t x = sRng; x ^= x << 13; x ^= x >> 17; x ^= x << 5; sRng = x; return x;
}
static inline float randf() { return (float)(xorshift32() >> 8) * (1.0f / 16777216.0f); } // [0,1)
static inline float rand2() { return randf() * 2.0f; }                                     // [0,2)

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

bool textWasTruncated() { return sTextTruncated; }

// ── Shared torch/afterglow/spark renderer ────────────────────────────────────
// Walks the already-flattened path (`nodes`/`liftS`/`pathLen`, built by
// buildArcLengthPath() from whichever source called in) and assembles the
// same torch-head + afterglow + spark geometry regardless of source.
static size_t renderTorch(const PathNode* nodes, size_t nc,
                           const float* liftS, size_t liftCount, float pathLen,
                           LaserPoint* out, size_t maxPts,
                           const optimizer::OptimizerConfig& optCfg) {
    if (nc < 2 || pathLen <= 1.f) return 0;

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
    return optimizer::optimize(segs, segCount, out, maxPts, optCfg);
}

size_t generate(LaserPoint* out, size_t maxPts) {
    // PSRAM scratch: canvas snapshot (~9 KB) + flattened path buffer (~18 KB).
    // Rebuilt every frame -- the canvas can change at any time. generate() runs
    // only on the pattern task, so one shared buffer set is race-free.
    static const size_t MAXNODES = PAINT_STROKES_MAX * (PAINT_VERTS_PER_STROKE + 1);
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

    SourceStroke srcStrokes[PAINT_STROKES_MAX];
    size_t srcCount = 0;
    for (uint8_t st = 0; st < snap.stroke_count && st < PAINT_STROKES_MAX; st++) {
        const PaintStroke& ps = snap.strokes[st];
        if (ps.count < 2) continue;
        srcStrokes[srcCount].x      = ps.x;
        srcStrokes[srcCount].y      = ps.y;
        srcStrokes[srcCount].count  = ps.count > PAINT_VERTS_PER_STROKE ? PAINT_VERTS_PER_STROKE : ps.count;
        srcStrokes[srcCount].closed = ps.closed;
        srcCount++;
    }
    if (srcCount == 0) return 0;

    float  liftS[PAINT_STROKES_MAX];
    size_t liftCount = 0;
    float  pathLen   = 0.f;
    size_t nc = buildArcLengthPath(srcStrokes, srcCount, nodes, MAXNODES,
                                   liftS, PAINT_STROKES_MAX, liftCount, pathLen);
    if (nc == 0) return 0;

    return renderTorch(nodes, nc, liftS, liftCount, pathLen, out, maxPts, paint::liveOptimizerConfig());
}

size_t generateText(const TextConfig& cfg, LaserPoint* out, size_t maxPts) {
    if (!cfg.text[0]) return 0;

    // PSRAM scratch: raw glyph outline sub-paths + flattened path buffer.
    // Rebuilt every frame -- same race-free single-task rule as generate().
    static const size_t MAXNODES = textrender::TEXT_VERTICES_MAX_PATHS *
                                    (textrender::GlyphSubpath::MAX_PTS + 1);
    static textrender::GlyphSubpath* glyphs = nullptr;
    static PathNode*                 nodes  = nullptr;
    if (!psScratch(glyphs, textrender::TEXT_VERTICES_MAX_PATHS) || !psScratch(nodes, MAXNODES)) return 0;
    static bool tracked = false;
    if (!tracked) {
        tracked = true;
        memreg::track("Weld Text Scratch",
                      textrender::TEXT_VERTICES_MAX_PATHS * sizeof(textrender::GlyphSubpath) +
                      MAXNODES * sizeof(PathNode), true);
    }

    // Static glyph-outline layout at the Text tab's own Size mapping --
    // Welding supplies the path's motion, so the string's own animation mode
    // never runs here (see weld_patterns.h's generateText() doc comment).
    float  scale = textrender::sizeToScale(cfg.size_val);
    size_t pathCount = textrender::glyphOutlinePaths(cfg.text, scale, glyphs,
                                                      textrender::TEXT_VERTICES_MAX_PATHS);
    if (pathCount == 0) return 0;
    // TEXT_VERTICES_MAX_PATHS holds every sub-path glyphOutlinePaths() could
    // produce before it silently stops adding more (see its own doc comment)
    // -- landing exactly on the cap is the only observable signal that the
    // string ran longer than the buffer, mirrors textrender::wasTruncated().
    sTextTruncated = (pathCount >= textrender::TEXT_VERTICES_MAX_PATHS);

    SourceStroke srcStrokes[textrender::TEXT_VERTICES_MAX_PATHS];
    for (size_t i = 0; i < pathCount; i++) {
        srcStrokes[i].x      = glyphs[i].x;
        srcStrokes[i].y      = glyphs[i].y;
        srcStrokes[i].count  = glyphs[i].count;
        srcStrokes[i].closed = false;   // glyph strokes are pen paths, not filled regions
    }

    float  liftS[textrender::TEXT_VERTICES_MAX_PATHS];
    size_t liftCount = 0;
    float  pathLen   = 0.f;
    size_t nc = buildArcLengthPath(srcStrokes, pathCount, nodes, MAXNODES,
                                   liftS, textrender::TEXT_VERTICES_MAX_PATHS, liftCount, pathLen);
    if (nc == 0) return 0;

    return renderTorch(nodes, nc, liftS, liftCount, pathLen, out, maxPts, paint::liveOptimizerConfig());
}

} // namespace weld
