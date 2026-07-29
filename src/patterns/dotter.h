#pragma once
#include <stdint.h>

/**
 * dotter.h -- "Modular Module" extending pattern_engine.cpp's existing
 * Points-Only render mode (applyPointsOnlyMode(), see its header comment)
 * with modulatable spatial distribution ("scatter") of the dwelling dots.
 *
 * Points-Only mode already does the actual "pointification" (subsamples the
 * lit outline down to N dwelling dots) -- Dotter does not replace or rename
 * that, it bolts an extra stage onto it, same relationship Camera has to
 * preset_patterns.cpp's prj()/wf(). This module was originally sketched as a
 * generic "Point Distribution Modifier" living inside the optimizer (see
 * plans/generic-roaming-dahl.md's Phase 4 section); that was a scope
 * misunderstanding -- the actual ask is spatial scatter of the Points-Only
 * dots, not the continuous-stroke line optimizer, so it lives here instead.
 *
 * Registers 1 new ModTarget (DOT_SPREAD) with the modulator engine's
 * registry from dotter::init() -- same zero-changes-to-modulator_engine
 * proof as camera.cpp/duplicator.cpp/spatial_noise.cpp. Neutral base is 0
 * (no scatter) -- an idle Dotter (nothing bound) leaves Points-Only mode
 * pixel-identical to pre-Dotter behavior.
 *
 * API-only for now (no WebUI control) -- the generic Modulator Bindings
 * target dropdown (populated from /api/modulators/meta) already lists
 * DOT_SPREAD automatically, so it's bindable today; a dedicated Dotter UI
 * card (direct on/off + static-spread slider, not requiring a bound
 * modulator) is planned as a follow-up.
 */

namespace dotter {

namespace target_id {
    constexpr uint16_t DOT_SPREAD = 20;
}

// Max scatter radius, DAC units -- same order of magnitude as the
// optimizer's jitter_amount_units ceiling (point_optimizer.h).
constexpr float DOT_SPREAD_MAX_UNITS = 2000.0f;

// Registers the target above with modulator::registerModTarget(). Call once
// after modulator::init().
void init();

// Folds any bound modulation onto a neutral 0 base, clamped to
// [0, DOT_SPREAD_MAX_UNITS]. Cheap to call unconditionally (modulator::apply()
// early-outs when nothing is bound) -- call once per frame, not per dot.
float apply();

// Deterministic per-dot scatter: displaces (x,y) outward in a stable
// pseudo-random direction/distance derived from dotIdx and radiusUnits, so a
// static shape's scatter pattern holds still instead of shimmering frame to
// frame (same "static wobble, not noise" philosophy as point_optimizer.cpp's
// jitter). No-op if radiusUnits <= 0.
void scatter(int16_t& x, int16_t& y, uint32_t dotIdx, float radiusUnits);

} // namespace dotter
