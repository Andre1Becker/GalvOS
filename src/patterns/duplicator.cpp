#include "duplicator.h"
#include "../modulator_engine.h"
#include <math.h>

namespace duplicator {

static const paramui::ParamMeta kCountMeta = {
    target_id::DUP_COUNT, "dup_count", "Duplicator Count", "Duplicator", paramui::DataType::INT,
    0.0f, (float)MAX_EXTRA_COPIES, 0.0f, 1.0f, "",
    "Number of extra transformed copies chained after the original.",
    true, false, true};
static const paramui::ParamMeta kOffsetXMeta = {
    target_id::DUP_OFFSET_X, "dup_offset_x", "Duplicator Offset X", "Duplicator", paramui::DataType::FLOAT,
    -3.0f, 3.0f, 0.0f, 0.01f, "",
    "Per-copy X translate (native units); each copy adds one more step, rotated/scaled with it.",
    true, false, true};
static const paramui::ParamMeta kOffsetYMeta = {
    target_id::DUP_OFFSET_Y, "dup_offset_y", "Duplicator Offset Y", "Duplicator", paramui::DataType::FLOAT,
    -3.0f, 3.0f, 0.0f, 0.01f, "",
    "Per-copy Y translate (native units); each copy adds one more step, rotated/scaled with it.",
    true, false, true};
static const paramui::ParamMeta kAngleMeta = {
    target_id::DUP_ANGLE, "dup_angle", "Duplicator Angle", "Duplicator", paramui::DataType::FLOAT,
    -180.0f, 180.0f, 0.0f, 1.0f, "deg",
    "Per-copy rotation, compounding across the chain -- 360/count gives full radial symmetry.",
    true, false, true};
static const paramui::ParamMeta kScaleMeta = {
    target_id::DUP_SCALE, "dup_scale", "Duplicator Scale", "Duplicator", paramui::DataType::FLOAT,
    0.1f, 2.0f, 1.0f, 0.01f, "",
    "Per-copy scale multiplier, compounding across the chain -- <1 shrinks inward, >1 grows outward.",
    true, false, true};

static const modulator::ModTargetDescriptor kTargetCount   = {target_id::DUP_COUNT,    kCountMeta,   (float)MAX_EXTRA_COPIES, false};
static const modulator::ModTargetDescriptor kTargetOffsetX = {target_id::DUP_OFFSET_X, kOffsetXMeta, 2.0f,                    false};
static const modulator::ModTargetDescriptor kTargetOffsetY = {target_id::DUP_OFFSET_Y, kOffsetYMeta, 2.0f,                    false};
static const modulator::ModTargetDescriptor kTargetAngle   = {target_id::DUP_ANGLE,    kAngleMeta,   180.0f,                  false};
static const modulator::ModTargetDescriptor kTargetScale   = {target_id::DUP_SCALE,    kScaleMeta,   0.5f,                    false};

void init() {
    modulator::registerModTarget(&kTargetCount);
    modulator::registerModTarget(&kTargetOffsetX);
    modulator::registerModTarget(&kTargetOffsetY);
    modulator::registerModTarget(&kTargetAngle);
    modulator::registerModTarget(&kTargetScale);
}

void apply(int& extraCopies, float& offsetX, float& offsetY, float& angleRad, float& scale) {
    float countF = modulator::apply(target_id::DUP_COUNT, 0.0f);
    extraCopies  = (int)(countF >= 0.0f ? countF + 0.5f : countF - 0.5f);
    if (extraCopies < 0) extraCopies = 0;
    if (extraCopies > MAX_EXTRA_COPIES) extraCopies = MAX_EXTRA_COPIES;

    offsetX  = modulator::apply(target_id::DUP_OFFSET_X, 0.0f);
    offsetY  = modulator::apply(target_id::DUP_OFFSET_Y, 0.0f);
    angleRad = modulator::apply(target_id::DUP_ANGLE, 0.0f) * (float)(M_PI / 180.0);
    scale    = modulator::apply(target_id::DUP_SCALE, 1.0f);
}

} // namespace duplicator
