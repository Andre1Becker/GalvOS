#pragma once
#include "config.h"

/**
 * inverseFilter.h -- Model-Based Inverse Filtering (Galvo Deconvolution)
 *
 * Pillar 3 (point_optimizer.cpp's ZV shaper) input-shapes only the
 * blank-jump move, using a single SHARED ring_freq_hz/ring_damping_ratio
 * pair. This module is a different, complementary mechanism: a per-axis
 * discrete inverse of the galvo's measured 2nd-order mechanical resonance,
 * applied to EVERY emitted point (not just blank jumps) so the optical
 * trajectory tracks the commanded one instead of ringing at it.
 *
 * Model: forward dynamics assumed G(s) = wn^2 / (s^2 + 2*zeta*wn*s + wn^2).
 * The literal inverse 1/G(s) is improper (a double differentiator) and
 * would have unbounded high-frequency gain once discretized -- amplifying
 * point-to-point jitter/quantization far more than it corrects ringing.
 * Regularized instead with a matching-order rolloff so the filter stays
 * causal and BIBO-stable for any regAlpha > 0, independent of zeta:
 *
 *   H_inv(s) = (s^2 + 2*zeta*wn*s + wn^2) / (wn^2 * (1 + regAlpha*s/wn)^2)
 *
 * The denominator's poles are a fixed double root at s = -wn/regAlpha
 * (always in the left half-plane for regAlpha > 0, wn > 0) -- the
 * regularization, not the measured zeta, determines stability. That
 * guarantee is about continuous-time BIBO stability, not peak digital
 * gain: if wn/regAlpha (the rolloff pole, in Hz) sits at or above Nyquist,
 * the pole is invisible to this sample rate and no longer actually bounds
 * the filter's gain within the representable band, so the design floors
 * the EFFECTIVE alpha used per-axis (never the stored regAlpha) so the
 * rolloff stays comfortably below Nyquist regardless of what was
 * configured -- see inverseFilter.cpp's designAxis(). Discretized
 * via the bilinear (Tustin) transform at the point-output sample rate
 * (Fs = galvo_kpps * 1000 Hz, matching Pillar 3's own uniform-Delta-t
 * assumption), with wn frequency-prewarped so the digital filter's
 * resonance/rolloff lands at the physically measured frequency rather than
 * a bilinear-warped one. Result is a standard 2nd-order IIR biquad (Direct
 * Form II Transposed), one independent instance per axis.
 *
 * See docs/feature-prompts/DECISIONS.md, Prompt 12b for the full
 * derivation, insertion-point rationale, and known limitations.
 */
namespace invfilter {

// Call once at boot, after gInverseFilter has been loaded from NVS (see
// web_ui.cpp::loadInverseFilter()) and the live sample rate is known.
// sampleRateHz should be gProjection.galvo_kpps * 1000.
void init(uint32_t sampleRateHz);

// Clears both axes' models back to "unmeasured" (wnHz=0). Does not touch
// gInverseFilter.enabled or regAlpha.
void reset();

// Recomputes both axes' biquad coefficients from the current
// gInverseFilter contents and sampleRateHz. Call after gInverseFilter.x/y/
// regAlpha is mutated from outside this module (REST API handlers in
// web_ui.cpp), or after gProjection.galvo_kpps changes -- coefficient
// computation involves trig, so this must NOT be called per point.
void refresh(uint32_t sampleRateHz);

// True when applying the filter would do anything (enabled AND at least
// one axis has a measured model) -- callers should skip the per-point call
// entirely otherwise.
bool isActive();

// Filters one point's X/Y in place, in native galvo-unit space (same space
// LaserPoint.x/y and applyCalibration() operate in), maintaining running
// per-axis filter state across calls. An axis with no measured model
// (wnHz <= 0) passes through unfiltered. No-op when !gInverseFilter.enabled.
void apply(float& x, float& y);

// Clears the running filter state (NOT the model/coefficients) for both
// axes. Call on any discontinuity the filter shouldn't carry a transient
// across -- preset switch being the primary one (mirrors the precedent set
// by the v6.33.0 cross-preset seam-bridge fix: stale continuity state
// carried across a SWITCH, not just within one running preset, produced a
// spurious artifact there too).
void resetState();

} // namespace invfilter
