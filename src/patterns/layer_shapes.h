#pragma once
/**
 * layer_shapes.h -- parametric 2D shape generators for the Layer Engine
 *
 * Phase 1 of the Layer Engine (see plans/mutable-dreaming-candy.md): unlike
 * preset_patterns.cpp's ~150 fixed shapes, each ShapeType here is a single
 * continuously-parametric generator (sides/points/petals/lobes via `param`)
 * feeding the same optimizer::optimize() pipeline every preset already uses
 * (corner-aware density, budget clamping) -- see preset_patterns.cpp's
 * ngon()/star()/curve() for the established precedent this mirrors.
 */
#include "config.h"

namespace layer_shapes {

enum class ShapeType : uint8_t {
    Circle = 0, Square, Polygon, Star, Wave, Rose, Spiral,
    Spirograph, Rosette, WaveformTunnel, COUNT
};

// Generates one layer's shape outline centered at the origin, sized by
// scaleX/scaleY (0..1 fractions of the shape's full radius), colored r/g/b.
// `param` is the shape-specific integer control (Polygon: side count 3..20,
// Star: point count 3..16, Rose: petal count 1..12, Spiral: turns 1..8,
// Spirograph: lobe count 1..5, Rosette: petal count 2..10, WaveformTunnel:
// ring count 3..8; ignored by Circle/Square). `maxOut` is this layer's share
// of the per-frame point budget (see pattern_engine.cpp's Layer-mode split).
// Returns the point count written (<= maxOut), 0 if shape/maxOut invalid.
size_t generate(ShapeType type, uint8_t param, float scaleX, float scaleY,
                 LaserPoint* out, size_t maxOut, uint8_t r, uint8_t g, uint8_t b);

const char* shapeName(ShapeType type);

}  // namespace layer_shapes
