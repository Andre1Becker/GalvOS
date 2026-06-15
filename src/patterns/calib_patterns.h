#pragma once
/**
 * calib_patterns.h — calibration- and color gradient-Test-Patterns
 *
 * 6 patterns for gamma and white balance verification:
 *   0  CALIB_GAMMA_RAMP       brightness ramp black->white
 *   1  CALIB_WHITE_BALANCE    R/G/B/W horizontal lines
 *   2  CALIB_RAINBOW          color wheel spiral
 *   3  CALIB_STEP_RAMP        8 discrete brightness steps
 *   4  CALIB_CHANNEL_SEP      channel separation test (7 colors)
 *   5  CALIB_SATURATION       saturation spoke wheel
 *
 * All patterns apply gamma + white balance from gConfig,
 * so the projected image exactly matches the preview simulation.
 *
 * API: POST /api/calib-pattern {"idx": 0-5, "brightness": 200}
 *      GET  /api/calib-pattern/list
 */
#include "config.h"

namespace calib_patterns {

constexpr uint8_t CALIB_PATTERN_COUNT = 12;

struct CalibPatternInfo {
    const char* name;
    const char* desc;
    const char* what_to_check;
};

extern const CalibPatternInfo CALIB_INFO[CALIB_PATTERN_COUNT];

/**
 * Generate pattern.
 * @param idx        0-5 (CALIB_GAMMA_RAMP … CALIB_SATURATION)
 * @param out        output buffer
 * @param max_pts    buffer size
 * @param phase      animation phase (from pattern_engine)
 * @param brightness 0-255 (master brightness, default 200)
 * @param channel    0=RGB, 1=R, 2=G, 3=B (for ramp/step patterns)
 * @return           number of generated points
 */
size_t generate(uint8_t idx, LaserPoint* out, size_t max_pts,
                uint32_t phase, uint8_t brightness = 200,
                uint8_t channel = 0);

} // namespace calib_patterns
