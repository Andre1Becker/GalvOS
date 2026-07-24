/**
 * text_renderer.cpp — vector text renderer for laser
 * v2.0 — Overhaul:
 *   - Y-axis: oy - sy*sc (correct: +y = up in font space, -y = up in DAC space)
 *   - Size: larger default, fills scan area properly at size_val=128
 *   - Speed: 4x faster animation across all modes
 *   - FONT_BOLD: dual-stroke with lateral offset
 *   - FONT_OUTLINE: 4-direction offset pass for outline effect
 *   - Orbit: text wrapped onto a spinning sphere (v5.46)
 *   - Star Wars: correct trapezoid perspective (wide bottom, narrow top)
 *   - Serif fix: text-specific optimizer floors keep short strokes (v5.46)
 *   - X/Y flip: per-string mirror around centroid (v5.46)
 *   3D Extrusion removed (v5.46)
 *   - Centering: glyph rendered at cell-center (half-advance offset) (v5.47)
 *   - Auto-scroll: static text > TEXT_MAX_STATIC_CHARS scrolls left (v5.47)
 *   - Orbit: thin equatorial band, reduced glyph scale (v5.47)
 */
#include "text_renderer.h"
#include "text_renderer.h"
#include "point_optimizer.h"
#include <math.h>
#include <string.h>
#include <Arduino.h>

