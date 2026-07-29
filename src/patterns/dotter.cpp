#include "dotter.h"
#include "../modulator_engine.h"
#include <math.h>
#include <Arduino.h>

namespace dotter {

static const paramui::ParamMeta kSpreadMeta = {
    target_id::DOT_SPREAD, "dot_spread", "Dotter Spread", "Dotter", paramui::DataType::FLOAT,
    0.0f, DOT_SPREAD_MAX_UNITS, 0.0f, 1.0f, "",
    "Points-Only mode: max radius each dwelling dot is scattered from its "
    "sampled outline position (DAC units). Deterministic per dot -- a static "
    "shape's scatter pattern holds still instead of shimmering.",
    true, false, true};

static const modulator::ModTargetDescriptor kTargetSpread = {
    target_id::DOT_SPREAD, kSpreadMeta, DOT_SPREAD_MAX_UNITS, false};

void init() {
    modulator::registerModTarget(&kTargetSpread);
}

float apply() {
    float radius = modulator::apply(target_id::DOT_SPREAD, 0.0f);
    if (radius < 0.0f) radius = 0.0f;
    if (radius > DOT_SPREAD_MAX_UNITS) radius = DOT_SPREAD_MAX_UNITS;
    return radius;
}

// Same integer-hash family as spatial_noise.cpp's hash2()/point_optimizer.cpp's
// jitterHash() -- deliberately not shared, each module stays a self-contained
// "add a .cpp" addition (see modulator_engine.h's registry header comment).
static inline float dotHash01(uint32_t idx) {
    uint32_t h = idx * 2654435761u + 0x9E3779B1u;
    h ^= h >> 15; h *= 0x85EBCA6Bu;
    h ^= h >> 13; h *= 0xC2B2AE35u;
    h ^= h >> 16;
    return (float)h / 4294967295.0f;
}

void scatter(int16_t& x, int16_t& y, uint32_t dotIdx, float radiusUnits) {
    if (radiusUnits <= 0.01f) return;

    float angle = dotHash01(dotIdx * 2u)     * (float)(2.0 * M_PI);
    float dist  = dotHash01(dotIdx * 2u + 1u) * radiusUnits;

    float nx = (float)x + cosf(angle) * dist;
    float ny = (float)y + sinf(angle) * dist;
    x = (int16_t)constrain(nx, -32760.0f, 32760.0f);
    y = (int16_t)constrain(ny, -32760.0f, 32760.0f);
}

} // namespace dotter
