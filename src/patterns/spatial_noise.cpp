#include "spatial_noise.h"
#include "../modulator_engine.h"
#include <math.h>

namespace spatial_noise {

// Self-contained 2D value-noise hash/interpolation -- deliberately NOT
// shared with modulator_engine.cpp's 1D hashNoise()/valueNoise() (those are
// file-local statics, not exported). Duplicating ~15 lines here keeps this
// module a true "add a .cpp, register from init()" addition per the
// registry's central architectural decision (see modulator_engine.h's file
// header) -- zero new header surface on modulator_engine.h/.cpp beyond the
// totalCycles() export every future producer-type module will also want.
static inline float hash2(uint32_t seed, int32_t xi, int32_t yi) {
    uint32_t h = (uint32_t)xi * 2654435761u + (uint32_t)yi * 2246822519u + seed * 0x9E3779B1u;
    h ^= h >> 15; h *= 0x85EBCA6Bu;
    h ^= h >> 13; h *= 0xC2B2AE35u;
    h ^= h >> 16;
    return ((float)h / 4294967295.0f) * 2.0f - 1.0f;
}

static inline float smootherstep(float t) {
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

// Bilinearly-interpolated 2D value noise (same smootherstep interpolant as
// modulator_engine.cpp's 1D valueNoise(), just one axis wider).
static float valueNoise2D(uint32_t seed, float x, float y) {
    int32_t xi0 = (int32_t)floorf(x), yi0 = (int32_t)floorf(y);
    float fx = x - (float)xi0, fy = y - (float)yi0;
    float sx = smootherstep(fx), sy = smootherstep(fy);
    float n00 = hash2(seed, xi0,     yi0);
    float n10 = hash2(seed, xi0 + 1, yi0);
    float n01 = hash2(seed, xi0,     yi0 + 1);
    float n11 = hash2(seed, xi0 + 1, yi0 + 1);
    float ix0 = n00 + (n10 - n00) * sx;
    float ix1 = n01 + (n11 - n01) * sx;
    return ix0 + (ix1 - ix0) * sy;
}

static float tickNoise2D(modulator::Modulator& m, uint32_t nowMs) {
    float x = modulator::totalCycles(m, nowMs) + m.phaseOffset;
    float y = x * m.shapeParam;
    return valueNoise2D(m.noiseSeed, x, y);
}

static const modulator::ModTypeDescriptor kTypeNoise2D = {
    type_id::NOISE2D, "noise2d", "Spatial Noise", tickNoise2D, nullptr,
    /*usesShape*/ false, /*usesEnvelope*/ false, /*usesSequencer*/ false,
    /*usesShapeParam*/ true};

void init() {
    modulator::registerModType(&kTypeNoise2D);
}

} // namespace spatial_noise
