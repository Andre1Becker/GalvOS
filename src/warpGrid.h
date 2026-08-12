#pragma once
#include "config.h"

/**
 * warpGrid.h -- Camera Closed-Loop Keystone (geometric warp correction)
 *
 * N x N control-point grid (gWarp, see config.h) mapping normalized [-1..1]
 * pattern-space coordinates onto normalized [-1..1] projected-surface
 * coordinates via bilinear interpolation between the surrounding four grid
 * cells. N=2 is exactly a 4-corner quad warp ("keystone" correction); N>2
 * adds interior control points for barrel/pincushion-style correction --
 * same bilinear code path handles both, no separate homography solver.
 *
 * Pipeline position: inserted into point_optimizer.cpp's optimize() right
 * after the Transform/Segment-Reorder stages and before Resample, so
 * corner-severity/edge-length/blank-jump-distance calculations all see the
 * already-warped geometry (see point_optimizer.cpp's pipeline-order
 * comment). Coordinate space in that stage is the pattern's native signed
 * galvo-unit range (same as LaserPoint.x/y, ±32767) -- NOT DAC codes; the
 * DAC-space calibration/outputScale/dac_limit stage in
 * pattern_engine.cpp::applyCalibration() runs later, on the already-warped
 * points.
 */
namespace warp {

// Call once at boot, after gWarp has been loaded from NVS (see
// web_ui.cpp::loadWarp()). Primes the identity fast-path cache.
void init();

// Resets gWarp's control points to the identity grid for the CURRENT
// gridSize. Does not touch gWarp.enabled.
void reset();

// Recomputes the identity fast-path cache. Call after gWarp.enabled,
// gWarp.gridSize, or gWarp.points[][][] is mutated from outside this module
// (e.g. the REST API handlers in web_ui.cpp).
void refresh();

// True when applying the warp would be a no-op (disabled, or the grid is
// numerically the identity grid) -- callers should skip the per-point pass
// entirely in that case.
bool isIdentity();

// Core transform: warps (x,y) in place, in native galvo-unit space
// (±32767). No-op when isIdentity(). Result is always clamped to the
// int16_t range -- never wraps.
void apply(float& x, float& y);

// Convenience overload for callers already holding a LaserPoint (e.g. a
// single-point calibration/debug preview). Rounds to the nearest int16.
void apply(LaserPoint& p);

} // namespace warp
