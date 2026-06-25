/**
 * calib_patterns.cpp -- calibration and color-gradient test patterns
 *
 * IMPORTANT: all patterns apply gamma LUT + white balance,
 * so that real laser projection matches the preview simulation.
 *
 * Coordinates: +-32767 (ILDA default, 16-bit signed)
 * SC = 32767 * 0.88 = 28834 (laesst 6% Rand)
 */
#include "calib_patterns.h"
#include "config.h"
#include <math.h>
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
    // 1. Master brightness
    // 2. white balance (gain from gConfig)
    // 3. Gamma correction
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
// prevents beam-on during galvo travel between segments
static void blankMove(LaserPoint* o, size_t& n, size_t mx,
                       float x1, float y1) {
    if (n == 0) {
        // no previous point -- just add a single blank anchor
        ap(o, n, mx, x1, y1, 0, 0, 0, 1);
        return;
    }
    float x0 = o[n-1].x;
    float y0 = o[n-1].y;
    float dx = x1 - x0, dy = y1 - y0;
    float dist = sqrtf(dx*dx + dy*dy);
    // scale steps with distance: ~1 step per 1000 units, min 4, max 20
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
// Zwei horizontale Rampen uebereinander:
//   top:    WITH gamma + white balance -> as the laser actually outputs
//   bottom: WITHOUT gamma (linear)    -> comparison reference
// Goal: both ramps should look visually equally smooth.
// ══════════════════════════════════════════════════════════════
static size_t gamma_ramp(LaserPoint* o, size_t mx,
                          uint32_t phase, uint8_t bright, uint8_t ch) {
    size_t n = 0;
    const int STEPS = 60;
    const float Y_TOP =  SC * 0.55f;  // upper ramp
    const float Y_BOT = -SC * 0.55f;  // lower ramp (without gamma)

    for (int i = 0; i <= STEPS; i++) {
        float t = (float)i / STEPS;
        float x = -SC * 0.88f + t * SC * 1.76f;
        uint8_t v = (uint8_t)(t * 255.0f);

        // channel-selection
        uint8_t ri = (ch==0||ch==1) ? v : 0;
        uint8_t gi = (ch==0||ch==2) ? v : 0;
        uint8_t bi = (ch==0||ch==3) ? v : 0;

        // Top: with gamma + white balance
        uint8_t ro, go, bo;
        colorOut(ri, gi, bi, bright, ro, go, bo);
        if (i == 0) blankMove(o, n, mx, x, Y_TOP);
        ap(o, n, mx, x, Y_TOP, ro, go, bo, 0);

        // Bottom: without gamma, only white balance
        uint8_t ro2 = (uint8_t)(((uint32_t)ri * bright * gConfig.gain_r) / (255UL*255));
        uint8_t go2 = (uint8_t)(((uint32_t)gi * bright * gConfig.gain_g) / (255UL*255));
        uint8_t bo2 = (uint8_t)(((uint32_t)bi * bright * gConfig.gain_b) / (255UL*255));
        blankMove(o, n, mx, x, Y_BOT);
        ap(o, n, mx, x, Y_BOT, ro2, go2, bo2, 0);
    }

    // label ticks at 0%, 25%, 50%, 75%, 100%
    for (int k = 0; k <= 4; k++) {
        float x = -SC * 0.88f + k * SC * 1.76f / 4;
        blankMove(o, n, mx, x,  SC*0.65f);
        ap(o, n, mx, x,  SC*0.65f, 40, 40, 40, 0);
        ap(o, n, mx, x,  SC*0.55f, 40, 40, 40, 0);
        blankMove(o, n, mx, x, -SC*0.55f);
        ap(o, n, mx, x, -SC*0.55f, 40, 40, 40, 0);
        ap(o, n, mx, x, -SC*0.65f, 40, 40, 40, 0);
    }

    // separator line center
    blankMove(o, n, mx, -SC*0.88f, 0);
    ap(o, n, mx, -SC*0.88f, 0, 20, 20, 20, 0);
    ap(o, n, mx,  SC*0.88f, 0, 20, 20, 20, 0);

    return n;
}

// ══════════════════════════════════════════════════════════════
// PATTERN 1: WHITE BALANCE
// 4 horizontal lines: white, Red, Green, Blue
// All with the same input value 'bright' -> should appear equally bright
// ══════════════════════════════════════════════════════════════
static size_t white_balance(LaserPoint* o, size_t mx,
                             uint32_t phase, uint8_t bright, uint8_t) {
    size_t n = 0;
    struct { uint8_t r,g,b; float y; } rows[4] = {
        {255,255,255,  SC*0.68f},  // White
        {255,  0,  0,  SC*0.23f},  // Red
        {  0,255,  0, -SC*0.23f},  // Green
        {  0,  0,255, -SC*0.68f},  // Blue
    };
    for (auto& row : rows) {
        uint8_t ro,go,bo;
        colorOut(row.r, row.g, row.b, bright, ro, go, bo);
        line(o, n, mx,
             -SC*0.85f, row.y, SC*0.85f, row.y,
             ro, go, bo, 50);
    }
    return n;
}

// ══════════════════════════════════════════════════════════════
// PATTERN 2: RAINBOW (color wheel)
// Spiral through the entire color wheel (HSV, S=255, V=bright)
// Checks that all hues are generated and transitions are smooth
// ══════════════════════════════════════════════════════════════
static size_t rainbow(LaserPoint* o, size_t mx,
                       uint32_t phase, uint8_t bright, uint8_t) {
    size_t n = 0;
    const int STEPS = 180;
    const float rot_offset = (phase % 3600) * 0.001f;  // slow rotation

    for (int i = 0; i <= STEPS; i++) {
        float t = (float)i / STEPS;
        float angle = t * PI2 + rot_offset;
        float r_dist = SC * 0.85f;

        float x = cosf(angle) * r_dist;
        float y = sinf(angle) * r_dist;

        uint8_t ri, gi, bi;
        hsv2rgb(t * 360.0f, 255, bright, ri, gi, bi);

        uint8_t ro, go, bo;
        colorOut(ri, gi, bi, 255, ro, go, bo);  // bright schon in hsv2rgb
        ap(o, n, mx, x, y, ro, go, bo, i==0?1:0);
    }

    // inner circle (desaturated) for comparison
    for (int i = 0; i <= 90; i++) {
        float t = (float)i / 90;
        float angle = t * PI2;
        float r_dist = SC * 0.4f;
        uint8_t ri, gi, bi;
        hsv2rgb(t * 360.0f, 120, bright, ri, gi, bi);
        uint8_t ro, go, bo;
        colorOut(ri, gi, bi, 255, ro, go, bo);
        ap(o, n, mx, cosf(angle)*r_dist, sinf(angle)*r_dist, ro, go, bo, i==0?1:0);
    }

    return n;
}

// ══════════════════════════════════════════════════════════════
// PATTERN 3: STEP RAMP
// 8 vertical bars with brightness steps 1/8 to 8/8
// Checks that all steps are distinguishable and appear equally large
// ══════════════════════════════════════════════════════════════
static size_t step_ramp(LaserPoint* o, size_t mx,
                         uint32_t phase, uint8_t bright, uint8_t ch) {
    size_t n = 0;
    const int STEPS_V = 30;
    const float anim = sinf(phase * 0.008f) * 0.1f;  // gentle vertical movement

    for (int s = 0; s < 8; s++) {
        float t = (float)(s + 1) / 8.0f;
        uint8_t v = (uint8_t)(t * 255.0f);

        uint8_t ri = (ch==0||ch==1) ? v : 0;
        uint8_t gi = (ch==0||ch==2) ? v : 0;
        uint8_t bi = (ch==0||ch==3) ? v : 0;

        uint8_t ro, go, bo;
        colorOut(ri, gi, bi, bright, ro, go, bo);

        // x position of the bars
        float x_center = -SC*0.85f + (s + 0.5f) / 8.0f * SC * 1.70f;
        float bar_w    = SC * 1.70f / 8.0f * 0.7f;
        float bar_h    = (SC * 0.8f) * t + anim * SC;

        // horizontal top edge
        blankMove(o, n, mx, x_center - bar_w/2, bar_h);
        ap(o, n, mx, x_center - bar_w/2, bar_h, ro, go, bo, 0);
        ap(o, n, mx, x_center + bar_w/2, bar_h, ro, go, bo, 0);

        // vertical edge downward
        blankMove(o, n, mx, x_center + bar_w/2, bar_h);
        for (int i = 0; i <= STEPS_V; i++) {
            float yy = bar_h - bar_h * 1.8f * i / STEPS_V;
            ap(o, n, mx, x_center + bar_w/2, yy, ro, go, bo, 0);
        }

        // connection line to base
        blankMove(o, n, mx, x_center - bar_w/2, -SC*0.85f);
        ap(o, n, mx, x_center - bar_w/2, -SC*0.85f, ro, go, bo, 0);
        ap(o, n, mx, x_center - bar_w/2,  bar_h,    ro, go, bo, 0);
    }

    // base line
    blankMove(o, n, mx, -SC*0.88f, -SC*0.85f);
    ap(o, n, mx, -SC*0.88f, -SC*0.85f, 30, 30, 30, 0);
    ap(o, n, mx,  SC*0.88f, -SC*0.85f, 30, 30, 30, 0);

    return n;
}

// ══════════════════════════════════════════════════════════════
// PATTERN 4: CHANNEL SEPARATION
// 7 horizontal lines: R, G, B, RG(Yellow), GB(Cyan), RB(Magenta), White
// Checks that channels are independent and no crosstalk occurs
// ══════════════════════════════════════════════════════════════
static size_t channel_sep(LaserPoint* o, size_t mx,
                            uint32_t phase, uint8_t bright, uint8_t) {
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
        colorOut(c.r, c.g, c.b, bright, ro, go, bo);

        // sinusoidal line (slightly animated)
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
// spoke wheel: outer = full color, inner = white
// 12 spokes with evenly distributed hues
// checks whether hue->white mixing is correct
// ══════════════════════════════════════════════════════════════
static size_t saturation_wheel(LaserPoint* o, size_t mx,
                                 uint32_t phase, uint8_t bright, uint8_t) {
    size_t n = 0;
    const int SPOKES    = 12;
    const int PTS_SPOKE = 35;
    const float rot = (phase % 36000) * 0.0001f;  // very slow rotation

    for (int s = 0; s < SPOKES; s++) {
        float hue = (float)s / SPOKES * 360.0f;
        float angle = (float)s / SPOKES * PI2 + rot;

        for (int i = 0; i <= PTS_SPOKE; i++) {
            float t = (float)i / PTS_SPOKE;
            float r_dist = t * SC * 0.88f;

            // color: outer = saturated, inner = white (s=0 -> all 255)
            uint8_t sat = (uint8_t)(t * 255.0f);
            uint8_t ri, gi, bi;
            hsv2rgb(hue, sat, bright, ri, gi, bi);

            uint8_t ro, go, bo;
            colorOut(ri, gi, bi, 255, ro, go, bo);

            float x = cosf(angle) * r_dist;
            float y = sinf(angle) * r_dist;
            ap(o, n, mx, x, y, ro, go, bo, i==0?1:0);
        }
    }

    // Mittelpunkt-Kreis (soll pure white be)
    uint8_t wro, wgo, wbo;
    colorOut(bright, bright, bright, 200, wro, wgo, wbo);
    for (int i = 0; i <= 20; i++) {
        float angle = (float)i / 20 * PI2;
        ap(o, n, mx,
           cosf(angle)*SC*0.08f, sinf(angle)*SC*0.08f,
           wro, wgo, wbo, i==0?1:0);
    }

    return n;
}

// ══════════════════════════════════════════════════════════════
// DISPATCH + METADATA
// ══════════════════════════════════════════════════════════════


// ══════════════════════════════════════════════════════════════
// PATTERN 6: FOCUS TEST
// Concentric circles + center crosshair — use to set beam focus.
// Inner ring = small, outer ring = full deflection.
// At correct focus: ALL rings appear equally sharp.
// ══════════════════════════════════════════════════════════════
static size_t focus_test(LaserPoint* o, size_t mx,
                          uint32_t phase, uint8_t bright, uint8_t ch) {
    size_t n = 0;
    uint8_t ro, go, bo;
    colorOut(ch==1?bright:0, ch==2?bright:0, ch==3?bright:0, bright, ro, go, bo);
    if (ch == 0) { ro = go = bo = bright; }

    // 5 concentric circles
    const float radii[] = { SC*0.1f, SC*0.25f, SC*0.45f, SC*0.65f, SC*0.88f };
    for (float r : radii) {
        int steps = (int)(r / SC * 80) + 20;
        blankMove(o, n, mx, cosf(0)*r, sinf(0)*r);
        for (int i = 0; i <= steps; i++) {
            float a = 6.2831853f * i / steps;
            ap(o, n, mx, cosf(a)*r, sinf(a)*r, ro, go, bo, 0);
        }
    }
    // Center crosshair
    uint8_t wr = bright, wg = bright, wb = bright;
    if (ch==1){ wg=0; wb=0; } else if(ch==2){ wr=0; wb=0; } else if(ch==3){ wr=0; wg=0; }
    blankMove(o, n, mx, -SC*0.15f, 0);
    ap(o, n, mx, -SC*0.15f, 0, wr, wg, wb, 0);
    ap(o, n, mx,  SC*0.15f, 0, wr, wg, wb, 0);
    blankMove(o, n, mx, 0, -SC*0.15f);
    ap(o, n, mx, 0, -SC*0.15f, wr, wg, wb, 0);
    ap(o, n, mx, 0,  SC*0.15f, wr, wg, wb, 0);
    // Center dot
    blankMove(o, n, mx, cosf(0)*SC*0.025f, sinf(0)*SC*0.025f);
    for (int i = 0; i <= 16; i++) {
        float a = 6.2831853f*i/16;
        ap(o, n, mx, cosf(a)*SC*0.025f, sinf(a)*SC*0.025f, wr, wg, wb, 0);
    }
    return n;
}

// ══════════════════════════════════════════════════════════════
// PATTERN 7: SCAN LINEARITY
// Horizontal and vertical lines at equal spacing.
// Lines should appear perfectly straight and evenly spaced.
// Barrel/pincushion distortion will show as bowed lines.
// ══════════════════════════════════════════════════════════════
static size_t scan_linearity(LaserPoint* o, size_t mx,
                              uint32_t phase, uint8_t bright, uint8_t ch) {
    size_t n = 0;
    uint8_t ro, go, bo;
    colorOut(ch==1?bright:0, ch==2?bright:0, ch==3?bright:0, bright, ro, go, bo);
    if (ch == 0) { ro=0; go=bright; bo=0; } // green for neutrality

    const int LINES = 7;  // 7 horizontal + 7 vertical
    for (int l = 0; l < LINES; l++) {
        float pos = -SC*0.86f + l * SC*1.72f / (LINES-1);
        // Horizontal
        blankMove(o, n, mx, -SC*0.86f, pos);
        ap(o, n, mx, -SC*0.86f, pos, ro, go, bo, 0);
        ap(o, n, mx,  SC*0.86f, pos, ro, go, bo, 0);
        // Vertical (cyan for contrast)
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
// Alternating on/off segments — tests blanking accuracy.
// Dark segments should be completely dark (no light leakage).
// Fast blanking = cleaner pattern.
// ══════════════════════════════════════════════════════════════
static size_t blanking_test(LaserPoint* o, size_t mx,
                             uint32_t phase, uint8_t bright, uint8_t ch) {
    size_t n = 0;
    uint8_t ro, go, bo;
    colorOut(ch==1?bright:0, ch==2?bright:0, ch==3?bright:0, bright, ro, go, bo);
    if (ch == 0) { ro = go = bo = bright; }

    // 16 segments on circle
    const int SEG = 16;
    const float R = SC * 0.75f;
    for (int s = 0; s < SEG; s++) {
        float a0 = 6.2831853f * s / SEG;
        float a1 = 6.2831853f * (s+1) / SEG;
        // Move to start (blank)
        blankMove(o, n, mx, cosf(a0)*R, sinf(a0)*R);
        if (s % 2 == 0) {
            // Lit segment: draw arc
            for (int i = 1; i <= 8; i++) {
                float a = a0 + (a1-a0)*i/8;
                ap(o, n, mx, cosf(a)*R, sinf(a)*R, ro, go, bo, 0);
            }
        } else {
            // Dark: just jump (blank move)
            ap(o, n, mx, cosf(a1)*R, sinf(a1)*R, 0, 0, 0, 0);
        }
    }
    // Inner: alternating radial lines
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
// A perfect square + circle.
// If X/Y gains are wrong: square becomes rectangle, circle becomes ellipse.
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
    // Square (should be perfect square)
    blankMove(o, n, mx, -S, -S);
    ap(o, n, mx, -S, -S, ro, go, bo, 0);
    ap(o, n, mx,  S, -S, ro, go, bo, 0);
    ap(o, n, mx,  S,  S, ro, go, bo, 0);
    ap(o, n, mx, -S,  S, ro, go, bo, 0);
    ap(o, n, mx, -S, -S, ro, go, bo, 0);
    // Circle with same radius as square side (should fit square exactly)
    blankMove(o, n, mx, cosf(0)*S, sinf(0)*S);
    for (int i = 0; i <= 60; i++) {
        float a = 6.2831853f * i / 60;
        ap(o, n, mx, cosf(a)*S, sinf(a)*S, wr, wg, wb, 0);
    }
    // Crosshair at center
    blankMove(o, n, mx, -S*1.05f, 0);
    ap(o, n, mx, -S*1.05f, 0, 50, 50, 50, 0);
    ap(o, n, mx,  S*1.05f, 0, 50, 50, 50, 0);
    blankMove(o, n, mx, 0, -S*1.05f);
    ap(o, n, mx, 0, -S*1.05f, 50, 50, 50, 0);
    ap(o, n, mx, 0,  S*1.05f, 50, 50, 50, 0);
    // Corner ticks
    for (float cx : {-S, S}) for (float cy : {-S, S}) {
        blankMove(o, n, mx, cx, cy-S*0.07f);
        ap(o, n, mx, cx, cy-S*0.07f, 200, 200, 0, 0);
        ap(o, n, mx, cx, cy+S*0.07f, 200, 200, 0, 0);
        blankMove(o, n, mx, cx-S*0.07f, cy);
        ap(o, n, mx, cx-S*0.07f, cy, 200, 200, 0, 0);
        ap(o, n, mx, cx+S*0.07f, cy, 200, 200, 0, 0);
    }
    return n;
}

// ══════════════════════════════════════════════════════════════
// PATTERN 10: CORNER / EDGE TEST
// Tight corners and edge-to-edge diagonal lines.
// At too-high kpps: corners round off, diagonals sag.
// Shows maximum scan rate vs. corner accuracy tradeoff.
// ══════════════════════════════════════════════════════════════
static size_t corner_test(LaserPoint* o, size_t mx,
                           uint32_t phase, uint8_t bright, uint8_t ch) {
    size_t n = 0;
    uint8_t ro, go, bo;
    colorOut(ch==1?bright:0, ch==2?bright:0, ch==3?bright:0, bright, ro, go, bo);
    if (ch == 0) { ro = go = bo = bright; }

    const float S = SC * 0.85f;
    // Outer square (sharp corners = good)
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

    // Diagonals (should be perfectly straight)
    line(o, n, mx, -S, -S,  S,  S, ro, go, bo, 40);
    line(o, n, mx,  S, -S, -S,  S, ro, go, bo, 40);

    // Star pattern (8 lines from center to corners and edges)
    float dirs[][2] = {{1,0},{-1,0},{0,1},{0,-1},{0.707f,0.707f},{-0.707f,0.707f},{0.707f,-0.707f},{-0.707f,-0.707f}};
    for (auto& d : dirs) {
        float ex = d[0]*S, ey = d[1]*S;
        blankMove(o, n, mx, 0, 0);
        ap(o, n, mx, 0, 0, 60, 60, 60, 0);
        ap(o, n, mx, ex, ey, 60, 60, 60, 0);
    }
    return n;
}

// ══════════════════════════════════════════════════════════════
// PATTERN 11: COLOR TEMPERATURE
// Side-by-side: pure white vs. R+G+B mixed.
// Helps judge perceptual color balance (CCT) and LED matching.
// Left = white output with current gains.
// Right = each channel individually stacked.
// ══════════════════════════════════════════════════════════════
static size_t color_temp(LaserPoint* o, size_t mx,
                          uint32_t phase, uint8_t bright, uint8_t) {
    size_t n = 0;
    // Left half: pure white (all channels at once)
    uint8_t wr, wg, wb;
    colorOut(bright, bright, bright, 200, wr, wg, wb);
    for (int i = 0; i <= 50; i++) {
        float y = -SC*0.75f + i * SC*1.5f/50;
        if (i == 0) blankMove(o, n, mx, -SC*0.65f, y);
        ap(o, n, mx, -SC*0.65f, y, wr, wg, wb, 0);
        ap(o, n, mx, -SC*0.08f, y, wr, wg, wb, 0);
        blankMove(o, n, mx, -SC*0.08f, y);
    }
    // Right half: R, G, B stacked bars
    struct { uint8_t r,g,b; float y0,y1; } bars[3] = {
        {bright,0,0,     SC*0.1f,  SC*0.75f},
        {0,bright,0,    -SC*0.25f, SC*0.1f },
        {0,0,bright,    -SC*0.75f,-SC*0.25f},
    };
    for (auto& b : bars) {
        uint8_t ro,go,bo;
        colorOut(b.r,b.g,b.b,200,ro,go,bo);
        for (int i = 0; i <= 30; i++) {
            float y = b.y0 + i*(b.y1-b.y0)/30;
            if (i == 0) blankMove(o, n, mx, SC*0.08f, y);
            ap(o, n, mx, SC*0.08f, y, ro,go,bo, 0);
            ap(o, n, mx, SC*0.65f, y, ro,go,bo, 0);
            blankMove(o, n, mx, SC*0.65f, y);
        }
    }
    // Divider
    blankMove(o, n, mx, -SC*0.08f, -SC*0.82f);
    ap(o, n, mx, -SC*0.08f, -SC*0.82f, 30, 30, 30, 0);
    ap(o, n, mx, -SC*0.08f,  SC*0.82f, 30, 30, 30, 0);
    blankMove(o, n, mx,  SC*0.08f, -SC*0.82f);
    ap(o, n, mx,  SC*0.08f, -SC*0.82f, 30, 30, 30, 0);
    ap(o, n, mx,  SC*0.08f,  SC*0.82f, 30, 30, 30, 0);
    return n;
}

// ══════════════════════════════════════════════════════════════
// PATTERN 12: ILDA TEST PATTERN (ITP-9000, Rev.002, Oct 1995)
//
// Implements the official ILDA Standard Test Pattern for galvo
// scanner alignment and calibration per the ILDA Technical
// Committee specification.
//
// Elements (per Figure 2 of spec):
//   A1: Outer square  — full scan boundary (max amplitude reference)
//   A2: Inner square  — 50% scale (tuning size — use THIS for damping)
//   A2: Inscribed circle — tangent to inner square at 4 points
//       Circle must be ROUND and touch square at top/bottom/left/right
//   A3: Center cross  — X/Y axis lines (X right, Y top)
//   A4: X / Y labels  — confirm polarity (X right, Y up; invert if wrong)
//   A5: Scale ref     — outer box is max; inner box is tuning size
//   B1: Blanking line — two horizontal segments with center gap
//   B2: Damping ref   — spacing of gap must be symmetric
//   B3: Speed dots    — row of 6 dots (5th barely lit, 6th blanked)
//
// Scan speed note:
//   - 12K mode: inner square ≈ 15° optical max
//   - 30K mode: inner square ≈  8° optical max
//   Scale pattern DOWN until circle just starts to distort, then back 10%.
//
// Tuning sequence (ILDA spec §3-§8):
//   1. Adjust Y damping (observe lower-right corner of inner square)
//   2. Adjust Y servo gain (circle top/bottom must touch square)
//   3. Adjust X damping (upper-right corner of inner square)
//   4. Adjust X servo gain (circle left/right must touch square)
//   5. Circle should be perfectly round and touch square at 4 points
//   6. Adjust DC offset — cross must be centered on screen
// ══════════════════════════════════════════════════════════════
static size_t ilda_test(LaserPoint* o, size_t mx,
                         uint32_t phase, uint8_t bright, uint8_t size_ch) {
    size_t n = 0;

    // size_ch (0–255) controls inner square / circle scan size
    // bright controls laser brightness via colorOut()
    // size_ch=128 → ~8° optical (@30kpps, ILDA rated)
    // size_ch=255 → ~15° optical (12K mode)
    const float OUTER = SC * 0.88f;
    const float INNER = OUTER * 0.5f * (0.3f + (size_ch / 255.0f) * 0.7f);

    // Pre-computed colors via colorOut() so gamma + white balance apply
    uint8_t WR, WG, WB;  colorOut(220, 220, 220, bright, WR, WG, WB);  // main geometry
    uint8_t AR, AG, AB;  colorOut(0,   200,   0, bright, AR, AG, AB);  // axis / cross
    uint8_t DR, DG, DB;  colorOut(180, 180, 180, bright, DR, DG, DB);  // outer square (dim)

    // ── A1: Outer square (full scan boundary) ────────────────────────
    // Scanned once as reference — do NOT use for damping adjustment
    blankMove(o, n, mx, -OUTER, -OUTER);
    ap(o, n, mx, -OUTER, -OUTER, DR, DR, DR, 0);
    ap(o, n, mx,  OUTER, -OUTER, DR, DR, DR, 0);
    ap(o, n, mx,  OUTER,  OUTER, DR, DR, DR, 0);
    ap(o, n, mx, -OUTER,  OUTER, DR, DR, DR, 0);
    ap(o, n, mx, -OUTER, -OUTER, DR, DR, DR, 0);

    // ── A2: Inner square (tuning square — observe corners here) ──────
    blankMove(o, n, mx, -INNER, -INNER);
    ap(o, n, mx, -INNER, -INNER, WR, WG, WB, 0);
    ap(o, n, mx,  INNER, -INNER, WR, WG, WB, 0);
    ap(o, n, mx,  INNER,  INNER, WR, WG, WB, 0);
    ap(o, n, mx, -INNER,  INNER, WR, WG, WB, 0);
    ap(o, n, mx, -INNER, -INNER, WR, WG, WB, 0);

    // ── A2: Inscribed circle (radius = INNER, tangent to inner square) ──
    // At correct Y gain: top and bottom of circle touch inner square edges
    // At correct X gain: left and right of circle touch inner square edges
    // Circle must be perfectly round (not elliptical)
    {
        const int CPTS = 80;
        blankMove(o, n, mx, cosf(0)*INNER, sinf(0)*INNER);
        for (int i = 0; i <= CPTS; i++) {
            float a = PI2 * i / CPTS;
            ap(o, n, mx, cosf(a)*INNER, sinf(a)*INNER, WR, WG, WB, 0);
        }
    }

    // ── A3/A4: Center cross + axis labels ────────────────────────────
    // X axis (horizontal line, full width to outer square)
    blankMove(o, n, mx, -OUTER, 0);
    ap(o, n, mx, -OUTER, 0, AR, AG, AB, 0);
    ap(o, n, mx,  OUTER, 0, AR, AG, AB, 0);
    // Y axis (vertical line)
    blankMove(o, n, mx, 0, -OUTER);
    ap(o, n, mx, 0, -OUTER, AR, AG, AB, 0);
    ap(o, n, mx, 0,  OUTER, AR, AG, AB, 0);

    // "X" label: right side of horizontal axis (A4 — confirms polarity)
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
    // "Y" label: top of vertical axis
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

    // ── B1/B2: Blanking test — two horizontal lines with center gap ──
    // Correct: gap is centered and symmetric around the vertical tick
    // Purpose: check blanking damping and servo gain
    {
        float by1 = -OUTER * 0.68f;  // B1: upper blanking line
        float by2 = -OUTER * 0.78f;  // B2: lower blanking line
        float bx  =  INNER * 0.3f;   // center gap half-width

        // Vertical tick (reference mark for gap centering)
        blankMove(o, n, mx, 0, by2 - OUTER*0.04f);
        ap(o, n, mx, 0, by2 - OUTER*0.04f, WR, WG, WB, 0);
        ap(o, n, mx, 0, by1 + OUTER*0.04f, WR, WG, WB, 0);

        // B1: upper line — left segment, gap, right segment
        blankMove(o, n, mx, -OUTER*0.55f, by1);
        ap(o, n, mx, -OUTER*0.55f, by1, WR, WG, WB, 0);
        ap(o, n, mx, -bx,          by1, WR, WG, WB, 0);
        blankMove(o, n, mx,  bx,   by1);  // blank over gap
        ap(o, n, mx,  bx,          by1, WR, WG, WB, 0);
        ap(o, n, mx,  OUTER*0.55f, by1, WR, WG, WB, 0);

        // B2: lower line — shorter, same gap structure
        blankMove(o, n, mx, -OUTER*0.40f, by2);
        ap(o, n, mx, -OUTER*0.40f, by2, WR, WG, WB, 0);
        ap(o, n, mx, -bx*0.7f,     by2, WR, WG, WB, 0);
        blankMove(o, n, mx,  bx*0.7f, by2);
        ap(o, n, mx,  bx*0.7f,     by2, WR, WG, WB, 0);
        ap(o, n, mx,  OUTER*0.40f, by2, WR, WG, WB, 0);
    }

    // ── B3/B4: Speed dots (6 dots, decreasing brightness) ──────────
    // Dot 5 (idx 4) should be barely visible; dot 6 (idx 5) fully blanked.
    // If dot 6 is visible: blanking DC offset too high → reduce it.
    // If dot 5 is invisible: blanking threshold too tight → raise it slightly.
    {
        const int NDOTS = 6;
        float dy = -OUTER * 0.88f;
        float dx_start = -OUTER * 0.30f;
        float dx_step  =  OUTER * 0.12f;
        // Brightness decreases linearly: dot 0 = full, dot 5 = 0
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

const CalibPatternInfo CALIB_INFO[CALIB_PATTERN_COUNT] = {
    {"Gamma Ramp",
     "Brightness from black to white — check linearity",
     "Both ramps must look equally smooth"},

    {"White Balance",
     "R / G / B / White each a line — compare channel brightness",
     "All four lines must appear equally bright"},

    {"Rainbow",
     "Full color wheel — check all hues",
     "Smooth transitions, all 6 primary colors visible"},

    {"Step Ramp",
     "8 discrete brightness steps — check gamma compression",
     "All 8 steps must be visible and appear equally large"},

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
     "White bar vs. stacked R/G/B bars — perceptual color balance",
     "Left white must visually match combined R+G+B on the right"},

    {"ILDA Test Pattern",
     "Official ILDA standard test pattern — galvo alignment & scanner tuning",
     "Circle must be perfectly round and touch inner square at 4 points. "
     "Adjust size slider until circle just stops distorting, then add 10%. "
     "Sequence: Y damping → Y gain → X damping → X gain → DC offset."},
};

using PFn = size_t(*)(LaserPoint*, size_t, uint32_t, uint8_t, uint8_t);
static const PFn DISPATCH[CALIB_PATTERN_COUNT] = {
    gamma_ramp, white_balance, rainbow,
    step_ramp,  channel_sep,  saturation_wheel,
    focus_test, scan_linearity, blanking_test,
    aspect_ratio, corner_test, color_temp,
    ilda_test,
};

size_t generate(uint8_t idx, LaserPoint* out, size_t max_pts,
                uint32_t phase, uint8_t brightness, uint8_t channel) {
    if (idx >= CALIB_PATTERN_COUNT || !out) return 0;
    return DISPATCH[idx](out, max_pts, phase, brightness, channel);
}

} // namespace calib_patterns