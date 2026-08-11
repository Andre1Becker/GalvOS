#include "paint_patterns.h"
#include "point_optimizer.h"
#include "mutex.h"
#include "util/mem_registry.h"
#include "util/ps_scratch.h"

namespace paint {

// liveOptimizerConfig() -- the shared live->optimizer mapping, with no
// specialization: paint strokes are user-drawn geometry and take the live
// settings exactly as configured (see optimizer::configFromLive()).
static inline optimizer::OptimizerConfig liveOptimizerConfig() {
    return optimizer::configFromLive(gOptimizerConfig,
                                     gProjection.galvo_rated_kpps,
                                     gProjection.galvo_kpps);
}

size_t generate(LaserPoint* out, size_t max_pts) {
    // PSRAM scratch: snapshot (~9 KB -- previously a stack local eating most
    // of the pattern task's 12 KB stack) + vertex scratch (~14 KB -- was
    // DRAM .bss). generate() only runs on the pattern task, so one shared
    // snapshot buffer is race-free.
    typedef optimizer::PathVertex VertRow[PAINT_VERTS_PER_STROKE];
    static PaintConfig* snapBuf = nullptr;
    static VertRow*     verts   = nullptr;
    if (!psScratch(snapBuf, 1) || !psScratch(verts, PAINT_STROKES_MAX)) return 0;
    static bool tracked = false;
    if (!tracked) {
        tracked = true;
        memreg::track("Paint Scratch", sizeof(PaintConfig) +
                      PAINT_STROKES_MAX * sizeof(VertRow), true);
    }

    // Thread-safe snapshot: pattern task (Core 1) reads while /api/paint/set
    // (Core 0) may write -- copy the whole canvas under mtx::paint so a
    // partial HTTP-body write can never be read mid-tear (same fix pattern
    // used for gZone).
    PaintConfig& snap = *snapBuf;
    { LOCK_PAINT(); memcpy(snapBuf, &gPaint, sizeof(PaintConfig)); }
    if (snap.stroke_count == 0) return 0;

    optimizer::PathSegment segs[PAINT_STROKES_MAX];
    uint8_t segCount = 0;

    for (uint8_t s = 0; s < snap.stroke_count && s < PAINT_STROKES_MAX; s++) {
        const PaintStroke& st = snap.strokes[s];
        if (st.count < 2) continue;
        uint16_t n = st.count > PAINT_VERTS_PER_STROKE ? PAINT_VERTS_PER_STROKE : st.count;
        for (uint16_t i = 0; i < n; i++) {
            verts[segCount][i] = optimizer::PathVertex(st.x[i], st.y[i], st.r, st.g, st.b, i == 0);
        }
        segs[segCount] = optimizer::PathSegment(verts[segCount], n, st.closed);
        segCount++;
    }
    if (segCount == 0) return 0;
    return optimizer::optimize(segs, segCount, out, max_pts, liveOptimizerConfig());
}

} // namespace paint
