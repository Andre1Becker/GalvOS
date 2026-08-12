#pragma once
/**
 * paint_patterns.h -- Paint-by-Finger canvas renderer
 *
 * Converts the WebUI-composed stroke list (gPaint, config.h) into an
 * optimized LaserPoint frame via point_optimizer.h. Strokes are generic
 * vertex paths (open or closed) -- shape semantics (rect/triangle/circle/
 * line/freehand) are resolved client-side before upload.
 */
#include "config.h"
#include "point_optimizer.h"
#include <stddef.h>

namespace paint {

/**
 * Generate the current paint canvas as an optimized point-cloud frame.
 * @param out      output buffer
 * @param max_pts  buffer size
 * @return         point count (0 if canvas is empty)
 */
size_t generate(LaserPoint* out, size_t max_pts);

/**
 * Shared live->optimizer config mapping for the Paint stroke list. Paint
 * strokes are user-drawn geometry and take the live settings exactly as
 * configured (no per-family specialization). Shared so the Welding renderer
 * (weld_patterns.cpp), an alternative renderer of the same strokes, reuses it
 * instead of duplicating configFromLive() a fifth time.
 */
optimizer::OptimizerConfig liveOptimizerConfig();

} // namespace paint
