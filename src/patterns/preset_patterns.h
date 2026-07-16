#pragma once
#include "config.h"

namespace presets {

constexpr uint8_t PRESET_COUNT = 74;

// Type-safe preset selection. Values are the raw dispatch index into
// PRESETS[]/DISPATCH[] below and MUST stay in sync with the order of that
// table. None (-1) means "no preset active", matching the legacy int8_t
// sentinel. Kept as a scoped enum (not plain int8_t) so an out-of-range
// value can no longer reach setPreset() undetected -- see presetFromIndex().
enum class Preset : int8_t {
    None = -1,

    Circle = 0, Square, Triangle, Pentagon, Hexagon, Octagon,
    Star4, Star5, Star6, Star8,

    CrossPlus, XShape, Grid3x3, HLine, Diagonal,

    ArchimedeanSpiral, Lissajous1To2, Lissajous2To3, Lissajous3To4,
    Lissajous3To5, Lissajous5To6, DoubleSpiral, Rose3,

    Rose4, Heart, Infinity, Astroid, Epitrochoid,

    RotatingCube, StaticCube, Pyramid, Octahedron, Tetrahedron,

    SineWave, StandingWave, MultiWave, OceanWave, WaveInterference,
    Sawtooth, SquareWave, WavePacket, BeatWave, RadialWaves, FmWave,
    Vortex, SineHelix, WaveField, FourierSquare, GravityWaves, Tsunami,
    WaveSpectrum,

    Hypotrochoid, Butterfly, Spirograph5To3, ConcentricRings,
    NestedSquares, PulsingCircle,

    Starburst, ChaosBouncer, LaserDiamond, ConfettiBurst, DiscoBall,

    Hibiscus, StarburstParty,

    Starfield,

    CountdownTimer,

    Pentagram, DnaHelix, YinYang,

    RandomPoints,

    ThreeCircles, PointSpread,

    SolarSystem,

    BouncingPoints,
};

// Sanitizes a raw index (WebUI JSON, encoder, ...) into a valid Preset.
// Single source of truth for the bounds check -- anything outside the
// valid range (0 <= raw < PRESET_COUNT) collapses to Preset::None instead
// of propagating an invalid ID into pattern_engine state.
Preset presetFromIndex(int raw);

// PresetClass -- optimizer profile selector derived from the preset category.
enum class PresetClass : uint8_t {
    Simple = 0,   // Geometry, Lines
    Curves = 1,   // Spirals, Curves, Waves, Complex, Combo
    ThreeD = 2,   // 3D
    Scenes = 3,   // Scenes, Party, Vehicles, Symbols, Timers, unknown
    Solar  = 4,   // Solar System (long blank jumps, multi-object)
};
PresetClass presetClassOf(Preset p);

struct PresetInfo { const char* name; const char* category; };
extern const PresetInfo PRESETS[PRESET_COUNT];

size_t generate(uint8_t idx, LaserPoint* out, size_t max_pts,
                uint32_t phase, uint8_t speed, uint8_t size_val);

} // namespace presets

// Countdown Timer API — see countdown_timer.h
#include "countdown_timer.h"
