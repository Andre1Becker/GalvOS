#include "pattern_engine.h"
#include "mutex.h"
#include "preset_patterns.h"
#include "calib_patterns.h"
#include "text_renderer.h"
#include "curve_patterns.h"
#include "ilda/ilda_player.h"
#include "control/dmx_in.h"
#include "output/galvo_out.h"
#include "safety/safety.h"
#include "util/log_buffer.h"
#include "net/web_ui.h"
#include <Arduino.h>
#include <math.h>
#include <esp_log.h>

namespace patterns {

static const char* TAG = "pattern";
static LaserPoint s_frame[PATTERN_POINTS_MAX];
static volatile int      s_test_pattern  = -1;
static volatile uint32_t s_test_started  = 0;
static volatile int8_t   s_preset_idx    = -1;  // -1 = no Preset active

void setPreset(int8_t idx) {
    s_preset_idx = (idx >= 0 && idx < (int8_t)presets::PRESET_COUNT) ? idx : -1;
    s_test_pattern = -1;
    gState.calib_active = false;
}
int8_t getPreset() { return s_preset_idx; }

/* ============================================================
 * Geometrie-Primitives
 * ============================================================ */
static size_t genCircle(LaserPoint* out, uint16_t radius, uint8_t r, uint8_t g, uint8_t b) {
    const size_t N = 128;
    static_assert(N <= PATTERN_POINTS_MAX, "genCircle exceeds frame buffer");
    for (size_t i = 0; i < N; i++) {
        float a = (2.0f * PI * i) / N;
        out[i].x = (int16_t)(cosf(a) * radius);
        out[i].y = (int16_t)(sinf(a) * radius);
        out[i].r = r; out[i].g = g; out[i].b = b;
        out[i].blank = 0;
    }
    return N;
}

static size_t genSquare(LaserPoint* out, uint16_t size, uint8_t r, uint8_t g, uint8_t b) {
    const size_t per_side = 40;  // more points = smoother on fast galvos
    static_assert(1 + 4 * (40 + 1) <= PATTERN_POINTS_MAX, "genSquare exceeds frame buffer");
    int16_t s = (int16_t)size;
    // corners: bottom-left → bottom-right → top-right → top-left → close
    int16_t corners[5][2] = {
        {(int16_t)(-s), (int16_t)(-s)},  // 0: bottom-left  (start)
        {s,             (int16_t)(-s)},  // 1: bottom-right
        {s,             s            },  // 2: top-right
        {(int16_t)(-s), s            },  // 3: top-left
        {(int16_t)(-s), (int16_t)(-s)},  // 4: back to start (close)
    };
    size_t idx = 0;
    // Blank jump to start position
    out[idx] = {corners[0][0], corners[0][1], 0, 0, 0, 1};  idx++;
    // Draw 4 sides (corner 0→1, 1→2, 2→3, 3→4=0)
    for (int seg = 0; seg < 4; seg++) {
        for (size_t i = 0; i <= per_side; i++) {
            float t = (float)i / per_side;
            int16_t x = corners[seg][0] + (int16_t)((corners[seg+1][0] - corners[seg][0]) * t);
            int16_t y = corners[seg][1] + (int16_t)((corners[seg+1][1] - corners[seg][1]) * t);
            out[idx].x = x; out[idx].y = y;
            out[idx].r = r; out[idx].g = g; out[idx].b = b;
            out[idx].blank = (seg == 0 && i == 0) ? 1 : 0;  // blank only at very start
            idx++;
        }
    }
    return idx;
}

static size_t genStar(LaserPoint* out, uint16_t radius, uint8_t r, uint8_t g, uint8_t b) {
    const size_t POINTS = 5;
    const size_t per_seg = 40;
    static_assert(5 * 40 <= PATTERN_POINTS_MAX, "genStar exceeds frame buffer");
    int16_t verts[POINTS][2];
    for (size_t i = 0; i < POINTS; i++) {
        float a = (2.0f * PI * 2 * i) / POINTS - PI / 2;
        verts[i][0] = (int16_t)(cosf(a) * radius);
        verts[i][1] = (int16_t)(sinf(a) * radius);
    }
    size_t idx = 0;
    for (size_t s = 0; s < POINTS; s++) {
        size_t n = (s + 1) % POINTS;
        for (size_t i = 0; i < per_seg; i++) {
            float t = (float)i / per_seg;
            out[idx].x = verts[s][0] + (int16_t)((verts[n][0] - verts[s][0]) * t);
            out[idx].y = verts[s][1] + (int16_t)((verts[n][1] - verts[s][1]) * t);
            out[idx].r = r; out[idx].g = g; out[idx].b = b;
            out[idx].blank = 0;
            idx++;
        }
    }
    return idx;
}

static size_t genCenterPoint(LaserPoint* out) {
    out[0] = LaserPoint(0, 0, 255, 0, 0, 0);
    return 1;
}

static size_t genCross(LaserPoint* out) {
    const size_t per_line = 60;
    static_assert(1 + 2 * 60 <= PATTERN_POINTS_MAX, "genCross exceeds frame buffer");
    size_t idx = 0;
    // Blank to start of H-line
    out[idx] = LaserPoint((int16_t)(-20000), 0, 0, 0, 0, 1); idx++;
    for (size_t i = 0; i < per_line; i++) {
        float t = ((float)i / (per_line - 1)) * 2 - 1;
        out[idx] = LaserPoint((int16_t)(t * 20000), 0, 255, 0, 0, 0);
        idx++;
    }
    for (size_t i = 0; i < per_line; i++) {
        float t = ((float)i / (per_line - 1)) * 2 - 1;
        out[idx] = LaserPoint(0, (int16_t)(t * 20000), 0, 255, 0, (uint8_t)(i == 0 ? 1 : 0));
        idx++;
    }
    return idx;
}

static size_t genIldaTestPattern(LaserPoint* out) {
    size_t idx = 0;
    int16_t s = 20000;
    int16_t cn[4][2] = {
        {(int16_t)(-s), (int16_t)(-s)},
        {s,             (int16_t)(-s)},
        {s,             s},
        {(int16_t)(-s), s}
    };
    for (int c = 0; c < 4; c++) {
        int c2 = (c + 1) & 3;
        for (int i = 0; i < 20; i++) {
            float t = (float)i / 19;
            out[idx].x = cn[c][0] + (int16_t)((cn[c2][0]-cn[c][0]) * t);
            out[idx].y = cn[c][1] + (int16_t)((cn[c2][1]-cn[c][1]) * t);
            out[idx].r = 255; out[idx].g = 255; out[idx].b = 255;
            out[idx].blank = 0;
            idx++;
        }
    }
    for (int i = 0; i < 64; i++) {
        float a = (2.0f * PI * i) / 64;
        out[idx].x = (int16_t)(cosf(a) * 12000);
        out[idx].y = (int16_t)(sinf(a) * 12000);
        out[idx].r = 0; out[idx].g = 255; out[idx].b = 0;
        out[idx].blank = (uint8_t)(i == 0 ? 1 : 0);
        idx++;
    }
    return idx;
}

/* ============================================================
 * DMX read with override
 * ============================================================ */
struct DmxView {
    uint8_t master, color, color_speed, pattern_group, pattern_select;
    uint8_t effect, effect_speed, size, auto_scale, rotation;
    uint8_t hflip, vflip, hmove, vmove, wave_x, tapered;
};

static void readDmx(DmxView& v) {
    uint8_t raw[DMX_CHANNELS_USED];
    if (gOverride.active || gState.ui_override.load()) {
        for (int i = 0; i < DMX_CHANNELS_USED; i++) raw[i] = gOverride.values[i];
        gState.source = SRC_WEBUI;
    } else if (dmx_in::isReceiving()) {
        dmx_in::getChannels(raw);
        gState.source = SRC_DMX;
    } else {
        // No DMX and no UI override — use gOverride.values as safe defaults
        // (hmove=128=center, vmove=128=center, rotation=0, etc.)
        for (int i = 0; i < DMX_CHANNELS_USED; i++) raw[i] = gOverride.values[i];
        gState.source = SRC_WEBUI;
    }
    v.master = raw[DMX_MASTER]; v.color = raw[DMX_COLOR];
    v.color_speed = raw[DMX_COLOR_SPEED]; v.pattern_group = raw[DMX_PATTERN_GROUP];
    v.pattern_select = raw[DMX_PATTERN_SELECT]; v.effect = raw[DMX_DYN_EFFECT];
    v.effect_speed = raw[DMX_EFFECT_SPEED]; v.size = raw[DMX_SIZE];
    v.auto_scale = raw[DMX_AUTO_SCALE]; v.rotation = raw[DMX_ROTATION];
    v.hflip = raw[DMX_HFLIP]; v.vflip = raw[DMX_VFLIP];
    v.hmove = raw[DMX_HMOVE]; v.vmove = raw[DMX_VMOVE];
    v.wave_x = raw[DMX_WAVE_AMP]; v.tapered = raw[DMX_WAVE_FREQ];
    // Pass ILDA DMX channels directly to the player
    ilda::setFromDMX(raw[DMX_ILDA_SELECT]);
    if (ilda::gILDA.active) {
        ilda::gILDA.speed    = raw[DMX_ILDA_SPEED]   ? raw[DMX_ILDA_SPEED]   : 128;
        ilda::gILDA.size_val = raw[DMX_ILDA_SIZE]    ? raw[DMX_ILDA_SIZE]    : 128;
        ilda::gILDA.loop     = (raw[DMX_ILDA_LOOP]  > 0);
    }
}

static uint8_t resolveMasterDimmer(uint8_t v) {
    // UI master dimmer always wins if set
    uint8_t ui_dim = gState.ui_master_dimmer.load();
    if (ui_dim > 0) return ui_dim;
    // Normal DMX path
    if (v < 10) return 0;
    uint16_t mapped = ((uint16_t)(v - 10) * 255) / (255 - 10);
    return (mapped > 255) ? 255 : (uint8_t)mapped;
}

static void resolveColor(uint8_t ch2, uint8_t& r, uint8_t& g, uint8_t& b) {
    if (ch2 <= 9)        { r = 255; g = 255; b = 255; return; }
    if (ch2 <= 19)       { r = 255; g = 0;   b = 0;   return; }
    if (ch2 <= 29)       { r = 0;   g = 0;   b = 255; return; }
    if (ch2 <= 39)       { r = 255; g = 0;   b = 255; return; }
    if (ch2 <= 49)       { r = 0;   g = 255; b = 255; return; }
    if (ch2 <= 59)       { r = 255; g = 255; b = 0;   return; }
    if (ch2 <= 69)       { r = 0;   g = 255; b = 0;   return; }
    if (ch2 >= 90 && ch2 <= 92) { r = 255; g = 0; b = 0; return; }
    r = g = b = 200;
}

static size_t genPattern(const DmxView& v, LaserPoint* out) {
    uint16_t radius = 5000 + (v.size * 12000) / 255;
    if (v.pattern_group <= 24)  return genCircle(out, radius, 255, 255, 255);
    if (v.pattern_group <= 49)  return genSquare(out, radius, 255, 255, 255);
    if (v.pattern_group <= 74)  return genStar(out,   radius, 255, 255, 255);
    if (v.pattern_group <= 99) {
        size_t n = 32;
        for (size_t i = 0; i < n; i++) {
            float a = (2.0f * PI * i) / n;
            out[i] = LaserPoint((int16_t)(cosf(a) * radius), (int16_t)(sinf(a) * radius),
                                255, 255, 255, 0);
        }
        return n;
    }
    return genCircle(out, radius, 200, 200, 200);
}

static void applyTransform(LaserPoint* pts, size_t n, const DmxView& v, uint32_t phase) {
    float angle;
    if (v.rotation <= 127) angle = (v.rotation * 2.0f * PI) / 128.0f;
    else {
        int speed = v.rotation - 128;
        if (v.rotation > 191) speed = 192 - v.rotation;
        angle = (phase * (speed + 1)) * 0.0001f;
    }
    float ca = cosf(angle), sa = sinf(angle);
    int16_t tx = (int16_t)(((int16_t)v.hmove - 128) * 100);
    int16_t ty = (int16_t)(((int16_t)v.vmove - 128) * 100);
    float wave_amp = (v.wave_x >= 2) ? (v.wave_x / 32) * 500.0f : 0.0f;
    float wave_phase = phase * 0.05f;

    for (size_t i = 0; i < n; i++) {
        float x = pts[i].x, y = pts[i].y;
        if (wave_amp > 0.0f) y += wave_amp * sinf(wave_phase + i * 0.1f);
        float xr = x * ca - y * sa;
        float yr = x * sa + y * ca;
        pts[i].x = (int16_t)constrain(xr + tx, -32760.0f, 32760.0f);
        pts[i].y = (int16_t)constrain(yr + ty, -32760.0f, 32760.0f);
    }
}

static void applyCalibration(LaserPoint* pts, size_t n) {
    for (size_t i = 0; i < n; i++) {
        int32_t x = pts[i].x, y = pts[i].y;
        if (gConfig.swap_xy) { int32_t t = x; x = y; y = t; }
        if (gConfig.invert_x) x = -x;
        if (gConfig.invert_y) y = -y;
        x = (x * gConfig.galvo_x_gain) / 32767;
        y = (y * gConfig.galvo_y_gain) / 32767;
        x += gConfig.galvo_x_offset;
        y += gConfig.galvo_y_offset;
        int32_t lim_lo = (int32_t)gConfig.dac_limit_min - 0x8000;
        int32_t lim_hi = (int32_t)gConfig.dac_limit_max - 0x8000;
        pts[i].x = (int16_t)constrain(x, lim_lo, lim_hi);
        pts[i].y = (int16_t)constrain(y, lim_lo, lim_hi);
        //if (i < 3) ESP_LOGI("CAL","x=%.0f y=%.0f -> %d %d", (float)pts[i].x, (float)pts[i].y, pts[i].x, pts[i].y);
    }
}

static void applyRainbow(LaserPoint* pts, size_t n, uint8_t speed, uint32_t phase) {
    if (speed < 10) return;
    float hue = (phase * (speed / 50.0f)) * 0.01f;
    for (size_t i = 0; i < n; i++) {
        float h = hue + i * 0.005f;
        float frac = h - floorf(h);
        float r, g, b;
        int seg = (int)(frac * 6) % 6;
        float ff = frac * 6 - seg;
        switch (seg) {
            case 0: r = 1; g = ff; b = 0; break;
            case 1: r = 1 - ff; g = 1; b = 0; break;
            case 2: r = 0; g = 1; b = ff; break;
            case 3: r = 0; g = 1 - ff; b = 1; break;
            case 4: r = ff; g = 0; b = 1; break;
            default:r = 1; g = 0; b = 1 - ff; break;
        }
        pts[i].r = (uint8_t)(r * 255);
        pts[i].g = (uint8_t)(g * 255);
        pts[i].b = (uint8_t)(b * 255);
    }
}

/* ============================================================
 * Public API
 * ============================================================ */
void init() {}
void setManualMode(bool, uint8_t) {}

void setCurve(int8_t idx) {
    // Initialize defaults the first time
    if (!gCurves.initialized) {
        for (uint8_t i = 0; i < curves::CURVE_COUNT; i++) {
            curves::CurveParams tmp;
            curves::initDefaultParams(i, tmp);
            for (int j = 0; j < 5; j++) gCurves.params[i].p[j] = tmp.p[j];
            gCurves.params[i].r = tmp.r;
            gCurves.params[i].g = tmp.g;
            gCurves.params[i].b = tmp.b;
        }
        gCurves.initialized = true;
    }
    gCurves.active_curve = idx;
    if (idx >= 0) {
        // Deactivate competing modes
        s_preset_idx   = -1;
        gTextConfig.active = false;
        gState.calib_active = false;
    }
}
int8_t getCurve() { return gCurves.active_curve; }

void triggerTestPattern(const char* name) {
if (strcmp(name, "ilda") == 0) {
        s_test_pattern = -1;
        gState.calib_active  = true;
        gState.calib_idx     = 12;
        gState.calib_bright  = 200;   // laser brightness
        gState.calib_channel = 128;   // scan size (maps to ~8° @ 30kpps)
        gState.ui_master_dimmer.store(200);
        return;
    }
    int id = -1;
    if      (strcmp(name, "center") == 0) id = 0;
    else if (strcmp(name, "cross")  == 0) id = 1;
    else if (strcmp(name, "square") == 0) id = 2;
    else if (strcmp(name, "circle") == 0) id = 3;
    s_test_pattern = id;
    s_test_started = millis();
}

void stopTestPattern() {
    s_test_pattern = -1;
}
// ── Color Animation Engine ────────────────────────────────────────────────────
// Applies col_anim_type / col_override to s_frame[0..n-1].
// Call after generate(), before dimmer scaling.
static void applyColorAnim(size_t n) {
    static uint32_t s_anim_phase = 0;
    static bool     s_strobe_on  = false;

    static const uint8_t GRAD[10][6][3] = {
        {{255,0,0},{255,128,0},{255,255,0},{0,255,0},{0,200,255},{128,0,255}}, // Rainbow
        {{255,0,0},{255,80,0},{255,200,0},{255,255,80},{0xFF,0xFF,0xFF}},       // Fire
        {{0,0,80},{0,80,200},{0,200,255},{80,255,255},{0xFF,0xFF,0xFF}},        // Ocean
        {{255,0,128},{128,0,255},{0,255,200},{255,255,0},{0xFF,0xFF,0xFF}},     // Neon
        {{80,0,40},{200,0,80},{255,80,0},{255,200,0},{0xFF,0xFF,0xFF}},         // Sunset
        {{0,40,0},{0,128,40},{80,255,80},{200,255,100},{0xFF,0xFF,0xFF}},       // Forest
        {{200,230,255},{100,180,255},{40,100,200},{200,230,255},{0xFF,0xFF,0xFF}}, // Ice
        {{80,0,0},{255,0,0},{255,80,0},{255,255,0},{255,0,0},{0xFF,0xFF,0xFF}}, // Lava
        {{255,100,200},{200,100,255},{100,200,255},{255,255,100},{0xFF,0xFF,0xFF}}, // Candy
        {{0,0,0},{255,255,255},{0,0,0},{0xFF,0xFF,0xFF}},                       // Mono
    };
    static const uint8_t CHASE[10][6][3] = {
        {{255,0,0},{0,255,0},{0,0,255},{0xFF,0xFF,0xFF}},
        {{255,0,0},{0,255,0},{0,0,255},{255,255,255},{0xFF,0xFF,0xFF}},
        {{0,255,255},{255,0,255},{255,255,0},{0xFF,0xFF,0xFF}},
        {{255,150,150},{150,255,150},{150,150,255},{255,255,150},{0xFF,0xFF,0xFF}},
        {{255,0,0},{255,80,0},{255,200,0},{255,255,255},{0xFF,0xFF,0xFF}},
        {{0,0,255},{0,100,255},{0,200,255},{200,230,255},{0xFF,0xFF,0xFF}},
        {{255,0,0},{255,128,0},{255,255,0},{0,255,0},{0,0,255},{128,0,255}},
        {{255,255,255},{0,0,0},{0xFF,0xFF,0xFF}},
        {{255,0,0},{0,0,255},{0xFF,0xFF,0xFF}},
        {{255,0,0},{0,255,0},{255,255,255},{0xFF,0xFF,0xFF}},
    };
    static const uint8_t FLIP[4][3] = {
        {255,0,0},{0,255,0},{0,0,255},{255,255,255}
    };

    ColAnimType atype = gLivePreset.col_anim_type;
    uint8_t     aseq  = gLivePreset.col_anim_seq % 10;
    uint8_t     aspd  = gLivePreset.col_anim_speed;

    uint8_t ar = 255, ag = 0, ab = 0;

    if (atype == COL_ANIM_OFF) {
        if (gLivePreset.col_override) {
            ar = gLivePreset.col_r; ag = gLivePreset.col_g; ab = gLivePreset.col_b;
        } else { return; }  // no override, no anim: leave points as generated
    } else if (atype == COL_ANIM_GRADIENT) {
        s_anim_phase += (uint32_t)aspd * 10 + 10;
        const uint8_t (*stops)[3] = GRAD[aseq];
        int nstops = 0;
        while (nstops < 6 && !(stops[nstops][0]==0xFF && stops[nstops][1]==0xFF)) nstops++;
        if (nstops < 2) nstops = 2;
        uint32_t range = 65536UL / (nstops - 1);
        uint32_t ph    = s_anim_phase & 0xFFFF;
        int seg        = ph / range;
        if (seg >= nstops - 1) seg = nstops - 2;
        uint32_t f     = (ph - seg * range) * 255 / range;
        ar = (uint8_t)(stops[seg][0] + (int)(stops[seg+1][0]-stops[seg][0])*(int)f/255);
        ag = (uint8_t)(stops[seg][1] + (int)(stops[seg+1][1]-stops[seg][1])*(int)f/255);
        ab = (uint8_t)(stops[seg][2] + (int)(stops[seg+1][2]-stops[seg][2])*(int)f/255);
    } else if (atype == COL_ANIM_CHASE) {
        static uint32_t s_chase_acc  = 0;
        static uint8_t  s_chase_step = 0;
        s_chase_acc += aspd + 1;
        if (s_chase_acc >= 4096) {
            s_chase_acc -= 4096;
            int nc = 0;
            while (nc < 6 && !(CHASE[aseq][nc][0]==0xFF && CHASE[aseq][nc][1]==0xFF)) nc++;
            if (nc < 1) nc = 1;
            s_chase_step = (s_chase_step + 1) % nc;
        }
        {
            int nc = 0;
            while (nc < 6 && !(CHASE[aseq][nc][0]==0xFF && CHASE[aseq][nc][1]==0xFF)) nc++;
            if (nc < 1) nc = 1;
            uint8_t step = s_chase_step % nc;
            ar = CHASE[aseq][step][0]; ag = CHASE[aseq][step][1]; ab = CHASE[aseq][step][2];
        }
    } else if (atype == COL_ANIM_STROBE) {
        static uint32_t s_strobe_acc = 0;
        s_strobe_acc += (uint32_t)aspd * 4 + 4;
        if (s_strobe_acc >= 1024) { s_strobe_acc -= 1024; s_strobe_on = !s_strobe_on; }
        if (s_strobe_on) { ar = gLivePreset.col_r; ag = gLivePreset.col_g; ab = gLivePreset.col_b; }
        else             { ar = 0; ag = 0; ab = 0; }
    } else if (atype == COL_ANIM_PULSE) {
        s_anim_phase += (uint32_t)aspd * 5 + 5;
        float v = (sinf((float)(s_anim_phase & 0xFFFF) * 6.2832f / 65536.f) * 0.5f + 0.5f);
        ar = (uint8_t)(gLivePreset.col_r * v);
        ag = (uint8_t)(gLivePreset.col_g * v);
        ab = (uint8_t)(gLivePreset.col_b * v);
    } else if (atype == COL_ANIM_TWINKLE) {
        static uint8_t s_twink_val = 200, s_twink_tgt = 200;
        if (abs((int)s_twink_val - (int)s_twink_tgt) < 8) {
            uint32_t rnd = esp_random();
            s_twink_tgt = (rnd & 0xFF) > 200 ? (uint8_t)(rnd & 0xFF) : (uint8_t)(100 + (rnd & 0x63));
        }
        s_twink_val = (uint8_t)(s_twink_val + ((int)s_twink_tgt - (int)s_twink_val) / 4);
        float v = s_twink_val / 255.f;
        ar = (uint8_t)(gLivePreset.col_r * v);
        ag = (uint8_t)(gLivePreset.col_g * v);
        ab = (uint8_t)(gLivePreset.col_b * v);
    } else if (atype == COL_ANIM_FLIP) {
        static uint32_t s_flip_acc  = 0;
        static uint8_t  s_flip_step = 0;
        s_flip_acc += (uint32_t)aspd * 4 + 4;
        if (s_flip_acc >= 1024) { s_flip_acc -= 1024; s_flip_step = (s_flip_step + 1) % 4; }
        ar = FLIP[s_flip_step][0]; ag = FLIP[s_flip_step][1]; ab = FLIP[s_flip_step][2];
    }

    if (atype == COL_ANIM_SEGMENT) {
        static uint32_t s_seg_phase = 0;
        uint8_t nseg = gLivePreset.col_seg_count;
        if (nseg < 1) nseg = 1; if (nseg > 10) nseg = 10;
        int8_t dir = gLivePreset.col_seg_dir;
        s_seg_phase = (uint32_t)((int32_t)s_seg_phase + dir * (int32_t)(((uint32_t)aspd * aspd) / 6 + 64));
        size_t lit = 0;
        for (size_t i = 0; i < n; i++) if (!s_frame[i].blank) lit++;
        if (lit == 0) lit = 1;
        const uint8_t (*stops)[3] = GRAD[aseq];
        int nstops = 0;
        while (nstops < 6 && !(stops[nstops][0]==0xFF && stops[nstops][1]==0xFF && stops[nstops][2]==0xFF)) nstops++;
        if (nstops < 2) nstops = 2;
        uint8_t seg_r[10], seg_g[10], seg_b[10];
        for (uint8_t s = 0; s < nseg; s++) {
            uint32_t t  = (uint32_t)s * 65536UL / nseg;
            uint32_t rg = 65536UL / (nstops - 1);
            int sg      = (int)(t / rg); if (sg >= nstops-1) sg = nstops-2;
            uint32_t f  = (t - sg * rg) * 255 / rg;
            seg_r[s] = (uint8_t)(stops[sg][0] + (int)(stops[sg+1][0]-stops[sg][0])*(int)f/255);
            seg_g[s] = (uint8_t)(stops[sg][1] + (int)(stops[sg+1][1]-stops[sg][1])*(int)f/255);
            seg_b[s] = (uint8_t)(stops[sg][2] + (int)(stops[sg+1][2]-stops[sg][2])*(int)f/255);
        }
        size_t lit_idx = 0;
        uint32_t phase_off = (s_seg_phase >> 6) % lit;
        for (size_t i = 0; i < n; i++) {
            if (s_frame[i].blank) continue;
            uint32_t shifted = (lit_idx + phase_off) % lit;
            uint8_t  seg     = (uint8_t)(shifted * nseg / lit);
            s_frame[i].r = seg_r[seg]; s_frame[i].g = seg_g[seg]; s_frame[i].b = seg_b[seg];
            lit_idx++;
        }
    } else {
        for (size_t i = 0; i < n; i++) {
            if (s_frame[i].blank) continue;
            s_frame[i].r = ar; s_frame[i].g = ag; s_frame[i].b = ab;
        }
    }
}

void task(void*) {
    uint32_t phase = 0;

    for (;;) {
        // Notify safety subsystem that the pattern engine is alive.
        // Without this, safety::subsystemsOk() would time out and DISARM
        // the laser mid-pattern, causing spurious blank-point frames.
        safety::subsystemHeartbeat(0);  // SYS_PATTERN = 0
        if (s_test_pattern >= 0) {
            size_t n = 0;
            switch (s_test_pattern) {
                case 0: n = genCenterPoint(s_frame); break;
                case 1: n = genCross(s_frame); break;
                case 2: n = genSquare(s_frame, 18000, 255, 255, 255); break;
                case 3: n = genCircle(s_frame, 15000, 0, 255, 0); break;
                case 4: n = genIldaTestPattern(s_frame); break;
            }
            gState.master_dimmer.store(255);
            if (n == 0) { static LaserPoint blank_pt={0,0,0,0,0,1}; galvo::pushFrame(&blank_pt,1); vTaskDelay(pdMS_TO_TICKS(50)); continue; }  // max 20fps — headroom for galvoTask drain
            applyCalibration(s_frame, n);
            web_ui::publishPreviewFrame(s_frame, n);   // immer
            { uint32_t _t0=millis(); while (!galvo::pushFrame(s_frame, n)) { if (millis()-_t0 > 500) { safety::emergencyStop(); LOG_E(logbuf::CAT_SAFETY,"Pattern engine: pushFrame timeout, emergency stop"); break; } vTaskDelay(pdMS_TO_TICKS(2)); } }
            vTaskDelay(pdMS_TO_TICKS(50));
            if (millis() - s_test_started > 10000) s_test_pattern = -1;
            continue;
        }

        DmxView v;
        readDmx(v);
        gState.master_dimmer.store(resolveMasterDimmer(v.master));

        // ---- ILDA SD-card mode (highest priority) ----
        if (ilda::gILDA.active && ilda::hasNewFrame()) {
            size_t n = ilda::getFrame(s_frame, PATTERN_POINTS_MAX);
            if (n > 0) {
                applyCalibration(s_frame, n);
                web_ui::publishPreviewFrame(s_frame, n);
                if (gState.master_dimmer.load() > 0) {
                    { uint32_t _t0=millis(); while (!galvo::pushFrame(s_frame, n)) { if (millis()-_t0 > 500) { safety::emergencyStop(); LOG_E(logbuf::CAT_SAFETY,"Pattern engine: pushFrame timeout, emergency stop"); break; } vTaskDelay(pdMS_TO_TICKS(2)); } }
                } else {
                    static LaserPoint blank_pt = {0,0,0,0,0,1};
                    galvo::pushFrame(&blank_pt, 1);
                }
                vTaskDelay(pdMS_TO_TICKS(5));
                continue;
            }
        }

        // ---- calibration-Pattern-Modus ----
        // Priority: ILDA > Calib > Text > Preset > DMX
        if (gState.calib_active) {
            const uint32_t safe_ph = phase % 0xFFFFFF;
            size_t n = calib_patterns::generate(
                gState.calib_idx, s_frame, PATTERN_POINTS_MAX,
                safe_ph, gState.calib_bright, gState.calib_channel);
            if (n > 0) {
                applyCalibration(s_frame, n);
                web_ui::publishPreviewFrame(s_frame, n);
                if (gState.master_dimmer.load() > 0 || gState.ui_master_dimmer.load() > 0) {
                    { uint32_t _t0=millis(); while (!galvo::pushFrame(s_frame, n)) { if (millis()-_t0 > 500) { safety::emergencyStop(); LOG_E(logbuf::CAT_SAFETY,"Pattern engine: pushFrame timeout, emergency stop"); break; } vTaskDelay(pdMS_TO_TICKS(2)); } }
                    { uint32_t drain_ms = n / (uint32_t)gProjection.galvo_kpps;
                      if (drain_ms < 10) drain_ms = 10;
                      vTaskDelay(pdMS_TO_TICKS(drain_ms + drain_ms / 4)); }
                } else {
                    static LaserPoint blank = {0,0,0,0,0,1};
                    galvo::pushFrame(&blank, 1);
                    vTaskDelay(pdMS_TO_TICKS(40));
                }
                phase++;
                continue;
            }
        }

        // ---- Text Mode (highest preset priority) ----
        TextConfig textSnap;
        { LOCK_STATE(); textSnap = gTextConfig; }
        if (textSnap.active && textSnap.text[0]) {
            size_t n = textrender::generate(s_frame, PATTERN_POINTS_MAX, textSnap, phase);
            if (n == 0) { static LaserPoint blank_pt={0,0,0,0,0,1}; galvo::pushFrame(&blank_pt,1); vTaskDelay(pdMS_TO_TICKS(40)); continue; }  // guard
            // ESP_LOGI("TXT","frame n=%d", (int)n);
            applyCalibration(s_frame, n);
            web_ui::publishPreviewFrame(s_frame, n);
            if (gState.master_dimmer.load() > 0) {
                { uint32_t _t0=millis(); while (!galvo::pushFrame(s_frame, n)) { if (millis()-_t0 > 500) { safety::emergencyStop(); LOG_E(logbuf::CAT_SAFETY,"Pattern engine: pushFrame timeout, emergency stop"); break; } vTaskDelay(pdMS_TO_TICKS(2)); } }
            } else {
                static LaserPoint blank_pt = {0,0,0,0,0,1};
                galvo::pushFrame(&blank_pt, 1);
            }
            phase++;
            vTaskDelay(pdMS_TO_TICKS(40));
            continue;
        }

        // ---- Mathematical Curve Mode ----
        if (gCurves.active_curve >= 0 && gCurves.active_curve < (int8_t)curves::CURVE_COUNT) {
            curves::CurveType ct = (curves::CurveType)gCurves.active_curve;
            const CurveConfig::Params& cp = gCurves.params[gCurves.active_curve];
            curves::CurveParams params;
            for (int j = 0; j < 5; j++) params.p[j] = cp.p[j];
            params.r = cp.r; params.g = cp.g; params.b = cp.b;

            size_t n = curves::generate(ct, params, phase, s_frame, PATTERN_POINTS_MAX);
            if (n > 0) {
                // Apply color override / animation to curve points
                if (gLivePreset.col_override) {
                    uint8_t cr = gLivePreset.col_r;
                    uint8_t cg = gLivePreset.col_g;
                    uint8_t cb = gLivePreset.col_b;
                    for (size_t i = 0; i < n; i++) {
                        if (!s_frame[i].blank) {
                            s_frame[i].r = cr;
                            s_frame[i].g = cg;
                            s_frame[i].b = cb;
                        }
                    }
                }
                
                // Apply color animation / override (same engine as preset mode)
                applyColorAnim(n);
                // Apply master dimmer
                uint8_t dim = gState.master_dimmer.load();
                for (size_t i = 0; i < n; i++) {
                    if (!s_frame[i].blank) {
                        s_frame[i].r = (s_frame[i].r * dim) >> 8;
                        s_frame[i].g = (s_frame[i].g * dim) >> 8;
                        s_frame[i].b = (s_frame[i].b * dim) >> 8;
                    }
                }
                applyCalibration(s_frame, n);
                web_ui::publishPreviewFrame(s_frame, n);
                if (dim > 0) {
                    { uint32_t _t0=millis(); while (!galvo::pushFrame(s_frame, n)) { if (millis()-_t0 > 500) { safety::emergencyStop(); LOG_E(logbuf::CAT_SAFETY,"Pattern engine: pushFrame timeout, emergency stop"); break; } vTaskDelay(pdMS_TO_TICKS(2)); } }
                } else {
                    static LaserPoint blank_pt = {0,0,0,0,0,1};
                    galvo::pushFrame(&blank_pt, 1);
                }
                phase++;
                vTaskDelay(pdMS_TO_TICKS(35));  // ~28fps — longest curve (Butterfly 1024pts @ 30kpps = 34ms drain)
                continue;
            }
        }

        // ---- Preset Mode (overrride DMX) ----
        if (s_preset_idx >= 0) {
            uint8_t speed   = gLivePreset.speed;
            uint8_t sz      = gLivePreset.size_val;
            size_t n = presets::generate((uint8_t)s_preset_idx, s_frame,
                                         PATTERN_POINTS_MAX, phase, speed, sz);

            // Color override from Live-Controls
            // ── Firmware color animation engine ──────────────────────────
            // Runs per-frame on Core 0; no HTTP overhead.
            // Also DMX-controllable via CH 23-25.
            
            // Apply color animation / override
            applyColorAnim(n);

            // Additional Rotation from Live-Controls
            // Z-rotation (classic in-plane)
            if (gLivePreset.rot_z) {
                gLivePreset.rot_angle_z += gLivePreset.rot_speed_z;
                float cz = cosf(gLivePreset.rot_angle_z);
                float sz2 = sinf(gLivePreset.rot_angle_z);
                for (size_t i=0;i<n;i++){
                    int16_t nx=(int16_t)(s_frame[i].x*cz - s_frame[i].y*sz2);
                    int16_t ny=(int16_t)(s_frame[i].x*sz2+ s_frame[i].y*cz);
                    s_frame[i].x=nx; s_frame[i].y=ny;
                }
            } else if (fabsf((float)gLivePreset.rotation) > 0.5f) {
                float er = gLivePreset.rotation * (float)(M_PI/180.);
                float cr=cosf(er), sr=sinf(er);
                for (size_t i=0;i<n;i++){
                    int16_t nx=(int16_t)(s_frame[i].x*cr-s_frame[i].y*sr);
                    int16_t ny=(int16_t)(s_frame[i].x*sr+s_frame[i].y*cr);
                    s_frame[i].x=nx; s_frame[i].y=ny;
                }
            }
            // Y-rotation (tilt left/right -- perspective X compression)
            if (gLivePreset.rot_y) {
                gLivePreset.rot_angle_y += gLivePreset.rot_speed_y;
                float cy = cosf(gLivePreset.rot_angle_y);
                float sy2 = sinf(gLivePreset.rot_angle_y);
                for (size_t i=0;i<n;i++){
                    float z3 = s_frame[i].x * sy2;
                    float nx = s_frame[i].x * cy;
                    float d  = 1.f + z3 * 0.35f / 32767.f;
                    if(d<0.1f)d=0.1f;
                    s_frame[i].x = (int16_t)(nx/d);
                    s_frame[i].y = (int16_t)(s_frame[i].y/d);
                }
            }
            // X-rotation (tilt up/down -- perspective Y compression)
            if (gLivePreset.rot_x) {
                gLivePreset.rot_angle_x += gLivePreset.rot_speed_x;
                float cx2 = cosf(gLivePreset.rot_angle_x);
                float sx3 = sinf(gLivePreset.rot_angle_x);
                for (size_t i=0;i<n;i++){
                    float z3 = s_frame[i].y * sx3;
                    float ny = s_frame[i].y * cx2;
                    float d  = 1.f + z3 * 0.35f / 32767.f;
                    if(d<0.1f)d=0.1f;
                    s_frame[i].y = (int16_t)(ny/d);
                    s_frame[i].x = (int16_t)(s_frame[i].x/d);
                }
            }

            // Mirror
            if (gLivePreset.mirror_x) for (size_t i=0;i<n;i++) s_frame[i].x = -s_frame[i].x;
            if (gLivePreset.mirror_y) for (size_t i=0;i<n;i++) s_frame[i].y = -s_frame[i].y;
            if (n == 0) { static LaserPoint blank_pt={0,0,0,0,0,1}; galvo::pushFrame(&blank_pt,1); vTaskDelay(pdMS_TO_TICKS(40)); continue; }  // guard: preset generated 0 points
            {
                // TEMP DEBUG: log point count (total/lit/blank) per frame,
                // rate-limited to ~1x/2s. Covers ALL presets (ngon/star/wf/
                // curves), since every preset path reaches this point
                // before pushFrame(). Remove once point_optimizer density
                // is validated across all migrated presets.
                static uint32_t s_lastLogMs = 0;
                uint32_t nowMs = millis();
                if (nowMs - s_lastLogMs > 2000) {
                    s_lastLogMs = nowMs;
                    size_t litCount = 0;
                    for (size_t i = 0; i < n; i++) if (!s_frame[i].blank) litCount++;
                    LOG_I(logbuf::CAT_GALVO, "Preset frame: n=%u lit=%u blank=%u",
                          (unsigned)n, (unsigned)litCount, (unsigned)(n - litCount));
                }
            }
            applyTransform(s_frame, n, v, phase);
            applyCalibration(s_frame, n);
            web_ui::publishPreviewFrame(s_frame, n);
            if (gState.master_dimmer.load() > 0) {
                { uint32_t _t0=millis(); while (!galvo::pushFrame(s_frame, n)) { if (millis()-_t0 > 500) { safety::emergencyStop(); LOG_E(logbuf::CAT_SAFETY,"Pattern engine: pushFrame timeout, emergency stop"); break; } vTaskDelay(pdMS_TO_TICKS(2)); } }
            } else {
                static LaserPoint blank_pt = {0,0,0,0,0,1};
                galvo::pushFrame(&blank_pt, 1);
                vTaskDelay(pdMS_TO_TICKS(40));
                phase++;
                continue;
            }
            phase++;
            // Dynamic delay: match push rate to galvoTask drain rate.
            { uint32_t drain_ms = n / (uint32_t)gProjection.galvo_kpps;
              if (drain_ms < 10) drain_ms = 10;  // FreeRTOS tick = 10ms minimum
              vTaskDelay(pdMS_TO_TICKS(drain_ms + drain_ms / 4)); }
            continue;
        }

        // always fully calculate Pattern (for Preview)
        size_t n = genPattern(v, s_frame);
        uint8_t r, g, b;
        resolveColor(v.color, r, g, b);
        for (size_t i = 0; i < n; i++) { s_frame[i].r = r; s_frame[i].g = g; s_frame[i].b = b; }
        if (v.color >= 90 && v.color <= 92) applyRainbow(s_frame, n, v.color_speed, phase);
        applyTransform(s_frame, n, v, phase);
        applyCalibration(s_frame, n);
        web_ui::publishPreviewFrame(s_frame, n);

        // Galvo-Output: only if Dimmer > 0 (honor Laser-Closure )
        if (gState.master_dimmer.load() == 0) {
            // Send a blanked point so galvos do not stand still
            static LaserPoint blank_pt = {0, 0, 0, 0, 0, 1};
            galvo::pushFrame(&blank_pt, 1);
            vTaskDelay(pdMS_TO_TICKS(30));
        } else {
            if (n == 0) { static LaserPoint blank_pt={0,0,0,0,0,1}; galvo::pushFrame(&blank_pt,1); vTaskDelay(pdMS_TO_TICKS(40)); continue; }
            { uint32_t _t0=millis(); while (!galvo::pushFrame(s_frame, n)) { if (millis()-_t0 > 500) { safety::emergencyStop(); LOG_E(logbuf::CAT_SAFETY,"Pattern engine: pushFrame timeout, emergency stop"); break; } vTaskDelay(pdMS_TO_TICKS(2)); } }
            phase++;
            vTaskDelay(pdMS_TO_TICKS(50)); // max 25fps, save Core-0-CPU ressources
        }
    }
}

}  // namespace patterns