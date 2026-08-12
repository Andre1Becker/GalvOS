#pragma once
#include "config.h"

/**
 * brightnessField.h -- Per-Segment Brightness Compensation
 *
 * Scan speed on the projection surface varies with throw distance and
 * angle, so exposure per unit path length (and therefore apparent
 * brightness) varies across the image even for identical RGB values.
 * gBrightness (see config.h) is an N x N gain grid, in the SAME normalized
 * [-1..1] space and bilinear-interpolated the same way as the warp grid
 * (reuses warp::sampleGrid(), see warpGrid.h -- deliberately not duplicated
 * here) -- but it is otherwise unrelated: it scales OUTPUT color, warp
 * moves geometry.
 *
 * Pipeline position: applied AFTER Blanking, BEFORE Velocity/Acceleration
 * Clamp (point_optimizer.cpp's optimize(), between emitAllSegments() and
 * clampScannerLimits()) -- so blanked points are skipped (blank stays
 * blank) and the clamp's own inserted interpolated points inherit already-
 * gain-corrected RGB instead of being missed by a stage that ran after it.
 *
 * IMPORTANT: this is strictly an OUTPUT-stage scale. It must never be
 * written back into a pattern's own color definition and must not be
 * confused with col_override -- pattern channel defaults stay 0 or 255 per
 * the project's color convention; brightness compensation only ever
 * multiplies the final emitted RGB.
 */
namespace brightness {

// Call once at boot, after gBrightness has been loaded from NVS (see
// web_ui.cpp::loadBrightness()). Primes the identity fast-path cache.
void init();

// Resets gBrightness's gain grid to identity (255 everywhere). Does not
// touch gBrightness.enabled.
void reset();

// Recomputes the identity fast-path cache. Call after gBrightness.enabled,
// gBrightness.gridSize, or gBrightness.gain[][] is mutated from outside
// this module (e.g. the REST API handlers in web_ui.cpp).
void refresh();

// True when applying the gain field would be a no-op (disabled, or every
// cell is 255) -- callers should skip the per-point pass entirely.
bool isIdentity();

// Bilinearly-interpolated gain (0..255) at (x,y), native galvo-unit space
// (±32767, same as LaserPoint.x/y -- same space warp::apply() operates in).
// Returns 255 (no attenuation) when isIdentity().
uint8_t gain(float x, float y);

} // namespace brightness
