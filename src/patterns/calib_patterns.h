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
 * API: POST /api/calib-pattern {"idx": 0-10, "brightness": 200}
 *      GET  /api/calib-pattern/list
 */
#include "config.h"

namespace calib_patterns {

constexpr uint8_t CALIB_PATTERN_COUNT = 11;

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
