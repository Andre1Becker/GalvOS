#include "warpGrid.h"
#include <math.h>
#include <algorithm>

// Defined here (not main.cpp, unlike gZone/gWeld's usual convention) so this
// translation unit is fully self-contained -- point_optimizer.cpp's native/
// host test build links warpGrid.cpp directly without pulling in the rest of
// the firmware (see platformio.ini's [env:native] build_src_filter).
WarpConfig gWarp;

namespace warp {

namespace {
    // ±32767 = the pattern's native full-scale galvo-unit range (same space
    // as LaserPoint.x/y), used as the [-1..1] normalization reference.
    constexpr float kFullScale = 32767.0f;
    constexpr float kIdentityEps = 0.0005f;

    bool s_cachedIdentity = true;

    inline uint8_t clampedGridSize() {
        uint8_t n = gWarp.gridSize;
        if (n < 2) n = 2;
        if (n > WARP_GRID_MAX) n = WARP_GRID_MAX;
        return n;
    }

    // True if points[][][] currently equals the identity grid for gridSize.
    bool gridIsIdentity() {
        uint8_t n = clampedGridSize();
        for (uint8_t r = 0; r < n; r++) {
            for (uint8_t c = 0; c < n; c++) {
                float u = (n > 1) ? (-1.0f + (2.0f * c) / (n - 1)) : 0.0f;
                float v = (n > 1) ? (-1.0f + (2.0f * r) / (n - 1)) : 0.0f;
                if (fabsf(gWarp.points[r][c][0] - u) > kIdentityEps ||
                    fabsf(gWarp.points[r][c][1] - v) > kIdentityEps) {
                    return false;
                }
            }
        }
        return true;
    }
} // namespace

void init() {
    refresh();
}

void reset() {
    gWarp.resetIdentity();
    refresh();
}

void refresh() {
    s_cachedIdentity = gridIsIdentity();
}

bool isIdentity() {
    return !gWarp.enabled || s_cachedIdentity;
}

GridSample sampleGrid(uint8_t gridSize, float x, float y) {
    uint8_t n = gridSize;

    // Normalize into [-1..1] grid space, then into fractional cell coords
    // [0..n-1]. Outside the unit square (a point past the outermost control
    // points) is edge-clamped rather than extrapolated -- keeps both warp and
    // brightness well-defined for off-canvas geometry (e.g. scrolling text)
    // instead of diverging.
    float u = x / kFullScale;
    float v = y / kFullScale;
    float gx = (u + 1.0f) * 0.5f * (n - 1);
    float gy = (v + 1.0f) * 0.5f * (n - 1);
    gx = std::max(0.0f, std::min((float)(n - 1), gx));
    gy = std::max(0.0f, std::min((float)(n - 1), gy));

    GridSample s;
    s.c0 = (uint8_t)gx;
    s.r0 = (uint8_t)gy;
    s.c1 = (s.c0 + 1 < n) ? s.c0 + 1 : s.c0;
    s.r1 = (s.r0 + 1 < n) ? s.r0 + 1 : s.r0;
    s.fx = gx - s.c0;
    s.fy = gy - s.r0;
    return s;
}

void apply(float& x, float& y) {
    if (isIdentity()) return;

    GridSample s = sampleGrid(clampedGridSize(), x, y);

    const float* p00 = gWarp.points[s.r0][s.c0];
    const float* p10 = gWarp.points[s.r0][s.c1];
    const float* p01 = gWarp.points[s.r1][s.c0];
    const float* p11 = gWarp.points[s.r1][s.c1];

    float topX = p00[0] + (p10[0] - p00[0]) * s.fx;
    float topY = p00[1] + (p10[1] - p00[1]) * s.fx;
    float botX = p01[0] + (p11[0] - p01[0]) * s.fx;
    float botY = p01[1] + (p11[1] - p01[1]) * s.fx;

    float outU = topX + (botX - topX) * s.fy;
    float outV = topY + (botY - topY) * s.fy;

    float nx = outU * kFullScale;
    float ny = outV * kFullScale;
    x = std::max(-32767.0f, std::min(32767.0f, nx));
    y = std::max(-32767.0f, std::min(32767.0f, ny));
}

void apply(LaserPoint& p) {
    if (isIdentity()) return;
    float x = p.x, y = p.y;
    apply(x, y);
    p.x = (int16_t)lroundf(x);
    p.y = (int16_t)lroundf(y);
}

} // namespace warp
