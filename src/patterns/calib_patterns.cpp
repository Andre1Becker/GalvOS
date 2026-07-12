/**
 * calib_patterns.cpp -- calibration and color-gradient test patterns
 *
 * IMPORTANT: all patterns apply gamma LUT + white balance.
 *
 * Coordinates: +-32767 (ILDA default, 16-bit signed)
 * SC = 32767 * 0.88 = 28834 (laesst 6% Rand)
 */
#include "calib_patterns.h"
#include "config.h"
#include "point_optimizer.h"
#include <math.h>
#include <string.h>
#include <Arduino.h>

namespace calib_patterns {

// ── constants ────────────────────────────────────────────────
static constexpr float PI2  = 6.2831853f;
static constexpr float SC   = 28000.0f;  // ±88% full deflection

// ── helper functions ──────────────────────────────────────────

// Gamma LUT (γ=2.2) -- identical to galvo_out.cpp
// Applied if gamma_enable=true, otherwise linear pass-through
static inline uint8_t applyGamma(uint8_t v) {
    if (!gConfig.gamma_enable) return v;
    // Inline LUT (copy from galvo_out.cpp -- no cross-include needed)
    static const uint8_t LUT[256] = {
          0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  1,
          1,  1,  1,  1,  1,  1,  1,  1,  1,  2,  2,  2,  2,  2,  2,  2,
          3,  3,  3,  3,  3,  4,  4,  4,  4,  5,  5,  5,  5,  6,  6,  6,
          6,  7,  7,  7,  8,  8,  8,  9,  9,  9, 10, 10, 11, 11, 11, 12,
         12, 13, 13, 13, 14, 14, 15, 15, 16, 16, 17, 17, 18, 18, 19, 19,
         20, 20, 21, 22, 22, 23, 23, 24, 25, 25, 26, 26, 27, 28, 28, 29,
         30, 30, 31, 32, 33, 33, 34, 35, 35, 36, 37, 38, 39, 39, 40, 41,
         42, 43, 43, 44, 45, 46, 47, 48, 49, 49, 50, 51, 52, 53, 54, 55,
         56, 57, 58, 59, 60, 61, 62, 63, 64, 65, 66, 67, 68, 69, 70, 71,
         73, 74, 75, 76, 77, 78, 79, 81, 82, 83, 84, 85, 87, 88, 89, 90,
         91, 93, 94, 95, 97, 98, 99,100,102,103,105,106,107,109,110,111,
        113,114,116,117,119,120,121,123,124,126,127,129,130,132,133,135,
        137,138,140,141,143,145,146,148,149,151,153,154,156,158,159,161,
        163,165,166,168,170,172,173,175,177,179,181,182,184,186,188,190,
        192,194,196,197,199,201,203,205,207,209,211,213,215,217,219,221,
        223,225,227,229,231,234,236,238,240,242,244,246,248,251,253,255
    };
    return LUT[v];
}

// apply white balance + gamma to RGB triple
static inline void colorOut(uint8_t ri, uint8_t gi, uint8_t bi,
                             uint8_t bright,
                             uint8_t& ro, uint8_t& go, uint8_t& bo) {
    ro = applyGamma((uint8_t)(((uint32_t)ri * bright * gConfig.gain_r) / (255UL * 255)));
    go = applyGamma((uint8_t)(((uint32_t)gi * bright * gConfig.gain_g) / (255UL * 255)));
    bo = applyGamma((uint8_t)(((uint32_t)bi * bright * gConfig.gain_b) / (255UL * 255)));
}

// add point (with bounds check)
static inline void ap(LaserPoint* o, size_t& n, size_t mx,
                       float x, float y,
                       uint8_t r, uint8_t g, uint8_t b,
                       uint8_t blank = 0) {
    if (n >= mx) return;
    o[n] = {
        (int16_t)constrain(x, -32767.f, 32767.f),
        (int16_t)constrain(y, -32767.f, 32767.f),
        r, g, b, blank
    };
    n++;
}

// blank move: interpolate NSTEPS blanked points from last point to (x1,y1)
static void blankMove(LaserPoint* o, size_t& n, size_t mx,
                       float x1, float y1) {
    if (n == 0) {
        ap(o, n, mx, x1, y1, 0, 0, 0, 1);
        return;
    }
    float x0 = o[n-1].x;
    float y0 = o[n-1].y;
    float dx = x1 - x0, dy = y1 - y0;
    float dist = sqrtf(dx*dx + dy*dy);
    int steps = (int)(dist / 1000.f + 0.5f);
    if (steps < 4)  steps = 4;
    if (steps > 20) steps = 20;
    for (int i = 1; i <= steps; i++) {
        float t = (float)i / steps;
        ap(o, n, mx, x0 + dx*t, y0 + dy*t, 0, 0, 0, 1);
    }
}

// draw line (N interpolated points)
static void line(LaserPoint* o, size_t& n, size_t mx,
                  float x0, float y0, float x1, float y1,
                  uint8_t r, uint8_t g, uint8_t b,
                  int steps = 30) {
    for (int i = 0; i <= steps; i++) {
        float t = (float)i / steps;
        ap(o, n, mx,
           x0 + (x1-x0)*t, y0 + (y1-y0)*t,
           r, g, b, i == 0 ? 1 : 0);
    }
}

// HSV → RGB (h=0-360, s=0-255, v=0-255)
static void hsv2rgb(float h, uint8_t s, uint8_t v,
                    uint8_t& r, uint8_t& g, uint8_t& b) {
    if (s == 0) { r = g = b = v; return; }
    h = fmodf(h, 360.0f);
    float H = h / 60.0f;
    int   i = (int)H;
    float f = H - i;
    float sf = s / 255.0f;
    uint8_t p = (uint8_t)(v * (1.0f - sf));
    uint8_t q = (uint8_t)(v * (1.0f - sf * f));
    uint8_t t2 = (uint8_t)(v * (1.0f - sf * (1.0f - f)));
    switch (i % 6) {
        case 0: r=v;  g=t2; b=p;  break;
        case 1: r=q;  g=v;  b=p;  break;
        case 2: r=p;  g=v;  b=t2; break;
        case 3: r=p;  g=q;  b=v;  break;
        case 4: r=t2; g=p;  b=v;  break;
        default:r=v;  g=p;  b=q;  break;
    }
}

// ══════════════════════════════════════════════════════════════
// PATTERN 0: GAMMA-RAMPE
// ══════════════════════════════════════════════════════════════
static size_t gamma_ramp(LaserPoint* o, size_t mx,
                          uint32_t phase, uint8_t bright, uint8_t ch) {
    size_t n = 0;
    const int STEPS = 60;
    const float Y_TOP =  SC * 0.55f;
    const float Y_BOT = -SC * 0.55f;

    for (int i = 0; i <= STEPS; i++) {
        float t = (float)i / STEPS;
        float x = -SC * 0.88f + t * SC * 1.76f;
        uint8_t v = (uint8_t)(t * 255.0f);
        uint8_t ri = (ch==0||ch==1) ? v : 0;
        uint8_t gi = (ch==0||ch==2) ? v : 0;
        uint8_t bi = (ch==0||ch==3) ? v : 0;
        uint8_t ro, go, bo;
        colorOut(ri, gi, bi, bright, ro, go, bo);
        if (i == 0) blankMove(o, n, mx, x, Y_TOP);
        ap(o, n, mx, x, Y_TOP, ro, go, bo, 0);
        uint8_t ro2 = (uint8_t)(((uint32_t)ri * bright * gConfig.gain_r) / (255UL*255));
        uint8_t go2 = (uint8_t)(((uint32_t)gi * bright * gConfig.gain_g) / (255UL*255));
        uint8_t bo2 = (uint8_t)(((uint32_t)bi * bright * gConfig.gain_b) / (255UL*255));
        blankMove(o, n, mx, x, Y_BOT);
        ap(o, n, mx, x, Y_BOT, ro2, go2, bo2, 0);
    }
    for (int k = 0; k <= 4; k++) {
        float x = -SC * 0.88f + k * SC * 1.76f / 4;
        blankMove(o, n, mx, x,  SC*0.65f);
        ap(o, n, mx, x,  SC*0.65f, 0, 0, 40, 0);
        ap(o, n, mx, x,  SC*0.55f, 0, 0, 40, 0);
        blankMove(o, n, mx, x, -SC*0.55f);
        ap(o, n, mx, x, -SC*0.55f, 0, 0, 40, 0);
        ap(o, n, mx, x, -SC*0.65f, 0, 0, 40, 0);
    }
    blankMove(o, n, mx, -SC*0.88f, 0);
    ap(o, n, mx, -SC*0.88f, 0, 0, 0, 20, 0);
    ap(o, n, mx,  SC*0.88f, 0, 0, 0, 20, 0);
    return n;
}

// ══════════════════════════════════════════════════════════════
// PATTERN 1: WHITE BALANCE
// ══════════════════════════════════════════════════════════════
// Four stacked short strokes (W / R / G / B) instead of full-width lines,
// keeping DAC travel small while the per-channel brightness stays comparable.
static size_t white_balance(LaserPoint* o, size_t mx,
                             uint32_t phase, uint8_t bright, uint8_t) {
    size_t n = 0;
    const float STROKE = SC * 0.20f;   // short stroke half-width
    struct { uint8_t r,g,b; float y; } rows[4] = {
        {255,255,255,  SC*0.60f},
        {255,  0,  0,  SC*0.20f},
        {  0,255,  0, -SC*0.20f},
        {  0,  0,255, -SC*0.60f},
    };
    for (auto& row : rows) {
        uint8_t ro,go,bo;
        colorOut(row.r, row.g, row.b, bright, ro, go, bo);
        blankMove(o, n, mx, -STROKE, row.y);
        line(o, n, mx, -STROKE, row.y, STROKE, row.y, ro, go, bo, 8);
    }
    return n;
}

// ══════════════════════════════════════════════════════════════
// PATTERN 2: RAINBOW
// ══════════════════════════════════════════════════════════════
static size_t rainbow(LaserPoint* o, size_t mx,
                       uint32_t phase, uint8_t bright, uint8_t) {
    size_t n = 0;
    const int STEPS = 180;
    const float rot_offset = (phase % 3600) * 0.001f;
    for (int i = 0; i <= STEPS; i++) {
        float t = (float)i / STEPS;
        float angle = t * PI2 + rot_offset;
        float r_dist = SC * 0.85f;
        float x = cosf(angle) * r_dist;
        float y = sinf(angle) * r_dist;
        uint8_t ri, gi, bi;
        hsv2rgb(t * 360.0f, 255, bright, ri, gi, bi);
        uint8_t ro, go, bo;
        colorOut(ri, gi, bi, bright, ro, go, bo);
        ap(o, n, mx, x, y, ro, go, bo, i==0?1:0);
    }
    for (int i = 0; i <= 90; i++) {
        float t = (float)i / 90;
        float angle = t * PI2;
        float r_dist = SC * 0.4f;
        uint8_t ri, gi, bi;
        hsv2rgb(t * 360.0f, 120, bright, ri, gi, bi);
        uint8_t ro, go, bo;
        colorOut(ri, gi, bi, bright, ro, go, bo);
        ap(o, n, mx, cosf(angle)*r_dist, sinf(angle)*r_dist, ro, go, bo, i==0?1:0);
    }
    return n;
}

// ══════════════════════════════════════════════════════════════
// PATTERN 3: STEP RAMP
// ══════════════════════════════════════════════════════════════
// Galvo cannot fill bars. Render 8 short horizontal strokes at increasing
// brightness, arranged left->right. Each stroke is short (minimal DAC travel)
// but the discrete brightness steps stay clearly distinguishable.
static size_t step_ramp(LaserPoint* o, size_t mx,
                         uint32_t phase, uint8_t bright, uint8_t ch) {
    size_t n = 0;
    const float STROKE = SC * 0.09f;   // short stroke half-width
    for (int s = 0; s < 8; s++) {
        float t = (float)(s + 1) / 8.0f;
        uint8_t v = (uint8_t)(t * 255.0f);
        uint8_t ri = (ch==0||ch==1) ? v : 0;
        uint8_t gi = (ch==0||ch==2) ? v : 0;
        uint8_t bi = (ch==0||ch==3) ? v : 0;
        uint8_t ro, go, bo;
        colorOut(ri, gi, bi, bright, ro, go, bo);
        float x = -SC*0.80f + (s + 0.5f) / 8.0f * SC * 1.60f;
        blankMove(o, n, mx, x - STROKE, 0);
        line(o, n, mx, x - STROKE, 0, x + STROKE, 0, ro, go, bo, 4);
    }
    // Dim blue baseline for orientation
    blankMove(o, n, mx, -SC*0.85f, -SC*0.30f);
    ap(o, n, mx, -SC*0.85f, -SC*0.30f, 0, 0, 30, 0);
    ap(o, n, mx,  SC*0.85f, -SC*0.30f, 0, 0, 30, 0);
    return n;
}

// ══════════════════════════════════════════════════════════════
// PATTERN 4: CHANNEL SEPARATION
// ══════════════════════════════════════════════════════════════
static size_t channel_sep(LaserPoint* o, size_t mx,
                            uint32_t phase, uint8_t bright, uint8_t ch) {
    size_t n = 0;
    const float t = phase * 0.010f;
    struct { uint8_t r,g,b; float y; const char* name; } combos[7] = {
        {255,  0,  0,  SC*0.80f, "R"},
        {  0,255,  0,  SC*0.53f, "G"},
        {  0,  0,255,  SC*0.27f, "B"},
        {255,255,  0,  0.00f,    "R+G"},
        {  0,255,255, -SC*0.27f, "G+B"},
        {255,  0,255, -SC*0.53f, "R+B"},
        {255,255,255, -SC*0.80f, "RGB"},
    };
    for (int i = 0; i < 7; i++) {
        auto& c = combos[i];
        uint8_t ro, go, bo;
        uint8_t ri = (ch==0) ? c.r : (ch==1 ? (c.r ? bright : 0) : 0);
        uint8_t gi = (ch==0) ? c.g : (ch==2 ? (c.g ? bright : 0) : 0);
        uint8_t bi = (ch==0) ? c.b : (ch==3 ? (c.b ? bright : 0) : 0);
        colorOut(ri, gi, bi, bright, ro, go, bo);
        const int STEPS = 60;
        for (int k = 0; k <= STEPS; k++) {
            float tt = (float)k / STEPS;
            float x = -SC*0.85f + tt * SC*1.70f;
            float y = c.y + sinf(t * 1.5f + tt * PI2) * SC * 0.04f;
            ap(o, n, mx, x, y, ro, go, bo, k==0?1:0);
        }
    }
    return n;
}

// ══════════════════════════════════════════════════════════════
// PATTERN 5: SATURATION WHEEL
// ══════════════════════════════════════════════════════════════
static size_t saturation_wheel(LaserPoint* o, size_t mx,
                                 uint32_t phase, uint8_t bright, uint8_t) {
    size_t n = 0;
    const int SPOKES    = 12;
    const int PTS_SPOKE = 35;
    const float rot = (phase % 36000) * 0.0001f;
    for (int s = 0; s < SPOKES; s++) {
        float hue = (float)s / SPOKES * 360.0f;
        float angle = (float)s / SPOKES * PI2 + rot;
        for (int i = 0; i <= PTS_SPOKE; i++) {
            float t = (float)i / PTS_SPOKE;
            float r_dist = t * SC * 0.88f;
            uint8_t sat = (uint8_t)(t * 255.0f);
            uint8_t ri, gi, bi;
            hsv2rgb(hue, sat, bright, ri, gi, bi);
            uint8_t ro, go, bo;
            colorOut(ri, gi, bi, bright, ro, go, bo);
            float x = cosf(angle) * r_dist;
            float y = sinf(angle) * r_dist;
            ap(o, n, mx, x, y, ro, go, bo, i==0?1:0);
        }
    }
    uint8_t wro, wgo, wbo;
        colorOut(bright, bright, bright, bright, wro, wgo, wbo);
    for (int i = 0; i <= 20; i++) {
        float angle = (float)i / 20 * PI2;
        ap(o, n, mx,
           cosf(angle)*SC*0.08f, sinf(angle)*SC*0.08f,
           wro, wgo, wbo, i==0?1:0);
    }
    return n;
}

// ══════════════════════════════════════════════════════════════
// PATTERN 6: FOCUS TEST
// ══════════════════════════════════════════════════════════════
static size_t focus_test(LaserPoint* o, size_t mx,
                          uint32_t phase, uint8_t bright, uint8_t ch) {
    size_t n = 0;
    uint8_t ro, go, bo;
    colorOut(ch==1?bright:0, ch==2?bright:0, ch==3?bright:0, bright, ro, go, bo);
    if (ch == 0) { ro = go = bo = bright; }
    const float radii[] = { SC*0.1f, SC*0.25f, SC*0.45f, SC*0.65f, SC*0.88f };
    for (float r : radii) {
        int steps = (int)(r / SC * 80) + 20;
        blankMove(o, n, mx, cosf(0)*r, sinf(0)*r);
        for (int i = 0; i <= steps; i++) {
            float a = 6.2831853f * i / steps;
            ap(o, n, mx, cosf(a)*r, sinf(a)*r, ro, go, bo, 0);
        }
    }
    uint8_t wr = bright, wg = bright, wb = bright;
    if (ch==1){ wg=0; wb=0; } else if(ch==2){ wr=0; wb=0; } else if(ch==3){ wr=0; wg=0; }
    blankMove(o, n, mx, -SC*0.15f, 0);
    ap(o, n, mx, -SC*0.15f, 0, wr, wg, wb, 0);
    ap(o, n, mx,  SC*0.15f, 0, wr, wg, wb, 0);
    blankMove(o, n, mx, 0, -SC*0.15f);
    ap(o, n, mx, 0, -SC*0.15f, wr, wg, wb, 0);
    ap(o, n, mx, 0,  SC*0.15f, wr, wg, wb, 0);
    blankMove(o, n, mx, cosf(0)*SC*0.025f, sinf(0)*SC*0.025f);
    for (int i = 0; i <= 16; i++) {
        float a = 6.2831853f*i/16;
        ap(o, n, mx, cosf(a)*SC*0.025f, sinf(a)*SC*0.025f, wr, wg, wb, 0);
    }
    return n;
}

// ══════════════════════════════════════════════════════════════
// PATTERN 7: SCAN LINEARITY
// ══════════════════════════════════════════════════════════════
static size_t scan_linearity(LaserPoint* o, size_t mx,
                              uint32_t phase, uint8_t bright, uint8_t ch) {
    size_t n = 0;
    uint8_t ro, go, bo;
    colorOut(ch==1?bright:0, ch==2?bright:0, ch==3?bright:0, bright, ro, go, bo);
    if (ch == 0) { ro=0; go=bright; bo=0; }
    const int LINES = 7;
    for (int l = 0; l < LINES; l++) {
        float pos = -SC*0.86f + l * SC*1.72f / (LINES-1);
        blankMove(o, n, mx, -SC*0.86f, pos);
        ap(o, n, mx, -SC*0.86f, pos, ro, go, bo, 0);
        ap(o, n, mx,  SC*0.86f, pos, ro, go, bo, 0);
        uint8_t vr, vg, vb;
        colorOut(0, ch==3?0:bright, ch==2?0:bright, bright, vr, vg, vb);
        if (ch==0){ vr=0; vg=0; vb=bright; }
        blankMove(o, n, mx, pos, -SC*0.86f);
        ap(o, n, mx, pos, -SC*0.86f, vr, vg, vb, 0);
        ap(o, n, mx, pos,  SC*0.86f, vr, vg, vb, 0);
    }
    return n;
}

// ══════════════════════════════════════════════════════════════
// PATTERN 8: BLANKING TEST
// ══════════════════════════════════════════════════════════════
static size_t blanking_test(LaserPoint* o, size_t mx,
                             uint32_t phase, uint8_t bright, uint8_t ch) {
    size_t n = 0;
    uint8_t ro, go, bo;
    colorOut(ch==1?bright:0, ch==2?bright:0, ch==3?bright:0, bright, ro, go, bo);
    if (ch == 0) { ro = go = bo = bright; }
    const int SEG = 16;
    const float R = SC * 0.75f;
    for (int s = 0; s < SEG; s++) {
        float a0 = 6.2831853f * s / SEG;
        float a1 = 6.2831853f * (s+1) / SEG;
        blankMove(o, n, mx, cosf(a0)*R, sinf(a0)*R);
        if (s % 2 == 0) {
            for (int i = 1; i <= 8; i++) {
                float a = a0 + (a1-a0)*i/8;
                ap(o, n, mx, cosf(a)*R, sinf(a)*R, ro, go, bo, 0);
            }
        } else {
            ap(o, n, mx, cosf(a1)*R, sinf(a1)*R, 0, 0, 0, 0);
        }
    }
    const int SPOKES = 8;
    for (int s = 0; s < SPOKES; s++) {
        float a = 6.2831853f * s / SPOKES;
        blankMove(o, n, mx, 0, 0);
        if (s % 2 == 0) {
            ap(o, n, mx, cosf(a)*R*0.55f, sinf(a)*R*0.55f, ro, go, bo, 0);
        } else {
            ap(o, n, mx, cosf(a)*R*0.55f, sinf(a)*R*0.55f, 0, 0, 0, 0);
        }
    }
    return n;
}

// ══════════════════════════════════════════════════════════════
// PATTERN 9: ASPECT RATIO
// ══════════════════════════════════════════════════════════════
static size_t aspect_ratio(LaserPoint* o, size_t mx,
                            uint32_t phase, uint8_t bright, uint8_t ch) {
    size_t n = 0;
    uint8_t ro, go, bo, wr, wg, wb;
    colorOut(ch==1?bright:0, ch==2?bright:0, ch==3?bright:0, bright, ro, go, bo);
    if (ch == 0) { ro = bright; go = 0; bo = 0; }
    colorOut(0, ch==2?0:bright, ch==3?0:bright, bright, wr, wg, wb);
    if (ch == 0) { wr = 0; wg = bright; wb = 0; }
    const float S = SC * 0.75f;
    line(o, n, mx, -S, -S,  S, -S, ro, go, bo, 40);
    line(o, n, mx,  S, -S,  S,  S, ro, go, bo, 40);
    line(o, n, mx,  S,  S, -S,  S, ro, go, bo, 40);
    line(o, n, mx, -S,  S, -S, -S, ro, go, bo, 40);
    blankMove(o, n, mx, cosf(0)*S, sinf(0)*S);
    for (int i = 0; i <= 60; i++) {
        float a = 6.2831853f * i / 60;
        ap(o, n, mx, cosf(a)*S, sinf(a)*S, wr, wg, wb, 0);
    }
    blankMove(o, n, mx, -S*1.05f, 0);
    ap(o, n, mx, -S*1.05f, 0, 0, 0, 50, 0);
    ap(o, n, mx,  S*1.05f, 0, 0, 0, 50, 0);
    blankMove(o, n, mx, 0, -S*1.05f);
    ap(o, n, mx, 0, -S*1.05f, 0, 0, 50, 0);
    ap(o, n, mx, 0,  S*1.05f, 0, 0, 50, 0);
    for (float cx : {-S, S}) for (float cy : {-S, S}) {
        blankMove(o, n, mx, cx, cy-S*0.07f);
        ap(o, n, mx, cx, cy-S*0.07f, 200, 0, 0, 0);
        ap(o, n, mx, cx, cy+S*0.07f, 200, 0, 0, 0);
        blankMove(o, n, mx, cx-S*0.07f, cy);
        ap(o, n, mx, cx-S*0.07f, cy, 200, 0, 0, 0);
        ap(o, n, mx, cx+S*0.07f, cy, 200, 0, 0, 0);
    }
    return n;
}

// ══════════════════════════════════════════════════════════════
// PATTERN 10: CORNER / EDGE TEST
// ══════════════════════════════════════════════════════════════
static size_t corner_test(LaserPoint* o, size_t mx,
                           uint32_t phase, uint8_t bright, uint8_t ch) {
    size_t n = 0;
    uint8_t ro, go, bo;
    colorOut(ch==1?bright:0, ch==2?bright:0, ch==3?bright:0, bright, ro, go, bo);
    if (ch == 0) { ro = go = bo = bright; }
    const float S = SC * 0.85f;
    auto draw_sq = [&](float s, uint8_t r, uint8_t g, uint8_t b) {
        blankMove(o, n, mx, -s, -s);
        ap(o, n, mx, -s, -s, r,g,b, 0);
        ap(o, n, mx,  s, -s, r,g,b, 0);
        ap(o, n, mx,  s,  s, r,g,b, 0);
        ap(o, n, mx, -s,  s, r,g,b, 0);
        ap(o, n, mx, -s, -s, r,g,b, 0);
    };
    draw_sq(S, ro, go, bo);
    draw_sq(S*0.6f, ro/2, go/2, bo/2);
    line(o, n, mx, -S, -S,  S,  S, ro, go, bo, 40);
    line(o, n, mx,  S, -S, -S,  S, ro, go, bo, 40);
    float dirs[][2] = {{1,0},{-1,0},{0,1},{0,-1},{0.707f,0.707f},{-0.707f,0.707f},{0.707f,-0.707f},{-0.707f,-0.707f}};
    for (auto& d : dirs) {
        float ex = d[0]*S, ey = d[1]*S;
        blankMove(o, n, mx, 0, 0);
        ap(o, n, mx, 0, 0, 0, 0, 60, 0);
        ap(o, n, mx, ex, ey, 0, 0, 60, 0);
    }
    return n;
}

// ══════════════════════════════════════════════════════════════
// PATTERN 11: COLOR TEMPERATURE
// ══════════════════════════════════════════════════════════════
// Galvo cannot fill areas. Represent white reference and each color as one
// short horizontal stroke (minimal DAC travel). Left column = white, right
// column = stacked R/G/B, so brightness can still be compared side by side.
static size_t color_temp(LaserPoint* o, size_t mx,
                          uint32_t phase, uint8_t bright, uint8_t) {
    size_t n = 0;
    const float STROKE = SC * 0.22f;   // short: keep DAC travel small
    const float X_L    = -SC * 0.40f;  // white column center
    const float X_R    =  SC * 0.40f;  // R/G/B column center

    // White reference stroke (left)
    uint8_t wr, wg, wb;
    colorOut(bright, bright, bright, 200, wr, wg, wb);
    blankMove(o, n, mx, X_L - STROKE, 0);
    line(o, n, mx, X_L - STROKE, 0, X_L + STROKE, 0, wr, wg, wb, 6);

    // Stacked R / G / B strokes (right)
    struct { uint8_t r,g,b; float y; } bars[3] = {
        {bright, 0, 0,  SC*0.40f},
        {0, bright, 0,  0.0f     },
        {0, 0, bright, -SC*0.40f},
    };
    for (auto& b : bars) {
        uint8_t ro, go, bo;
        colorOut(b.r, b.g, b.b, 200, ro, go, bo);
        blankMove(o, n, mx, X_R - STROKE, b.y);
        line(o, n, mx, X_R - STROKE, b.y, X_R + STROKE, b.y, ro, go, bo, 6);
    }
    return n;
}

// ══════════════════════════════════════════════════════════════
// PATTERN 12: ILDA TEST PATTERN
// ══════════════════════════════════════════════════════════════
static size_t ilda_test(LaserPoint* o, size_t mx,
                         uint32_t phase, uint8_t bright, uint8_t size_ch) {
    size_t n = 0;
    const float OUTER = SC * 0.88f;
    const float INNER = OUTER * 0.5f * (0.3f + (size_ch / 255.0f) * 0.7f);
    uint8_t WR, WG, WB;  colorOut(0,     0,   bright, bright, WR, WG, WB);  // blue: inner box + circle
    uint8_t AR, AG, AB;  colorOut(0,   bright,   0, bright, AR, AG, AB);  // green: crosshair
    uint8_t DR, DG, DB;  colorOut(bright, 0,     0, bright, DR, DG, DB);  // red: outer box
    blankMove(o, n, mx, -OUTER, -OUTER);
    ap(o, n, mx, -OUTER, -OUTER, DR, DR, DR, 0);
    ap(o, n, mx,  OUTER, -OUTER, DR, DR, DR, 0);
    ap(o, n, mx,  OUTER,  OUTER, DR, DR, DR, 0);
    ap(o, n, mx, -OUTER,  OUTER, DR, DR, DR, 0);
    ap(o, n, mx, -OUTER, -OUTER, DR, DR, DR, 0);
    blankMove(o, n, mx, -INNER, -INNER);
    ap(o, n, mx, -INNER, -INNER, WR, WG, WB, 0);
    ap(o, n, mx,  INNER, -INNER, WR, WG, WB, 0);
    ap(o, n, mx,  INNER,  INNER, WR, WG, WB, 0);
    ap(o, n, mx, -INNER,  INNER, WR, WG, WB, 0);
    ap(o, n, mx, -INNER, -INNER, WR, WG, WB, 0);
    {
        const int CPTS = 32;
        blankMove(o, n, mx, cosf(0)*INNER, sinf(0)*INNER);
        for (int i = 0; i <= CPTS; i++) {
            float a = PI2 * i / CPTS;
            ap(o, n, mx, cosf(a)*INNER, sinf(a)*INNER, WR, WG, WB, 0);
        }
    }
    blankMove(o, n, mx, -OUTER, 0);
    ap(o, n, mx, -OUTER, 0, AR, AG, AB, 0);
    ap(o, n, mx,  OUTER, 0, AR, AG, AB, 0);
    blankMove(o, n, mx, 0, -OUTER);
    ap(o, n, mx, 0, -OUTER, AR, AG, AB, 0);
    ap(o, n, mx, 0,  OUTER, AR, AG, AB, 0);
    {
        float lx = OUTER * 1.02f, ly = 0;
        float s = OUTER * 0.03f;
        blankMove(o, n, mx, lx-s, ly+s);
        ap(o, n, mx, lx-s, ly+s, WR, WG, WB, 0);
        ap(o, n, mx, lx+s, ly-s, WR, WG, WB, 0);
        blankMove(o, n, mx, lx+s, ly+s);
        ap(o, n, mx, lx+s, ly+s, WR, WG, WB, 0);
        ap(o, n, mx, lx-s, ly-s, WR, WG, WB, 0);
    }
    {
        float lx = 0, ly = OUTER * 1.02f;
        float s = OUTER * 0.03f;
        blankMove(o, n, mx, lx-s, ly+s);
        ap(o, n, mx, lx-s, ly+s, WR, WG, WB, 0);
        ap(o, n, mx, lx,   ly,   WR, WG, WB, 0);
        blankMove(o, n, mx, lx+s, ly+s);
        ap(o, n, mx, lx+s, ly+s, WR, WG, WB, 0);
        ap(o, n, mx, lx,   ly,   WR, WG, WB, 0);
        ap(o, n, mx, lx,   ly-s, WR, WG, WB, 0);
    }
    {
        float by1 = -OUTER * 0.68f;
        float by2 = -OUTER * 0.78f;
        float bx  =  INNER * 0.3f;
        blankMove(o, n, mx, 0, by2 - OUTER*0.04f);
        ap(o, n, mx, 0, by2 - OUTER*0.04f, WR, WG, WB, 0);
        ap(o, n, mx, 0, by1 + OUTER*0.04f, WR, WG, WB, 0);
        blankMove(o, n, mx, -OUTER*0.55f, by1);
        ap(o, n, mx, -OUTER*0.55f, by1, WR, WG, WB, 0);
        ap(o, n, mx, -bx,          by1, WR, WG, WB, 0);
        blankMove(o, n, mx,  bx,   by1);
        ap(o, n, mx,  bx,          by1, WR, WG, WB, 0);
        ap(o, n, mx,  OUTER*0.55f, by1, WR, WG, WB, 0);
        blankMove(o, n, mx, -OUTER*0.40f, by2);
        ap(o, n, mx, -OUTER*0.40f, by2, WR, WG, WB, 0);
        ap(o, n, mx, -bx*0.7f,     by2, WR, WG, WB, 0);
        blankMove(o, n, mx,  bx*0.7f, by2);
        ap(o, n, mx,  bx*0.7f,     by2, WR, WG, WB, 0);
        ap(o, n, mx,  OUTER*0.40f, by2, WR, WG, WB, 0);
    }
    {
        const int NDOTS = 6;
        float dy = -OUTER * 0.88f;
        float dx_start = -OUTER * 0.30f;
        float dx_step  =  OUTER * 0.12f;
        for (int i = 0; i < NDOTS; i++) {
            float bv = (float)(NDOTS - 1 - i) / (NDOTS - 1);
            uint8_t dv = (uint8_t)(bv * WR);
            float px = dx_start + i * dx_step;
            blankMove(o, n, mx, px, dy);
            ap(o, n, mx, px,               dy, dv, dv, dv, 0);
            ap(o, n, mx, px + OUTER*0.01f, dy, dv, dv, dv, 0);
        }
    }
    return n;
}

// ══════════════════════════════════════════════════════════════
// PATTERN 13: DAC RANGE BOX
//
// Draws a rectangle exactly at the current dac_limit_max / dac_limit_min
// boundary, plus diagonals and an inscribed circle at the same limit.
//
// HOW TO USE:
//   1. Select this pattern in the Calibration tab.
//   2. Raise dac_limit_max (and lower dac_limit_min symmetrically) until
//      the rectangle corners just begin to clip on the projection surface.
//   3. Back off ~5% — that is your safe operating limit.
//   4. All presets will now stay within the mechanical galvo range.
//
// The "bright" parameter controls laser brightness (default 200).
// The "channel" parameter selects color: 0=white, 1=R, 2=G, 3=B.
//
// Coordinates: raw DAC signed values (-32767..+32767) mapped from
// the configured dac_limit_min/max. Center = 0x8000 in DAC space.
// ══════════════════════════════════════════════════════════════
static size_t dac_range_box(LaserPoint* o, size_t mx,
                              uint32_t phase, uint8_t bright, uint8_t ch) {
    size_t n = 0;

    // Draw at full pattern range; galvo_out clamps to dac_limit_min/max,
    // making the mechanical scan limit visible as clipped corners/edges.
    static constexpr float S = 29000.f;  // slightly under ±32767 for blanking headroom
    float sym_x = S, sym_y = S;

    // Colors
    uint8_t bxR, bxG, bxB;  // box color (yellow)
    uint8_t dgR, dgG, dgB;  // diagonal color (dim cyan)
    uint8_t ciR, ciG, ciB;  // circle color (green)
    uint8_t lnR, lnG, lnB;  // limit lines (dim)

    if (ch == 1) {
        colorOut(bright, 0, 0, bright, bxR, bxG, bxB);
        colorOut(bright/2, 0, 0, bright, dgR, dgG, dgB);
        colorOut(bright, 0, 0, bright, ciR, ciG, ciB);
        lnR=40; lnG=0; lnB=0;
    } else if (ch == 2) {
        colorOut(0, bright, 0, bright, bxR, bxG, bxB);
        colorOut(0, bright/2, 0, bright, dgR, dgG, dgB);
        colorOut(0, bright, 0, bright, ciR, ciG, ciB);
        lnR=0; lnG=40; lnB=0;
    } else if (ch == 3) {
        colorOut(0, 0, bright, bright, bxR, bxG, bxB);
        colorOut(0, 0, bright/2, bright, dgR, dgG, dgB);
        colorOut(0, 0, bright, bright, ciR, ciG, ciB);
        lnR=0; lnG=0; lnB=40;
    } else {
        colorOut(bright, 0, 0, bright, bxR, bxG, bxB);           // red box
        colorOut(0, 0, bright/2, bright, dgR, dgG, dgB);          // dim blue diags
        colorOut(0, bright, 0, bright, ciR, ciG, ciB);            // green circle
        lnR=0; lnG=40; lnB=0;
    }

    // ── Outer box (clipped by dac_limit in galvo_out) ────────────
    line(o, n, mx, -sym_x, -sym_y,  sym_x, -sym_y, bxR, bxG, bxB, 40);
    line(o, n, mx,  sym_x, -sym_y,  sym_x,  sym_y, bxR, bxG, bxB, 40);
    line(o, n, mx,  sym_x,  sym_y, -sym_x,  sym_y, bxR, bxG, bxB, 40);
    line(o, n, mx, -sym_x,  sym_y, -sym_x, -sym_y, bxR, bxG, bxB, 40);

    // ── Diagonals ─────────────────────────────────────────────────
    float diag_len = sqrtf(sym_x*sym_x*4.f + sym_y*sym_y*4.f);
    int dpts = (int)(diag_len / 800.f);
    if (dpts < 40)  dpts = 40;
    if (dpts > 120) dpts = 120;
    line(o, n, mx, -sym_x, -sym_y,  sym_x,  sym_y, dgR, dgG, dgB, dpts);
    line(o, n, mx,  sym_x, -sym_y, -sym_x,  sym_y, dgR, dgG, dgB, dpts);

    // ── Inscribed circle (radius = smaller of sym_x, sym_y) ───────
    float ciR_len = (sym_x < sym_y) ? sym_x : sym_y;
    int cpts = (int)(ciR_len / 400.f);
    if (cpts < 80)  cpts = 80;
    if (cpts > 200) cpts = 200;
    blankMove(o, n, mx, ciR_len, 0);
    for (int i = 0; i <= cpts; i++) {
        float a = PI2 * i / cpts;
        ap(o, n, mx, cosf(a)*ciR_len, sinf(a)*ciR_len, ciR, ciG, ciB, 0);
    }

    // ── Center crosshair ──────────────────────────────────────────
    float ch_x = S * 0.1f, ch_y = S * 0.1f;
    blankMove(o, n, mx, -ch_x, 0);
    ap(o, n, mx, -ch_x, 0, lnR, lnG, lnB, 0);
    ap(o, n, mx,  ch_x, 0, lnR, lnG, lnB, 0);
    blankMove(o, n, mx, 0, -ch_y);
    ap(o, n, mx, 0, -ch_y, lnR, lnG, lnB, 0);
    ap(o, n, mx, 0,  ch_y, lnR, lnG, lnB, 0);

    // ── 50% reference box (dim) ───────────────────────────────────
    float hx = S * 0.5f, hy = S * 0.5f;
    line(o, n, mx, -hx, -hy,  hx, -hy, lnR, lnG, lnB, 20);
    line(o, n, mx,  hx, -hy,  hx,  hy, lnR, lnG, lnB, 20);
    line(o, n, mx,  hx,  hy, -hx,  hy, lnR, lnG, lnB, 20);
    line(o, n, mx, -hx,  hy, -hx, -hy, lnR, lnG, lnB, 20);

    return n;
}
// ══════════════════════════════════════════════════════════════
// PATTERN 14: PROJECTION ZONE OUTLINE
//
// Projects the user-defined projection zone polygon (gZone) as a closed
// red outline with a green dot marker at each vertex and a dim
// center crosshair. Used during setup to verify the touch-defined safe scan
// area on the real projection surface before enabling zone clipping.
//
// The polygon is edited in the WebUI (Calibration tab -> Projection Zone);
// this pattern only visualises the stored gZone vertices.
// ══════════════════════════════════════════════════════════════
static size_t zone_outline(LaserPoint* o, size_t mx,
                            uint32_t phase, uint8_t bright, uint8_t ch) {
    size_t n = 0;

    uint8_t pR, pG, pB;   // polygon edge color (pure R/G/B only)
    uint8_t vR, vG, vB;   // vertex marker color (pure R/G/B only)
    if (ch == 1)      { colorOut(bright, 0, 0, bright, pR, pG, pB); vR=pR; vG=pG; vB=pB; }
    else if (ch == 2) { colorOut(0, bright, 0, bright, pR, pG, pB); vR=pR; vG=pG; vB=pB; }
    else if (ch == 3) { colorOut(0, 0, bright, bright, pR, pG, pB); vR=pR; vG=pG; vB=pB; }
    else {
        colorOut(bright, 0, 0, bright, pR, pG, pB);   // red edges
        colorOut(0, bright, 0, bright, vR, vG, vB);   // green vertices
    }

    uint8_t cnt = gZone.count;
    if (cnt < 3)               cnt = 3;
    if (cnt > ZONE_POINTS_MAX) cnt = ZONE_POINTS_MAX;

    const int PARK_DWELL = 8;   // blanked settle points at frame-wrap park

    // ── Closed polygon outline (interpolated edges only, no dwell) ─
    // X/Y inversion handled globally via gConfig.invert_x/invert_y in
    // pattern_engine::applyCalibration(), same as every other pattern.
    for (uint8_t i = 0; i <= cnt; i++) {
        float x = (float)gZone.x[i % cnt];
        float y = (float)gZone.y[i % cnt];
        if (i == 0) {
            blankMove(o, n, mx, x, y);
            ap(o, n, mx, x, y, pR, pG, pB, 0);
        } else {
            float x0 = (float)gZone.x[(i - 1) % cnt];
            float y0 = (float)gZone.y[(i - 1) % cnt];
            const int steps = 24;
            for (int s = 1; s <= steps; s++) {
                float t = (float)s / steps;
                ap(o, n, mx, x0 + (x - x0)*t, y0 + (y - y0)*t, pR, pG, pB, 0);
            }
        }
    }

    // ── Vertex markers (single dwell point, pure color, uniform) ───
    const int MARKER_DWELL = 24;
    for (uint8_t i = 0; i < cnt; i++) {
        float vx = (float)gZone.x[i];
        float vy = (float)gZone.y[i];
        blankMove(o, n, mx, vx, vy);
        for (int d = 0; d < MARKER_DWELL; d++)
            ap(o, n, mx, vx, vy, vR, vG, vB, 0);
    }

    // ── Center crosshair (dim) ────────────────────────────────────
    blankMove(o, n, mx, -2000, 0);
    ap(o, n, mx, -2000, 0, 0, 0, 30, 0);
    ap(o, n, mx,  2000, 0, 0, 0, 30, 0);
    blankMove(o, n, mx, 0, -2000);
    ap(o, n, mx, 0, -2000, 0, 0, 30, 0);
    ap(o, n, mx, 0,  2000, 0, 0, 30, 0);

    // ── Park blanked at loop start (prevents visible bleed line on
    //    buffer wraparound: crosshair end -> vertex 0) ──────────────
    if (cnt > 0) {
        float x0 = (float)gZone.x[0];
        float y0 = (float)gZone.y[0];
        blankMove(o, n, mx, x0, y0);
        for (int d = 0; d < PARK_DWELL; d++)
            ap(o, n, mx, x0, y0, 0, 0, 0, 1);
    }

    return n;
}

// ══════════════════════════════════════════════════════════════
// PATTERN 16: CORNER COLOR MAP (RGBW)
// ══════════════════════════════════════════════════════════════
// One solid colored dot in each corner so the projected orientation is
// unambiguous:  Red = top-left, Green = top-right, Blue = bottom-right,
// White = bottom-left. A dim frame connects the four dots as a reference.
// DAC space here is +y = up, so top = +SC, bottom = -SC.
static size_t corner_color_map(LaserPoint* o, size_t mx,
                                uint32_t phase, uint8_t bright, uint8_t ch) {
    size_t n = 0;
    const float S = SC * 0.9f;   // corner distance from center

    // corner position + its RGB colour (before gamma / white-balance)
    struct Corner { float x, y; uint8_t r, g, b; };
    const Corner corners[4] = {
        { -S,  S, 255,   0,   0 },  // top-left    = Red
        {  S,  S,   0, 255,   0 },  // top-right   = Green
        {  S, -S,   0,   0, 255 },  // bottom-right= Blue
        { -S, -S, 255, 255, 255 },  // bottom-left = White
    };

    // dim neutral frame joining the corners (spatial reference)
    uint8_t fr, fg, fb;
    colorOut(40, 40, 40, bright, fr, fg, fb);
    line(o, n, mx, corners[0].x, corners[0].y, corners[1].x, corners[1].y, fr, fg, fb, 40);
    line(o, n, mx, corners[1].x, corners[1].y, corners[2].x, corners[2].y, fr, fg, fb, 40);
    line(o, n, mx, corners[2].x, corners[2].y, corners[3].x, corners[3].y, fr, fg, fb, 40);
    line(o, n, mx, corners[3].x, corners[3].y, corners[0].x, corners[0].y, fr, fg, fb, 40);

    // solid dot per corner: several concentric rings so the spot reads as
    // filled rather than a thin outline.
    const float radii[] = { SC*0.02f, SC*0.045f, SC*0.07f };
    for (const Corner& c : corners) {
        uint8_t ro, go, bo;
        // channel filter: when isolating a channel, drop the others so the
        // dot only lights if it carries that channel (same convention as the
        // other patterns' `ch` argument).
        uint8_t cr = c.r, cg = c.g, cb = c.b;
        if (ch == 1) { cg = 0; cb = 0; }
        else if (ch == 2) { cr = 0; cb = 0; }
        else if (ch == 3) { cr = 0; cg = 0; }
        colorOut(cr, cg, cb, bright, ro, go, bo);
        for (float r : radii) {
            blankMove(o, n, mx, c.x + r, c.y);
            for (int i = 0; i <= 20; i++) {
                float a = 6.2831853f * i / 20;
                ap(o, n, mx, c.x + cosf(a)*r, c.y + sinf(a)*r, ro, go, bo, i == 0 ? 1 : 0);
            }
        }
    }
    return n;
}

// ══════════════════════════════════════════════════════════════
// DISPATCH + METADATA
// ══════════════════════════════════════════════════════════════
const CalibPatternInfo CALIB_INFO[CALIB_PATTERN_COUNT] = {
    {"Gamma Ramp",
     "Brightness from black to white — check linearity",
     "Both ramps must look equally smooth"},

    {"White Balance",
     "R / G / B / White each a short stroke — compare channel brightness",
     "All four strokes must appear equally bright"},

    {"Rainbow",
     "Full color wheel — check all hues",
     "Smooth transitions, all 6 primary colors visible"},

    {"Step Ramp",
     "8 short strokes of rising brightness — check gamma compression",
     "All 8 brightness steps must be individually distinguishable"},

    {"Channel Separation",
     "7 color lines — check crosstalk between R / G / B",
     "Red line must contain no green or blue"},

    {"Saturation Wheel",
     "Color to white — mixing quality and white balance",
     "Center of wheel must be pure neutral white"},

    {"Focus Test",
     "Concentric circles + crosshair — set beam focus here",
     "All rings must appear equally sharp at correct focus"},

    {"Scan Linearity",
     "Equal-spaced H and V grid — checks for barrel / pincushion distortion",
     "All lines must be perfectly straight and evenly spaced"},

    {"Blanking Test",
     "Alternating on/off segments — checks blanking accuracy",
     "Dark segments must be completely dark (no light leakage)"},

    {"Aspect Ratio",
     "Square + circle of identical size — checks X/Y gain match",
     "Circle must fit exactly inside the square corners"},

    {"Corner Test",
     "Tight corners, diagonals, star — checks scan rate vs. accuracy",
     "Corners must be sharp; diagonals must be straight"},

    {"Color Temperature",
     "White stroke vs. stacked R/G/B strokes — perceptual color balance",
     "Left white must visually match combined R+G+B on the right"},

    {"ILDA Test Pattern",
     "Official ILDA standard test pattern — galvo alignment & scanner tuning",
     "Circle must be perfectly round and touch inner square at 4 points. "
     "Adjust size slider until circle just stops distorting, then add 10%. "
     "Sequence: Y damping -> Y gain -> X damping -> X gain -> DC offset."},

    {"DAC Range Box",
     "Rectangle + circle at exact dac_limit_max boundary — set safe scan range",
     "Raise dac_limit_max until box corners just clip, then back off 5%. "
     "Yellow box = limit boundary. Green circle = inscribed at same limit. "
     "Dim inner box = 50% reference. Adjust X/Y gain if circle is not round."},

    {"Projection Zone",
     "Outline of the touch-defined projection zone polygon — verify safe area",
     "Red = zone boundary, green dots = vertices. Edit the polygon "
     "in the Calibration tab, then enable zone clipping to blank the laser "
     "outside this area."},

    {"Corner Color Map",
     "One colored dot per corner (RGBW) — shows how the image is projected",
     "Position mapping: Red = top-left, Green = top-right, Blue = "
     "bottom-right, White = bottom-left. If a dot appears in the wrong "
     "corner the image is mirrored/rotated — fix with X/Y flip or invert."},
};

using PFn = size_t(*)(LaserPoint*, size_t, uint32_t, uint8_t, uint8_t);
static const PFn DISPATCH[CALIB_PATTERN_COUNT] = {
    gamma_ramp, white_balance, rainbow,
    step_ramp,  channel_sep,  saturation_wheel,
    focus_test, scan_linearity, blanking_test,
    aspect_ratio, corner_test, color_temp,
    ilda_test, dac_range_box, zone_outline,
    corner_color_map,
};

static inline optimizer::OptimizerConfig liveOptimizerConfig() {
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
    cfg.transform                    = optimizer::gLiveTransform;  // Phase 3: live Z-rot + move
    return cfg;
}

size_t generate(uint8_t idx, LaserPoint* out, size_t max_pts,
                uint32_t phase, uint8_t brightness, uint8_t channel) {
    if (idx >= CALIB_PATTERN_COUNT || !out) return 0;
    size_t n = DISPATCH[idx](out, max_pts, phase, brightness, channel);

    // Cross-frame seam bridge (#4), same as presets::generate(). rainbow and
    // saturation_wheel rotate via phase; their first lit point jumps from the
    // previous frame's end each push. Static patterns produce ~0 jump -> skip.
    static float sLastX[CALIB_PATTERN_COUNT] = {0};
    static float sLastY[CALIB_PATTERN_COUNT] = {0};
    static bool  sHas[CALIB_PATTERN_COUNT]   = {false};
    static constexpr float kSeamThresh2 = 100.f;
    if (n > 0) {
        size_t f = 0; while (f < n && out[f].blank) f++;
        if (f < n && sHas[idx]) {
            float dx = (float)out[f].x - sLastX[idx];
            float dy = (float)out[f].y - sLastY[idx];
            if (dx*dx + dy*dy > kSeamThresh2) {
                const optimizer::OptimizerConfig cfg = liveOptimizerConfig();
                LaserPoint br[130];
                br[0] = LaserPoint((int16_t)sLastX[idx], (int16_t)sLastY[idx], 0,0,0,1);
                size_t bn = 1;
                optimizer::emitBlankTo(br, bn, 130, (float)out[f].x, (float)out[f].y, cfg);
                size_t jc = bn - 1;
                if (jc > 0 && max_pts > jc) {
                    if (n + jc > max_pts) n = max_pts - jc;
                    memmove(out + jc, out, n * sizeof(LaserPoint));
                    memcpy(out, br + 1, jc * sizeof(LaserPoint));
                    n += jc;
                }
            }
        }
        sLastX[idx] = (float)out[n-1].x; sLastY[idx] = (float)out[n-1].y; sHas[idx] = true;
    }
    return n;
}

} // namespace calib_patterns
