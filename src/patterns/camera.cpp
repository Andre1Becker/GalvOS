#include "camera.h"
#include "../modulator_engine.h"
#include <math.h>

namespace camera {

static const paramui::ParamMeta kYawMeta = {
    target_id::CAMERA_YAW, "camera_yaw", "Camera Yaw", "Camera", paramui::DataType::FLOAT,
    -180.0f, 180.0f, 0.0f, 1.0f, "deg",
    "Extra yaw rotation added on top of a 3D preset's own orientation.",
    true, false, true};
static const paramui::ParamMeta kPitchMeta = {
    target_id::CAMERA_PITCH, "camera_pitch", "Camera Pitch", "Camera", paramui::DataType::FLOAT,
    -180.0f, 180.0f, 0.0f, 1.0f, "deg",
    "Extra pitch rotation added on top of a 3D preset's own orientation.",
    true, false, true};
static const paramui::ParamMeta kRollMeta = {
    target_id::CAMERA_ROLL, "camera_roll", "Camera Roll", "Camera", paramui::DataType::FLOAT,
    -180.0f, 180.0f, 0.0f, 1.0f, "deg",
    "Screen-plane roll rotation applied after yaw/pitch.",
    true, false, true};
static const paramui::ParamMeta kDistMeta = {
    target_id::CAMERA_DIST, "camera_dist", "Camera Dolly", "Camera", paramui::DataType::FLOAT,
    -3.0f, 3.0f, 0.0f, 0.01f, "",
    "Translate along view Z (model-space units) before perspective divide.",
    true, false, true};
static const paramui::ParamMeta kFovMeta = {
    target_id::CAMERA_FOV, "camera_fov", "Camera FOV", "Camera", paramui::DataType::FLOAT,
    0.0f, 2.0f, 0.0f, 0.01f, "",
    "Perspective strength; 0 keeps the original orthographic projection.",
    true, false, true};

static const modulator::ModTargetDescriptor kTargetYaw   = {target_id::CAMERA_YAW,   kYawMeta,   180.0f, false};
static const modulator::ModTargetDescriptor kTargetPitch = {target_id::CAMERA_PITCH, kPitchMeta, 180.0f, false};
static const modulator::ModTargetDescriptor kTargetRoll  = {target_id::CAMERA_ROLL,  kRollMeta,  180.0f, false};
static const modulator::ModTargetDescriptor kTargetDist  = {target_id::CAMERA_DIST,  kDistMeta,  2.0f,   false};
static const modulator::ModTargetDescriptor kTargetFov   = {target_id::CAMERA_FOV,   kFovMeta,   1.0f,   false};

void init() {
    modulator::registerModTarget(&kTargetYaw);
    modulator::registerModTarget(&kTargetPitch);
    modulator::registerModTarget(&kTargetRoll);
    modulator::registerModTarget(&kTargetDist);
    modulator::registerModTarget(&kTargetFov);
}

void apply(float& yawRad, float& pitchRad, float& rollRad, float& dist, float& fov) {
    yawRad   = modulator::apply(target_id::CAMERA_YAW,   0.0f) * (float)(M_PI / 180.0);
    pitchRad = modulator::apply(target_id::CAMERA_PITCH, 0.0f) * (float)(M_PI / 180.0);
    rollRad  = modulator::apply(target_id::CAMERA_ROLL,  0.0f) * (float)(M_PI / 180.0);
    dist     = modulator::apply(target_id::CAMERA_DIST,  0.0f);
    fov      = modulator::apply(target_id::CAMERA_FOV,   0.0f);
}

} // namespace camera
