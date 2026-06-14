#pragma once
#include "config.h"

namespace presets {

constexpr uint8_t PRESET_COUNT = 101;

struct PresetInfo { const char* name; const char* category; };
extern const PresetInfo PRESETS[PRESET_COUNT];

size_t generate(uint8_t idx, LaserPoint* out, size_t max_pts,
                uint32_t phase, uint8_t speed, uint8_t size_val);

} // namespace presets

// Countdown Timer API — see countdown_timer.h
#include "countdown_timer.h"
