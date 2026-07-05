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

// ============================================================
// glyphOutlinePaths -- raw (un-optimized) glyph outline sub-paths
// ============================================================
// Used by the Paint by Finger "Text" tool (GET /api/text/vertices).
// Unlike generate(), this returns RAW pen-stroke geometry (one sub-path
// per pen-lift segment, across the whole string) with NO point-optimizer
// pass -- Paint strokes need low, editable vertex counts, not dense
// laser-ready point clouds.
struct GlyphSubpath {
    static constexpr size_t MAX_PTS = 20;   // longest glyph sub-path is 15 pts ('8')
    float   x[MAX_PTS];
    float   y[MAX_PTS];
    uint8_t count = 0;
};

// max sub-paths returned per call (buffer ceiling, independent of the
// Paint canvas' own PAINT_STROKES_MAX -- caller enforces that budget)
static constexpr size_t TEXT_VERTICES_MAX_PATHS = 32;

/**
 * Build raw glyph outline sub-paths for `text`, world-unit scaled.
 * @param text      input string; chars not covered by GLYPHS are skipped
 *                  (cursor still advances), same behaviour as renderTextString
 * @param scale     glyph-unit -> world-unit scale factor
 * @param out       output buffer of sub-paths
 * @param max_paths capacity of out[] (see TEXT_VERTICES_MAX_PATHS)
 * @return          number of sub-paths written (0 if text empty/unsupported)
 */
size_t glyphOutlinePaths(const char* text, float scale,
                         GlyphSubpath* out, size_t max_paths);

} // namespace textrender
