/**
 * text_renderer.cpp — vector text renderer for laser
 * v1.1 — Fixes:
 *   - Typewriter: temporary string, truly only N characters rendered
 *   - Blanking between letters: explicit blank point after each glyph
 */
#include "text_renderer.h"
#include <math.h>
#include <string.h>
#include <Arduino.h>

namespace textrender {

// ============================================================
// Stroke-Font  (x,y) Paare, x=127 = Pen-Up, Terminator = {127,127}
// coordinate space: x ∈ [-5,5], y ∈ [-7,7]  (+y = up)
// renderGlyph: screen_y = oy - sy * sc  →  y-Flip korrekt
// ============================================================

#define PU 126
#define EN 127

static const int8_t FONT_A[] = {-4,-7, 0,7, 4,-7, PU,0, -3,-2, 3,-2, EN,EN};
static const int8_t FONT_B[] = {-4,-7,-4,7, PU,0,-4,7,1,7,3,5,3,2,1,0,-4,0, PU,0,-4,0,1,0,3,-2,3,-5,1,-7,-4,-7, EN,EN};
static const int8_t FONT_C[] = {4,-5,2,-7,-2,-7,-4,-5,-4,5,-2,7,2,7,4,5, EN,EN};
static const int8_t FONT_D[] = {-4,-7,-4,7, PU,0,-4,7,1,7,4,4,4,-4,1,-7,-4,-7, EN,EN};
static const int8_t FONT_E[] = {-4,7,4,7, PU,0,-4,7,-4,-7, PU,0,-4,-7,4,-7, PU,0,-4,0,2,0, EN,EN};
static const int8_t FONT_F[] = {-4,7,4,7, PU,0,-4,7,-4,-7, PU,0,-4,0,2,0, EN,EN};
static const int8_t FONT_G[] = {4,-5,2,-7,-2,-7,-4,-5,-4,5,-2,7,2,7,4,5,4,0,1,0, EN,EN};
static const int8_t FONT_H[] = {-4,-7,-4,7, PU,0,4,-7,4,7, PU,0,-4,0,4,0, EN,EN};
static const int8_t FONT_I[] = {-2,-7,2,-7, PU,0,0,-7,0,7, PU,0,-2,7,2,7, EN,EN};
static const int8_t FONT_J[] = {4,-7,4,5,2,7,-1,7,-3,5,-3,3, EN,EN};
static const int8_t FONT_K[] = {-4,-7,-4,7, PU,0,4,-7,-4,0, PU,0,-1,3,4,7, EN,EN};
static const int8_t FONT_L[] = {-4,7,-4,-7, PU,0,-4,-7,4,-7, EN,EN};
static const int8_t FONT_M[] = {-4,-7,-4,7,0,1,4,7,4,-7, EN,EN};
static const int8_t FONT_N[] = {-4,-7,-4,7,4,-7,4,7, EN,EN};
static const int8_t FONT_O[] = {-2,-7,-4,-5,-4,5,-2,7,2,7,4,5,4,-5,2,-7,-2,-7, EN,EN};
static const int8_t FONT_P[] = {-4,-7,-4,7, PU,0,-4,7,2,7,4,5,4,2,2,0,-4,0, EN,EN};
static const int8_t FONT_Q[] = {-2,-7,-4,-5,-4,5,-2,7,2,7,4,5,4,-5,2,-7,-2,-7, PU,0,1,4,4,7, EN,EN};
static const int8_t FONT_R[] = {-4,-7,-4,7, PU,0,-4,7,2,7,4,5,4,2,2,0,-4,0, PU,0,-1,0,4,-7, EN,EN};
static const int8_t FONT_S[] = {4,6,2,7,-2,7,-4,5,-4,2,-2,0,2,0,4,-2,4,-5,2,-7,-2,-7,-4,-6, EN,EN};
static const int8_t FONT_T[] = {-4,7,4,7, PU,0,0,7,0,-7, EN,EN};
static const int8_t FONT_U[] = {-4,7,-4,-5,-2,-7,2,-7,4,-5,4,7, EN,EN};
static const int8_t FONT_V[] = {-4,7,0,-7,4,7, EN,EN};
static const int8_t FONT_W[] = {-4,7,-2,-7,0,1,2,-7,4,7, EN,EN};
static const int8_t FONT_X[] = {-4,-7,4,7, PU,0,4,-7,-4,7, EN,EN};
static const int8_t FONT_Y[] = {-4,7,0,0, PU,0,4,7,0,0,0,-7, EN,EN};
static const int8_t FONT_Z[] = {-4,7,4,7,4,5,-4,-5,-4,-7,4,-7, EN,EN};
static const int8_t FONT_0[] = {-2,-7,-4,-5,-4,5,-2,7,2,7,4,5,4,-5,2,-7,-2,-7, PU,0,-3,5,3,-5, EN,EN};
static const int8_t FONT_1[] = {-2,5,0,7,0,-7, PU,0,-3,-7,3,-7, EN,EN};
static const int8_t FONT_2[] = {-4,5,-2,7,2,7,4,5,4,2,-4,-4,-4,-7,4,-7, EN,EN};
static const int8_t FONT_3[] = {-4,6,-2,7,2,7,4,5,4,2,2,0, PU,0,2,0,4,-2,4,-5,2,-7,-2,-7,-4,-6, EN,EN};
static const int8_t FONT_4[] = {4,-2,-4,-2,-1,7, PU,0,4,7,4,-7, EN,EN};
static const int8_t FONT_5[] = {4,7,-4,7,-4,0,2,0,4,-2,4,-5,2,-7,-2,-7,-4,-5, EN,EN};
static const int8_t FONT_6[] = {3,7,0,7,-4,4,-4,-5,-2,-7,2,-7,4,-5,4,-2,2,0,-4,0, EN,EN};
static const int8_t FONT_7[] = {-4,7,4,7,4,5,-1,-7, EN,EN};
static const int8_t FONT_8[] = {-2,0,-4,2,-4,5,-2,7,2,7,4,5,4,2,-2,0,-4,-2,-4,-5,-2,-7,2,-7,4,-5,4,-2,-2,0, EN,EN};
static const int8_t FONT_9[] = {4,2,4,5,2,7,-2,7,-4,5,-4,2,-2,0,4,0,4,-6,2,-7,-1,-7, EN,EN};
static const int8_t FONT_SP[]    = {EN,EN};
static const int8_t FONT_DOT[]   = {0,7,0,6, EN,EN};
static const int8_t FONT_COMMA[] = {0,7,0,6, PU,0,0,7,-1,9, EN,EN};
static const int8_t FONT_EXCL[]  = {0,-7,0,2, PU,0,0,5,0,7, EN,EN};
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
//   FIX v1.1: returns the actual last output position,
//   so the caller can set a clean blank point.
// ============================================================
struct GlyphResult {
    float last_x, last_y;   // last drawn coordinate (for blanking)
    bool  had_points;        // true if any points were actually output
};

static GlyphResult renderGlyph(LaserPoint* out, size_t& n, size_t max,
                                 const int8_t* strokes,
                                 float ox, float oy, float sc,
                                 uint8_t r, uint8_t g, uint8_t b,
                                 float bold_offset = 0.f) {
    GlyphResult res = {ox, oy, false};
    bool pen_up = true;

    for (int i = 0; ; i += 2) {
        int8_t sx = strokes[i];
        if (sx == EN) break;
        int8_t sy = strokes[i+1];
        if (sx == PU) { pen_up = true; continue; }

        float x = ox + sx * sc;
        float y = oy - sy * sc;  // no flip — +y=up in font and laser space

        addPt(out, n, max, x, y, r, g, b, pen_up ? 1 : 0);
        // for Debugging text ESP_LOGI("TXT", "pt sx=%d sy=%d x=%.0f y=%.0f blank=%d", sx, sy, x, y, pen_up?1:0);
        // Bold is rendered as a second offset pass after the glyph — see below
        pen_up = false;
        res.last_x = x;
        res.last_y = y;
        res.had_points = true;
    }

    // Bold second pass: repeat glyph slightly offset (clean separate path)
    if (bold_offset > 0.f && res.had_points) {
        bool pen2 = true;
        for (int i = 0; ; i += 2) {
            int8_t sx = strokes[i], sy = strokes[i+1];
            if (sx == EN && sy == EN) break;
            if (sx == PU) { pen2 = true; continue; }
            float x2 = ox + sx * sc + bold_offset;
            float y2 = oy + sy * sc + bold_offset * 0.3f;
            addPt(out, n, max, x2, y2, r, g, b, pen2 ? 1 : 0);
            pen2 = false;
        }
    }

    return res;
}

// ============================================================
// textWidth -- width in font units (scaled with sc)
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
// renderTextString -- renders text[] with transformation
//   FIX v1.1 Blanking:
//     After each glyph there is a blank point at the NEXT
//     Glyph-Startposition eingefuegt.
//     → galvo moves there with laser OFF, no drag line.
// ============================================================
static size_t renderTextString(LaserPoint* out, size_t max,
                                const char* text, int text_len,
                                const TextConfig& cfg,
                                float tx, float ty, float sc,
                                float rot = 0.f,
                                bool wave_on = false, float wave_t = 0.f) {
    size_t n = 0;
    const float bold = (cfg.font == FONT_BOLD) ? sc * 0.4f : 0.f;
    float cx = tx;

    for (int ci = 0; ci < text_len && text[ci]; ci++) {
        char c = toupper((unsigned char)text[ci]);

        // glyph lookup
        const FontGlyph* glyph = nullptr;
        for (int i = 0; i < GLYPH_COUNT; i++) {
            if (GLYPHS[i].ch == (uint8_t)c) { glyph = &GLYPHS[i]; break; }
        }
        if (!glyph) { cx += 10.f * sc; continue; }

        // Wave: Y offset per character
        float char_ty = ty;
        if (wave_on) char_ty += sinf(wave_t + ci * 0.6f) * sc * 3.f;

        // color (rainbow or fest)
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

        // render glyph
        GlyphResult gr = renderGlyph(out, n, max,
                                      glyph->strokes, cx, char_ty, display_sc,
                                      r, g, b, bold);

        cx += glyph->advance * sc;

        // ── FIX v1.1: Blanking after each glyph ──────────────────────
        // Set blank point at the position of the NEXT character start.
        // The next glyph starts with pen_up=true (blank=1), so the galvo moves
        // the galvo moves with laser OFF from gr.last_xy -> cx,char_ty.
        // Without this explicit blank the galvo would draw the path with
        // through with the laser still active -> visible drag line.
        if (gr.had_points && n < max) {
            // determine next character position (wave-corrected if active)
            float next_ty = ty;
            if (wave_on && text[ci+1])
                next_ty += sinf(wave_t + (ci+1) * 0.6f) * sc * 3.f;
            addPt(out, n, max, cx, next_ty, 0, 0, 0, /*blank=*/1);
        }
        // ────────────────────────────────────────────────────────────────
    }

    // apply rotation around origin
    if (fabsf(rot) > 0.001f) {
        const float cos_r = cosf(rot), sin_r = sinf(rot);
        for (size_t i = 0; i < n; i++) {
            float px = out[i].x, py = out[i].y;
            out[i].x = (int16_t)(px * cos_r - py * sin_r);
            out[i].y = (int16_t)(px * sin_r + py * cos_r);
        }
    }

    return n;
}

// ============================================================
// generate — public interface
// ============================================================
size_t generate(LaserPoint* out, size_t max_pts, const TextConfig& cfg, uint32_t phase) {
    if (!cfg.active || !cfg.text[0]) return 0;

    // Phase overflow protection (wraps after ~49 days at 1kHz)
    const uint32_t safe_phase = phase % 0xFFFFFF;

    const float BASE_SCALE = 18000.f / 55.f;
    float sc  = BASE_SCALE * (0.25f + cfg.size_val / 255.f * 1.5f);
    float spd = cfg.speed / 255.f;
    float t   = safe_phase * spd * 0.02f;

    const int full_len = (int)strlen(cfg.text);
    float tw = textWidth(cfg.text, full_len) * sc;
    ESP_LOGI("TXT","tw=%.0f sc=%.1f start_x=%.0f full_len=%d", tw, display_sc, -tw/2.f, full_len);

    float max_half = 28000.f;
    float display_sc = (tw / 2.f > max_half) ? sc * max_half / (tw / 2.f) : sc;
    if (display_sc != sc) tw = textWidth(cfg.text, full_len) * display_sc;
    float start_x = -tw / 2.f;
    float base_y  = 0.f;

    switch (cfg.animation) {

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
            // frames-per-char: at speed=80 -> 38 frames -> ~1 char/s
            // phase is incremented ~40x/s (25ms task period)
            const int fpc     = max(4, (int)(255 * 12 / max(1, (int)cfg.speed)));
            const int tw_cycle = full_len + 3;  // +3 = pause at end
            int visible = (int)(safe_phase / fpc) % tw_cycle;
            if (visible > full_len) visible = full_len;  // pause shows full text

            if (visible == 0) return 0;

            // Use temporary string trimmed to visible characters.
            // cfg.text is const -> local copy on the stack (max 128 bytes).
            char temp[128];
            strncpy(temp, cfg.text, (size_t)visible);
            temp[visible] = '\0';

            float vw = textWidth(temp, visible) * sc;
            return renderTextString(out, max_pts, temp, visible,
                                    cfg, -vw * 0.5f, base_y, display_sc);
        }

        case TANIM_WAVE:
            return renderTextString(out, max_pts, cfg.text, full_len,
                                    cfg, start_x, base_y, display_sc,
                                    0.f, /*wave_on=*/true, t);

        case TANIM_PULSE: {
            float pulse_sc = sc * (0.7f + 0.3f * fabsf(sinf(t * 3.f)));
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
            float zoom_sc = sdisplay_sc * zoom;
            float zw      = textWidth(cfg.text, full_len) * zoom_sc;
            return renderTextString(out, max_pts, cfg.text, full_len,
                                    cfg, -zw * 0.5f, base_y, zoom_sc);
        }

        case TANIM_3D_EXT: {
            // 3D-Extrusion: Text + Schattenkopie versetzt
            size_t n = renderTextString(out, max_pts, cfg.text, full_len,
                                        cfg, start_x, base_y, display_sc);
            // Shadow copy to bottom-right
            size_t base_n = n;
            float sdx = sc * 0.55f, sdy = -sc * 0.25f;
            for (size_t i = 0; i < base_n && n < max_pts; i++) {
                out[n] = out[i];
                out[n].x += (int16_t)sdx;
                out[n].y += (int16_t)sdy;
                out[n].r  = (uint8_t)(out[i].r * 0.3f);
                out[n].g  = (uint8_t)(out[i].g * 0.3f);
                out[n].b  = (uint8_t)(out[i].b * 0.3f);
                n++;
            }
            return n;
        }

        case TANIM_ORBIT: {
            // Text orbits on ellipse
            float rot2 = fmodf(t * 1.2f, 2.f * (float)M_PI);
            float sp2  = 0.6f + 0.4f * (0.5f + 0.5f * sinf(rot2));
            float orx  = cosf(rot2) * 10000.f;
            float ory  = sinf(rot2) * 5000.f;
            size_t n = renderTextString(out, max_pts, cfg.text, full_len,
                                        cfg, start_x * sp2 + orx,
                                        base_y * sp2 + ory, display_sc * sp2);
            return n;
        }

        case TANIM_STARWARS: {
            // Star Wars scroll: bottom to top, perspective
            float period = 2.8f;
            float yScroll = fmodf(t * 3.f, period);
            size_t total = 0;
            for (int copy = 0; copy < 2; copy++) {
                float yBase = -32767.f * 0.7f + yScroll * 32767.f + copy * period * 32767.f;
                // Perspective: bottom large, top small
                for (int ci = 0; ci < full_len && cfg.text[ci]; ci++) {
                    char ch = toupper((unsigned char)cfg.text[ci]);
                    const FontGlyph* g2 = nullptr;
                    for (int i = 0; i < GLYPH_COUNT; i++) {
                        if (GLYPHS[i].ch == (uint8_t)ch) { g2 = &GLYPHS[i]; break; }
                    }
                    if (!g2) continue;
                    float yW = base_y * 0.25f + yBase / 32767.f * sc * 16.f;
                    float persp = fmaxf(0.1f, (yW / (sc * 8.f) + 1.5f) / 3.0f);
                    float scP = sc * (0.25f + persp * 1.5f);
                    float cx2 = start_x * scP / sc;
                    for (int j = 0; j < ci; j++) {
                        char prev = toupper((unsigned char)cfg.text[j]);
                        for (int k = 0; k < GLYPH_COUNT; k++) {
                            if (GLYPHS[k].ch == (uint8_t)prev) { cx2 += GLYPHS[k].advance * scP; break; }
                        }
                    }
                    renderGlyph(out, total, max_pts, g2->strokes,
                                cx2, yW, scP,
                                cfg.col_r, cfg.col_g, cfg.col_b);
                }
            }
            return total;
        }

        default:
            return renderTextString(out, max_pts, cfg.text, full_len,
                                    cfg, start_x, base_y, display_sc);
    }
}

} // namespace textrender
