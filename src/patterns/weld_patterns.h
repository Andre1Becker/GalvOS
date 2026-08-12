#pragma once
/**
 * weld_patterns.h -- Laser Welding effect
 *
 * An alternative renderer of the same gPaint stroke list (paint_patterns.cpp):
 * a single bright torch head travels the drawn path, trailing a fading
 * afterglow and throwing ballistic sparks. It is NOT a new pattern source --
 * it renders the Paint canvas, and is active only while gPaint.active is set
 * and gWeld.enabled is true (see config.h's WeldConfig).
 *
 * Head motion is wall-clock driven (millis() delta), so travel speed is
 * independent of the frame rate the optimizer's point budget produces. Output
 * geometry is handed to point_optimizer.h via paint::liveOptimizerConfig(),
 * exactly like the plain canvas.
 */
#include "config.h"
#include <stddef.h>

namespace weld {

// Generate one animation frame of the welding effect into `out` (max maxPts).
// Returns the point count (0 when the canvas has no usable path).
size_t generate(LaserPoint* out, size_t maxPts);

// Reset head position, travel direction and all sparks (e.g. on enable or on
// Paint-mode (de)activation).
void reset();

// Start the run from the far end of the path (headPos = pathLen). One-shot:
// pathLen is only known inside the render task, so this sets a flag consumed by
// the next generate() right after it rebuilds the path. Used when switching to
// REVERSE so the run does not stall at position 0.
void seekEnd();

} // namespace weld
