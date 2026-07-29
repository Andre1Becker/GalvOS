#pragma once
#include <stdint.h>

/**
 * duplicator.h -- Phase 3 of the Animation & Modulation System (see
 * plans/generic-roaming-dahl.md). Chains extraCopies transformed copies of
 * the final, post-optimizer LaserPoint frame -- same post-processing stage
 * as the existing Mirror/Kaleidoscope effects (pattern_engine.cpp's
 * applyMirror()/applyKaleidoscope()), generalized to an arbitrary count
 * driven by a single compounding per-copy translate+rotate+scale step
 * instead of a fixed reflection/radial-symmetry rule. Grid (offset only),
 * radial (angle only) and spiral (offset+angle+scale combined) all fall out
 * of the same accumulator -- see applyDuplicator() in pattern_engine.cpp for
 * the actual point-array math.
 *
 * Registers 5 new ModTargets (DUP_COUNT/OFFSET_X/OFFSET_Y/ANGLE/SCALE) with
 * the modulator engine's registry from duplicator::init() -- same registry-
 * extensibility proof as camera.cpp's Phase 2: zero changes to
 * modulator_engine.h/.cpp, zero changes to the WebUI.
 *
 * All 5 targets are purely additive on top of a neutral base (count=0,
 * offset=0, angle=0, scale=1) -- an idle duplicator (nothing bound) is
 * numerically identical to no duplicator at all.
 */

namespace duplicator {

namespace target_id {
    constexpr uint16_t DUP_COUNT    = 15;
    constexpr uint16_t DUP_OFFSET_X = 16;
    constexpr uint16_t DUP_OFFSET_Y = 17;
    constexpr uint16_t DUP_ANGLE    = 18;
    constexpr uint16_t DUP_SCALE    = 19;
}

// Cap on extra copies -- keeps worst-case blank-jump/decimation cost in the
// same order of magnitude as Kaleidoscope's KALEIDO_SEGMENTS_MAX.
constexpr int MAX_EXTRA_COPIES = 15;

// Registers the 5 targets above with modulator::registerModTarget(). Call
// once after modulator::init().
void init();

// Folds any bound modulation onto a neutral base. extraCopies is rounded to
// the nearest int and clamped to [0, MAX_EXTRA_COPIES]; angleRad is
// converted from the target's native degrees, matching camera::apply()'s
// convention. Cheap to call unconditionally (5x modulator::apply()
// early-outs when nothing is bound).
void apply(int& extraCopies, float& offsetX, float& offsetY, float& angleRad, float& scale);

} // namespace duplicator
