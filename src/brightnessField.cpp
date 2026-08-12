#include "brightnessField.h"
#include "warpGrid.h"
#include <algorithm>

// Defined here (not main.cpp, unlike gZone/gWeld's usual convention) --
// same reasoning as gWarp in warpGrid.cpp: keeps this translation unit
// self-contained so it can be linked directly by the native/host optimizer
// test build without pulling in the rest of the firmware.
BrightnessConfig gBrightness;

namespace brightness {

namespace {
    bool s_cachedIdentity = true;

    inline uint8_t clampedGridSize() {
        uint8_t n = gBrightness.gridSize;
        if (n < 2) n = 2;
        if (n > WARP_GRID_MAX) n = WARP_GRID_MAX;
        return n;
    }

    bool gridIsIdentity() {
        uint8_t n = clampedGridSize();
        for (uint8_t r = 0; r < n; r++)
            for (uint8_t c = 0; c < n; c++)
                if (gBrightness.gain[r][c] != 255)
                    return false;
        return true;
    }
} // namespace

void init() {
    refresh();
}

void reset() {
    gBrightness.resetIdentity();
    refresh();
}

void refresh() {
    s_cachedIdentity = gridIsIdentity();
}

bool isIdentity() {
    return !gBrightness.enabled || s_cachedIdentity;
}

uint8_t gain(float x, float y) {
    if (isIdentity()) return 255;

    warp::GridSample s = warp::sampleGrid(clampedGridSize(), x, y);

    float g00 = gBrightness.gain[s.r0][s.c0];
    float g10 = gBrightness.gain[s.r0][s.c1];
    float g01 = gBrightness.gain[s.r1][s.c0];
    float g11 = gBrightness.gain[s.r1][s.c1];

    float top = g00 + (g10 - g00) * s.fx;
    float bot = g01 + (g11 - g01) * s.fx;
    float v   = top + (bot - top) * s.fy;

    return (uint8_t)std::max(0.0f, std::min(255.0f, v + 0.5f));
}

} // namespace brightness
