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
#include <stddef.h>

namespace paint {

/**
 * Generate the current paint canvas as an optimized point-cloud frame.
 * @param out      output buffer
 * @param max_pts  buffer size
 * @return         point count (0 if canvas is empty)
 */
size_t generate(LaserPoint* out, size_t max_pts);

} // namespace paint
