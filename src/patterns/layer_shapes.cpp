#include "layer_shapes.h"
#include "point_optimizer.h"
#include "../modulator_engine.h"
#include <Arduino.h>
#include <math.h>

namespace layer_shapes {

static constexpr float PI2 = 2.0f * (float)M_PI;
static constexpr float SC  = 18000.0f;   // base radius, matches preset_patterns.cpp's SC
static constexpr int   VERTS_MAX = 256;  // scratch capacity, well above any Phase-1 layer budget

// Static (BSS) scratch, not stack -- same rationale as pattern_engine.cpp's
// s_frame: this is called from patterns::task (Core 0, PSRAM-eligible but
// small enough here to leave in internal DRAM alongside the rest of this
// file's tiny footprint).
static optimizer::PathVertex s_verts[VERTS_MAX];

// Local equivalent of preset_patterns.cpp's liveOptimizerConfig() (that one
// is TU-static, unreachable from here) -- same field-for-field copy from the
// same public globals, so Layer-mode shapes get identical corner/density/
// budget behavior to every existing preset.
static optimizer::OptimizerConfig liveCfg() {
    optimizer::OptimizerConfig cfg;
    cfg.corner_angle_deg   = gOptimizerConfig.corner_angle_deg;
    cfg.min_corner_pts     = gOptimizerConfig.min_corner_pts;
    cfg.max_corner_pts     = gOptimizerConfig.max_corner_pts;
    cfg.pts_per_1000_units = gOptimizerConfig.pts_per_1000_units;
    cfg.min_segment_pts    = gOptimizerConfig.min_segment_pts;
    cfg.blank_samples      = gOptimizerConfig.blank_samples;
    cfg.max_pts_per_frame  = gOptimizerConfig.max_pts_per_frame;
    cfg.min_blank_samples  = gOptimizerConfig.min_blank_samples;
    cfg.blank_pts_per_1000_units = gOptimizerConfig.blank_pts_per_1000_units;
    cfg.min_interior_pts_per_segment = gOptimizerConfig.min_interior_pts_per_segment;
    cfg.stage1_blank_target = gOptimizerConfig.stage1_blank_target;
    cfg.resample_enabled       = gOptimizerConfig.resample_enabled;
    cfg.resample_spacing_units = gOptimizerConfig.resample_spacing_units;
    cfg.ringing_comp_enabled = gOptimizerConfig.ringing_comp_enabled;
    cfg.ring_freq_hz         = gOptimizerConfig.ring_freq_hz;
    cfg.ring_damping_ratio   = gOptimizerConfig.ring_damping_ratio;
    cfg.galvo_kpps           = gProjection.galvo_kpps;
    cfg.vel_clamp_enabled    = gOptimizerConfig.vel_clamp_enabled;
    cfg.max_step_units       = gOptimizerConfig.max_step_units;
    cfg.accel_clamp_enabled  = gOptimizerConfig.accel_clamp_enabled;
    cfg.max_accel_units      = gOptimizerConfig.max_accel_units;
    optimizer::applyPpsScaling(cfg, gProjection.galvo_rated_kpps, gProjection.galvo_kpps);
    cfg.pts_per_1000_units *= modulator::apply(modulator::ModTarget::OPT_DENSITY, 1.0f);
    // Transform (per-layer scale/shift/rotation) is set by the caller in
    // pattern_engine.cpp, not here -- Shape generation stays origin-centered
    // and transform-agnostic so the Transform panel composes cleanly on top.
    return cfg;
}

static size_t emit(LaserPoint* o, size_t mx, int count, bool closed) {
    if (count < 2) return 0;
    optimizer::PathSegment seg(s_verts, (size_t)count, closed);
    return optimizer::optimize(&seg, 1, o, mx, liveCfg());
}

static size_t genPolygon(int sides, float rx, float ry, LaserPoint* o, size_t mx,
                          uint8_t r, uint8_t g, uint8_t b) {
    sides = constrain(sides, 3, VERTS_MAX);
    for (int i = 0; i < sides; i++) {
        float a = PI2 * i / (float)sides;
        s_verts[i] = optimizer::PathVertex(cosf(a) * rx, sinf(a) * ry, r, g, b, false);
    }
    return emit(o, mx, sides, true);
}

static size_t genStar(int points, float rx, float ry, LaserPoint* o, size_t mx,
                       uint8_t r, uint8_t g, uint8_t b) {
    points = constrain(points, 3, VERTS_MAX / 2);
    const int n = points * 2;
    const float inner = 0.45f;  // fixed inner/outer ratio, matches preset_patterns.cpp's star()
    for (int i = 0; i < n; i++) {
        float a = PI2 * i / (float)n - (float)(M_PI / 2);
        float k = (i % 2 == 0) ? 1.0f : inner;
        s_verts[i] = optimizer::PathVertex(cosf(a) * rx * k, sinf(a) * ry * k, r, g, b, false);
    }
    return emit(o, mx, n, true);
}

static size_t genWave(float rx, float ry, LaserPoint* o, size_t mx,
                       uint8_t r, uint8_t g, uint8_t b) {
    const int n = 96;
    for (int i = 0; i < n; i++) {
        float t = (float)i / (float)(n - 1);            // 0..1 across full width
        float x = (t * 2.0f - 1.0f) * rx;
        float y = sinf(t * PI2 * 2.0f) * ry;             // 2 full cycles
        s_verts[i] = optimizer::PathVertex(x, y, r, g, b, false);
    }
    return emit(o, mx, n, false);
}

static size_t genRose(int k, float rx, float ry, LaserPoint* o, size_t mx,
                       uint8_t r, uint8_t g, uint8_t b) {
    k = constrain(k, 1, 12);
    const int n = 200;
    // Odd k closes after PI, even k needs the full 2*PI sweep to trace all petals.
    float sweep = (k % 2 == 0) ? PI2 : (float)M_PI;
    for (int i = 0; i < n; i++) {
        float t = sweep * i / (float)n;
        float rad = cosf(k * t);
        s_verts[i] = optimizer::PathVertex(cosf(t) * rad * rx, sinf(t) * rad * ry, r, g, b, false);
    }
    return emit(o, mx, n, true);
}

static size_t genSpiral(int turns, float rx, float ry, LaserPoint* o, size_t mx,
                         uint8_t r, uint8_t g, uint8_t b) {
    turns = constrain(turns, 1, 8);
    const int n = 32 * turns;
    for (int i = 0; i < n; i++) {
        float t = (float)i / (float)(n - 1);             // 0..1
        float a = t * PI2 * turns;
        s_verts[i] = optimizer::PathVertex(cosf(a) * t * rx, sinf(a) * t * ry, r, g, b, false);
    }
    return emit(o, mx, n, false);
}

// Hypotrochoid, same formula/normalization as preset_patterns.cpp's p53
// (R=6, r=1, d=3 -- proven to trace a clean closed rosette with no
// self-crossing through the center). `lobes` varies r, which changes the
// petal count while keeping R and the peak-radius normalization fixed.
static size_t genSpirograph(int lobes, float rx, float ry, LaserPoint* o, size_t mx,
                             uint8_t r, uint8_t g, uint8_t b) {
    lobes = constrain(lobes, 1, 5);
    const float R = 6.0f, rr = (float)lobes, d = 3.0f;
    const float peakNorm = 1.0f / ((R - rr) + d);
    const int n = 220;
    for (int i = 0; i < n; i++) {
        float t = PI2 * i / (float)n;
        float x = (R - rr) * cosf(t) + d * cosf((R - rr) * t / rr);
        float y = (R - rr) * sinf(t) - d * sinf((R - rr) * t / rr);
        s_verts[i] = optimizer::PathVertex(x * peakNorm * rx, y * peakNorm * ry, r, g, b, false);
    }
    return emit(o, mx, n, true);
}

// Rounded-petal polar rosette: r = (1+cos(k*theta))/2 stays non-negative
// (unlike the classic rose curve), giving smooth round petals -- visually
// distinct from Rose (ShapeType::Rose) which can self-cross at the center.
static size_t genRosette(int k, float rx, float ry, LaserPoint* o, size_t mx,
                          uint8_t r, uint8_t g, uint8_t b) {
    k = constrain(k, 2, 10);
    const int n = 220;
    for (int i = 0; i < n; i++) {
        float t = PI2 * i / (float)n;
        float rad = 0.5f * (1.0f + cosf(k * t));
        s_verts[i] = optimizer::PathVertex(cosf(t) * rad * rx, sinf(t) * rad * ry, r, g, b, false);
    }
    return emit(o, mx, n, true);
}

// Concentric rings, same technique as preset_patterns.cpp's p56 (Concentric
// Rings): one optimize() call per ring, accumulating into `o`.
static size_t genTunnel(int rings, float rx, float ry, LaserPoint* o, size_t mx,
                         uint8_t r, uint8_t g, uint8_t b) {
    rings = constrain(rings, 3, 8);
    size_t n = 0;
    for (int ring = 1; ring <= rings; ring++) {
        float f = (float)ring / (float)rings;
        int sides = 32;
        for (int i = 0; i < sides; i++) {
            float a = PI2 * i / (float)sides;
            s_verts[i] = optimizer::PathVertex(cosf(a) * rx * f, sinf(a) * ry * f, r, g, b, false);
        }
        if (n >= mx) break;
        n += emit(o + n, mx - n, sides, true);
    }
    return n;
}

size_t generate(ShapeType type, uint8_t param, float scaleX, float scaleY,
                 LaserPoint* out, size_t maxOut, uint8_t r, uint8_t g, uint8_t b) {
    if (!out || maxOut == 0) return 0;
    float rx = SC * constrain(scaleX, 0.05f, 1.0f);
    float ry = SC * constrain(scaleY, 0.05f, 1.0f);
    switch (type) {
        case ShapeType::Circle:         return genPolygon(64, rx, ry, out, maxOut, r, g, b);
        case ShapeType::Square:         return genPolygon(4,  rx, ry, out, maxOut, r, g, b);
        case ShapeType::Polygon:        return genPolygon(param, rx, ry, out, maxOut, r, g, b);
        case ShapeType::Star:           return genStar(param, rx, ry, out, maxOut, r, g, b);
        case ShapeType::Wave:           return genWave(rx, ry, out, maxOut, r, g, b);
        case ShapeType::Rose:           return genRose(param, rx, ry, out, maxOut, r, g, b);
        case ShapeType::Spiral:         return genSpiral(param, rx, ry, out, maxOut, r, g, b);
        case ShapeType::Spirograph:     return genSpirograph(param, rx, ry, out, maxOut, r, g, b);
        case ShapeType::Rosette:        return genRosette(param, rx, ry, out, maxOut, r, g, b);
        case ShapeType::WaveformTunnel: return genTunnel(param, rx, ry, out, maxOut, r, g, b);
        default: return 0;
    }
}

const char* shapeName(ShapeType type) {
    switch (type) {
        case ShapeType::Circle:         return "Circle";
        case ShapeType::Square:         return "Square";
        case ShapeType::Polygon:        return "Polygon";
        case ShapeType::Star:           return "Star";
        case ShapeType::Wave:           return "Wave";
        case ShapeType::Rose:           return "Rose";
        case ShapeType::Spiral:         return "Spiral";
        case ShapeType::Spirograph:     return "Spirograph";
        case ShapeType::Rosette:        return "Rosette";
        case ShapeType::WaveformTunnel: return "Waveform Tunnel";
        default: return "?";
    }
}

}  // namespace layer_shapes
