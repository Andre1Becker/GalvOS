#include "inverseFilter.h"
#include <algorithm>
#include <math.h>

// Defined here (not main.cpp) -- same self-containment reasoning as gWarp
// (warpGrid.cpp) / gBrightness (brightnessField.cpp): keeps this
// translation unit linkable directly by the native/host optimizer test
// build without pulling in the rest of the firmware.
InverseFilterConfig gInverseFilter;

namespace invfilter {

namespace {

static constexpr float PI_F  = 3.14159265358979323846f;
static constexpr float TAU_F = 2.0f * PI_F;

// Direct Form II Transposed biquad: y[n] = b0*x[n] + w1;
// w1' = b1*x[n] - a1*y[n] + w2; w2' = b2*x[n] - a2*y[n]. a0 is always
// normalized to 1 (folded into b0/b1/b2/a1/a2 at design time).
//
// Coefficients AND running state are double, not float: this filter is
// intentionally usable at low wn relative to Fs (e.g. a 50 Hz mechanical
// resonance at a 30 kHz point rate, a ~600:1 ratio), which puts its poles
// very close to z=1 -- (1 + a1 + a2), the value that governs DC/steady-
// state gain, is then a small residual from subtracting numbers near 2.
// That is a textbook poorly-conditioned case for Direct-Form biquads in
// float32: measured empirically (see the DC-gain-unity test) to produce a
// >0.1% steady-state error at these ratios purely from per-step float32
// rounding, despite the coefficient DESIGN math already being exact in
// double. This runs once per emitted point (pattern_engine.cpp's frame
// loop, not the SPI-tick galvo output ISR), so the extra cost of
// software-emulated double on the S3's single-precision-only FPU is not a
// real-time concern -- correctness wins here per project priority.
struct Biquad {
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
    double w1 = 0.0, w2 = 0.0;
    bool   active = false;

    inline float step(float xIn) {
        if (!active) return xIn;
        double x = (double)xIn;
        double y = b0 * x + w1;
        w1 = b1 * x - a1 * y + w2;
        w2 = b2 * x - a2 * y;
        return (float)y;
    }
    inline void clearState() { w1 = 0.0; w2 = 0.0; }
};

Biquad s_bqX;
Biquad s_bqY;

// Designs one axis's biquad from its measured (wnHz, zeta), the shared
// regAlpha regularization, and the sample rate. wnHz <= 0 or fs <= 0 ->
// axis left inactive (pass-through). See inverseFilter.h's header comment
// for the model/discretization derivation.
//
// Runs entirely in double: the bilinear-transform intermediates (Kb0 etc.)
// sum a K^2 term (K=2*Fs, e.g. ~3.6e9 at 30 kpps) against a wn^2 term many
// orders of magnitude smaller -- in float32 that addition silently loses
// almost all of the smaller term's precision (7 significant decimal digits
// total, already spent on the K^2 term), producing a measurable DC-gain
// error despite the exact analytical result being 1. Only the final
// normalized coefficients are cast down to float for per-sample use.
void designAxis(Biquad& bq, const InverseFilterAxisModel& m, float regAlpha, float fs) {
    if (m.wnHz <= 0.0f || fs <= 0.0f) {
        bq.active = false;
        bq.clearState();
        return;
    }

    double wnHz  = (double)m.wnHz;
    double wn    = (double)TAU_F * wnHz;                    // rad/s
    double zeta  = std::max(0.0, std::min(0.9, (double)m.zeta));
    double fsD   = (double)fs;
    double nyquistHz = fsD * 0.5;

    // Regularization floor: the rolloff pole sits at wnHz/alpha (Hz). If
    // that lands at or above Nyquist, the pole is invisible to this sample
    // rate -- the continuous-domain BIBO-stability guarantee (poles always
    // left-half-plane, see the header comment) still holds, but the
    // DISCRETE filter's peak gain within the representable band is no
    // longer bounded the way the derivation assumes, since the rolloff
    // that's supposed to tame it never actually engages before Nyquist.
    // Floor alpha so the rolloff stays at least 2.5x below Nyquist
    // regardless of what regAlpha was configured to.
    double alphaNyquistFloor = (nyquistHz > 1.0) ? (wnHz / (0.4 * nyquistHz)) : 0.05;
    double alpha = std::max({0.05, (double)regAlpha, alphaNyquistFloor});

    // Frequency-prewarp wn so the bilinear transform's frequency warping
    // doesn't shift the digital filter's resonance/rolloff away from the
    // physically measured frequency.
    double K   = 2.0 * fsD;
    double wnw = K * tan(std::min(wn, 0.99 * K) / K);        // clamp arg below Nyquist*pi/2
                                                               // so tan() can't blow up for a
                                                               // pathological wn near/above Fs

    // Continuous-time H_inv(s) = (b2 s^2 + b1 s + b0) / (a2 s^2 + a1 s + a0),
    // H_inv(s) = (s^2 + 2*zeta*wn*s + wn^2) / (wn^2 * (1 + alpha*s/wn)^2).
    double b2 = 1.0,         b1 = 2.0 * zeta * wnw,  b0 = wnw * wnw;
    double a2 = alpha*alpha, a1 = 2.0 * alpha * wnw, a0 = wnw * wnw;

    // Standard bilinear transform of a biquad (s = K*(1-z^-1)/(1+z^-1)).
    double Kb0 = b0 + b1 * K + b2 * K * K;
    double Kb1 = 2.0 * (b0 - b2 * K * K);
    double Kb2 = b0 - b1 * K + b2 * K * K;

    double Ka0 = a0 + a1 * K + a2 * K * K;
    double Ka1 = 2.0 * (a0 - a2 * K * K);
    double Ka2 = a0 - a1 * K + a2 * K * K;

    // Ka0 > 0 always for this design (both continuous poles at -wn/alpha,
    // strictly left-half-plane for alpha>0/wn>0 -> bilinear transform maps
    // them strictly inside the unit circle) -- guarded anyway rather than
    // trusting the derivation blindly on safety-critical output.
    if (Ka0 <= 0.0) {
        bq.active = false;
        bq.clearState();
        return;
    }

    bq.b0 = Kb0 / Ka0;
    bq.b1 = Kb1 / Ka0;
    bq.b2 = Kb2 / Ka0;
    bq.a1 = Ka1 / Ka0;
    bq.a2 = Ka2 / Ka0;
    bq.active = true;
    bq.clearState();
}

} // namespace

void init(uint32_t sampleRateHz) {
    refresh(sampleRateHz);
}

void reset() {
    gInverseFilter.x = InverseFilterAxisModel();
    gInverseFilter.y = InverseFilterAxisModel();
    s_bqX.active = false;
    s_bqY.active = false;
    s_bqX.clearState();
    s_bqY.clearState();
}

void refresh(uint32_t sampleRateHz) {
    designAxis(s_bqX, gInverseFilter.x, gInverseFilter.regAlpha, (float)sampleRateHz);
    designAxis(s_bqY, gInverseFilter.y, gInverseFilter.regAlpha, (float)sampleRateHz);
}

bool isActive() {
    return gInverseFilter.enabled && (s_bqX.active || s_bqY.active);
}

void apply(float& x, float& y) {
    if (!gInverseFilter.enabled) return;
    x = s_bqX.step(x);
    y = s_bqY.step(y);
}

void resetState() {
    s_bqX.clearState();
    s_bqY.clearState();
}

} // namespace invfilter
