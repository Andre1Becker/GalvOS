#pragma once
#include <stdint.h>

/**
 * spatial_noise.h -- Phase 4 (part 1) of the Animation & Modulation System
 * (see plans/generic-roaming-dahl.md). Registers a new ModType, NOISE2D, via
 * modulator::registerModType() -- the first exercise of the ModType half of
 * the registry (Phase 1 built registerModType(), but Phase 2/3's Camera and
 * Duplicator only ever exercised registerModTarget()).
 *
 * NOISE2D still ticks to a single [-1..1] scalar per frame like every other
 * ModType (Oscillator/Noise/Envelope/Sequencer) -- it is NOT a per-point
 * spatial displacement field. What's "2D" is the noise FUNCTION: it samples
 * a 2D value-noise lattice at (x, y) = (t, t * shapeParam), t = the same
 * BPM-synced/free-running time-phase modulator_engine.cpp's totalCycles()
 * gives every other type, instead of walking a 1D line the way
 * modulator_engine.cpp's own valueNoise() does. shapeParam (reused -- the
 * same field Oscillator's Square/Triangle shapes repurpose as duty-cycle/
 * morph) controls how fast the Y coordinate advances relative to X: 0 keeps
 * Y frozen (a single smooth 1D-like slice through the field), 1 walks the
 * field diagonally (visibly less self-similar/periodic than 1D noise -- the
 * classic benefit of sampling a higher-dimensional lattice). noiseSeed
 * offsets which region of the field a slot reads, same as 1D NOISE, so
 * multiple NOISE2D slots stay decorrelated and persisted curves are stable
 * across save/reload.
 *
 * A true per-point displacement field (each vertex in point_optimizer.cpp
 * sampling noise at its own x,y) is explicitly out of scope here -- it would
 * need modulator::apply() to take a position, which no consumer (including
 * Camera/Duplicator) uses today. Left for a future phase if ever needed; see
 * plans/generic-roaming-dahl.md's Phase 4 write-up for the tradeoff.
 */

namespace spatial_noise {

namespace type_id {
    constexpr uint8_t NOISE2D = 4;
}

// Registers the NOISE2D type with modulator::registerModType(). Call once
// after modulator::init().
void init();

} // namespace spatial_noise
