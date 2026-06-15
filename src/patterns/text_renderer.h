#pragma once
/**
 * text_renderer.h -- vector text renderer for the laser
 *
 * Single-stroke and outline font (Hershey-inspired).
 * All letters A-Z, 0-9, space, .,:!?-+
 *
 * Animations: Static, Scroll L/R, Bounce, Typewriter,
 *              Wave, Pulse, Rotate, Zoom
 */
#include "config.h"
#include <stddef.h>

namespace textrender {

// maximum output points
static constexpr size_t TEXT_MAX_PTS = 1024;

/**
 * Generate text frame.
 * @param out      output buffer
 * @param max_pts  buffer size
 * @param cfg      TextConfig (text, font, anim, ...)
 * @param phase    animation phase (incremented by the task)
 * @return         point count
 */
size_t generate(LaserPoint* out, size_t max_pts,
                const TextConfig& cfg, uint32_t phase);

} // namespace textrender
