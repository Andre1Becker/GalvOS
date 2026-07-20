// curve_patterns.cpp -- Mathematical curve animations for laser galvo
// All coordinates normalized to ±28000 (safe margin below ±32767)
// Each curve: p[0..2] = shape params, p[3] = speed, p[4] = zoom

#include "curve_patterns.h"
#include "../util/mem_registry.h"
#include <math.h>
#include <string.h>
#include <algorithm>
#include <Arduino.h>
#include <esp_heap_caps.h>

namespace curves {

// Shared two-pass scratch, PSRAM. Several curve generators compute an (x,y)
// array first, find its max radius, then normalise and emit -- they used to
// keep this in per-function `static float xs[N], ys[N]` arrays living in
// internal DRAM permanently (the 1025-element pair alone was ~8 KB, and a
// second function held a 513 pair). Since all curve rendering runs in
// patterns::task (Core 1, millisecond budget) and never in the galvo ISR, the
// scratch can live in PSRAM. One shared pair, sized for the largest user
// (1025), is lazily allocated on first use. No static internal fallback: such
// an array would occupy the DRAM permanently and defeat the whole point; if
// PSRAM is genuinely unavailable the affected curve simply renders nothing
// (returns 0) rather than costing 8 KB of the heap we are trying to free.
static constexpr size_t CURVE_SCRATCH_N = 1025;
static float* s_curveXs = nullptr;
static float* s_curveYs = nullptr;

// On success sets xs/ys to CURVE_SCRATCH_N-element PSRAM buffers and returns
// true. Returns false if PSRAM allocation fails -- caller must bail out.
static bool curveScratch(float*& xs, float*& ys) {
    if (!s_curveXs || !s_curveYs) {
        if (!s_curveXs) s_curveXs = (float*)ps_malloc(CURVE_SCRATCH_N * sizeof(float));
        if (!s_curveYs) s_curveYs = (float*)ps_malloc(CURVE_SCRATCH_N * sizeof(float));
        if (!s_curveXs || !s_curveYs) return false;
        memreg::track("Curve Scratch Buffers", 2 * CURVE_SCRATCH_N * sizeof(float), true);
    }
    xs = s_curveXs; ys = s_curveYs;
    return true;
}

// Use CURVE_PI to avoid conflict with Arduino.h #define CURVE_PI
static constexpr float CURVE_PI  = 3.14159265358979323846f;
static constexpr float TAU       = 2.0f * CURVE_PI;
static constexpr float SC   = 28000.0f;

// ── Helpers ──────────────────────────────────────────────────────────────────

static inline float clamp01(float v) { return v < 0.f ? 0.f : v > 1.f ? 1.f : v; }
static inline float lerp(float a, float b, float t) { return a + (b - a) * t; }

static inline LaserPoint pt(float x, float y, uint8_t r, uint8_t g, uint8_t b,
                             float zoom = 1.f) {
    int16_t xi = (int16_t)std::max(-32760.f, std::min(32760.f, x * SC * zoom));
    int16_t yi = (int16_t)std::max(-32760.f, std::min(32760.f, y * SC * zoom));
    return LaserPoint(xi, yi, r, g, b, 0);
}

static inline LaserPoint blankPt(float x, float y, float zoom = 1.f) {
    int16_t xi = (int16_t)std::max(-32760.f, std::min(32760.f, x * SC * zoom));
    int16_t yi = (int16_t)std::max(-32760.f, std::min(32760.f, y * SC * zoom));
    return LaserPoint(xi, yi, 0, 0, 0, 1);
}

// Rainbow color from hue 0..1
static void hue2rgb(float h, uint8_t& r, uint8_t& g, uint8_t& b,
                    uint8_t base_r, uint8_t base_g, uint8_t base_b, float mix) {
    h = h - floorf(h);
    float frac = h * 6.f;
    int   seg  = (int)frac % 6;
    float ff   = frac - (int)frac;
    float rv,gv,bv;
    switch (seg) {
        case 0: rv=1;    gv=ff;   bv=0;    break;
        case 1: rv=1-ff; gv=1;    bv=0;    break;
        case 2: rv=0;    gv=1;    bv=ff;   break;
        case 3: rv=0;    gv=1-ff; bv=1;    break;
        case 4: rv=ff;   gv=0;    bv=1;    break;
        default:rv=1;    gv=0;    bv=1-ff; break;
    }
    float m = clamp01(mix);
    r = (uint8_t)((base_r * (1.f-m) + rv*255.f * m));
    g = (uint8_t)((base_g * (1.f-m) + gv*255.f * m));
    b = (uint8_t)((base_b * (1.f-m) + bv*255.f * m));
}

// ── Curve Definitions ────────────────────────────────────────────────────────

const CurveDef CURVE_DEFS[CURVE_COUNT] = {

    // 0 EPITROCHOID
    {"Epitrochoid",
     "Circle rolling outside another circle. Adjust ratio for petals.",
     {{"Ratio R/r",    1.0f, 8.0f,  3.0f, 0.1f},
      {"Offset d",     0.0f, 1.5f,  0.8f, 0.05f},
      {"Color Phase",  0.0f, 1.0f,  0.0f, 0.01f},
      {"Speed",        0.1f, 4.0f,  1.0f, 0.1f},
      {"Zoom",         0.2f, 1.2f,  0.8f, 0.05f}},
     0, 180, 255},  // cyan-blue

    // 1 TALBOT
    {"Talbot",
     "Ellipse deformation with cusps. Adjust cusp depth for dramatic shapes.",
     {{"Cusp Depth f", 0.0f, 0.95f, 0.55f, 0.01f},
      {"Axis b/a",     0.3f, 1.0f,  0.6f,  0.01f},
      {"Rotation",     0.0f, 6.28f, 0.0f,  0.05f},
      {"Speed",        0.1f, 4.0f,  0.4f,  0.1f},
      {"Zoom",         0.2f, 1.2f,  0.85f, 0.05f}},
     255, 0, 180},  // magenta

    // 2 HARMONOGRAPH
    {"Harmonograph",
     "Dual pendulum figure. Frequency ratio controls shape complexity.",
     {{"Freq Ratio",   1.0f, 4.0f,  2.0f, 0.01f},
      {"Phase",        0.0f, 6.28f, 0.78f, 0.05f},
      {"Amplitude Mix",0.1f, 0.9f,  0.5f,  0.01f},
      {"Speed",        0.1f, 4.0f,  0.6f,  0.1f},
      {"Zoom",         0.2f, 1.2f,  0.85f, 0.05f}},
     0, 255, 80},   // green

    // 3 PHYLLOTAXIS
    {"Phyllotaxis",
     "Sunflower spiral. Golden angle ~137.5 deg gives natural packing.",
     {{"Angle deg",    90.f, 180.f, 137.5f, 0.1f},
      {"Spread",       0.3f, 2.0f,  1.0f,   0.05f},
      {"Density",      0.2f, 1.0f,  0.7f,   0.01f},
      {"Speed",        0.1f, 4.0f,  0.5f,   0.1f},
      {"Zoom",         0.2f, 1.2f,  0.85f,  0.05f}},
     255, 180, 0},  // golden

    // 4 TREFOIL
    {"Trefoil",
     "Three-lobed curve. Scale controls lobe prominence.",
     {{"Harmonic Scale",0.5f, 3.0f, 2.0f, 0.05f},
      {"Inner Ratio",   0.3f, 2.0f, 1.0f, 0.05f},
      {"Twist",         0.0f, 1.0f, 0.0f, 0.01f},
      {"Speed",         0.1f, 4.0f, 0.7f, 0.1f},
      {"Zoom",          0.2f, 1.2f, 0.7f, 0.05f}},
     150, 0, 255},  // purple

    // 5 SUPERFORMULA
    {"Superformula",
     "Gielis superformula. m sets symmetry, n1/n2 control roundness.",
     {{"Symmetry m",   2.0f, 12.0f, 5.0f, 0.5f},
      {"Roundness n1", 0.2f, 8.0f,  1.0f, 0.1f},
      {"Sharpness n2", 0.2f, 8.0f,  1.5f, 0.1f},
      {"Speed",        0.1f, 4.0f,  0.3f, 0.1f},
      {"Zoom",         0.2f, 1.2f,  0.85f,0.05f}},
     255, 40, 0},   // red-orange

    // 6 BUTTERFLY
    {"Butterfly",
     "Temple H. Fay butterfly curve. Exp and cos-freq reshape the wings.",
     {{"Exp Scale",    0.5f, 2.0f,  1.0f, 0.05f},
      {"Cos Freq",     2.0f, 7.0f,  4.0f, 0.5f},
      {"Color Phase",  0.0f, 1.0f,  0.0f, 0.01f},
      {"Speed",        0.1f, 3.0f,  0.4f, 0.1f},
      {"Zoom",         0.1f, 0.8f,  0.25f,0.05f}},
     255, 120, 0},  // orange

    // 7 ASTROID
    {"Astroid",
     "Hypocycloid family. Exponent 3 = classic astroid, higher = star.",
     {{"Exponent n",   1.5f, 6.0f,  3.0f, 0.1f},
      {"Aspect a/b",   0.5f, 2.0f,  1.0f, 0.05f},
      {"Rotation",     0.0f, 6.28f, 0.0f, 0.05f},
      {"Speed",        0.1f, 4.0f,  0.5f, 0.1f},
      {"Zoom",         0.2f, 1.2f,  0.9f, 0.05f}},
     0, 220, 255},  // sky blue

    // 8 DELTOID
    {"Deltoid",
     "Three-cusped hypocycloid. k-ratio morphs between oval and star.",
     {{"Harmonic k",   0.5f, 2.5f,  1.0f, 0.05f},
      {"Offset d",     0.0f, 1.5f,  1.0f, 0.05f},
      {"Color Phase",  0.0f, 1.0f,  0.0f, 0.01f},
      {"Speed",        0.1f, 4.0f,  0.8f, 0.1f},
      {"Zoom",         0.2f, 1.2f,  0.85f,0.05f}},
     120, 255, 0},  // lime
};

// ── Default Init ─────────────────────────────────────────────────────────────

void initDefaultParams(uint8_t idx, CurveParams& out) {
    if (idx >= CURVE_COUNT) return;
    const CurveDef& d = CURVE_DEFS[idx];
    for (int i = 0; i < 5; i++) out.p[i] = d.params[i].def_val;
    out.r = d.def_r; out.g = d.def_g; out.b = d.def_b;
}

// ── Individual Curve Generators ──────────────────────────────────────────────

// 0 — EPITROCHOID
// x(t) = (R+r)cos(t) − d·cos((R+r)/r · t)
// y(t) = (R+r)sin(t) − d·sin((R+r)/r · t)
// Normalized so peak radius ≈ 1.
static size_t gen_epitrochoid(const CurveParams& p, uint32_t phase,
                               LaserPoint* buf, size_t max) {
    float k     = p.p[0];                    // R/r ratio (1..8)
    float d     = p.p[1];                    // offset (0..1.5)
    float cphase= p.p[2];                    // color phase
    float speed = p.p[3];
    float zoom  = p.p[4];

    float anim  = phase * speed * 0.003f;
    float norm  = 1.0f / (k + 1.0f + d + 0.001f);  // normalize to ±1

    const size_t N = 640;
    size_t n = 0;
    uint8_t r,g,b;

    for (size_t i = 0; i <= N && n < max; i++) {
        float t  = (float)i / N * TAU + anim;
        float x  = ((k + 1.f) * cosf(t) - d * cosf((k + 1.f) * t)) * norm;
        float y  = ((k + 1.f) * sinf(t) - d * sinf((k + 1.f) * t)) * norm;
        float hu = cphase + (float)i / N * 0.5f;
        hue2rgb(hu, r, g, b, p.r, p.g, p.b, cphase > 0.05f ? 1.f : 0.f);
        buf[n++] = (i == 0) ? blankPt(x, y, zoom) : pt(x, y, r, g, b, zoom);
    }
    return n;
}

// 1 — TALBOT CURVE
// x(t) = (a² + f²sin²t)cos(t) / a
// y(t) = (b² − 2f² + f²sin²t)sin(t) / b
// a = 1, b = p[1], f = p[0]
static size_t gen_talbot(const CurveParams& p, uint32_t phase,
                          LaserPoint* buf, size_t max) {
    float f     = p.p[0];                    // cusp depth (0..0.95)
    float ba    = p.p[1];                    // b/a ratio (0.3..1.0)
    float rot   = p.p[2] + phase * p.p[3] * 0.003f;
    float zoom  = p.p[4];
    float cr    = cosf(rot), sr = sinf(rot);

    float a = 1.0f, b = ba;
    float f2 = f * f;

    const size_t N = 512;
    size_t n = 0;

    for (size_t i = 0; i <= N && n < max; i++) {
        float t    = (float)i / N * TAU;
        float s2   = sinf(t) * sinf(t);
        float c2   = cosf(t) * cosf(t);
        float xr   = (a * a + f2 * s2) * cosf(t) / (a * a);
        float yr   = (b * b - 2.f * f2 + f2 * s2) * sinf(t) / (b * b);
        // normalize
        xr *= 0.8f; yr *= 0.8f;
        // rotate
        float xfin = xr * cr - yr * sr;
        float yfin = xr * sr + yr * cr;
        (void)c2;
        buf[n++] = (i == 0) ? blankPt(xfin, yfin, zoom)
                             : pt(xfin, yfin, p.r, p.g, p.b, zoom);
    }
    return n;
}

// 2 — HARMONOGRAPH
// Stable (no damping) dual-pendulum:
// x(t) = sin(t + phi) + mix·sin(k·t)
// y(t) = sin(t)       + mix·sin(k·t + pi/2)
static size_t gen_harmonograph(const CurveParams& p, uint32_t phase,
                                LaserPoint* buf, size_t max) {
    float k     = p.p[0];                    // freq ratio (1..4)
    float phi   = p.p[1];                    // phase offset
    float mix   = p.p[2];                    // amplitude mix (0.1..0.9)
    float speed = p.p[3];
    float zoom  = p.p[4];

    float anim  = phase * speed * 0.002f;
    float norm  = 1.0f / (1.0f + mix);      // normalize amplitude

    const size_t N = 768;
    size_t n = 0;
    uint8_t r,g,b;

    for (size_t i = 0; i <= N && n < max; i++) {
        float t  = (float)i / N * TAU * 2.0f + anim;  // two full periods
        float x  = (sinf(t + phi) + mix * sinf(k * t)) * norm;
        float y  = (sinf(t)       + mix * sinf(k * t + CURVE_PI * 0.5f)) * norm;
        float hu = (float)i / N;
        hue2rgb(hu, r, g, b, p.r, p.g, p.b, 0.4f);
        buf[n++] = (i == 0) ? blankPt(x, y, zoom) : pt(x, y, r, g, b, zoom);
    }
    return n;
}

// 3 — PHYLLOTAXIS (dot spiral)
// n-th dot: theta = n * angle_rad, r = sqrt(n/N) * spread
static size_t gen_phyllotaxis(const CurveParams& p, uint32_t phase,
                               LaserPoint* buf, size_t max) {
    float angle_deg = p.p[0];               // 90..180°
    float spread    = p.p[1];               // 0.3..2.0
    float density   = p.p[2];               // 0.2..1.0
    float speed     = p.p[3];
    float zoom      = p.p[4];

    float angle_rad = angle_deg * (CURVE_PI / 180.0f);
    float rot_anim  = phase * speed * 0.002f;

    // Per-dot budget: RAMP blanked travel steps + DWELL lit copies. galvo_out
    // writes samples straight to the DAC with no interpolation of its own, so
    // jumping directly to blankPt(x,y) commands an instant step -- the mirror
    // physically can't get there in one 50 kHz tick and instead slews across
    // during the following *lit* samples, painting a streak from the previous
    // dot to this one instead of a clean point (the actual "wrong spiral
    // arrangement" bug: consecutive golden-angle dots are always far apart,
    // so this fired on every single dot). RAMP interpolates the blanked move
    // over several samples so the beam has arrived before DWELL lights it.
    const int RAMP  = 4;
    const int DWELL = 3;
    const int perDot = RAMP + DWELL;
    int N = (int)(density * 400);
    if (N < 20)  N = 20;
    if (N > (int)max / perDot) N = (int)max / perDot;

    size_t n = 0;
    float px = 0.f, py = 0.f;                      // previous dot (ramp start)
    for (int i = 0; i < N && n + perDot <= max; i++) {
        float theta = i * angle_rad + rot_anim;
        // r_ in [0,1]: sqrt(i/N) already spans the unit disc, so it fills the
        // frame without the previous double /(spread+.01) division that
        // collapsed the pattern. `spread` now only shapes radial density.
        float r_ = powf((float)i / N, 0.5f * spread * 0.5f + 0.25f);
        float x  = r_ * cosf(theta);
        float y  = r_ * sinf(theta);

        uint8_t r,g,b;
        hue2rgb((float)i / N, r, g, b, p.r, p.g, p.b, 0.5f);

        for (int s = 1; s <= RAMP; s++) {
            float f = (float)s / RAMP;
            buf[n++] = blankPt(px + (x - px) * f, py + (y - py) * f, zoom);
        }
        for (int d = 0; d < DWELL; d++)
            buf[n++] = pt(x, y, r, g, b, zoom);    // hold lit dot
        px = x; py = y;
    }
    return n;
}

// 4 — TREFOIL
// x(t) = sin(t) + k·sin(2t),  y(t) = cos(t) − k·cos(2t)
// Optionally with inner radius and twist
static size_t gen_trefoil(const CurveParams& p, uint32_t phase,
                           LaserPoint* buf, size_t max) {
    float k     = p.p[0];                   // harmonic scale (0.5..3)
    float inner = p.p[1];                   // inner ratio (0.3..2.0)
    float twist = p.p[2];                   // twist (0..1)
    float speed = p.p[3];
    float zoom  = p.p[4];

    float anim  = phase * speed * 0.003f;
    // Normalization: peak ≈ 1 + k
    float norm  = 1.0f / (1.0f + k + 0.001f) * inner;

    const size_t N = 576;
    size_t n = 0;
    uint8_t r,g,b;

    for (size_t i = 0; i <= N && n < max; i++) {
        float t  = (float)i / N * TAU + anim;
        float tw = twist * sinf(3.f * t) * 0.2f;
        float x  = (sinf(t + tw) + k * sinf(2.f * t)) * norm;
        float y  = (cosf(t + tw) - k * cosf(2.f * t)) * norm;
        float hu = (float)i / N;
        hue2rgb(hu, r, g, b, p.r, p.g, p.b, twist > 0.05f ? 0.6f : 0.f);
        buf[n++] = (i == 0) ? blankPt(x, y, zoom) : pt(x, y, r, g, b, zoom);
    }
    return n;
}

// 5 — SUPERFORMULA (Gielis)
// r(θ) = [ |cos(m·θ/4)/a|^n2 + |sin(m·θ/4)/b|^n3 ]^(-1/n1)
// a=b=1, n3=n2 (simplified)
static size_t gen_superformula(const CurveParams& p, uint32_t phase,
                                LaserPoint* buf, size_t max) {
    float m     = p.p[0];                   // symmetry 2..12
    float n1    = p.p[1];                   // roundness 0.2..8
    float n2    = p.p[2];                   // sharpness 0.2..8
    float speed = p.p[3];
    float zoom  = p.p[4];

    float anim  = phase * speed * 0.002f;

    const size_t N = 512;
    size_t n = 0;
    float max_r = 0.001f;

    // Two-pass: compute then normalize. Scratch in shared PSRAM buffer.
    float *xs, *ys;
    if (!curveScratch(xs, ys)) return 0;

    for (size_t i = 0; i <= N; i++) {
        float theta = (float)i / N * TAU;
        float mt4   = m * theta / 4.0f;
        float ca    = fabsf(cosf(mt4));
        float sa    = fabsf(sinf(mt4));
        float r_    = powf(powf(ca, n2) + powf(sa, n2), -1.0f / n1);
        if (!isfinite(r_)) r_ = 0.f;
        float ang   = theta + anim;
        xs[i] = r_ * cosf(ang);
        ys[i] = r_ * sinf(ang);
        if (r_ > max_r) max_r = r_;
    }

    float norm = 0.9f / max_r;
    uint8_t r,g,b;
    for (size_t i = 0; i <= N && n < max; i++) {
        float x = xs[i] * norm, y = ys[i] * norm;
        hue2rgb((float)i / N, r, g, b, p.r, p.g, p.b, 0.3f);
        buf[n++] = (i == 0) ? blankPt(x, y, zoom) : pt(x, y, r, g, b, zoom);
    }
    return n;
}

// 6 — BUTTERFLY (Temple H. Fay)
// r(t) = exp(e_scale·sin(t)) − 2·cos(freq·t) + sin^5((2t−π)/24)
static size_t gen_butterfly(const CurveParams& p, uint32_t phase,
                             LaserPoint* buf, size_t max) {
    float e_scale = p.p[0];                 // exp scale 0.5..2.0
    float freq    = roundf(p.p[1]);         // cos frequency 2..7
    float cphase  = p.p[2];
    float speed   = p.p[3];
    float zoom    = p.p[4];

    float anim    = phase * speed * 0.002f;

    const size_t N = 1024;
    size_t n = 0;
    float max_r = 0.001f;

    // Scratch in shared PSRAM buffer (sized CURVE_SCRATCH_N = 1025 = N+1).
    float *xs, *ys;
    if (!curveScratch(xs, ys)) return 0;

    for (size_t i = 0; i <= N; i++) {
        float t   = (float)i / N * TAU * 2.0f;  // two full loops
        float sv  = (2.f * t - CURVE_PI) / 24.f;
        float sv5 = sv * sv * sv * sv * sv;
        float r_  = expf(e_scale * sinf(t)) - 2.f * cosf(freq * t) + sv5;
        float ang  = t + anim;
        xs[i] = r_ * cosf(ang);
        ys[i] = r_ * sinf(ang);
        if (fabsf(r_) > max_r) max_r = fabsf(r_);
    }

    float norm = 0.9f / max_r;
    uint8_t r,g,b;
    for (size_t i = 0; i <= N && n < max; i++) {
        float x = xs[i] * norm, y = ys[i] * norm;
        float hu = cphase + (float)i / N * 0.7f;
        hue2rgb(hu, r, g, b, p.r, p.g, p.b, cphase > 0.05f ? 0.7f : 0.2f);
        buf[n++] = (i == 0) ? blankPt(x, y, zoom) : pt(x, y, r, g, b, zoom);
    }
    return n;
}

// 7 — ASTROID (generalized squircle/hypocycloid family)
// x(t) = a · |cos(t)|^(2/n) · sign(cos(t))
// y(t) = b · |sin(t)|^(2/n) · sign(sin(t))
// n=3 → astroid, n=4 → more rounded, n→∞ → square
static size_t gen_astroid(const CurveParams& p, uint32_t phase,
                           LaserPoint* buf, size_t max) {
    float n_exp = p.p[0];                   // exponent 1.5..6
    float ab    = p.p[1];                   // aspect a/b 0.5..2
    float rot   = p.p[2] + phase * p.p[3] * 0.003f;
    float zoom  = p.p[4];
    float cr    = cosf(rot), sr = sinf(rot);
    float exp_  = 2.0f / n_exp;

    const size_t N = 512;
    size_t n = 0;
    uint8_t r,g,b;

    for (size_t i = 0; i <= N && n < max; i++) {
        float t   = (float)i / N * TAU;
        float ct  = cosf(t), st = sinf(t);
        float x   = powf(fabsf(ct), exp_) * (ct < 0 ? -1.f : 1.f);
        float y   = powf(fabsf(st), exp_) * (st < 0 ? -1.f : 1.f) / ab;
        float xr  = x * cr - y * sr;
        float yr  = x * sr + y * cr;
        hue2rgb((float)i / N * 0.5f, r, g, b, p.r, p.g, p.b, 0.15f);
        buf[n++] = (i == 0) ? blankPt(xr, yr, zoom) : pt(xr, yr, r, g, b, zoom);
    }
    return n;
}

// 8 — DELTOID (generalized: R=3r hypocycloid, with k and d params)
// x(t) = 2r·cos(t) + d·r·cos(k·t)  →  normalized
// y(t) = 2r·sin(t) − d·r·sin(k·t)
// Classic deltoid: k=2, d=1
static size_t gen_deltoid(const CurveParams& p, uint32_t phase,
                           LaserPoint* buf, size_t max) {
    float k      = p.p[0];                  // harmonic 0.5..2.5
    float d      = p.p[1];                  // offset 0..1.5
    float cphase = p.p[2];
    float speed  = p.p[3];
    float zoom   = p.p[4];

    float anim   = phase * speed * 0.003f;
    float norm   = 1.0f / (2.0f + d + 0.001f);

    const size_t N = 576;
    size_t n = 0;
    uint8_t r,g,b;

    for (size_t i = 0; i <= N && n < max; i++) {
        float t  = (float)i / N * TAU + anim;
        float x  = (2.f * cosf(t) + d * cosf(k * t)) * norm;
        float y  = (2.f * sinf(t) - d * sinf(k * t)) * norm;
        float hu = cphase + (float)i / N * 0.4f;
        hue2rgb(hu, r, g, b, p.r, p.g, p.b, cphase > 0.05f ? 0.6f : 0.f);
        buf[n++] = (i == 0) ? blankPt(x, y, zoom) : pt(x, y, r, g, b, zoom);
    }
    return n;
}

// ── Dispatcher ───────────────────────────────────────────────────────────────

size_t generate(CurveType type, const CurveParams& params,
                uint32_t phase, LaserPoint* buf, size_t max_pts) {
    if (!buf || max_pts < 4) return 0;
    switch (type) {
        case EPITROCHOID:  return gen_epitrochoid (params, phase, buf, max_pts);
        case TALBOT:       return gen_talbot       (params, phase, buf, max_pts);
        case HARMONOGRAPH: return gen_harmonograph (params, phase, buf, max_pts);
        case PHYLLOTAXIS:  return gen_phyllotaxis  (params, phase, buf, max_pts);
        case TREFOIL:      return gen_trefoil      (params, phase, buf, max_pts);
        case SUPERFORMULA: return gen_superformula (params, phase, buf, max_pts);
        case BUTTERFLY:    return gen_butterfly    (params, phase, buf, max_pts);
        case ASTROID:      return gen_astroid      (params, phase, buf, max_pts);
        case DELTOID:      return gen_deltoid      (params, phase, buf, max_pts);
        default: return 0;
    }
}

} // namespace curves
