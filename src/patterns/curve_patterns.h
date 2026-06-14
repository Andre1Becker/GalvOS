#pragma once
// curve_patterns.h -- Mathematical curve animation patterns
// 9 curves: Epitrochoid, Talbot, Harmonograph, Phyllotaxis,
//           Trefoil, Superformula, Butterfly, Astroid, Deltoid

#include "config.h"   // LaserPoint, PATTERN_POINTS_MAX
#include <stdint.h>
#include <stddef.h>

namespace curves {

constexpr uint8_t CURVE_COUNT = 9;

enum CurveType : uint8_t {
    EPITROCHOID  = 0,
    TALBOT       = 1,
    HARMONOGRAPH = 2,
    PHYLLOTAXIS  = 3,
    TREFOIL      = 4,
    SUPERFORMULA = 5,
    BUTTERFLY    = 6,
    ASTROID      = 7,
    DELTOID      = 8,
};

// Parameter definition (for UI labels + range)
struct ParamDef {
    const char* label;
    float min_val;
    float max_val;
    float def_val;
    float step;
};

// Per-curve: 5 parameters (p0..p2 = shape, p3 = speed, p4 = zoom)
struct CurveDef {
    const char* name;
    const char* description;
    ParamDef    params[5];
    uint8_t     def_r, def_g, def_b;
};

// Declared in .cpp
extern const CurveDef CURVE_DEFS[CURVE_COUNT];

// Runtime parameters (stored in gConfig)
struct CurveParams {
    float   p[5];         // actual values (not normalized)
    uint8_t r, g, b;      // color
};

// Initialize params from defaults
void initDefaultParams(uint8_t curve_idx, CurveParams& out);

// Generate laser points for one frame.
// phase: monotonically increasing frame counter
// Returns number of points written to buf.
size_t generate(CurveType type, const CurveParams& params,
                uint32_t phase, LaserPoint* buf, size_t max_pts);

} // namespace curves
