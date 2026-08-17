#pragma once
/**
 * weld_patterns.h -- Laser Welding effect
 *
 * An alternative renderer of a stroke-path source: a single bright torch
 * head travels the drawn path, trailing a fading afterglow and throwing
 * ballistic sparks. It is NOT a new pattern source -- it renders geometry
 * handed to it by whichever mode currently owns the beam, and is active only
 * while gWeld.enabled is true together with that mode's own active flag:
 *   - generate()     renders the gPaint stroke list, while gPaint.active
 *   - generateText()  renders the current Text Mode string, while
 *                      gTextConfig.active
 * Both funnel through the same arc-length path builder (weld_path.h) and the
 * same torch/afterglow/spark renderer -- see weld_patterns.cpp's shared
 * renderTorch() -- so there is exactly one Welding implementation, not one
 * per source.
 *
 * Head motion is wall-clock driven (millis() delta), so travel speed is
 * independent of the frame rate the optimizer's point budget produces. Output
 * geometry is handed to point_optimizer.h via paint::liveOptimizerConfig(),
 * exactly like the plain canvas/text render.
 */
#include "config.h"
#include <stddef.h>

namespace weld {

// Generate one animation frame of the welding effect over the gPaint canvas
// into `out` (max maxPts). Returns the point count (0 when the canvas has no
// usable path).
size_t generate(LaserPoint* out, size_t maxPts);

// Generate one animation frame of the welding effect over the current Text
// Mode string (`cfg`, i.e. gTextConfig) into `out` (max maxPts). Renders the
// string's STATIC glyph-outline layout (textrender::glyphOutlinePaths) --
// Text's own animation (Scroll/Wave/Orbit/...) is bypassed because Welding
// already supplies its own motion along the path; `cfg.animation`/`speed`
// are ignored here. Returns the point count (0 when the string is empty/
// unsupported).
size_t generateText(const TextConfig& cfg, LaserPoint* out, size_t maxPts);

// Whether the most recent generateText() call had to drop trailing glyphs
// because the string produced more sub-paths than the glyph-outline buffer
// holds (TEXT_VERTICES_MAX_PATHS, see text_renderer.h). Mirrors
// textrender::wasTruncated()'s role for the non-Welding Text renderer.
bool textWasTruncated();

// Reset head position, travel direction and all sparks (e.g. on enable or on
// Paint-mode (de)activation).
void reset();

// Start the run from the far end of the path (headPos = pathLen). One-shot:
// pathLen is only known inside the render task, so this sets a flag consumed by
// the next generate()/generateText() call right after it rebuilds the path.
// Used when switching to REVERSE so the run does not stall at position 0.
void seekEnd();

} // namespace weld
