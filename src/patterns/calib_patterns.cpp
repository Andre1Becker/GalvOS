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
#include "text_renderer.h"
#include "util/mem_registry.h"
#include "util/ps_scratch.h"
#include <math.h>
#include <string.h>
#include <initializer_list>
#include <Arduino.h>

namespace calib_patterns {

// Running total of PSRAM vertex scratch (was DRAM .bss before v6.04.0)
static size_t s_scratchBytes = 0;
static void trackScratch(size_t bytes) {
    s_scratchBytes += bytes;
    memreg::track("Calib Scratch", s_scratchBytes, true);
}

// ── constants ────────────────────────────────────────────────
static constexpr float PI2  = 6.2831853f;
static constexpr float SC   = 28000.0f;  // ±88% full deflection

// ── helper functions ──────────────────────────────────────────

// Apply brightness scale to RGB triple.
// NOTE: gamma is intentionally NOT applied here. galvo_out.cpp:rgbWrite()
// applies it exactly once to every LaserPoint regardless of source.
// Applying it a second time here squares the curve and crushes mid/low
// brightness below thresh_r/g/b, making gain_r/g/b sliders appear to
// have no effect on calib patterns.
static inline void colorOut(uint8_t ri, uint8_t gi, uint8_t bi,
                             uint8_t bright,
                             uint8_t& ro, uint8_t& go, uint8_t& bo) {
    ro = (uint8_t)(((uint16_t)ri * bright) / 255);
    go = (uint8_t)(((uint16_t)gi * bright) / 255);
    bo = (uint8_t)(((uint16_t)bi * bright) / 255);
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

// ══════════════════════════════════════════════════════════════
// PATTERN 0: GAMMA-RAMPE
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
//
// The fixed physical X-mirror of this build (see pattern_engine::
// applyCalibration()) is corrected once, globally, downstream of every
// pattern generator -- so this table states plain DAC-space positions and
// does not need to compensate for it itself.
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
// PATTERN 16: THREE CIRCLES -- RGB brightness matching
// ══════════════════════════════════════════════════════════════
// Three same-size circles side by side, pure R / G / B. Unlike the other
// patterns here, channel is deliberately ignored: the whole point is
// spatial separation so all three colors are visible at once, with no
// need to switch a channel selector. Rendered through the normal
// generate() pipeline (gain_r/g/b + gamma applied downstream in
// galvo_out.cpp, see colorOut() comment above) -- so the Color gain
// (white balance) sliders directly change what's on screen: raise/lower
// Gain R/G/B until all three circles look equally bright.
static size_t three_circles(LaserPoint* o, size_t mx,
                             uint32_t phase, uint8_t bright, uint8_t ch) {
    (void)phase; (void)ch;  // static layout, channel-agnostic
    size_t n = 0;
    const float cx = SC * 0.5f, rad = SC * 0.28f;
    const float cxs[3] = { -cx, 0.f, cx };
    const uint8_t base[3][3] = { {255,0,0}, {0,255,0}, {0,0,255} };
    for (int c = 0; c < 3; c++) {
        uint8_t ro, go, bo;
        colorOut(base[c][0], base[c][1], base[c][2], bright, ro, go, bo);
        blankMove(o, n, mx, cxs[c] + rad, 0.f);
        for (int i = 0; i <= 40; i++) {
            float a = PI2 * i / 40;
            ap(o, n, mx, cxs[c] + cosf(a)*rad, sinf(a)*rad, ro, go, bo, 0);
        }
    }
    return n;
}

// ══════════════════════════════════════════════════════════════
// DISPATCH + METADATA

// The shared live->optimizer mapping, deliberately with NO specialization: a
// calibration pattern must show the live settings themselves, so anything
// added on top here (a modulator binding, a per-family density policy) would
// make the tuning target move while it is being read.
static inline optimizer::OptimizerConfig liveOptimizerConfig() {
    return optimizer::configFromLive(gOptimizerConfig,
                                     gProjection.galvo_rated_kpps,
                                     gProjection.galvo_kpps);
}

// ──────────────────────────────────────────────────────────────
// PATTERN 7: OPT CORNER SWEEP
// Tune: corner_angle_deg | min_corner_pts | max_corner_pts
// 8 V-notches, identical edge length, tip angle 8°→175° left to right.
// cornerSeverity() is purely angular, so ONLY the tip angle drives dwell.
// Notches whose tip angle is BELOW corner_angle_deg get min_corner_pts;
// above it dwell grows toward max_corner_pts at 175°.
// Correct tuning: leftmost 1-2 notches sweep
// through almost without pause; rightmost notch has a clearly visible
// dwell dot at its tip. Adjust corner_angle_deg until the transition
// falls at the desired sharpness threshold.
// ──────────────────────────────────────────────────────────────
static size_t opt_corner_sweep(LaserPoint* o, size_t m,
                                uint32_t ph, uint8_t bright, uint8_t) {
    (void)ph;
    static constexpr float PI2 = 6.2831853f;
    const int N = 8;
    static const float angDeg[8] = {8,20,35,55,80,110,145,175};
    const float sc = SC * 0.85f;
    const float L  = sc * 0.11f;
    const float spacing = sc * 2.0f / (N - 1);
    const optimizer::OptimizerConfig cfg = liveOptimizerConfig();

    optimizer::PathVertex verts[8][3];
    optimizer::PathSegment segs[8];
    for (int i = 0; i < N; i++) {
        float th = angDeg[i] * (float)M_PI / 180.f;
        float w = L * cosf(th * 0.5f), h = L * sinf(th * 0.5f);
        float cx = -sc + i * spacing;
        float hue = PI2 * i / (float)N;
        uint8_t r, g, b;
        colorOut((uint8_t)(128 + 127 * sinf(hue)),
                 (uint8_t)(128 + 127 * sinf(hue + 2.094f)),
                 (uint8_t)(128 + 127 * sinf(hue + 4.189f)),
                 bright, r, g, b);
        verts[i][0] = optimizer::PathVertex(cx - w, 0.f, r, g, b, true);
        verts[i][1] = optimizer::PathVertex(cx,     h,   r, g, b, false);
        verts[i][2] = optimizer::PathVertex(cx + w, 0.f, r, g, b, false);
        segs[i] = optimizer::PathSegment(verts[i], 3, false);
    }
    return optimizer::optimize(segs, N, o, m, cfg);
}

// ──────────────────────────────────────────────────────────────
// PATTERN 8: OPT DENSITY RAMP
// Tune: pts_per_1000_units | resample_enabled | resample_spacing_units
// 5 horizontal lines of increasing length (8%→96% frame width).
// Per-unit-length point spacing must look identical on all 5 lines.
// Density mismatch between short/long = pts_per_1000_units and
// resample_spacing_units are not mutually consistent (target:
// 1000/ppu ≈ spacing_units). Longest line also probes max_step_units:
// enable vel_clamp_enabled and reduce max_step_units until subdivision
// dots become visible on the longest line.
// ──────────────────────────────────────────────────────────────
static size_t opt_density_ramp(LaserPoint* o, size_t m,
                                uint32_t ph, uint8_t bright, uint8_t) {
    (void)ph;
    static constexpr float PI2 = 6.2831853f;
    const int N = 5;
    static const float halfLen[5] = {0.08f, 0.30f, 0.52f, 0.74f, 0.96f};
    const optimizer::OptimizerConfig cfg = liveOptimizerConfig();

    optimizer::PathVertex verts[5][2];
    optimizer::PathSegment segs[5];
    for (int i = 0; i < N; i++) {
        float y  = SC * (-0.8f + i * 0.4f);
        float hw = SC * halfLen[i];
        float hue = PI2 * i / (float)N;
        uint8_t r, g, b;
        colorOut((uint8_t)(128 + 127 * sinf(hue)),
                 (uint8_t)(128 + 127 * sinf(hue + 2.094f)),
                 (uint8_t)(128 + 127 * sinf(hue + 4.189f)),
                 bright, r, g, b);
        verts[i][0] = optimizer::PathVertex(-hw, y, r, g, b, true);
        verts[i][1] = optimizer::PathVertex( hw, y, r, g, b, false);
        segs[i] = optimizer::PathSegment(verts[i], 2, false);
    }
    return optimizer::optimize(segs, N, o, m, cfg);
}

// ──────────────────────────────────────────────────────────────
// PATTERN 9: OPT JUMP RING TEST
// Tune: blank_samples | min_blank_samples | blank_pts_per_1000_units
//       ringing_comp_enabled | ring_freq_hz | ring_damping_ratio
// 5 small rings with strictly increasing gaps (0.15→0.75 SC).
// One optimize() call over all 5 segments so inter-ring jumps use
// the real distance-proportional, ZV-shaped path (emitBlankJump,
// n≠0 branch). Under-tuned ringing: visible flare/offset on the
// first few points of each ring, worst after the longest jump
// (rightmost). Measure ring_freq_hz from a scope step-response,
// then tune ring_damping_ratio until the flare disappears.
// ──────────────────────────────────────────────────────────────
static size_t opt_jump_ring(LaserPoint* o, size_t m,
                             uint32_t ph, uint8_t bright, uint8_t) {
    (void)ph;
    static constexpr float PI2 = 6.2831853f;
    const int N = 5, RS = 16;
    static const float gap[4] = {0.15f, 0.35f, 0.55f, 0.75f};
    float cx[5]; cx[0] = -0.9f * SC;
    for (int i = 1; i < N; i++) cx[i] = cx[i-1] + gap[i-1] * SC;
    const float rad = SC * 0.045f;
    const optimizer::OptimizerConfig cfg = liveOptimizerConfig();

    optimizer::PathVertex verts[5][16];
    optimizer::PathSegment segs[5];
    for (int i = 0; i < N; i++) {
        float hue = PI2 * i / (float)N;
        uint8_t r, g, b;
        colorOut((uint8_t)(128 + 127 * sinf(hue)),
                 (uint8_t)(128 + 127 * sinf(hue + 2.094f)),
                 (uint8_t)(128 + 127 * sinf(hue + 4.189f)),
                 bright, r, g, b);
        for (int k = 0; k < RS; k++) {
            float a = PI2 * k / (float)RS;
            verts[i][k] = optimizer::PathVertex(
                cx[i] + cosf(a) * rad, sinf(a) * rad, r, g, b, k == 0);
        }
        segs[i] = optimizer::PathSegment(verts[i], RS, true);
    }
    return optimizer::optimize(segs, N, o, m, cfg);
}

// ──────────────────────────────────────────────────────────────
// PATTERN 10: OPT VEL/ACCEL TEST
// Tune: vel_clamp_enabled | max_step_units
//       accel_clamp_enabled | max_accel_units
// Two probes in one optimize() call:
//   Diagonal: longest single lit run → first to show step subdivision
//     once max_step_units < natural per-tick spacing.
//   6-spike star (outer:inner = 0.95:0.06): the extreme radius ratio
//     creates large per-tick velocity swings at every spike/valley
//     transition → max_accel_units limits that swing. Without clamping:
//     overshoot past spike tips. With tuned values: sharp, settled tips.
// ──────────────────────────────────────────────────────────────
static size_t opt_vel_accel(LaserPoint* o, size_t m,
                             uint32_t ph, uint8_t bright, uint8_t) {
    (void)ph;
    static constexpr float PI2 = 6.2831853f;
    uint8_t wr, wg, wb;
    colorOut(255, 255, 255, bright, wr, wg, wb);

    optimizer::PathVertex diag[2];
    diag[0] = optimizer::PathVertex(-SC, -SC, wr, wg, wb, true);
    diag[1] = optimizer::PathVertex( SC,  SC, wr, wg, wb, false);

    const int SP = 6;
    optimizer::PathVertex spike[SP * 2];
    const float outer = SC * 0.95f, inner = SC * 0.06f;
    for (int i = 0; i < SP * 2; i++) {
        float a = PI2 * i / (float)(SP * 2) - (float)M_PI / 2.f;
        float rr = (i % 2 == 0) ? outer : inner;
        uint8_t r, g, b;
        colorOut(255, (uint8_t)(80 + i * 10), 0, bright, r, g, b);
        spike[i] = optimizer::PathVertex(cosf(a) * rr, sinf(a) * rr, r, g, b, i == 0);
    }

    // Diagonal: deliberately sparse — no interpolation, just the two
    // endpoints.  The raw inter-point step equals the full frame diagonal
    // (~79 k units).  vel_clamp (not density) adds subdivisions, so the
    // effect of max_step_units becomes directly visible.  Reduce
    // max_step_units below ~200 to see dots appear on the line.
    optimizer::OptimizerConfig cfgDiag = liveOptimizerConfig();
    cfgDiag.pts_per_1000_units  = 0.0f;   // suppress density interpolation
    cfgDiag.resample_enabled    = false;
    cfgDiag.accel_clamp_enabled = false;   // vel probe only

    const optimizer::OptimizerConfig cfg = liveOptimizerConfig();
    optimizer::PathSegment diagSeg(diag, 2, false);
    optimizer::PathSegment starSeg(spike, SP * 2, true);

    size_t n = optimizer::optimize(&diagSeg, 1, o, m, cfgDiag);

    // Second call into the same frame -- hand it the diagonal's end position
    // and what is left of the frame budget, or the star plans a whole frame
    // of its own and jumps to its first vertex from nowhere.
    optimizer::OptimizerConfig cfgStar = cfg;
    if (optimizer::frameContext(cfgStar, o, n))
        n += optimizer::optimize(&starSeg, 1, o + n, m - n, cfgStar);
    return n;
}

// ──────────────────────────────────────────────────────────────
// CAMERA-IN-THE-LOOP PATTERNS (11-16)
// Geometry mirrors idealPolylines() in the host-side optimizeGalvo.py --
// r = 30000, h = r/2 = 15000 -- so the ground truth the host rasterizes for
// homography/error scoring matches what actually gets drawn. Color is
// caller-selectable via the `ch` argument (same 0=white/1=R/2=G/3=B
// convention as ilda_test/aspect_ratio/dac_range_box/zone_outline) so the
// host script can pick it per session via POST /api/calib-cam/start
// {"channel": N} -- the ESP32 defaults to 3 (blue) when the field is
// omitted: a single-diode dot can't smear/offset on the mono camera the
// way a combined white dot could if R/G/B aren't perfectly co-boresighted.
// All routed through liveOptimizerConfig() like the opt_* patterns above.
static inline void camColorOut(uint8_t ch, uint8_t bright,
                                uint8_t& ro, uint8_t& go, uint8_t& bo) {
    if (ch == 1)      colorOut(bright, 0, 0, bright, ro, go, bo);
    else if (ch == 2) colorOut(0, bright, 0, bright, ro, go, bo);
    else if (ch == 3) colorOut(0, 0, bright, bright, ro, go, bo);
    else              colorOut(bright, bright, bright, bright, ro, go, bo);
}
// ──────────────────────────────────────────────────────────────
static constexpr float CAM_R = 30000.0f;
static constexpr float CAM_H = CAM_R * 0.5f;   // 15000

// corners4Radius() -- the DAC-space half-extent CAM_CORNERS4's 4 dots are
// actually drawn at, derived fresh from the LIVE dac_limit_min/max
// output-limiting window (Config tab -> Output Limiting) instead of the
// fixed CAM_R design radius above. Without this, a dac_limit window tighter
// than +-CAM_R force-blanks the corner dots outright (galvo_out.cpp's
// rail-clip: any point outside [dac_limit_min, dac_limit_max] is clamped
// AND blanked, never just dimmed) -- 'calibrate' then finds 0 dots even
// though the laser is armed and every OTHER calib-cam pattern (CAM_H=15000,
// half of CAM_R, comfortably inside any sane window) still renders fine.
// Called fresh every generate() (not cached), so a live dac_limit retune
// takes effect on the very next frame with no restart needed.
static float corners4Radius() {
    int32_t lo = (int32_t)gConfig.dac_limit_min - 0x8000;   // <= 0 typically
    int32_t hi = (int32_t)gConfig.dac_limit_max - 0x8000;   // >= 0 typically
    float safe = (float)min(-lo, hi);
    // 3% headroom so the dwell dots sit safely inside the clamp rather than
    // right on its edge (float/int rounding, DAC settling) -- never exceeds
    // the original CAM_R design radius when the window is wide enough for it.
    return constrain(safe * 0.97f, 1000.0f, CAM_R);
}

// PATTERN 11: CORNERS4 -- 4 static dots, homography reference.
// Deliberately does NOT go through optimizer::optimize()'s corner-dwell
// heuristic (corner_angle_deg/min_corner_pts/max_corner_pts): those are
// exactly what a host tuning run sweeps, and could shrink dwell to nothing.
// Instead each dot is a manually-held point (CALIB_CAM_DOT_DWELL_PTS ticks),
// same "patterns that manage their own point emission" pattern documented
// on optimizer::emitBlankTo() -- only the inter-dot jump goes through the
// optimizer (Pillar 2/3 distance-proportional, ringing-shaped blanking).
static size_t cam_corners4(LaserPoint* o, size_t m,
                            uint32_t, uint8_t bright, uint8_t ch) {
    size_t n = 0;
    uint8_t wr, wg, wb; camColorOut(ch, bright, wr, wg, wb);
    const float r = corners4Radius();
    const float dots[4][2] = {
        { -r, -r }, { r, -r }, { r, r }, { -r, r },
    };
    const optimizer::OptimizerConfig cfg = liveOptimizerConfig();
    for (const auto& d : dots) {
        optimizer::emitBlankTo(o, n, m, d[0], d[1], cfg);
        for (uint8_t k = 0; k < CALIB_CAM_DOT_DWELL_PTS && n < m; k++)
            o[n++] = LaserPoint((int16_t)d[0], (int16_t)d[1], wr, wg, wb, 0);
    }
    return n;
}

// Exposed for /api/calib-cam/start's response (web_ui.cpp) so
// optimizeGalvo.py's 'calibrate' can build its homography target corners
// from the value the controller ACTUALLY used, instead of assuming a fixed
// dacRange from camConfig.json that can silently drift out of sync with the
// live dac_limit_min/max window.
float cornersRadius() { return corners4Radius(); }

// PATTERN 12: SQUARE -- half-size +-15000, sharp (90 deg) corners.
static size_t cam_square(LaserPoint* o, size_t m,
                          uint32_t, uint8_t bright, uint8_t ch) {
    uint8_t wr, wg, wb; camColorOut(ch, bright, wr, wg, wb);
    optimizer::PathVertex verts[4] = {
        optimizer::PathVertex(-CAM_H, -CAM_H, wr, wg, wb, true),
        optimizer::PathVertex( CAM_H, -CAM_H, wr, wg, wb, false),
        optimizer::PathVertex( CAM_H,  CAM_H, wr, wg, wb, false),
        optimizer::PathVertex(-CAM_H,  CAM_H, wr, wg, wb, false),
    };
    optimizer::PathSegment seg(verts, 4, true);
    return optimizer::optimize(&seg, 1, o, m, liveOptimizerConfig());
}

// PATTERN 13: STAR -- 5-point self-intersecting star (pentagram), half-size.
// Vertex k at angle k*(4*pi/5) - pi/2, radius CAM_H, connecting every 2nd
// point of a regular pentagon -- matches the host's idealPolylines() star.
static size_t cam_star(LaserPoint* o, size_t m,
                        uint32_t, uint8_t bright, uint8_t ch) {
    uint8_t wr, wg, wb; camColorOut(ch, bright, wr, wg, wb);
    optimizer::PathVertex verts[5];
    for (int k = 0; k < 5; k++) {
        float a = k * (4.0f * (float)M_PI / 5.0f) - (float)M_PI / 2.0f;
        verts[k] = optimizer::PathVertex(cosf(a) * CAM_H, sinf(a) * CAM_H,
                                          wr, wg, wb, k == 0);
    }
    optimizer::PathSegment seg(verts, 5, true);
    return optimizer::optimize(&seg, 1, o, m, liveOptimizerConfig());
}

// PATTERN 14: SEGMENTS -- 4 vertical lines, x = -15000,-5000,5000,15000,
// y from -h to h. One optimize() call over all 4 so the inter-line jumps
// use the real distance-proportional, ZV-shaped blank path -- exercises the
// blanking S-curve (Pillar 2/3) the same way opt_jump_ring does.
static size_t cam_segments(LaserPoint* o, size_t m,
                            uint32_t, uint8_t bright, uint8_t ch) {
    uint8_t wr, wg, wb; camColorOut(ch, bright, wr, wg, wb);
    static const float xs[4] = { -CAM_H, -CAM_H * (1.0f / 3.0f),
                                   CAM_H * (1.0f / 3.0f),  CAM_H };
    optimizer::PathVertex verts[4][2];
    optimizer::PathSegment segs[4];
    for (int i = 0; i < 4; i++) {
        verts[i][0] = optimizer::PathVertex(xs[i], -CAM_H, wr, wg, wb, true);
        verts[i][1] = optimizer::PathVertex(xs[i],  CAM_H, wr, wg, wb, false);
        segs[i] = optimizer::PathSegment(verts[i], 2, false);
    }
    return optimizer::optimize(segs, 4, o, m, liveOptimizerConfig());
}

// PATTERN 15: CIRCLE -- radius 15000, 128 base points (matches the host's
// ground-truth vertex count so optimizer resampling only adds points, never
// changes the primitive the host scores against).
static size_t cam_circle(LaserPoint* o, size_t m,
                          uint32_t, uint8_t bright, uint8_t ch) {
    uint8_t wr, wg, wb; camColorOut(ch, bright, wr, wg, wb);
    static constexpr int N = 128;
    // PSRAM, not stack: patterns::task's stack is a tight 12 KB budget
    // (see main.cpp's startTask comment on a prior stack-canary crash).
    // Rewritten every call; formerly DRAM .bss.
    static optimizer::PathVertex* verts = nullptr;
    if (!psScratch(verts, N)) return 0;
    static bool tracked = false;
    if (!tracked) { tracked = true; trackScratch(N * sizeof(optimizer::PathVertex)); }
    for (int i = 0; i < N; i++) {
        float a = PI2 * i / (float)N;
        verts[i] = optimizer::PathVertex(cosf(a) * CAM_H, sinf(a) * CAM_H,
                                          wr, wg, wb, i == 0);
    }
    optimizer::PathSegment seg(verts, N, true);
    return optimizer::optimize(&seg, 1, o, m, liveOptimizerConfig());
}

// PATTERN 16: SPIRAL -- 3-turn Archimedean spiral, radius 0.15*h (2250) to
// h (15000), 512 base points. Open path (no closing edge) -- matches the
// host's theta 0..6*pi, radius linspace(0.15*h, h) ground truth.
static size_t cam_spiral(LaserPoint* o, size_t m,
                          uint32_t, uint8_t bright, uint8_t ch) {
    uint8_t wr, wg, wb; camColorOut(ch, bright, wr, wg, wb);
    static constexpr int N = 512;
    static const float rInner = CAM_H * 0.15f;
    // PSRAM, not stack -- 512 PathVertex is 6 KB, half of patterns::task's
    // entire 12 KB stack budget (see main.cpp's startTask comment on a prior
    // stack-canary crash). Rewritten every call; formerly DRAM .bss.
    static optimizer::PathVertex* verts = nullptr;
    if (!psScratch(verts, N)) return 0;
    static bool tracked = false;
    if (!tracked) { tracked = true; trackScratch(N * sizeof(optimizer::PathVertex)); }
    for (int i = 0; i < N; i++) {
        float t = (float)i / (float)(N - 1);
        float theta = t * 6.0f * (float)M_PI;
        float r = rInner + (CAM_H - rInner) * t;
        verts[i] = optimizer::PathVertex(cosf(theta) * r, sinf(theta) * r,
                                          wr, wg, wb, i == 0);
    }
    optimizer::PathSegment seg(verts, N, false);
    return optimizer::optimize(&seg, 1, o, m, liveOptimizerConfig());
}

// ══════════════════════════════════════════════════════════════
// PATTERN 17: WARP TEST GRID
//
// Border rectangle + gWarp.gridSize's interior grid lines (one pair of
// lines per interior control-point column/row; gridSize 2 draws just the
// border, same as N=2's plain-keystone case having no interior points).
// Goes through the normal optimizer::optimize() path, so when Warp is
// enabled (gWarp.enabled) it picks up the SAME warp stage every other
// pattern uses (point_optimizer.cpp's Stage 0.4 Warp) -- what you see here
// is exactly what the keystone correction is currently doing, not a
// special-cased preview.
//
// HOW TO USE:
//   1. Warp OFF: note where the grid lines land on the projection surface.
//   2. Drag the Warp panel's control points (or run scripts/optimizegalvo.py
//      calibrate-warp, see Prompt 7b) until the projected lines line up
//      with the intended rectangle on the wall.
// ══════════════════════════════════════════════════════════════
static size_t warp_test_grid(LaserPoint* o, size_t mx,
                              uint32_t /*phase*/, uint8_t bright, uint8_t ch) {
    uint8_t r, g, b; camColorOut(ch, bright, r, g, b);

    uint8_t n = gWarp.gridSize;
    if (n < 2) n = 2;
    if (n > WARP_GRID_MAX) n = WARP_GRID_MAX;
    uint8_t interior = (n > 2) ? (uint8_t)(n - 2) : 0;

    optimizer::PathVertex borderV[4] = {
        optimizer::PathVertex(-CAM_H, -CAM_H, r, g, b, true),
        optimizer::PathVertex( CAM_H, -CAM_H, r, g, b, false),
        optimizer::PathVertex( CAM_H,  CAM_H, r, g, b, false),
        optimizer::PathVertex(-CAM_H,  CAM_H, r, g, b, false),
    };
    static constexpr uint8_t kMaxInterior = WARP_GRID_MAX - 2;  // 3
    optimizer::PathVertex vLineV[kMaxInterior][2];
    optimizer::PathVertex hLineV[kMaxInterior][2];
    optimizer::PathSegment segs[1 + 2 * kMaxInterior];
    size_t segCount = 0;
    segs[segCount++] = optimizer::PathSegment(borderV, 4, true);

    for (uint8_t i = 0; i < interior; i++) {
        float u = -1.0f + (2.0f * (i + 1)) / (n - 1);
        float x = u * CAM_H;
        vLineV[i][0] = optimizer::PathVertex(x, -CAM_H, r, g, b, true);
        vLineV[i][1] = optimizer::PathVertex(x,  CAM_H, r, g, b, false);
        segs[segCount++] = optimizer::PathSegment(vLineV[i], 2, false);
    }
    for (uint8_t i = 0; i < interior; i++) {
        float v = -1.0f + (2.0f * (i + 1)) / (n - 1);
        float y = v * CAM_H;
        hLineV[i][0] = optimizer::PathVertex(-CAM_H, y, r, g, b, true);
        hLineV[i][1] = optimizer::PathVertex( CAM_H, y, r, g, b, false);
        segs[segCount++] = optimizer::PathSegment(hLineV[i], 2, false);
    }

    return optimizer::optimize(segs, segCount, o, mx, liveOptimizerConfig());
}

// ══════════════════════════════════════════════════════════════
// PATTERNS 18-20: COLOR RAMP R/G/B -- duty->luminance linearity calibration
//
// CALIB_RAMP_FIELDS (32) equal-width vertical fields, left to right, PWM
// duty 0..255 linear across the fields. Drives only the target channel; the
// other two are forced to 0 (colorOut() is deliberately NOT used here --
// see below). Every field is an identical small raster block (same row
// count, same points per row, same dwell) -- geometry never varies, so
// measured luminance differs ONLY because of the commanded duty.
//
// Built with direct ap()/line() calls, bypassing optimizer::optimize()
// entirely (same style as three_circles/corner_color_map above): corner-
// dwell/density/velocity shaping would make point count and dwell vary
// between fields depending on the live optimizer sliders, which is exactly
// what this pattern must not do -- "identical points and dwell per field"
// is the whole measurement contract.
//
// gState.calib_raw_duty (set by /api/calib-pattern when idx is one of these
// three, see galvo_out.cpp galvoTask()) bypasses gain/dimmer/gamma/
// threshold entirely and feeds each point's r/g/b straight to PWM -- so the
// duty computed below IS the wire duty. colorOut()'s brightness scaling is
// therefore skipped too (it would reintroduce a `bright`-dependent
// scale factor into what must stay a raw, controlled ramp).
//
// scripts/calibrateColor.py arms each ramp in turn, captures one frame per
// ramp, and segments it into CALIB_RAMP_FIELDS equal-width column bins to
// read back per-field mean luminance.
// ══════════════════════════════════════════════════════════════
static constexpr int RAMP_ROWS      = 4;  // horizontal raster lines per field
static constexpr int RAMP_ROW_STEPS = 7;  // interpolated points per raster line

static size_t calibRampImpl(LaserPoint* o, size_t mx, uint8_t targetCh) {
    size_t n = 0;
    const float RW = SC * 0.85f;   // ramp half-width (total scanned extent)
    const float H  = SC * 0.35f;   // field half-height
    const float colW   = (RW * 2.0f) / CALIB_RAMP_FIELDS;
    const float rowGap = (H * 2.0f) / (RAMP_ROWS - 1);

    for (int i = 0; i < CALIB_RAMP_FIELDS; i++) {
        uint8_t duty = (uint8_t)((i * 255) / (CALIB_RAMP_FIELDS - 1));
        uint8_t r = (targetCh == 1) ? duty : 0;
        uint8_t g = (targetCh == 2) ? duty : 0;
        uint8_t b = (targetCh == 3) ? duty : 0;

        float xL = -RW + i * colW;
        float xR = xL + colW;

        // Zigzag raster fill within the field's column: each row starts
        // exactly where the previous one ended, so only a small vertical
        // hop (rowGap) needs to be blanked between rows -- no horizontal
        // jump within a field. line() itself blanks its own first point.
        for (int row = 0; row < RAMP_ROWS; row++) {
            float y = H - row * rowGap;
            bool leftToRight = (row % 2) == 0;
            float x0 = leftToRight ? xL : xR;
            float x1 = leftToRight ? xR : xL;
            line(o, n, mx, x0, y, x1, y, r, g, b, RAMP_ROW_STEPS);
        }
    }
    return n;
}

static size_t calib_ramp_r(LaserPoint* o, size_t mx, uint32_t, uint8_t, uint8_t) {
    return calibRampImpl(o, mx, 1);
}
static size_t calib_ramp_g(LaserPoint* o, size_t mx, uint32_t, uint8_t, uint8_t) {
    return calibRampImpl(o, mx, 2);
}
static size_t calib_ramp_b(LaserPoint* o, size_t mx, uint32_t, uint8_t, uint8_t) {
    return calibRampImpl(o, mx, 3);
}

// ══════════════════════════════════════════════════════════════
// PATTERNS 21-23: SECOND CAMERA BLOCK
//
// One camera pattern each for the three optimizer profiles that previously
// had none (Wireframe / Text / Particles), so the host tool can score them
// instead of leaving them tuned by eye. Same contract as 11-16: static
// geometry, flat camColorOut() color (a per-vertex hue would show up as
// real brightness non-uniformity), and everything routed through
// liveOptimizerConfig() so what the camera sees is what the live sliders
// actually do. Geometry is mirrored exactly in optimizeGalvo.py's
// idealPolylines() -- change one, change the other.
// ══════════════════════════════════════════════════════════════

// PATTERN 21: WIREFRAME -- cube, orthographic projection, 4 open 3-edge chains.
//
// Projection: the first two rows of a yaw-35deg / pitch-25deg rotation
// matrix, written out as literal coefficients rather than sinf/cosf calls so
// the firmware and the host's NumPy mirror produce identical numbers with no
// trig-implementation drift. A classic isometric view was rejected: it
// collapses two of the eight cube vertices onto the projected center and
// makes three edge pairs overlap, which reads as false double brightness in
// every brightness/deviation metric.
//
// Chain decomposition: every cube vertex has odd degree (3), so the 12 edges
// cannot be walked in one stroke -- 4 open trails of 3 edges each is the
// minimum, and each vertex ends up an endpoint of exactly one trail and an
// interior vertex of exactly one other. That is deliberately the same shape
// of geometry the Wireframe profile exists for: shared vertices between
// chains (open-endpoint full-dwell, see cornerSeverity()) plus three real
// blank jumps per frame for blankCorridorLeakage to score.
static constexpr float CAM_WF_HALF = CAM_H * 0.63f;   // 9450 -- projected
                                                       // extent stays < CAM_H
static size_t cam_wireframe(LaserPoint* o, size_t m,
                             uint32_t, uint8_t bright, uint8_t ch) {
    uint8_t wr, wg, wb; camColorOut(ch, bright, wr, wg, wb);
    static constexpr float PX_X = 0.81915f, PX_Z = 0.57358f;
    static constexpr float PY_X = 0.24238f, PY_Y = 0.90631f, PY_Z = 0.34614f;
    const float s = CAM_WF_HALF;

    float vx[8], vy[8];
    for (int i = 0; i < 8; i++) {
        // bit0 = x sign, bit1 = y sign, bit2 = z sign
        float x = (i & 1) ? s : -s;
        float y = (i & 2) ? s : -s;
        float z = (i & 4) ? s : -s;
        vx[i] = PX_X * x + PX_Z * z;
        vy[i] = PY_X * x + PY_Y * y - PY_Z * z;
    }

    static const uint8_t CHAINS[4][4] = {
        {1, 0, 2, 6},   // edges 0-1, 0-2, 2-6
        {2, 3, 1, 5},   // edges 2-3, 1-3, 1-5
        {0, 4, 5, 7},   // edges 0-4, 4-5, 5-7
        {4, 6, 7, 3},   // edges 4-6, 6-7, 3-7
    };
    optimizer::PathVertex verts[4][4];
    optimizer::PathSegment segs[4];
    for (int c = 0; c < 4; c++) {
        for (int k = 0; k < 4; k++) {
            uint8_t v = CHAINS[c][k];
            verts[c][k] = optimizer::PathVertex(vx[v], vy[v], wr, wg, wb, k == 0);
        }
        segs[c] = optimizer::PathSegment(verts[c], 4, false);
    }
    return optimizer::optimize(segs, 4, o, m, liveOptimizerConfig());
}

// PATTERN 22: TEXT -- fixed short string, real glyph geometry.
//
// Uses textrender::glyphOutlinePaths(), i.e. the SAME stroke font and the
// same layout math (advance widths, half-advance cell centering, string
// centered on x=0) the Text tool renders with -- not a hand-copied outline.
// What it deliberately does NOT reuse is text_renderer.cpp's internal
// textOptimizerConfig(): that applies a text-specific density override on
// top of the live profile, which would mean the camera scored something
// other than the Text profile's own sliders. Here the raw pen strokes go
// straight into optimizer::optimize() under liveOptimizerConfig().
//
// Three glyphs, not the full project name: at 3 chars the strokes are large
// enough for the host's corner ROIs (24 px across) to resolve individual
// vertices; at 6+ chars neighbouring strokes fall inside one ROI and
// cornerHotspot stops meaning anything.
static const char CAM_TEXT_STRING[] = "GAL";
static constexpr float CAM_TEXT_SCALE = 950.0f;   // half-width 13775, half-height 6650
static size_t cam_text(LaserPoint* o, size_t m,
                        uint32_t, uint8_t bright, uint8_t ch) {
    uint8_t wr, wg, wb; camColorOut(ch, bright, wr, wg, wb);
    static constexpr size_t MAX_PATHS = 16;
    static constexpr size_t MAX_VERTS = MAX_PATHS * textrender::GlyphSubpath::MAX_PTS;

    // PSRAM, not stack: GlyphSubpath is ~164 B, so 16 of them plus the
    // flattened vertex array would be ~10 KB against patterns::task's 12 KB
    // budget (same reasoning as cam_circle/cam_spiral above).
    static textrender::GlyphSubpath* paths = nullptr;
    static optimizer::PathVertex*    verts = nullptr;
    if (!psScratch(paths, MAX_PATHS)) return 0;
    if (!psScratch(verts, MAX_VERTS)) return 0;
    static bool tracked = false;
    if (!tracked) {
        tracked = true;
        trackScratch(MAX_PATHS * sizeof(textrender::GlyphSubpath) +
                     MAX_VERTS * sizeof(optimizer::PathVertex));
    }

    size_t np = textrender::glyphOutlinePaths(CAM_TEXT_STRING, CAM_TEXT_SCALE,
                                               paths, MAX_PATHS);
    if (np == 0) return 0;

    optimizer::PathSegment segs[MAX_PATHS];
    size_t vn = 0, sn = 0;
    for (size_t p = 0; p < np; p++) {
        const textrender::GlyphSubpath& sp = paths[p];
        if (sp.count < 2 || vn + sp.count > MAX_VERTS) continue;
        optimizer::PathVertex* base = verts + vn;
        for (uint8_t k = 0; k < sp.count; k++)
            verts[vn++] = optimizer::PathVertex(sp.x[k], sp.y[k], wr, wg, wb, k == 0);
        segs[sn++] = optimizer::PathSegment(base, sp.count, false);
    }
    if (sn == 0) return 0;
    return optimizer::optimize(segs, sn, o, m, liveOptimizerConfig());
}

// PATTERN 23: PARTICLES -- 12 isolated dwell dots, graded jump distances.
//
// This is the pattern that would have caught the v6.65.1 Starfield streaking
// regression: a Particles blank window sized for short hops, applied to a
// preset that scatters points across the whole canvas, leaves the beam still
// in flight when the laser re-arms -- "connect the dots" streaks instead of
// dots.
//
// Each dot is a degenerate 2-vertex OPEN segment (both vertices at the same
// coordinate). That is not a trick: both vertices are open endpoints, so
// cornerSeverity() returns 1.0 for each and the dot's dwell is exactly
// 2 x max_corner_pts, straight from the live profile. The edge between them
// has zero length, so it contributes no interior points at any density.
// Unlike cam_corners4 there is deliberately NO fixed-dwell override here --
// the whole measurement is "what does the LIVE config do to an isolated
// dot", so both the dwell and the jump into it must come from the sliders
// under test.
//
// Layout: a 4x3 grid (10000 units apart in x, 15000 in y, all inside the
// same +-CAM_H box the other camera patterns use, so the camera framing and
// the host's homography are unchanged). The visit order below is NOT the
// grid order -- it is chosen so the 12 jumps span short (10000-15000),
// medium (18000-25000) and long (31000-36000 units) in one frame, which is
// what makes a too-small blank window visible as elongation on the long
// jumps while the short ones still look fine.
static constexpr int CAM_PARTICLE_COUNT = 12;
static size_t cam_particles(LaserPoint* o, size_t m,
                             uint32_t, uint8_t bright, uint8_t ch) {
    uint8_t wr, wg, wb; camColorOut(ch, bright, wr, wg, wb);
    // {column 0..3, row 0..2} in visit order.
    static const uint8_t ORDER[CAM_PARTICLE_COUNT][2] = {
        {0,0}, {1,0}, {1,1}, {0,1}, {0,2}, {2,0},
        {3,2}, {2,1}, {3,0}, {1,2}, {3,1}, {2,2},
    };
    const float colStep = CAM_H * (2.0f / 3.0f);   // 10000

    optimizer::PathVertex verts[CAM_PARTICLE_COUNT][2];
    optimizer::PathSegment segs[CAM_PARTICLE_COUNT];
    for (int i = 0; i < CAM_PARTICLE_COUNT; i++) {
        float x = -CAM_H + ORDER[i][0] * colStep;
        float y = -CAM_H + ORDER[i][1] * CAM_H;
        verts[i][0] = optimizer::PathVertex(x, y, wr, wg, wb, false);
        verts[i][1] = optimizer::PathVertex(x, y, wr, wg, wb, false);
        segs[i] = optimizer::PathSegment(verts[i], 2, false);
    }
    return optimizer::optimize(segs, CAM_PARTICLE_COUNT, o, m, liveOptimizerConfig());
}

// Both camera blocks in one lookup: 11..16 then 21..23. Kept as two arrays
// rather than renumbering into one contiguous range because CALIB_WARP_GRID_IDX
// (web_ui.cpp) and CALIB_RAMP_BASE (scripts/calibrateColor.py) sit between them.
static const char* CAM_NAMES[CALIB_CAM_COUNT] = {
    "corners4", "square", "star", "segments", "circle", "spiral"
};
static const char* CAM2_NAMES[CALIB_CAM2_COUNT] = {
    "wireframe", "text", "particles"
};

int8_t camPatternIndex(const char* name) {
    if (!name) return -1;
    for (uint8_t i = 0; i < CALIB_CAM_COUNT; i++)
        if (strcmp(name, CAM_NAMES[i]) == 0) return (int8_t)(CALIB_CAM_BASE + i);
    for (uint8_t i = 0; i < CALIB_CAM2_COUNT; i++)
        if (strcmp(name, CAM2_NAMES[i]) == 0) return (int8_t)(CALIB_CAM2_BASE + i);
    return -1;
}

const char* camPatternName(uint8_t idx) {
    if (idx >= CALIB_CAM_BASE && idx < CALIB_CAM_BASE + CALIB_CAM_COUNT)
        return CAM_NAMES[idx - CALIB_CAM_BASE];
    if (idx >= CALIB_CAM2_BASE && idx < CALIB_CAM2_BASE + CALIB_CAM2_COUNT)
        return CAM2_NAMES[idx - CALIB_CAM2_BASE];
    return "";
}

// ══════════════════════════════════════════════════════════════
// DISPATCH + METADATA
// ══════════════════════════════════════════════════════════════
uint8_t profileOf(uint8_t idx) {
    switch (idx) {
        case 1:  // Aspect Ratio     -- square + circle
        case 3:  // DAC Range Box    -- rectangle + inscribed circle
        case 4:  // Zone Outline     -- polygon outline
        case 7:  // Opt Corner Sweep -- isolates corner_angle_deg / corner pts
        case 12: // Cam Square       -- sharp-corner polygon, ringing/dwell
        case 13: // Cam Star         -- self-intersecting star, corner dwell
            return OPT_PROFILE_VECTOR;
        case 0:  // Blanking Test    -- arc segments split by blank jumps
        case 2:  // ILDA Test        -- circle + square + blanked sub-figures
        case 6:  // Three Circles    -- three separate closed circles
        case 9:  // Opt Jump Ring    -- isolates blank_samples / ringing_comp
        case 14: // Cam Segments     -- 4 blanked lines, blanking S-curve
            return OPT_PROFILE_MULTIOBJECT;
        case 5:  // Corner Color Map -- four isolated dots
        case 11: // Cam Corners4     -- four isolated dwell dots
            return OPT_PROFILE_PARTICLES;
        case 8:  // Opt Density Ramp -- isolates pts_per_1000_units / resample
        case 15: // Cam Circle       -- density uniformity
            return OPT_PROFILE_SMOOTH;
        case 10: // Opt Vel/Accel    -- isolates max_step_units / max_accel
        case 16: // Cam Spiral       -- velocity clamps
            return OPT_PROFILE_WAVES;
        case 17: // Warp Grid Test   -- blanked border + interior lines
            return OPT_PROFILE_MULTIOBJECT;
        case 21: // Cam Wireframe    -- cube, 4 open chains + blank jumps
            return OPT_PROFILE_WIREFRAME;
        case 22: // Cam Text         -- real glyph strokes
            return OPT_PROFILE_TEXT;
        case 23: // Cam Particles    -- 12 isolated dwell dots, graded jumps
            return OPT_PROFILE_PARTICLES;
        case 18: // Ramp R           -- 32 static duty fields, geometry fixed
        case 19: // Ramp G
        case 20: // Ramp B
            return OPT_PROFILE_MULTIOBJECT;
        default:
            return OPT_PROFILE_VECTOR;
    }
}

const CalibPatternInfo CALIB_INFO[CALIB_PATTERN_COUNT] = {
    {"Blanking Test",
     "Alternating on/off segments \u2014 checks blanking accuracy",
     "Dark segments must be completely dark (no light leakage)"},

    {"Aspect Ratio",
     "Square + circle of identical size \u2014 checks X/Y gain match",
     "Circle must fit exactly inside the square corners"},

    {"ILDA Test Pattern",
     "Official ILDA standard test pattern \u2014 galvo alignment & scanner tuning",
     "Circle must be perfectly round and touch inner square at 4 points. "
     "Adjust size slider until circle just stops distorting, then add 10%. "
     "Sequence: Y damping -> Y gain -> X damping -> X gain -> DC offset."},

    {"DAC Range Box",
     "Rectangle + circle at exact dac_limit_max boundary \u2014 set safe scan range",
     "Raise dac_limit_max until box corners just clip, then back off 5%. "
     "Yellow box = limit boundary. Green circle = inscribed at same limit. "
     "Dim inner box = 50% reference. Adjust X/Y gain if circle is not round."},

    {"Projection Zone",
     "Outline of the touch-defined projection zone polygon \u2014 verify safe area",
     "Red = zone boundary, green dots = vertices. Edit the polygon "
     "in the Calibration tab, then enable zone clipping to blank the laser "
     "outside this area."},

    {"Corner Color Map",
     "One colored dot per corner (RGBW) \u2014 shows how the image is projected",
     "Position mapping: Red = top-left, Green = top-right, Blue = "
     "bottom-right, White = bottom-left. If a dot appears in the wrong "
     "corner the image is mirrored/rotated \u2014 fix with X/Y flip or invert."},

    {"Three Circles",
     "R / G / B circles side by side \u2014 match channel brightness by eye",
     "All three circles must appear equally bright; adjust Color gain "
     "R/G/B (Galvo Calibration card) until matched."},

    {"Corner Sweep",
     "8 V-notches, tip angle 8\u00b0\u2192175\u00b0, identical edge length",
     "Tune: corner_angle_deg / min_corner_pts / max_corner_pts. "
     "Notches below corner_angle_deg get min dwell; above it dwell grows to max. "
     "Adjust threshold until sharp patterns show a visible pause at the tip."},

    {"Density Ramp",
     "5 horizontal lines of increasing length (8%\u219296% frame)",
     "Tune: pts_per_1000_units / resample_enabled / resample_spacing_units. "
     "Point spacing must look identical on all 5 lines. "
     "Enable vel_clamp + reduce max_step_units to see subdivision on longest line."},

    {"Jump Ring Test",
     "5 small rings with strictly increasing inter-ring gaps",
     "Tune: blank_samples / blank_pts_per_1000_units / ringing_comp settings. "
     "Under-damped ringing appears as a flare at ring entry, worst on rightmost ring. "
     "Measure ring_freq_hz on scope, then tune ring_damping_ratio until flare gone."},

    {"Velocity & Accel Test",
     "Full-frame diagonal + 6-spike star (outer:inner = 0.95:0.06)",
     "Diagonal = vel_clamp probe (2 raw endpoints, no interpolation): "
     "enable vel_clamp and reduce max_step_units below ~200 to see "
     "subdivision dots appear on the diagonal. "
     "Star = accel_clamp probe: enable accel_clamp and reduce "
     "max_accel_units until spike tips stop overshooting."},

    {"Cam Corners4",
     "4 static dots at the frame corners -- camera homography reference",
     "Selected via /api/calib-cam/start, not the idx-based calib-pattern API. "
     "Each dot holds for CALIB_CAM_DOT_DWELL_PTS ticks regardless of live "
     "optimizer overrides, so it stays camera-visible during autotuning."},

    {"Cam Square",
     "Square, half-size, sharp corners -- ringing / corner-dwell probe",
     "Selected via /api/calib-cam/start. Runs under OPT_PROFILE_VECTOR."},

    {"Cam Star",
     "5-point self-intersecting star, half-size -- corner-dwell probe",
     "Selected via /api/calib-cam/start. Runs under OPT_PROFILE_VECTOR."},

    {"Cam Segments",
     "4 vertical lines with blanked jumps between -- blanking S-curve probe",
     "Selected via /api/calib-cam/start. Runs under OPT_PROFILE_MULTIOBJECT."},

    {"Cam Circle",
     "Circle, 128 base points -- point-density uniformity probe",
     "Selected via /api/calib-cam/start. Runs under OPT_PROFILE_SMOOTH."},

    {"Cam Spiral",
     "3-turn Archimedean spiral, 512 base points -- velocity-clamp probe",
     "Selected via /api/calib-cam/start. Runs under OPT_PROFILE_WAVES."},

    {"Warp Grid Test",
     "Border rectangle + gWarp.gridSize interior lines -- keystone preview",
     "Selected via /api/warp/test. Compare the projected grid against the "
     "intended rectangle, then adjust the Warp panel's control points (or "
     "run scripts/optimizegalvo.py calibrate-warp) until they line up."},

    {"Color Ramp R",
     "32 equal-width red duty fields, linear 0..255 left to right",
     "Bypasses gain/dimmer/gamma/threshold -- measured luminance should equal "
     "commanded duty. Run scripts/calibrateColor.py to capture and plot."},

    {"Color Ramp G",
     "32 equal-width green duty fields, linear 0..255 left to right",
     "Bypasses gain/dimmer/gamma/threshold -- measured luminance should equal "
     "commanded duty. Run scripts/calibrateColor.py to capture and plot."},

    {"Color Ramp B",
     "32 equal-width blue duty fields, linear 0..255 left to right",
     "Bypasses gain/dimmer/gamma/threshold -- measured luminance should equal "
     "commanded duty. Run scripts/calibrateColor.py to capture and plot."},

    {"Cam Wireframe",
     "Cube in orthographic projection, 4 open 3-edge chains -- chained-edge probe",
     "Selected via /api/calib-cam/start. Runs under OPT_PROFILE_WIREFRAME. "
     "Struts must meet cleanly at the shared vertices and the three blank "
     "jumps between chains must leave no visible trace."},

    {"Cam Text",
     "Fixed 3-glyph string via the real stroke font -- short-stroke probe",
     "Selected via /api/calib-cam/start. Runs under OPT_PROFILE_TEXT. "
     "Short crossbars must stay full length and the pen-up gaps inside a "
     "glyph must stay dark."},

    {"Cam Particles",
     "12 isolated dwell dots, jump distances graded short/medium/long",
     "Selected via /api/calib-cam/start. Runs under OPT_PROFILE_PARTICLES. "
     "All 12 must read as round, separate dots -- elongation or a missing "
     "dot on the LONG jumps means the blank window is too short for the "
     "distance (the v6.65.1 Starfield streaking failure)."},

};

using PFn = size_t(*)(LaserPoint*, size_t, uint32_t, uint8_t, uint8_t);
static const PFn DISPATCH[CALIB_PATTERN_COUNT] = {
    blanking_test, aspect_ratio, ilda_test,
    dac_range_box, zone_outline, corner_color_map, three_circles,
    opt_corner_sweep, opt_density_ramp, opt_jump_ring, opt_vel_accel,
    cam_corners4, cam_square, cam_star, cam_segments, cam_circle, cam_spiral,
    warp_test_grid,
    calib_ramp_r, calib_ramp_g, calib_ramp_b,
    cam_wireframe, cam_text, cam_particles,
};


// Cross-frame seam bridge (#4) state, same as presets::generate() -- moved out
// of generate() to file scope (was function-local static) so resetSeamState()
// below can reach into it. Persists across calib-cam sessions by design (a
// pattern's own animation continues smoothly frame to frame while it stays
// active), but that means it's stale the FIRST time a pattern starts after
// having been inactive for a while: sLastX/Y[idx] still holds wherever that
// pattern was last drawn, possibly minutes/sessions ago and nowhere near
// where it's about to start now. Left alone, the very next generate() call
// for that idx bridges from that stale position via a real (blanked, but
// large/fast) ZV-shaped jump -- see resetSeamState().
static float sLastX[CALIB_PATTERN_COUNT] = {0};
static float sLastY[CALIB_PATTERN_COUNT] = {0};
static bool  sHas[CALIB_PATTERN_COUNT]   = {false};

void resetSeamState(uint8_t idx) {
    if (idx < CALIB_PATTERN_COUNT) sHas[idx] = false;
}

size_t generate(uint8_t idx, LaserPoint* out, size_t max_pts,
                uint32_t phase, uint8_t brightness, uint8_t channel) {
    if (idx >= CALIB_PATTERN_COUNT || !out) return 0;
    size_t n = DISPATCH[idx](out, max_pts, phase, brightness, channel);

    // Static patterns produce ~0 jump -> skip.
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
