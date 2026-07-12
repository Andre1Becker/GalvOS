#include "pattern_engine.h"
#include "mutex.h"
#include "preset_patterns.h"
#include "calib_patterns.h"
#include "text_renderer.h"
#include "curve_patterns.h"
#include "paint_patterns.h"
#include "point_optimizer.h"
#include "ilda/ilda_player.h"
#include "control/dmx_in.h"
#include "net/artnet_in.h"
#include "output/galvo_out.h"
#include "safety/safety.h"
#include "util/log_buffer.h"
#include "net/web_ui.h"
#include <Arduino.h>
#include <math.h>
#include <esp_log.h>
#include <esp_heap_caps.h>

namespace patterns {

static const char* TAG = "pattern";
// s_frame is the per-frame staging buffer the whole pattern pipeline writes
// into before pushFrame() memcpy's it into the PSRAM output ring. It is only
// ever touched from patterns::task (Core 1, millisecond budget) -- never from
// the galvo ISR, which reads exclusively from s_ring -- so it can live in
// PSRAM. At 2048*8 = 16 KB this is the single largest internal-DRAM static in
// the firmware; moving it to PSRAM is the biggest cheap win for the cold-boot
// internal-heap baseline. Allocated in init(); internal-DRAM fallback keeps
// rendering alive if PSRAM is ever exhausted.
static LaserPoint* s_frame = nullptr;   // PATTERN_POINTS_MAX points, PSRAM (init())
static LaserPoint* s_pm_lit = nullptr;  // PSRAM buffer, allocated in init()
static LaserPoint* s_pm_kaleido = nullptr;  // PSRAM buffer, allocated in init()
static volatile int      s_test_pattern  = -1;
static volatile uint32_t s_test_started  = 0;
static volatile int8_t   s_preset_idx    = -1;  // -1 = no Preset active

void setPreset(int8_t idx) {
    s_preset_idx = (idx >= 0 && idx < (int8_t)presets::PRESET_COUNT) ? idx : -1;
    s_test_pattern = -1;
    gState.calib_active = false;
    if (idx >= 0) gPaint.active = false;
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
    } else if (artnet_in::isReceiving()) {
        artnet_in::getChannels(raw);
        gState.source = SRC_ARTNET;
    } else if (dmx_in::isReceiving()) {
        dmx_in::getChannels(raw);
        gState.source = SRC_DMX;
    } else {
        // No Art-Net, no DMX and no UI override — use gOverride.values as safe defaults
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
    // Color-Animation DMX channels (CH23-25). Only applied on real DMX/Art-Net
    // input -- WebUI sets gLivePreset.col_anim_* directly (/api/preset) and
    // must not be overwritten by gOverride.values fallback data.
    if (gState.source == SRC_DMX || gState.source == SRC_ARTNET) {
        uint8_t animType = raw[DMX_COL_ANIM_TYPE];
        gLivePreset.col_anim_type  = (animType <= COL_ANIM_FLIP) ? (ColAnimType)animType : COL_ANIM_OFF;
        gLivePreset.col_anim_seq   = raw[DMX_COL_ANIM_SEQ];
        gLivePreset.col_anim_speed = raw[DMX_COL_ANIM_SPEED];
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
void init() {
    // Frame staging buffer -> PSRAM (frees 16 KB internal DRAM). Fall back to
    // internal DRAM only if PSRAM is unavailable, so rendering never runs on a
    // null pointer.
    s_frame = (LaserPoint*)ps_malloc(PATTERN_POINTS_MAX * sizeof(LaserPoint));
    if (!s_frame) {
        ESP_LOGE(TAG, "PSRAM alloc failed for s_frame -- falling back to internal DRAM");
        s_frame = (LaserPoint*)heap_caps_malloc(PATTERN_POINTS_MAX * sizeof(LaserPoint),
                                                MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    s_pm_lit = (LaserPoint*)ps_malloc(PATTERN_POINTS_MAX * sizeof(LaserPoint));
    if (!s_pm_lit) ESP_LOGE(TAG, "PSRAM alloc failed for points-only buffer");
    s_pm_kaleido = (LaserPoint*)ps_malloc(PATTERN_POINTS_MAX * sizeof(LaserPoint));
    if (!s_pm_kaleido) ESP_LOGE(TAG, "PSRAM alloc failed for kaleidoscope buffer");
}
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
        gPaint.active = false;
    }
}
int8_t getCurve() { return gCurves.active_curve; }

// setPaintActive() -- Paint mode sits above Curve/Preset but below
// Calib/Text/ILDA in task() priority. Activating it must therefore clear
// every flag checked earlier in the loop (or it would never be reached);
// clearing Curve/Preset too is not required for rendering but keeps their
// WebUI "active" labels honest (mirrors setCurve() clearing s_preset_idx).
void setPaintActive(bool active) {
    gPaint.active = active;
    if (active) {
        s_test_pattern = -1;
        gState.calib_active = false;
        gTextConfig.active = false;
        s_preset_idx = -1;
        gCurves.active_curve = -1;
    }
}
bool getPaintActive() { return gPaint.active; }

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
        while (nstops < 6 && !(stops[nstops][0]==0xFF && stops[nstops][1]==0xFF && stops[nstops][2]==0xFF)) nstops++;
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
            while (nc < 6 && !(CHASE[aseq][nc][0]==0xFF && CHASE[aseq][nc][1]==0xFF && CHASE[aseq][nc][2]==0xFF)) nc++;
            if (nc < 1) nc = 1;
            s_chase_step = (s_chase_step + 1) % nc;
        }
        {
            int nc = 0;
            while (nc < 6 && !(CHASE[aseq][nc][0]==0xFF && CHASE[aseq][nc][1]==0xFF && CHASE[aseq][nc][2]==0xFF)) nc++;
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

// Speed-driven size oscillation (0 <-> size_val). autoscaleSpeed 0 = off.
// kAutoScaleRate tuned so speed=100% completes one full cycle in ~1-2s;
// retune if a specific loop duration is needed.
static constexpr float kAutoScaleRate = 0.01f;

static uint8_t computeAutoScaleSize(uint8_t baseSize, float* outFrac) {
    if (gLivePreset.autoscaleSpeed == 0) { *outFrac = 1.0f; return baseSize; }

    float phase;
    { LOCK_STATE();
        gLivePreset.autoscalePhase += (gLivePreset.autoscaleSpeed / 100.0f) * kAutoScaleRate;
        if (gLivePreset.autoscalePhase >= 1.0f) gLivePreset.autoscalePhase -= 1.0f;
        phase = gLivePreset.autoscalePhase;
    }

    float f;
    switch (gLivePreset.autoscaleMode) {
        case AUTOSCALE_SMALL_BIG:
            f = phase;
            break;
        case AUTOSCALE_BIG_SMALL:
            f = 1.0f - phase;
            break;
        case AUTOSCALE_SMALL_BIG_SMALL:
        default:
            f = (phase < 0.5f) ? phase * 2.0f : 2.0f - phase * 2.0f;
            break;
    }
    *outFrac = f;
    return (uint8_t)(f * baseSize);
}

static float clamp01(float t) { return (t < 0.0f) ? 0.0f : (t > 1.0f ? 1.0f : t); }

static float smoothstep01(float t) {
    t = clamp01(t);
    return t * t * (3.0f - 2.0f * t);
}

// Maps a dot's position to a normalized wipe coordinate in [0,1) for the
// selected Points-Only fade direction, so dots turn on/off in a spatial
// sweep across the shape instead of in perimeter/index order.
static float fadeWipePosition(uint8_t dir, float x, float y, float cx, float cy,
                               float minX, float maxX, float minY, float maxY,
                               float halfDiag) {
    switch (dir) {
        case FADE_DIR_OUT_IN: {
            float d = sqrtf((x - cx) * (x - cx) + (y - cy) * (y - cy));
            return 1.0f - clamp01(d / halfDiag);
        }
        case FADE_DIR_LEFT_RIGHT:
            return (maxX > minX) ? clamp01((x - minX) / (maxX - minX)) : 0.0f;
        case FADE_DIR_RIGHT_LEFT:
            return (maxX > minX) ? 1.0f - clamp01((x - minX) / (maxX - minX)) : 0.0f;
        case FADE_DIR_TOP_BOTTOM:
            return (maxY > minY) ? clamp01((y - minY) / (maxY - minY)) : 0.0f;
        case FADE_DIR_BOTTOM_TOP:
            return (maxY > minY) ? 1.0f - clamp01((y - minY) / (maxY - minY)) : 0.0f;
        case FADE_DIR_IN_OUT:
        default: {
            float d = sqrtf((x - cx) * (x - cx) + (y - cy) * (y - cy));
            return clamp01(d / halfDiag);
        }
    }
}

// ── Shared N-fold rotational copy core (Kaleidoscope + Mirror/Radial4) ───
// Copies the current frame `segments` times around the origin, each copy
// rotated by k*(360/segments)°. Odd-indexed copies are optionally
// mirrored (altMirrorH = flip X, altMirrorV = flip Y) before rotating,
// giving the alternating symmetry of a true kaleidoscope; with both flags
// false this is a plain rotational repeat (Paint-by-Finger's "Radial4"
// mode, generalized to a configurable segment count).
//
// Segment count is silently capped so `segments` copies + blank
// transitions still fit gOptimizerConfig.max_pts_per_frame -- same
// philosophy as applyPointsOnlyMode(): never drop points mid-copy, only
// ever fewer copies.
static void applyRadialCopy(size_t& n, uint8_t segments, bool altMirrorH, bool altMirrorV) {
    if (n == 0 || !s_pm_kaleido) return;  // PSRAM alloc failed in init() -- skip, don't crash

    uint8_t segs = segments;
    if (segs < 2) segs = 2;
    if (segs > KALEIDO_SEGMENTS_MAX) segs = KALEIDO_SEGMENTS_MAX;

    const uint8_t blankSamples = gOptimizerConfig.min_blank_samples;
    uint16_t budget = gOptimizerConfig.max_pts_per_frame;
    if (budget > PATTERN_POINTS_MAX) budget = PATTERN_POINTS_MAX;

    size_t srcN = (n > PATTERN_POINTS_MAX) ? PATTERN_POINTS_MAX : n;
    size_t perCopy = srcN + blankSamples;
    size_t maxSegs = perCopy ? (budget / perCopy) : 0;
    if (maxSegs < 2) return;  // can't fit even 2 copies this frame -- leave source frame untouched
    if (segs > maxSegs) segs = (uint8_t)maxSegs;

    // Snapshot the source wedge into PSRAM before overwriting s_frame in place.
    memcpy(s_pm_kaleido, s_frame, srcN * sizeof(LaserPoint));

    size_t o = 0;
    for (uint8_t k = 0; k < segs; k++) {
        if (o + srcN + blankSamples > PATTERN_POINTS_MAX) break;

        float angle = k * (2.0f * PI / segs);
        float ca = cosf(angle), sa = sinf(angle);
        bool  flip = (k % 2) == 1;
        float fx = (flip && altMirrorH) ? -1.0f : 1.0f;
        float fy = (flip && altMirrorV) ? -1.0f : 1.0f;

        // Blank jump from the end of the previous copy to the start of this
        // one -- galvo must settle before the laser re-enables (distance-
        // proportional single-point blanks cause streaks).
        if (k > 0) {
            const LaserPoint& first = s_pm_kaleido[0];
            float fsx = first.x * fx, fsy = first.y * fy;
            int16_t dstX = (int16_t)(fsx * ca - fsy * sa);
            int16_t dstY = (int16_t)(fsx * sa + fsy * ca);
            int16_t px = s_frame[o - 1].x, py = s_frame[o - 1].y;
            for (uint8_t d = 0; d < blankSamples; d++) {
                float t = (float)(d + 1) / (float)blankSamples;
                s_frame[o++] = LaserPoint((int16_t)(px + (dstX - px) * t),
                                          (int16_t)(py + (dstY - py) * t),
                                          0, 0, 0, 1);
            }
        }

        for (size_t i = 0; i < srcN; i++) {
            const LaserPoint& src = s_pm_kaleido[i];
            float sx = src.x * fx, sy = src.y * fy;
            int16_t nx = (int16_t)(sx * ca - sy * sa);
            int16_t ny = (int16_t)(sx * sa + sy * ca);
            s_frame[o++] = LaserPoint(nx, ny, src.r, src.g, src.b, src.blank);
        }
    }
    n = o;
}

// ── Kaleidoscope effect (global toggle, Preset + Curve mode) ─────────────
static void applyKaleidoscope(size_t& n) {
    if (!gLivePreset.kaleido_enabled) return;
    applyRadialCopy(n, gLivePreset.kaleido_segments,
                     gLivePreset.kaleido_mirror_h, gLivePreset.kaleido_mirror_v);
}

// ── Mirror effect (global toggle, separate from Kaleidoscope) ────────────
// Off / Horizontal flip / Vertical flip / 4-fold radial copy -- same mode
// set as Paint-by-Finger's mirror brush (see index.html::paintMirrorPoints()).
static void applyMirror(size_t& n) {
    switch (gLivePreset.mirror_mode) {
        case MIRROR_X:
            for (size_t i = 0; i < n; i++) s_frame[i].x = -s_frame[i].x;
            break;
        case MIRROR_Y:
            for (size_t i = 0; i < n; i++) s_frame[i].y = -s_frame[i].y;
            break;
        case MIRROR_RADIAL4:
            applyRadialCopy(n, 4, false, false);
            break;
        default:
            break;  // MIRROR_OFF
    }
}

// ── Points-Only render mode (global toggle, Proposal B) ──────────────────
// Subsamples the already-transformed preset frame (post color-anim, post
// rotation, post mirror) down to a handful of dwelling dots with a fade
// in/out cycle. Runs once per frame for every preset (ngon/star/wireframe/
// wave/3D/text-glyph/curve-based, etc.) since it operates purely on the
// final LaserPoint array, not on preset-specific geometry.
//
// Dwell length is derived from the flicker budget (gOptimizerConfig.
// max_pts_per_frame) so raising the dot-count slider automatically shortens
// dwell instead of silently exceeding the budget (same philosophy as
// optimizer::optimize()'s density scale-down).
static void applyPointsOnlyMode(size_t& n) {
    if (!gLivePreset.points_mode_enabled || n == 0) return;
    if (!s_pm_lit) return;  // PSRAM alloc failed in init() -- skip, don't crash

    uint8_t count = gLivePreset.points_count;
    if (count < 2) count = 2;
    if (count > POINTS_MODE_MAX_DOTS) count = POINTS_MODE_MAX_DOTS;

    uint16_t budget = gOptimizerConfig.max_pts_per_frame;
    if (budget > PATTERN_POINTS_MAX) budget = PATTERN_POINTS_MAX;
    int dwell = (int)(budget / count) / 2;
    if (dwell < POINTS_MODE_MIN_DWELL) dwell = POINTS_MODE_MIN_DWELL;
    if (dwell > POINTS_MODE_MAX_DWELL) dwell = POINTS_MODE_MAX_DWELL;

    // Snapshot lit points into PSRAM buffer -- lets the write-back loop
    // below overwrite s_frame in place without clobbering not-yet-read
    // source points.
    size_t nl = 0;
    float minX = 32767, maxX = -32768, minY = 32767, maxY = -32768;
    for (size_t i = 0; i < n && nl < PATTERN_POINTS_MAX; i++) {
        if (s_frame[i].blank) continue;
        s_pm_lit[nl++] = s_frame[i];
        if (s_frame[i].x < minX) minX = s_frame[i].x;
        if (s_frame[i].x > maxX) maxX = s_frame[i].x;
        if (s_frame[i].y < minY) minY = s_frame[i].y;
        if (s_frame[i].y > maxY) maxY = s_frame[i].y;
    }
    if (nl == 0) { n = 0; return; }

    float cx = (minX + maxX) * 0.5f, cy = (minY + maxY) * 0.5f;
    float halfDiag = (maxX - minX > maxY - minY) ? (maxX - minX) * 0.5f : (maxY - minY) * 0.5f;
    if (halfDiag < 1.0f) halfDiag = 1.0f;

    static uint32_t s_pm_acc_ms  = 0;
    static uint32_t s_pm_last_ms = 0;
    uint32_t now_ms = millis();
    uint32_t dt_ms  = s_pm_last_ms ? (now_ms - s_pm_last_ms) : 0;
    s_pm_last_ms = now_ms;
    uint32_t cycleMs = (uint32_t)gLivePreset.points_fade_in_ms + gLivePreset.points_fade_out_ms;
    if (cycleMs == 0) cycleMs = 1;
    s_pm_acc_ms = (s_pm_acc_ms + dt_ms) % cycleMs;
    const uint8_t blankSamples = gOptimizerConfig.min_blank_samples;  // snapshot once — avoid TOCTOU vs. live WebUI writes

    size_t o = 0;
    for (uint8_t k = 0; k < count; k++) {
        if (o + (size_t)dwell + (size_t)(2 * blankSamples) + 1 > PATTERN_POINTS_MAX) break;
        size_t src_idx = (size_t)((uint32_t)k * nl / count);
        const LaserPoint& src = s_pm_lit[src_idx];

        float v = 1.0f;
        if (!gLivePreset.points_static_on) {
            float wipeT = fadeWipePosition(gLivePreset.points_fade_dir, src.x, src.y,
                                            cx, cy, minX, maxX, minY, maxY, halfDiag);
            uint32_t dotPhaseMs = (s_pm_acc_ms + (uint32_t)(wipeT * cycleMs)) % cycleMs;

            if (dotPhaseMs < gLivePreset.points_fade_in_ms) {
                float t = gLivePreset.points_fade_in_ms
                        ? (float)dotPhaseMs / (float)gLivePreset.points_fade_in_ms : 1.0f;
                v = gLivePreset.points_fade_in_on ? smoothstep01(t) : 1.0f;
            } else {
                uint32_t fallMs = dotPhaseMs - gLivePreset.points_fade_in_ms;
                float t = gLivePreset.points_fade_out_ms
                        ? (float)fallMs / (float)gLivePreset.points_fade_out_ms : 1.0f;
                v = gLivePreset.points_fade_out_on ? (1.0f - smoothstep01(t)) : 0.0f;
            }
        }

        uint8_t r = (uint8_t)(src.r * v);
        uint8_t g = (uint8_t)(src.g * v);
        uint8_t b = (uint8_t)(src.b * v);

        float px = (o > 0) ? s_frame[o - 1].x : src.x;
        float py = (o > 0) ? s_frame[o - 1].y : src.y;
        int moveTicks   = blankSamples;
        int settleTicks = blankSamples;
        for (int d = 0; d < moveTicks; d++) {
            float t = (float)(d + 1) / (float)moveTicks;
            int16_t bx = (int16_t)(px + (src.x - px) * t);
            int16_t by = (int16_t)(py + (src.y - py) * t);
            s_frame[o++] = LaserPoint(bx, by, 0, 0, 0, 1);
        }
        for (int d = 0; d < settleTicks; d++)
            s_frame[o++] = LaserPoint(src.x, src.y, 0, 0, 0, 1);
        for (int d = 0; d < dwell; d++)
            s_frame[o++] = LaserPoint(src.x, src.y, r, g, b, 0);
    }
    n = o;
}

void task(void*) {
    uint32_t phase = 0;

    // s_frame must exist (PSRAM, or internal-DRAM fallback in init()). If both
    // allocations failed the device is out of memory entirely -- park the task
    // instead of dereferencing null and crashing the whole engine.
    if (!s_frame) {
        ESP_LOGE(TAG, "s_frame unavailable -- pattern task parked");
        for (;;) { safety::subsystemHeartbeat(0); vTaskDelay(pdMS_TO_TICKS(1000)); }
    }

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
            { uint32_t _t0=millis(); while (!galvo::pushFrame(s_frame, n)) { if (millis()-_t0 > 500) { safety::emergencyStop(); LOG_E(logbuf::CAT_SAFETY,"Pattern engine: pushFrame timeout, emergency stop"); break; } vTaskDelay(pdMS_TO_TICKS(2)); } }
            vTaskDelay(pdMS_TO_TICKS(50));
            if (millis() - s_test_started > 10000) s_test_pattern = -1;
            continue;
        }

        DmxView v;
        readDmx(v);
        gState.master_dimmer.store(resolveMasterDimmer(v.master));

        // Phase 3: reset the live optimizer transform to identity each frame.
        // Only the Preset and Paint paths (optimizer-fed, with Z-rot controls)
        // republish a non-identity transform below; every other path (text,
        // calib, ILDA, DMX, curves) inherits identity so a stale rotation from
        // a previous preset frame can never leak in.
        { LOCK_STATE(); optimizer::gLiveTransform = optimizer::AffineTransform(); }

        // ---- ILDA SD-card mode (highest priority) ----
        if (ilda::gILDA.active && ilda::hasNewFrame()) {
            size_t n = ilda::getFrame(s_frame, PATTERN_POINTS_MAX);
            if (n > 0) {
                applyCalibration(s_frame, n);
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

        // ---- Paint-by-Finger Mode ----
        if (gPaint.active) {
            // 3-axis rotation -- identical engine to Preset Live-Controls
            // (shared gLivePreset.rot_* state/angle accumulation, same
            // WebUI-reset race guard: snapshot + advance under mtx::state,
            // trig from local copies only). Z (affine) is published into
            // optimizer::gLiveTransform BEFORE generate(); Y/X perspective
            // tilt stays a post-optimizer point pass using the snapshots.
            bool  rotZActive, rotYActive, rotXActive;
            float rotZAngle, rotYAngle, rotXAngle;
            { LOCK_STATE();
                rotZActive = gLivePreset.rot_z;
                if (rotZActive) gLivePreset.rot_angle_z += gLivePreset.rot_speed_z;
                rotZAngle = gLivePreset.rot_angle_z;

                rotYActive = gLivePreset.rot_y;
                if (rotYActive) gLivePreset.rot_angle_y += gLivePreset.rot_speed_y;
                rotYAngle = gLivePreset.rot_angle_y;

                rotXActive = gLivePreset.rot_x;
                if (rotXActive) gLivePreset.rot_angle_x += gLivePreset.rot_speed_x;
                rotXAngle = gLivePreset.rot_angle_x;
            }
            { LOCK_STATE(); optimizer::gLiveTransform =
                  optimizer::makeTransform(rotZActive ? rotZAngle : 0.f, 0.f, 0.f); }

            size_t n = paint::generate(s_frame, PATTERN_POINTS_MAX);
            if (n == 0) { static LaserPoint blank_pt={0,0,0,0,0,1}; galvo::pushFrame(&blank_pt,1); vTaskDelay(pdMS_TO_TICKS(40)); continue; }  // guard: empty canvas

            // Color animation (same engine as Preset/Curve Live-Controls).
            // col_anim_type==OFF && !col_override leaves each stroke's own
            // picked color untouched (multi-color canvas); Segment mode
            // overrides all points with the cycling gradient.
            applyColorAnim(n);

            if (rotYActive) {
                float cy = cosf(rotYAngle);
                float sy2 = sinf(rotYAngle);
                for (size_t i=0;i<n;i++){
                    float z3 = s_frame[i].x * sy2;
                    float nx = s_frame[i].x * cy;
                    float d  = 1.f + z3 * 0.35f / 32767.f;
                    if(d<0.1f)d=0.1f;
                    s_frame[i].x = (int16_t)(nx/d);
                    s_frame[i].y = (int16_t)(s_frame[i].y/d);
                }
            }
            if (rotXActive) {
                float cx2 = cosf(rotXAngle);
                float sx3 = sinf(rotXAngle);
                for (size_t i=0;i<n;i++){
                    float z3 = s_frame[i].y * sx3;
                    float ny = s_frame[i].y * cx2;
                    float d  = 1.f + z3 * 0.35f / 32767.f;
                    if(d<0.1f)d=0.1f;
                    s_frame[i].y = (int16_t)(ny/d);
                    s_frame[i].x = (int16_t)(s_frame[i].x/d);
                }
            }

            uint8_t dim = gState.master_dimmer.load();
            for (size_t i = 0; i < n; i++) {
                if (!s_frame[i].blank) {
                    s_frame[i].r = (s_frame[i].r * dim) >> 8;
                    s_frame[i].g = (s_frame[i].g * dim) >> 8;
                    s_frame[i].b = (s_frame[i].b * dim) >> 8;
                }
            }
            applyCalibration(s_frame, n);
            if (dim > 0) {
                { uint32_t _t0=millis(); while (!galvo::pushFrame(s_frame, n)) { if (millis()-_t0 > 500) { safety::emergencyStop(); LOG_E(logbuf::CAT_SAFETY,"Pattern engine: pushFrame timeout, emergency stop"); break; } vTaskDelay(pdMS_TO_TICKS(2)); } }
            } else {
                static LaserPoint blank_pt = {0,0,0,0,0,1};
                galvo::pushFrame(&blank_pt, 1);
            }
            phase++;
            { uint32_t drain_ms = n / (uint32_t)gProjection.galvo_kpps;
              if (drain_ms < 10) drain_ms = 10;
              vTaskDelay(pdMS_TO_TICKS(drain_ms + drain_ms / 4)); }
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

                // 3-axis rotation -- identical engine to Preset/Paint Live-Controls.
                // Previously missing entirely in Curve Mode.
                bool  rotZActive, rotYActive, rotXActive;
                float rotZAngle, rotYAngle, rotXAngle;
                { LOCK_STATE();
                    rotZActive = gLivePreset.rot_z;
                    if (rotZActive) gLivePreset.rot_angle_z += gLivePreset.rot_speed_z;
                    rotZAngle = gLivePreset.rot_angle_z;

                    rotYActive = gLivePreset.rot_y;
                    if (rotYActive) gLivePreset.rot_angle_y += gLivePreset.rot_speed_y;
                    rotYAngle = gLivePreset.rot_angle_y;

                    rotXActive = gLivePreset.rot_x;
                    if (rotXActive) gLivePreset.rot_angle_x += gLivePreset.rot_speed_x;
                    rotXAngle = gLivePreset.rot_angle_x;
                }
                if (rotZActive) {
                    float cz = cosf(rotZAngle);
                    float sz2 = sinf(rotZAngle);
                    for (size_t i=0;i<n;i++){
                        int16_t nx=(int16_t)(s_frame[i].x*cz - s_frame[i].y*sz2);
                        int16_t ny=(int16_t)(s_frame[i].x*sz2+ s_frame[i].y*cz);
                        s_frame[i].x=nx; s_frame[i].y=ny;
                    }
                }
                if (rotYActive) {
                    float cy = cosf(rotYAngle);
                    float sy2 = sinf(rotYAngle);
                    for (size_t i=0;i<n;i++){
                        float z3 = s_frame[i].x * sy2;
                        float nx = s_frame[i].x * cy;
                        float d  = 1.f + z3 * 0.35f / 32767.f;
                        if(d<0.1f)d=0.1f;
                        s_frame[i].x = (int16_t)(nx/d);
                        s_frame[i].y = (int16_t)(s_frame[i].y/d);
                    }
                }
                if (rotXActive) {
                    float cx2 = cosf(rotXAngle);
                    float sx3 = sinf(rotXAngle);
                    for (size_t i=0;i<n;i++){
                        float z3 = s_frame[i].y * sx3;
                        float ny = s_frame[i].y * cx2;
                        float d  = 1.f + z3 * 0.35f / 32767.f;
                        if(d<0.1f)d=0.1f;
                        s_frame[i].y = (int16_t)(ny/d);
                        s_frame[i].x = (int16_t)(s_frame[i].x/d);
                    }
                }

                applyKaleidoscope(n);

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
                if (dim > 0) {
                    { uint32_t _t0=millis(); while (!galvo::pushFrame(s_frame, n)) { if (millis()-_t0 > 500) { safety::emergencyStop(); LOG_E(logbuf::CAT_SAFETY,"Pattern engine: pushFrame timeout, emergency stop"); break; } vTaskDelay(pdMS_TO_TICKS(2)); } }
                } else {
                    static LaserPoint blank_pt = {0,0,0,0,0,1};
                    galvo::pushFrame(&blank_pt, 1);
                }
                phase++;
                // Dynamic delay matching actual drain time (was fixed 35ms,
                // artificially capping fps -- and thus animation speed -- for
                // every curve shorter than the worst case/Butterfly).
                { uint32_t drain_ms = n / (uint32_t)gProjection.galvo_kpps;
                  if (drain_ms < 10) drain_ms = 10;
                  vTaskDelay(pdMS_TO_TICKS(drain_ms + drain_ms / 4)); }
                continue;
            }
        }

        // ---- Preset Mode (overrride DMX) ----
        if (s_preset_idx >= 0) {
            uint8_t speed   = gLivePreset.speed;
            float   scaleFrac;
            uint8_t sz      = computeAutoScaleSize(gLivePreset.size_val, &scaleFrac);

            // Additional Rotation from Live-Controls (Phase 3).
            // rot_angle_x/y/z: read-modify-write here (Core 1) races WebUI
            // resets (Core 0, /api/preset-live) -> snapshot + advance under
            // mtx::state, then compute trig from local copies only.
            //
            // The in-plane Z rotation is affine, so it is published into
            // optimizer::gLiveTransform BEFORE generate() runs -- the
            // optimizer then samples the already-rotated geometry (corner
            // detection / resampling see true edge angles). Y/X perspective
            // tilt is NOT affine and stays a post-optimizer point pass below,
            // reusing the rotYAngle/rotXAngle snapshotted here (accumulated
            // exactly once per frame).
            bool  rotZActive, rotYActive, rotXActive;
            float rotZAngle, rotYAngle, rotXAngle, rotationDeg;
            { LOCK_STATE();
                rotZActive = gLivePreset.rot_z;
                if (rotZActive) gLivePreset.rot_angle_z += gLivePreset.rot_speed_z;
                rotZAngle = gLivePreset.rot_angle_z;

                rotYActive = gLivePreset.rot_y;
                if (rotYActive) gLivePreset.rot_angle_y += gLivePreset.rot_speed_y;
                rotYAngle = gLivePreset.rot_angle_y;

                rotXActive = gLivePreset.rot_x;
                if (rotXActive) gLivePreset.rot_angle_x += gLivePreset.rot_speed_x;
                rotXAngle = gLivePreset.rot_angle_x;

                rotationDeg = gLivePreset.rotation;
            }
            // Publish the affine (Z-rot) part for the optimizer. rot_z takes
            // precedence over the static `rotation` degrees control, matching
            // the legacy if/else-if order.
            float zRad = rotZActive ? rotZAngle
                       : (fabsf(rotationDeg) > 0.5f ? rotationDeg * (float)(M_PI/180.) : 0.f);
            { LOCK_STATE(); optimizer::gLiveTransform = optimizer::makeTransform(zRad, 0.f, 0.f); }

            size_t n = presets::generate((uint8_t)s_preset_idx, s_frame,
                                         PATTERN_POINTS_MAX, phase, speed, sz);

            // Continuous collapse toward a single point through Auto-Scaling's
            // low end (both directions). ssc() floors at 0.25, so sz alone
            // jumps from a rendered shape straight to size_val==0 -- this
            // scales the whole frame by the true, unquantized progress instead.
            if (scaleFrac < 1.0f) {
                for (size_t i = 0; i < n; i++) {
                    s_frame[i].x = (int16_t)(s_frame[i].x * scaleFrac);
                    s_frame[i].y = (int16_t)(s_frame[i].y * scaleFrac);
                }
            }

            // Color override from Live-Controls
            // ── Firmware color animation engine ──────────────────────────
            // Runs per-frame on Core 0; no HTTP overhead.
            // Also DMX-controllable via CH 23-25.
            
            // Apply color animation / override
            applyColorAnim(n);

            // Z-rotation is now applied via optimizer::gLiveTransform (Phase 3),
            // published above before generate(). Only the non-affine Y/X
            // perspective tilt remains as a post-optimizer point pass, using
            // the rotYAngle/rotXAngle already snapshotted above.

            // Y-rotation (tilt left/right -- perspective X compression)
            if (rotYActive) {
                float cy = cosf(rotYAngle);
                float sy2 = sinf(rotYAngle);
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
            if (rotXActive) {
                float cx2 = cosf(rotXAngle);
                float sx3 = sinf(rotXAngle);
                for (size_t i=0;i<n;i++){
                    float z3 = s_frame[i].y * sx3;
                    float ny = s_frame[i].y * cx2;
                    float d  = 1.f + z3 * 0.35f / 32767.f;
                    if(d<0.1f)d=0.1f;
                    s_frame[i].y = (int16_t)(ny/d);
                    s_frame[i].x = (int16_t)(s_frame[i].x/d);
                }
            }

            // Mirror (separate from Kaleidoscope, see applyMirror())
            applyMirror(n);
            if (n == 0) { static LaserPoint blank_pt={0,0,0,0,0,1}; galvo::pushFrame(&blank_pt,1); vTaskDelay(pdMS_TO_TICKS(40)); continue; }  // guard: preset generated 0 points

            applyKaleidoscope(n);
            applyPointsOnlyMode(n);
            if (n == 0) { static LaserPoint blank_pt={0,0,0,0,0,1}; galvo::pushFrame(&blank_pt,1); vTaskDelay(pdMS_TO_TICKS(40)); continue; }  // guard: points mode emptied frame
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

        // always fully calculate pattern (also needed for non-preset galvo output)
        size_t n = genPattern(v, s_frame);
        uint8_t r, g, b;
        resolveColor(v.color, r, g, b);
        for (size_t i = 0; i < n; i++) { s_frame[i].r = r; s_frame[i].g = g; s_frame[i].b = b; }
        if (v.color >= 90 && v.color <= 92) applyRainbow(s_frame, n, v.color_speed, phase);
        applyTransform(s_frame, n, v, phase);
        applyCalibration(s_frame, n);
       
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