static inline optimizer::OptimizerConfig textOptimizerConfig(float sc = 0.f) {
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
    cfg.ringing_comp_enabled         = gOptimizerConfig.ringing_comp_enabled;
    cfg.ring_freq_hz                 = gOptimizerConfig.ring_freq_hz;
    cfg.ring_damping_ratio           = gOptimizerConfig.ring_damping_ratio;
    cfg.galvo_kpps                   = gProjection.galvo_kpps;
    cfg.transform                    = optimizer::gLiveTransform;  // Phase 3: live Z-rot + move
    cfg.vel_clamp_enabled            = gOptimizerConfig.vel_clamp_enabled;
    cfg.max_step_units               = gOptimizerConfig.max_step_units;
    cfg.accel_clamp_enabled          = gOptimizerConfig.accel_clamp_enabled;
    cfg.max_accel_units              = gOptimizerConfig.max_accel_units;

    // Serif fix (point-based, scale-INDEPENDENT): guarantee every stroke keeps
    // both endpoints so short crossbars (E, F, T, ...) never collapse to a
    // single point. These floors are counts, so they do not blow up with scale.
    if (cfg.min_segment_pts < 3)              cfg.min_segment_pts = 3;
    if (cfg.min_interior_pts_per_segment < 1) cfg.min_interior_pts_per_segment = 1;

    // Interior density is length-proportional in DAC units, so at a large text
    // scale one glyph would otherwise consume hundreds of points and fill the
    // frame buffer after ~8 characters (truncating the rest of the string).
    // Couple the density to the render scale so the point count PER GLYPH stays
    // roughly constant regardless of text size: ppu = 8000 / sc, clamped.
    if (sc > 1.f) {
        float ppu = 8000.f / sc;
        if (ppu < 2.f)  ppu = 2.f;
        if (ppu > 30.f) ppu = 30.f;
        cfg.pts_per_1000_units = ppu;
    }
    // PPS-derived scaling: density + both clamps from rated/output kpps.
    // Runs after the serif/scale density override so it scales the final
    // density (and both clamps, which the text-specific logic never touches).
    optimizer::applyPpsScaling(cfg, gProjection.galvo_rated_kpps, gProjection.galvo_kpps);
    return cfg;
}
namespace textrender {

// ============================================================
// Stroke-Font  (x,y) pairs, x=PU = Pen-Up, Terminator = {EN,EN}
// coordinate space: x ∈ [-5,5], y ∈ [-7,7]  (+y = up)
// renderGlyph: screen_y = oy + sy * sc  -- font +y=up maps directly onto
// DAC +y=up (confirmed via the Corner Color Map calibration pattern; no
// flip needed and none should be added here).
// ============================================================

#define PU 126
#define EN 127

static const int8_t FONT_A[] = {-4,-7, 0,7, 4,-7, PU,0, -3,-2, 3,-2, EN,EN};
static const int8_t FONT_B[] = {-4,-7,-4,7, PU,0,-4,7,1,7,3,5,3,2,1,0,-4,0, PU,0,-4,0,1,0,3,-2,3,-5,1,-7,-4,-7, EN,EN};
static const int8_t FONT_C[] = {4,-5,2,-7,-2,-7,-4,-5,-4,5,-2,7,2,7,4,5, EN,EN};
static const int8_t FONT_D[] = {-4,-7,-4,7, PU,0,-4,7,1,7,4,4,4,-4,1,-7,-4,-7, EN,EN};
static const int8_t FONT_E[] = {-4,7,4,7, PU,0,-4,7,-4,-7, PU,0,-4,-7,4,-7, PU,0,-4,0,2,0, EN,EN};
static const int8_t FONT_F[] = {-4,7,4,7, PU,0,-4,7,-4,-7, PU,0,-4,0,2,0, EN,EN};
static const int8_t FONT_G[] = {4,5,2,7,-2,7,-4,5,-4,-5,-2,-7,2,-7,4,-5,4,0,1,0, EN,EN};
static const int8_t FONT_H[] = {-4,-7,-4,7, PU,0,4,-7,4,7, PU,0,-4,0,4,0, EN,EN};
static const int8_t FONT_I[] = {-2,-7,2,-7, PU,0,0,-7,0,7, PU,0,-2,7,2,7, EN,EN};
static const int8_t FONT_J[] = {4,7,4,-5,2,-7,-1,-7,-3,-5,-3,-3, EN,EN};
static const int8_t FONT_K[] = {-4,-7,-4,7, PU,0,4,-7,-4,0, PU,0,-4,0,4,7, EN,EN};
static const int8_t FONT_L[] = {-4,7,-4,-7, PU,0,-4,-7,4,-7, EN,EN};
static const int8_t FONT_M[] = {-4,-7,-4,7,0,1,4,7,4,-7, EN,EN};
static const int8_t FONT_N[] = {-4,-7,-4,7,4,-7,4,7, EN,EN};
static const int8_t FONT_O[] = {-2,-7,-4,-5,-4,5,-2,7,2,7,4,5,4,-5,2,-7,-2,-7, EN,EN};
static const int8_t FONT_P[] = {-4,-7,-4,7, PU,0,-4,7,2,7,4,5,4,2,2,0,-4,0, EN,EN};
static const int8_t FONT_Q[] = {-2,-7,-4,-5,-4,5,-2,7,2,7,4,5,4,-5,2,-7,-2,-7, PU,0,1,-4,4,-7, EN,EN};
static const int8_t FONT_R[] = {-4,-7,-4,7, PU,0,-4,7,2,7,4,5,4,2,2,0,-4,0, PU,0,-1,0,4,-7, EN,EN};
static const int8_t FONT_S[] = {4,6,2,7,-2,7,-4,5,-4,2,-2,0,2,0,4,-2,4,-5,2,-7,-2,-7,-4,-6, EN,EN};
static const int8_t FONT_T[] = {-4,7,4,7, PU,0,0,7,0,-7, EN,EN};
static const int8_t FONT_U[] = {-4,7,-4,-5,-2,-7,2,-7,4,-5,4,7, EN,EN};
static const int8_t FONT_V[] = {-4,7,0,-7,4,7, EN,EN};
static const int8_t FONT_W[] = {-4,7,-2,-7,0,1,2,-7,4,7, EN,EN};
static const int8_t FONT_X[] = {-4,-7,4,7, PU,0,4,-7,-4,7, EN,EN};
static const int8_t FONT_Y[] = {-4,7,0,0, PU,0,4,7,0,0,0,-7, EN,EN};
static const int8_t FONT_Z[] = {-4,7,4,7,4,5,-4,-5,-4,-7,4,-7, EN,EN};
static const int8_t FONT_0[] = {-2,-7,-4,-5,-4,5,-2,7,2,7,4,5,4,-5,2,-7,-2,-7, PU,0,-3,-5,3,5, EN,EN};
static const int8_t FONT_1[] = {-2,5,0,7,0,-7, PU,0,-3,-7,3,-7, EN,EN};
static const int8_t FONT_2[] = {-4,5,-2,7,2,7,4,5,4,2,-4,-4,-4,-7,4,-7, EN,EN};
static const int8_t FONT_3[] = {-4,6,-2,7,2,7,4,5,4,2,2,0, PU,0,2,0,4,-2,4,-5,2,-7,-2,-7,-4,-6, EN,EN};
static const int8_t FONT_4[] = {4,-2,-4,-2,-1,7, PU,0,4,7,4,-7, EN,EN};
static const int8_t FONT_5[] = {4,7,-4,7,-4,0,2,0,4,-2,4,-5,2,-7,-2,-7,-4,-5, EN,EN};
static const int8_t FONT_6[] = {3,7,0,7,-4,4,-4,-5,-2,-7,2,-7,4,-5,4,-2,2,0,-4,0, EN,EN};
static const int8_t FONT_7[] = {-4,7,4,7,4,5,-1,-7, EN,EN};
static const int8_t FONT_8[] = {0,0,-4,2,-4,5,-2,7,2,7,4,5,4,2,0,0,-4,-2,-4,-5,-2,-7,2,-7,4,-5,4,-2,0,0, EN,EN};
static const int8_t FONT_9[] = {4,2,4,5,2,7,-2,7,-4,5,-4,2,-2,0,4,0,4,-6,2,-7,-1,-7, EN,EN};
static const int8_t FONT_SP[]    = {EN,EN};
static const int8_t FONT_DOT[]   = {0,-7,0,-6, EN,EN};
static const int8_t FONT_COMMA[] = {0,-7,0,-6, PU,0,0,-7,-1,-9, EN,EN};
static const int8_t FONT_EXCL[]  = {0,7,0,-2, PU,0,0,-5,0,-7, EN,EN};
static const int8_t FONT_QUES[]  = {-3,5,-2,7,2,7,4,5,4,3,0,0,0,-3, PU,0,0,-6,0,-7, EN,EN};
static const int8_t FONT_HYPH[]  = {-3,0,3,0, EN,EN};
static const int8_t FONT_PLUS[]  = {0,4,0,-4, PU,0,-4,0,4,0, EN,EN};
static const int8_t FONT_COLON[] = {0,3,0,2, PU,0,0,-2,0,-3, EN,EN};
static const int8_t FONT_HASH[]  = {-2,5,-2,-5, PU,0,2,5,2,-5, PU,0,-4,2,4,2, PU,0,-4,-2,4,-2, EN,EN};
static const int8_t FONT_AT[]    = {2,0,-1,0,-3,2,-3,5,-1,7,2,7,4,5,4,-5,2,-7,-2,-7,-4,-5,-4,5,-2,7, EN,EN};
static const int8_t FONT_STAR[]  = {0,5,0,-5, PU,0,-4,3,4,-3, PU,0,-4,-3,4,3, EN,EN};

struct FontGlyph { uint8_t ch; const int8_t* strokes; int8_t advance; };

static const FontGlyph GLYPHS[] = {
    {' ', FONT_SP,    10},
    {'A', FONT_A,     10}, {'B', FONT_B,   10}, {'C', FONT_C,   10},
    {'D', FONT_D,     10}, {'E', FONT_E,   10}, {'F', FONT_F,   10},
    {'G', FONT_G,     10}, {'H', FONT_H,   10}, {'I', FONT_I,    6},
    {'J', FONT_J,      8}, {'K', FONT_K,   10}, {'L', FONT_L,    9},
    {'M', FONT_M,     12}, {'N', FONT_N,   10}, {'O', FONT_O,   10},
    {'P', FONT_P,     10}, {'Q', FONT_Q,   11}, {'R', FONT_R,   10},
    {'S', FONT_S,     10}, {'T', FONT_T,   10}, {'U', FONT_U,   10},
    {'V', FONT_V,     10}, {'W', FONT_W,   12}, {'X', FONT_X,   10},
    {'Y', FONT_Y,     10}, {'Z', FONT_Z,   10},
    {'0', FONT_0,     10}, {'1', FONT_1,    7}, {'2', FONT_2,   10},
    {'3', FONT_3,     10}, {'4', FONT_4,   10}, {'5', FONT_5,   10},
    {'6', FONT_6,     10}, {'7', FONT_7,   10}, {'8', FONT_8,   10},
    {'9', FONT_9,     10},
    {'.', FONT_DOT,    6}, {',', FONT_COMMA, 6},
    {'!', FONT_EXCL,   6}, {'?', FONT_QUES, 10}, {'-', FONT_HYPH, 8},
    {'+', FONT_PLUS,   8}, {':', FONT_COLON, 6}, {'#', FONT_HASH, 10},
    {'@', FONT_AT,    12}, {'*', FONT_STAR,  8},
};
static const int GLYPH_COUNT = (int)(sizeof(GLYPHS) / sizeof(GLYPHS[0]));

// ============================================================
// addPt — helper function
// ============================================================
static inline void addPt(LaserPoint* o, size_t& n, size_t max,
                          float x, float y, uint8_t r, uint8_t g, uint8_t b,
                          uint8_t blank = 0) {
    if (n >= max) return;
    o[n].x = (int16_t)constrain(x, -32767.f, 32767.f);
    o[n].y = (int16_t)constrain(y, -32767.f, 32767.f);
    o[n].r = r; o[n].g = g; o[n].b = b;
    o[n].blank = blank;
    n++;
}

// ============================================================
// renderGlyph -- one character
//   screen_y = oy + sy * sc: font +y=up maps directly onto DAC +y=up,
//   no inversion (see coordinate-space note above the font tables).
// ============================================================
struct GlyphResult {
    float last_x, last_y;
    bool  had_points;
};

static GlyphResult renderGlyph(LaserPoint* out, size_t& n, size_t max,
                                 const int8_t* strokes,
                                 float ox, float oy, float sc,
                                 uint8_t r, uint8_t g, uint8_t b,
                                 float dx_offset = 0.f, float dy_offset = 0.f) {
    GlyphResult res = {ox, oy, false};

    optimizer::PathVertex verts[64];
    optimizer::PathSegment segs[16];
    int nsegs = 0, nverts = 0;
    int seg_start = 0;

    for (int i = 0; ; i += 2) {
        int8_t sx = strokes[i];
        if (sx == EN) {
            if (nverts > seg_start) {
                segs[nsegs++] = optimizer::PathSegment(&verts[seg_start], nverts - seg_start, false);
            }
            break;
        }
        int8_t sy = strokes[i+1];
        if (sx == PU) {
            if (nverts > seg_start) {
                segs[nsegs++] = optimizer::PathSegment(&verts[seg_start], nverts - seg_start, false);
                seg_start = nverts;
            }
            continue;
        }
        if (nverts >= 64) break;
        verts[nverts].x = ox + sx * sc + dx_offset;
        verts[nverts].y = oy + sy * sc + dy_offset;  // font +y=up -> DAC +y=up, no flip
        verts[nverts].r = r; verts[nverts].g = g; verts[nverts].b = b;
        verts[nverts].lift = false;
        if (nverts == seg_start) verts[nverts].lift = true;
        nverts++;
    }

    if (nsegs == 0) return res;

    size_t before = n;
    n += optimizer::optimize(segs, nsegs, out + n, max - n, textOptimizerConfig(sc));

    if (n > before) {
        res.last_x = out[n-1].x;
        res.last_y = out[n-1].y;
        res.had_points = true;
    }

    return res;
}

// ============================================================
// renderGlyphBold -- FONT_BOLD: render glyph twice with lateral offsets
// ============================================================
static GlyphResult renderGlyphBold(LaserPoint* out, size_t& n, size_t max,
                                    const int8_t* strokes,
                                    float ox, float oy, float sc,
                                    uint8_t r, uint8_t g, uint8_t b) {
    float off = sc * 0.25f;
    renderGlyph(out, n, max, strokes, ox, oy, sc, r, g, b, -off * 0.5f, 0.f);
    GlyphResult res = renderGlyph(out, n, max, strokes, ox, oy, sc, r, g, b,  off * 0.5f, 0.f);
    return res;
}

// ============================================================
// renderGlyphOutline -- FONT_OUTLINE: 4 offset passes
// ============================================================
static GlyphResult renderGlyphOutline(LaserPoint* out, size_t& n, size_t max,
                                       const int8_t* strokes,
                                       float ox, float oy, float sc,
                                       uint8_t r, uint8_t g, uint8_t b) {
    // Small offset — large values waste points and push outline off-glyph.
    // Cap each pass so one character cannot consume the whole buffer (5 passes).
    float off = sc * 0.055f;
    uint8_t dr = (uint8_t)(r / 3), dg = (uint8_t)(g / 3), db = (uint8_t)(b / 3);
    size_t cap = n + (max - n) / 5;  // each pass gets at most 1/5 of remaining space
    if (cap > max) cap = max;
    renderGlyph(out, n, cap, strokes, ox, oy, sc, dr, dg, db,  off,  0.f);
    renderGlyph(out, n, cap, strokes, ox, oy, sc, dr, dg, db, -off,  0.f);
    renderGlyph(out, n, cap, strokes, ox, oy, sc, dr, dg, db,  0.f,  off);
    renderGlyph(out, n, cap, strokes, ox, oy, sc, dr, dg, db,  0.f, -off);
    GlyphResult res = renderGlyph(out, n, cap, strokes, ox, oy, sc, r, g, b);
    return res;
}

// ============================================================
// textWidth
// ============================================================
static float textWidth(const char* txt, int len = -1) {
    float w = 0;
    for (int ci = 0; (len < 0 ? (bool)txt[ci] : ci < len) && txt[ci]; ci++) {
        char c = toupper((unsigned char)txt[ci]);
        for (int i = 0; i < GLYPH_COUNT; i++) {
            if (GLYPHS[i].ch == (uint8_t)c) { w += GLYPHS[i].advance; break; }
        }
    }
    return w;
}

// ============================================================
// renderTextString
// ============================================================
static size_t renderTextString(LaserPoint* out, size_t max,
                                const char* text, int text_len,
                                const TextConfig& cfg,
                                float tx, float ty, float sc,
                                float rot = 0.f,
                                bool wave_on = false, float wave_t = 0.f) {
    size_t n = 0;
    float cx = tx;

    for (int ci = 0; ci < text_len && text[ci]; ci++) {
        char c = toupper((unsigned char)text[ci]);

        const FontGlyph* glyph = nullptr;
        for (int i = 0; i < GLYPH_COUNT; i++) {
            if (GLYPHS[i].ch == (uint8_t)c) { glyph = &GLYPHS[i]; break; }
        }
        if (!glyph) { cx += 10.f * sc; continue; }

        float char_ty = ty;
        if (wave_on) char_ty += sinf(wave_t + ci * 0.6f) * sc * 3.f;

        uint8_t r = cfg.col_r, g = cfg.col_g, b = cfg.col_b;
        if (cfg.rainbow) {
            float hue = fmodf(wave_t * 0.5f + ci * 0.3f, 1.f);
            float h6 = hue * 6.f;
            int   hi = (int)h6;
            float f  = h6 - hi;
            switch (hi % 6) {
                case 0: r=255; g=(uint8_t)(255*f);     b=0;             break;
                case 1: r=(uint8_t)(255*(1-f)); g=255; b=0;             break;
                case 2: r=0;   g=255; b=(uint8_t)(255*f);               break;
                case 3: r=0;   g=(uint8_t)(255*(1-f)); b=255;           break;
                case 4: r=(uint8_t)(255*f); g=0; b=255;                 break;
                case 5: r=255; g=0;   b=(uint8_t)(255*(1-f));           break;
            }
        }

        // Glyph strokes are centered on their own origin (x in [-4,4]), but
        // the cursor `cx` marks the LEFT edge of the character cell. Offset the
        // render origin by half the advance so the glyph sits centered in its
        // cell -- otherwise the whole string is shifted by ~half a glyph and
        // no longer centered on screen.
        float gx = cx + glyph->advance * 0.5f * sc;

        GlyphResult gr;
        switch (cfg.font) {
            case FONT_BOLD:
                gr = renderGlyphBold(out, n, max, glyph->strokes, gx, char_ty, sc, r, g, b);
                break;
            case FONT_OUTLINE:
                gr = renderGlyphOutline(out, n, max, glyph->strokes, gx, char_ty, sc, r, g, b);
                break;
            default: // FONT_SIMPLE
                gr = renderGlyph(out, n, max, glyph->strokes, gx, char_ty, sc, r, g, b);
                break;
        }

        cx += glyph->advance * sc;

        if (gr.had_points && n < max) {
            float next_ty = ty;
            if (wave_on && text[ci+1])
                next_ty += sinf(wave_t + (ci+1) * 0.6f) * sc * 3.f;
            addPt(out, n, max, cx, next_ty, 0, 0, 0, /*blank=*/1);
        }
    }

    // Rotation around centroid
    if (fabsf(rot) > 0.001f && n > 0) {
        float cxc = 0.f, cyc = 0.f;
        for (size_t i = 0; i < n; i++) { cxc += out[i].x; cyc += out[i].y; }
        cxc /= n; cyc /= n;
        const float cos_r = cosf(rot), sin_r = sinf(rot);
        for (size_t i = 0; i < n; i++) {
            float px = out[i].x - cxc, py = out[i].y - cyc;
            out[i].x = (int16_t)(cxc + px * cos_r - py * sin_r);
            out[i].y = (int16_t)(cyc + px * sin_r + py * cos_r);
        }
    }

    // X/Y flip -- mirror the assembled string around its centroid so the
    // text reads mirrored without shifting its screen position.
    if ((cfg.flip_x || cfg.flip_y) && n > 0) {
        float cxc = 0.f, cyc = 0.f;
        for (size_t i = 0; i < n; i++) { cxc += out[i].x; cyc += out[i].y; }
        cxc /= n; cyc /= n;
        for (size_t i = 0; i < n; i++) {
            if (cfg.flip_x) out[i].x = (int16_t)(2.f * cxc - out[i].x);
            if (cfg.flip_y) out[i].y = (int16_t)(2.f * cyc - out[i].y);
        }
    }

    return n;
}

// ============================================================
// generate — public interface
// ============================================================
size_t generate(LaserPoint* out, size_t max_pts, const TextConfig& cfg, uint32_t phase) {
    if (!cfg.active || !cfg.text[0]) return 0;

    const uint32_t safe_phase = phase % 0xFFFFFF;

    // FIX v2.0: larger base scale — at size_val=128, letters ~40% of scan height
    // Font glyph spans ±7 units vertically. BASE_SCALE set so that
    // size_val=255 → letter height = 0.85 * 32767 (half-axis).
    const float BASE_SCALE = 32767.f * 0.85f / 14.f;  // ~1984
    float sc  = BASE_SCALE * (0.1f + cfg.size_val / 255.f * 0.9f);
    // FIX v2.0: 4x faster animation
    float spd = cfg.speed / 255.f;
    float t   = safe_phase * spd * 0.08f;

    const int full_len = (int)strlen(cfg.text);
    float tw = textWidth(cfg.text, full_len) * sc;

    // Auto-scroll decision uses the UNSHRUNK width: a static string that is
    // too long to fit legibly is scrolled instead of being crammed in.
    const float max_half = 30000.f;
    const float GLYPH_HALF_H = 7.f;
    TextAnim anim = cfg.animation;
    if (anim == TANIM_STATIC && full_len > TEXT_MAX_STATIC_CHARS)
        anim = TANIM_SCROLL_L;

    // Scale for the NON-scrolling modes (which must show the whole string at
    // once) is bound by BOTH height and width so the text never overflows the
    // +/-32767 DAC range -- overflow was collapsing glyphs onto the scan edge
    // (a single line on the right). Scrolling modes render the string off to
    // the side and move it through, so they keep the height-only scale.
    const bool scrolls = (anim == TANIM_SCROLL_L || anim == TANIM_SCROLL_R);
    float sc_h = (GLYPH_HALF_H * sc > max_half) ? max_half / GLYPH_HALF_H : sc;
    float sc_w = (tw * 0.5f > max_half)         ? sc * max_half / (tw * 0.5f) : sc;
    float display_sc = scrolls ? sc_h : fminf(sc_h, sc_w);
    if (display_sc != sc) tw = textWidth(cfg.text, full_len) * display_sc;
    float start_x = -tw / 2.f;
    float base_y  = 0.f;

    switch (anim) {

        case TANIM_STATIC:
            return renderTextString(out, max_pts, cfg.text, full_len,
                                    cfg, start_x, base_y, display_sc);

        case TANIM_SCROLL_L: {
            float period = tw + 20000.f;
            float ox = 18000.f - fmodf(t * 8000.f, period);
            return renderTextString(out, max_pts, cfg.text, full_len,
                                    cfg, ox, base_y, display_sc);
        }

        case TANIM_SCROLL_R: {
            float period = tw + 20000.f;
            float ox = -tw - 18000.f + fmodf(t * 8000.f, period);
            return renderTextString(out, max_pts, cfg.text, full_len,
                                    cfg, ox, base_y, display_sc);
        }

        case TANIM_BOUNCE: {
            float range = fmaxf(0.f, 18000.f - tw * 0.5f);
            float bx    = sinf(t * 2.f) * range;
            return renderTextString(out, max_pts, cfg.text, full_len,
                                    cfg, start_x + bx, base_y, display_sc);
        }

        case TANIM_TYPEWRITER: {
            const int fpc     = max(4, (int)(255 * 12 / max(1, (int)cfg.speed)));
            const int tw_cycle = full_len + 3;
            int visible = (int)(safe_phase / fpc) % tw_cycle;
            if (visible > full_len) visible = full_len;
            if (visible == 0) return 0;

            char temp[128];
            if (visible > (int)sizeof(temp) - 1) visible = sizeof(temp) - 1;
            strncpy(temp, cfg.text, (size_t)visible);
            temp[visible] = '\0';

            float vw = textWidth(temp, visible) * display_sc;
            return renderTextString(out, max_pts, temp, visible,
                                    cfg, -vw * 0.5f, base_y, display_sc);
        }

        case TANIM_WAVE:
            return renderTextString(out, max_pts, cfg.text, full_len,
                                    cfg, start_x, base_y, display_sc,
                                    0.f, /*wave_on=*/true, t);

        case TANIM_PULSE: {
            float pulse_sc = display_sc * (0.7f + 0.3f * fabsf(sinf(t * 3.f)));
            float pw       = textWidth(cfg.text, full_len) * pulse_sc;
            return renderTextString(out, max_pts, cfg.text, full_len,
                                    cfg, -pw * 0.5f, base_y, pulse_sc);
        }

        case TANIM_ROTATE: {
            float rot = fmodf(t * 1.5f, 2.f * (float)M_PI);
            return renderTextString(out, max_pts, cfg.text, full_len,
                                    cfg, start_x, base_y, display_sc, rot);
        }

        case TANIM_ZOOM: {
            float zoom    = 0.3f + 0.7f * (0.5f + 0.5f * sinf(t * 2.f));
            float zoom_sc = display_sc * zoom;
            float zw      = textWidth(cfg.text, full_len) * zoom_sc;
            return renderTextString(out, max_pts, cfg.text, full_len,
                                    cfg, -zw * 0.5f, base_y, zoom_sc);
        }

        // TANIM_3D_EXT removed (v5.46) -- the shadow-copy extrusion was
        // visually unconvincing on a vector laser and doubled the point
        // budget. Enum value kept for ABI; an unhandled value falls through
        // to the default (static) render below.

        case TANIM_ORBIT: {
            // True 3D orbit: the whole string is wrapped onto the surface of a
            // sphere and spun around the vertical (Y) axis, so the text reads
            // as if it were circling a globe -- glyphs shrink and slide away
            // toward the horizon on the far side instead of merely translating.
            //
            // 1) render flat, centered text into the buffer
            // 2) treat each point's flat X as an arc-length -> longitude phi
            //    (plus a time term for the spin) and its flat Y as height
            // 3) place the point on the sphere, perspective-project to screen
            // 4) blank points on the back hemisphere (facing away from viewer)
            // 1) render flat, centered text into the buffer, at a REDUCED
            //    scale -- the sphere + perspective magnify the near side, so a
            //    full-size render would look oversized.
            float orbit_sc = display_sc * 0.55f;
            float otw = textWidth(cfg.text, full_len) * orbit_sc;
            size_t n = renderTextString(out, max_pts, cfg.text, full_len,
                                        cfg, -otw * 0.5f, base_y, orbit_sc);
            if (n == 0) return 0;

            const float R      = 20000.f;               // sphere radius (DAC units)
            const float focal  = 42000.f;               // perspective focal length
            const float camZ   = R + focal;             // camera distance from center
            const float spin   = fmodf(t * 1.2f, 2.f * (float)M_PI);
            // Wrap the FULL text width onto a moderate arc (~80 deg) of the
            // sphere. Mapping half the width to 1.6 rad (as before) spread the
            // string over ~183 deg -> more than one hemisphere, so part of it
            // was always on the back side (Z<0) and blanked, leaving only the
            // few centre glyphs visible. ARC/otw keeps the whole string on the
            // front-facing arc; `spin` then carries it around the globe.
            const float ARC   = 1.4f;                   // total wrap, radians (~80 deg)
            const float fullW = fmaxf(1.f, otw);
            const float kLon  = ARC / fullW;            // X (DAC) -> longitude (rad)
            // Keep the text in a THIN band around the equator (~+-11 deg) so the
            // letters run around the equator and never climb toward the poles.
            const float LAT_BAND = 0.20f;               // half-band, radians
            const float glyphHalf = fmaxf(1.f, GLYPH_HALF_H * orbit_sc);
            const float kLat   = LAT_BAND / glyphHalf;  // Y (DAC) -> latitude (rad)

            for (size_t i = 0; i < n; i++) {
                float phi = out[i].x * kLon + spin;     // longitude
                float lat = out[i].y * kLat;            // latitude
                float clat = cosf(lat);

                // sphere surface point (viewer looks down -Z)
                float X = R * sinf(phi) * clat;
                float Y = R * sinf(lat);
                float Z = R * cosf(phi) * clat;

                // perspective projection
                float depth = camZ - Z;
                float proj  = focal / fmaxf(depth, 1000.f);
                out[i].x = (int16_t)constrain(X * proj, -32767.f, 32767.f);
                out[i].y = (int16_t)constrain(Y * proj, -32767.f, 32767.f);

                // blank the far hemisphere so the text wraps out of sight
                if (Z < 0.f) out[i].blank = 1;
            }
            return n;
        }

        case TANIM_STARWARS: {
            // Star Wars perspective crawl: text enters large at the bottom
            // (near the viewer) and scrolls up, shrinking toward a vanishing
            // point at the top. DAC space: +y = up = screen top, -y = down =
            // screen bottom (ground truth, confirmed via the Corner Color Map
            // calibration pattern; no invert_y dependency here).
            float scroll_speed = 8000.f;
            float yPos = -40000.f + fmodf(t * scroll_speed, 80000.f);  // scrolls -40k..+40k (bottom -> top)

            size_t total = 0;

            for (int copy = 0; copy < 2; copy++) {
                float yBase = yPos + copy * 75000.f;

                if (yBase > 70000.f || yBase < -36000.f) continue;

                // Perspective: -y (screen bottom, near viewer) = large;
                // +y (screen top, far/vanishing point) = small.
                float persp = (32767.f - yBase) / 65534.f;  // 1=bottom(near), 0=top(far)
                persp = fmaxf(0.05f, fminf(1.0f, persp));
                float scaleP = display_sc * (0.2f + persp * 0.8f);
                float twP    = textWidth(cfg.text, full_len) * scaleP;
                float startXP = -twP * 0.5f;

                // Render a single line of text at this y position and scale
                // Cap pts per glyph to keep total frame size stable across scale changes.
                // Budget: split remaining space evenly across letters, max 80 pts/glyph.
                size_t letters = (size_t)full_len > 0 ? (size_t)full_len : 1;
                size_t glyph_cap_base = (max_pts > total) ? (max_pts - total) / letters : 0;
                size_t glyph_cap = glyph_cap_base < 80 ? glyph_cap_base : 80;

                float cx2 = startXP;
                for (int ci = 0; ci < full_len && cfg.text[ci]; ci++) {
                    char ch = toupper((unsigned char)cfg.text[ci]);
                    const FontGlyph* g2 = nullptr;
                    for (int i = 0; i < GLYPH_COUNT; i++) {
                        if (GLYPHS[i].ch == (uint8_t)ch) { g2 = &GLYPHS[i]; break; }
                    }
                    if (!g2) { cx2 += 10.f * scaleP; continue; }

                    size_t cap = total + glyph_cap < max_pts ? total + glyph_cap : max_pts;
                    addPt(out, total, cap, cx2, yBase, 0, 0, 0, 1);
                    renderGlyph(out, total, cap, g2->strokes,
                                cx2 + g2->advance * 0.5f * scaleP, yBase, scaleP,
                                cfg.col_r, cfg.col_g, cfg.col_b);
                    cx2 += g2->advance * scaleP;
                    if (total >= max_pts - 16) break;
                }
            }
            return total;
        }

        default:
            return renderTextString(out, max_pts, cfg.text, full_len,
                                    cfg, start_x, base_y, display_sc);
    }
}

// ============================================================
// glyphOutlinePaths -- raw glyph geometry for the Paint text tool
// ============================================================
size_t glyphOutlinePaths(const char* text, float scale,
                         GlyphSubpath* out, size_t max_paths) {
    if (!text || !text[0] || scale <= 0.f) return 0;

    const int len = (int)strlen(text);
    const float tw = textWidth(text, len) * scale;
    float cx = -tw / 2.f;
    const float cy = 0.f;
    size_t n = 0;

    for (int ci = 0; ci < len && n < max_paths; ci++) {
        char c = toupper((unsigned char)text[ci]);
        const FontGlyph* glyph = nullptr;
        for (int i = 0; i < GLYPH_COUNT; i++) {
            if (GLYPHS[i].ch == (uint8_t)c) { glyph = &GLYPHS[i]; break; }
        }
        if (!glyph) { cx += 10.f * scale; continue; }

        const int8_t* s = glyph->strokes;
        const float gx = cx + glyph->advance * 0.5f * scale;  // center in cell
        GlyphSubpath tmp;
        tmp.count = 0;

        for (int i = 0; ; i += 2) {
            int8_t sx = s[i];
            bool flush = (sx == PU || sx == EN);
            if (!flush) {
                int8_t sy = s[i + 1];
                if (tmp.count < GlyphSubpath::MAX_PTS) {
                    tmp.x[tmp.count] = gx + sx * scale;
                    tmp.y[tmp.count] = cy + sy * scale;   // no flip, matches renderGlyph
                    tmp.count++;
                }
            }
            if (flush) {
                if (tmp.count >= 2 && n < max_paths) out[n++] = tmp;
                tmp.count = 0;
                if (sx == EN) break;
            }
        }
        cx += glyph->advance * scale;
    }
    return n;
}

} // namespace textrender
