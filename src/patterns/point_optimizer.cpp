#include "point_optimizer.h"
#include <math.h>
#include <algorithm>

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

// Number of points to place at a corner, scaled by severity between
// cfg.min_corner_pts (at/under threshold) and cfg.max_corner_pts (180°).
static uint8_t cornerPointCount(float exterior_angle_rad,
                                 const OptimizerConfig& cfg) {
    float angle_deg = exterior_angle_rad * (180.0f / PI_F);
    if (angle_deg <= cfg.corner_angle_deg) return cfg.min_corner_pts;
    // Linear ramp from threshold..180 -> min_corner_pts..max_corner_pts
    float span = 180.0f - cfg.corner_angle_deg;
    float t = (span > 0.01f)
                  ? (angle_deg - cfg.corner_angle_deg) / span
                  : 1.0f;
    t = std::max(0.0f, std::min(1.0f, t));
    float pts = cfg.min_corner_pts +
                t * (cfg.max_corner_pts - cfg.min_corner_pts);
    return (uint8_t)lroundf(pts);
}

// Interior (non-endpoint) sample count for one straight edge of the
// given length, before the corner points at either end.
static uint16_t edgeInteriorCount(float length, const OptimizerConfig& cfg) {
    float raw = (length / 1000.0f) * cfg.pts_per_1000_units;
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
// given pts_per_1000_units scale, without writing any output.
static uint16_t planSegment(const PathSegment& seg, const OptimizerConfig& cfg) {
    if (seg.count == 0) return 0;
    if (seg.count == 1) return 1;  // single isolated point

    size_t edge_count = seg.closed ? seg.count : (seg.count - 1);
    uint32_t total = 0;

    for (size_t i = 0; i < seg.count; i++) {
        // Corner point count at vertex i (only meaningful if this vertex
        // has both an incoming and outgoing edge -- true for all vertices
        // on a closed path, and for interior vertices on an open path).
        bool has_incoming = seg.closed || i > 0;
        bool has_outgoing = seg.closed || i + 1 < seg.count;
        uint8_t cpts = cfg.min_corner_pts;
        if (has_incoming && has_outgoing) {
            size_t pi = (i == 0) ? seg.count - 1 : i - 1;
            size_t ni = (i + 1) % seg.count;
            float ang = exteriorAngle(seg.vertices[pi].x, seg.vertices[pi].y,
                                       seg.vertices[i].x, seg.vertices[i].y,
                                       seg.vertices[ni].x, seg.vertices[ni].y);
            cpts = cornerPointCount(ang, cfg);
        }
        total += cpts;
    }

    for (size_t e = 0; e < edge_count; e++) {
        size_t a = e, b = (e + 1) % seg.count;
        float dx = seg.vertices[b].x - seg.vertices[a].x;
        float dy = seg.vertices[b].y - seg.vertices[a].y;
        float len = sqrtf(dx * dx + dy * dy);
        total += edgeInteriorCount(len, cfg);
    }

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
        bool has_incoming = seg.closed || a > 0;
        bool has_outgoing = seg.closed || a + 1 < seg.count;
        uint8_t cpts = cfg.min_corner_pts;
        if (has_incoming && has_outgoing) {
            size_t pi = (a == 0) ? seg.count - 1 : a - 1;
            float ang = exteriorAngle(seg.vertices[pi].x, seg.vertices[pi].y,
                                       va.x, va.y, vb.x, vb.y);
            cpts = cornerPointCount(ang, cfg);
        }
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
        for (uint16_t k = 1; k <= ipts; k++) {
            float t = (float)k / (ipts + 1);
            emit(out, n, max, va.x + dx * t, va.y + dy * t,
                 vb.r, vb.g, vb.b, 0);
        }
    }

    // Final vertex of an open path needs its own corner point(s) --
    // closed paths already covered the last vertex as the "a" of the
    // wrap-around edge above.
    if (!seg.closed) {
        const PathVertex& vlast = seg.vertices[seg.count - 1];
        emit(out, n, max, vlast.x, vlast.y, vlast.r, vlast.g, vlast.b, 0);
    }
}

// ── public entry point ───────────────────────────────────────────────────

size_t optimize(const PathSegment* segments, size_t segment_count,
                 LaserPoint* out, size_t max_out,
                 const OptimizerConfig& cfg_in) {
    if (segment_count == 0 || max_out == 0) return 0;

    OptimizerConfig cfg = cfg_in;

    // Budget check: plan at the requested density first.
    uint32_t planned_total = 0;
    for (size_t s = 0; s < segment_count; s++)
        planned_total += planSegment(segments[s], cfg);
    // Reserve room for inter-segment blank jumps (one per segment after
    // the first) plus the final closing blank back to the first point.
    uint32_t blank_overhead = (uint32_t)cfg.blank_samples * segment_count;
    uint32_t needed = planned_total + blank_overhead;

    if (needed > max_out && planned_total > 0) {
        // Scale pts_per_1000_units down once so the whole pass fits.
        // (Corner points are not scaled -- they're capped at
        // max_corner_pts already and matter most for tracking accuracy;
        // shaving interior density first is the same trade-off the old
        // adaptN()-based per-pattern tuning made.)
        float available = (float)max_out - (float)blank_overhead;
        if (available < (float)segment_count * cfg.min_segment_pts)
            available = (float)segment_count * cfg.min_segment_pts;
        float scale = available / (float)planned_total;
        cfg.pts_per_1000_units = std::max(0.1f, cfg.pts_per_1000_units * scale);
    }

    size_t n = 0;
    for (size_t s = 0; s < segment_count; s++) {
        const PathSegment& seg = segments[s];
        if (seg.count == 0) continue;

        // Blank jump to this segment's first vertex.
        emitBlankRun(out, n, max_out, seg.vertices[0].x, seg.vertices[0].y,
                     cfg.blank_samples);

        emitSegment(seg, cfg, out, n, max_out);
    }

    // Closing blank: return to the very first point with the laser off,
    // so the next frame doesn't start with a lit retrace -- same purpose
    // as the existing per-pattern closing-blank convention in
    // preset_patterns.cpp (ngon()/star()).
    if (n > 0 && segment_count > 0 && segments[0].count > 0) {
        emitBlankRun(out, n, max_out,
                     segments[0].vertices[0].x, segments[0].vertices[0].y,
                     cfg.blank_samples);
    }

    return n;
}

}  // namespace optimizer