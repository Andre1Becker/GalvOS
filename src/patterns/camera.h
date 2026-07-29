#pragma once
#include <stdint.h>

/**
 * camera.h -- Phase 2 of the Animation & Modulation System (see
 * plans/generic-roaming-dahl.md). Generalizes preset_patterns.cpp's prj()/
 * wf() (used by the 5 wireframe/3D presets: Rotating/Static Cube, Pyramid,
 * Octahedron, Tetrahedron) with yaw/pitch/roll + dolly (Z translate) +
 * optional perspective divide, on top of each preset's own animated
 * orientation.
 *
 * Registers 5 new ModTargets (CAMERA_YAW/PITCH/ROLL/DIST/FOV) with the
 * modulator engine's registry from camera::init() -- proof that Phase 1's
 * registry absorbs a whole new module with zero changes to
 * modulator_engine.h/.cpp and zero changes to the WebUI (the existing
 * generic Modulator Bindings target dropdown, populated from
 * /api/modulators/meta, lists these automatically).
 *
 * All 5 targets are purely additive on top of a neutral 0 base -- an idle
 * camera (nothing bound) is numerically identical to no camera at all, so
 * the 5 wireframe presets render pixel-identical to pre-Phase-2 behavior
 * until a modulator is actually bound to one of these targets.
 */

namespace camera {

namespace target_id {
    constexpr uint16_t CAMERA_YAW   = 10;
    constexpr uint16_t CAMERA_PITCH = 11;
    constexpr uint16_t CAMERA_ROLL  = 12;
    constexpr uint16_t CAMERA_DIST  = 13;
    constexpr uint16_t CAMERA_FOV   = 14;
}

// Registers the 5 targets above with modulator::registerModTarget(). Call
// once after modulator::init().
void init();

// Folds any bound modulation for the 5 camera targets onto a neutral base,
// converting angles to radians. Cheap to call unconditionally (5x
// modulator::apply() early-outs when nothing is bound) -- call once per
// frame per wireframe preset, not per point.
void apply(float& yawRad, float& pitchRad, float& rollRad, float& dist, float& fov);

} // namespace camera
