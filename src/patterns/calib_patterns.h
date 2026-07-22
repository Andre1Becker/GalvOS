#pragma once
/**
 * calib_patterns.h -- calibration and optimizer test patterns
 *
 * 7 scanner/alignment patterns:
 *   0  BLANKING_TEST      alternating on/off arc segments
 *   1  ASPECT_RATIO       square + circle (X/Y gain match)
 *   2  ILDA_TEST          ILDA standard test pattern
 *   3  DAC_RANGE_BOX      full-range rectangle + inscribed circle
 *   4  ZONE_OUTLINE       projection zone polygon outline
 *   5  CORNER_COLOR_MAP   RGBW corner orientation dots
 *   6  THREE_CIRCLES      R/G/B circles for channel brightness match
 *
 * 4 optimizer calibration patterns -- each runs through the full
 * point_optimizer pipeline (liveOptimizerConfig) and isolates one
 * group of optimizer sliders:
 *   7  OPT_CORNER_SWEEP   corner_angle_deg / min/max_corner_pts
 *   8  OPT_DENSITY_RAMP   pts_per_1000_units / resample stage
 *   9  OPT_JUMP_RING      blank_samples / ringing_comp (Pillar 2/3)
 *  10  OPT_VEL_ACCEL      max_step_units / max_accel_units
 *
 * 6 camera-in-the-loop patterns -- geometry mirrors idealPolylines() in the
 * host-side optimizeGalvo.py so the ground truth the host rasterizes matches
 * what the projector actually draws. Selected via /api/calib-cam/start
 * (web_ui.cpp), not the plain idx-based /api/calib-pattern:
 *  11  CAM_CORNERS4       4 static dots at (+-30000,+-30000) -- homography ref
 *  12  CAM_SQUARE         square, half-size +-15000, sharp corners -- ringing/dwell
 *  13  CAM_STAR           5-point self-intersecting star, half-size -- corner dwell
 *  14  CAM_SEGMENTS       4 vertical lines, blanked jumps between -- blanking S-curve
 *  15  CAM_CIRCLE         circle radius 15000, 128 base points -- density uniformity
 *  16  CAM_SPIRAL         3-turn Archimedean spiral, r 2250->15000 -- velocity clamps
 *
 * API: POST /api/calib-pattern {"idx": 0-10, "brightness": 200}
 *      GET  /api/calib-pattern/list
 *      POST /api/calib-cam/start  {"pattern": "square"}
 *      POST /api/calib-cam/params {optimizer overrides...}
 *      POST /api/calib-cam/stop
 *      GET  /api/calib-cam/status
 */
#include "config.h"

namespace calib_patterns {

constexpr uint8_t CALIB_PATTERN_COUNT = 17;

// Camera-in-the-loop patterns occupy indices CALIB_CAM_BASE..+CALIB_CAM_COUNT-1.
constexpr uint8_t CALIB_CAM_BASE  = 11;
constexpr uint8_t CALIB_CAM_COUNT = 6;

// corners4 dwell length: fixed regardless of the live optimizer overrides a
// host tuning run may be sweeping (e.g. corner_angle_deg/max_corner_pts could
// be tuned down to near-zero dwell), because corners4 is the homography
// reference and must stay camera-visible under every parameter combination.
constexpr uint8_t CALIB_CAM_DOT_DWELL_PTS = 50;

// Maps a calib-cam pattern name ("corners4","square","star","segments",
// "circle","spiral") to its calib_patterns:: index (11..16), or -1 if the
// name is not recognized.
int8_t camPatternIndex(const char* name);

// Inverse of camPatternIndex(): "" if idx is not a calib-cam pattern.
const char* camPatternName(uint8_t idx);

// Optimizer profile each calibration pattern runs under. The alignment
// patterns (0-6) are plain polygons and circles, so they belong with the
// presets of the same shape; the four optimizer test patterns (7-10) are
// each built to exercise one pipeline stage and are mapped to the profile
// whose sliders they isolate. Activating a calibration pattern switches
// the active profile the same way selecting a preset does, so what you see
// on the wall is what the sliders in front of you actually control.
uint8_t profileOf(uint8_t idx);

struct CalibPatternInfo {
    const char* name;
    const char* desc;
    const char* what_to_check;
};

extern const CalibPatternInfo CALIB_INFO[CALIB_PATTERN_COUNT];

/**
 * Generate pattern.
 * @param idx        0-10
 * @param out        output buffer
 * @param max_pts    buffer size
 * @param phase      animation phase (from pattern_engine)
 * @param brightness 0-255 (master brightness, default 200)
 * @param channel    0=RGB, 1=R, 2=G, 3=B
 * @return           number of generated points
 */
size_t generate(uint8_t idx, LaserPoint* out, size_t max_pts,
                uint32_t phase, uint8_t brightness = 200,
                uint8_t channel = 0);

} // namespace calib_patterns
