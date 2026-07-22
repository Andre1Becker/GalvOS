#include "paint_patterns.h"
#include "point_optimizer.h"
#include "mutex.h"
#include "util/mem_registry.h"
#include "util/ps_scratch.h"

namespace paint {

// liveOptimizerConfig() -- converts the WebUI-tunable OptimizerLiveConfig
// into the optimizer module's own OptimizerConfig type. Mirrors the
// translation-unit-local helper of the same name in preset_patterns.cpp
// (kept as a static duplicate there too -- see point_optimizer.h header
// comment on why OptimizerConfig is passed explicitly rather than global).
static inline optimizer::OptimizerConfig liveOptimizerConfig() {
    optimizer::OptimizerConfig cfg;
    cfg.corner_angle_deg             = gOptimizerConfig.corner_angle_deg;
    cfg.min_corner_pts               = gOptimizerConfig.min_corner_pts;
    cfg.max_corner_pts               = gOptimizerConfig.max_corner_pts;
    cfg.pts_per_1000_units           = gOptimizerConfig.pts_per_1000_units;
    cfg.min_segment_pts              = gOptimizerConfig.min_segment_pts;
    cfg.blank_samples                = gOptimizerConfig.blank_samples;
    cfg.max_pts_per_frame            = gOptimizerConfig.max_pts_per_frame;
    cfg.min_blank_samples            = gOptimizerConfig.min_blank_samples;
    cfg.blank_pts_per_1000_units     = gOptimizerConfig.blank_pts_per_1000_units;
    cfg.min_interior_pts_per_segment = gOptimizerConfig.min_interior_pts_per_segment;
    cfg.stage1_blank_target          = gOptimizerConfig.stage1_blank_target;
    cfg.resample_enabled             = gOptimizerConfig.resample_enabled;
    cfg.resample_spacing_units       = gOptimizerConfig.resample_spacing_units;
    cfg.ringing_comp_enabled         = gOptimizerConfig.ringing_comp_enabled;
    cfg.ring_freq_hz                 = gOptimizerConfig.ring_freq_hz;
    cfg.ring_damping_ratio           = gOptimizerConfig.ring_damping_ratio;
    cfg.galvo_kpps                   = gProjection.galvo_kpps;
    cfg.transform                    = optimizer::gLiveTransform;  // Phase 3: live Z-rot + move
    cfg.vel_clamp_enabled            = gOptimizerConfig.vel_clamp_enabled;
    cfg.max_step_units               = gOptimizerConfig.max_step_units;
    cfg.accel_clamp_enabled          = gOptimizerConfig.accel_clamp_enabled;
    cfg.max_accel_units              = gOptimizerConfig.max_accel_units;
    // PPS-derived scaling: density + both clamps from rated/output kpps.
    optimizer::applyPpsScaling(cfg, gProjection.galvo_rated_kpps, gProjection.galvo_kpps);
    return cfg;
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
