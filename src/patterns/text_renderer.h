#pragma once
/**
 * text_renderer.h -- vector text renderer for the laser
 *
 * Single-Stroke and Outline-Font (Hershey-inspiriert).
 * All letters A-Z, 0-9, space, .,:!?-+
 *
 * Animationen: Static, Scroll L/R, Bounce, Typewriter,
 *              Wave, Pulse, Rotate, Zoom
 */
#include "config.h"
#include <stddef.h>

namespace textrender {

// Maximale Ausgabe-Punkte
static constexpr size_t TEXT_MAX_PTS = 1024;

/**
 * Text-Frame erzeugen.
 * @param out      Ausgabe-Buffer
 * @param max_pts  Buffer-size
 * @param cfg      TextConfig (text, font, anim, ...)
 * @param phase    animation phase (incremented by the task)
 * @return         Count Punkte
 */
size_t generate(LaserPoint* out, size_t max_pts,
                const TextConfig& cfg, uint32_t phase);

} // namespace textrender
