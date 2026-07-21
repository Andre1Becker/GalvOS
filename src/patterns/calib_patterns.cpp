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
#include <initializer_list>
#include <Arduino.h>

namespace calib_patterns {

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
// NOTE: the optical path mirrors the image on X (DAC +x ends up on the
// physical left, DAC -x on the physical right; Y is not mirrored). The
// table below assigns colors to DAC coordinates so the *physical* result
// matches the mapping above -- do not "simplify" this back to DAC-space
// left/right without re-verifying on the actual projection.
static size_t corner_color_map(LaserPoint* o, size_t mx,
                                uint32_t phase, uint8_t bright, uint8_t ch) {
    size_t n = 0;
    const float S = SC * 0.9f;   // corner distance from center

    // corner position + its RGB colour (before gamma / white-balance)
    struct Corner { float x, y; uint8_t r, g, b; };
    const Corner corners[4] = {
        { -S,  S,   0, 255,   0 },  // DAC top-left     -> physical top-right:    Green
        {  S,  S, 255,   0,   0 },  // DAC top-right     -> physical top-left:    Red
        {  S, -S, 255, 255, 255 },  // DAC bottom-right  -> physical bottom-left: White
        { -S, -S,   0,   0, 255 },  // DAC bottom-left   -> physical bottom-right:Blue
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
    cfg.vel_clamp_enabled            = gOptimizerConfig.vel_clamp_enabled;
    cfg.max_step_units               = gOptimizerConfig.max_step_units;
    cfg.accel_clamp_enabled          = gOptimizerConfig.accel_clamp_enabled;
    cfg.max_accel_units              = gOptimizerConfig.max_accel_units;
    // PPS-derived scaling: density + both clamps from rated/output kpps.
    optimizer::applyPpsScaling(cfg, gProjection.galvo_rated_kpps, gProjection.galvo_kpps);
    return cfg;
}

// ──────────────────────────────────────────────────────────────
// PATTERN 7: OPT CORNER SWEEP
// Tune: corner_angle_deg | min_corner_pts | max_corner_pts
// 8 V-notches, identical edge length, tip angle 8°→175° left to right.
// cornerSeverity().speedT is constant (same edge length everywhere), so
// ONLY the angle drives dwell. Notches whose tip angle is BELOW
// corner_angle_deg get min_corner_pts; above it dwell grows toward
// max_corner_pts at 175°. Correct tuning: leftmost 1-2 notches sweep
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
    cfgDiag.min_segment_pts     = 2;       // exactly the two endpoints
    cfgDiag.accel_clamp_enabled = false;   // vel probe only

    const optimizer::OptimizerConfig cfg = liveOptimizerConfig();
    optimizer::PathSegment diagSeg(diag, 2, false);
    optimizer::PathSegment starSeg(spike, SP * 2, true);

    size_t n = optimizer::optimize(&diagSeg, 1, o,     m,     cfgDiag);
    n        += optimizer::optimize(&starSeg, 1, o + n, m - n, cfg);
    return n;
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
            return OPT_PROFILE_VECTOR;
        case 0:  // Blanking Test    -- arc segments split by blank jumps
        case 2:  // ILDA Test        -- circle + square + blanked sub-figures
        case 6:  // Three Circles    -- three separate closed circles
        case 9:  // Opt Jump Ring    -- isolates blank_samples / ringing_comp
            return OPT_PROFILE_MULTIOBJECT;
        case 5:  // Corner Color Map -- four isolated dots
            return OPT_PROFILE_PARTICLES;
        case 8:  // Opt Density Ramp -- isolates pts_per_1000_units / resample
            return OPT_PROFILE_SMOOTH;
        case 10: // Opt Vel/Accel    -- isolates max_step_units / max_accel
            return OPT_PROFILE_WAVES;
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

};

using PFn = size_t(*)(LaserPoint*, size_t, uint32_t, uint8_t, uint8_t);
static const PFn DISPATCH[CALIB_PATTERN_COUNT] = {
    blanking_test, aspect_ratio, ilda_test,
    dac_range_box, zone_outline, corner_color_map, three_circles,
    opt_corner_sweep, opt_density_ramp, opt_jump_ring, opt_vel_accel,
};


size_t generate(uint8_t idx, LaserPoint* out, size_t max_pts,
                uint32_t phase, uint8_t brightness, uint8_t channel) {
    if (idx >= CALIB_PATTERN_COUNT || !out) return 0;
    size_t n = DISPATCH[idx](out, max_pts, phase, brightness, channel);

    // Cross-frame seam bridge (#4), same as presets::generate().
    // Static patterns produce ~0 jump -> skip.
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
