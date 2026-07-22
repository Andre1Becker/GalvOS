#pragma once
#include "config.h"
#include <atomic>

namespace presets {

// Actual star count rendered by Starfield (p90) last frame -- the Size
// slider's 0-255 range is a *request*; the optimizer's per-profile frame
// budget (max_pts_per_frame) may cap it lower, so the WebUI reads this back
// to show the real, achieved count instead of the raw slider value.
extern std::atomic<uint16_t> gStarfieldStarCount;

constexpr uint8_t PRESET_COUNT = 81;

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

    ShootingStars,
    PythagorasTree,

    EndlessSpiral, EndlessTunnel, ExplosionSpread, Fireworks, MilkyWay,
};

// Sanitizes a raw index (WebUI JSON, encoder, ...) into a valid Preset.
// Single source of truth for the bounds check -- anything outside the
// valid range (0 <= raw < PRESET_COUNT) collapses to Preset::None instead
// of propagating an invalid ID into pattern_engine state.
Preset presetFromIndex(int raw);

// PresetClass -- optimizer profile selector derived from a pattern's
// scanner workload, not from its display category. Two patterns in the same
// UI category can stress the galvos very differently (a Heart is one smooth
// closed loop, a Confetti Burst is 40 blanked jumps), so the grouping below
// is by geometry topology:
//
//   Vector      closed polygons / straight runs -> corner dwell dominates
//   Smooth      continuous closed curves, no corners -> density dominates
//   Waves       open polylines, high spatial frequency -> velocity dominates
//   Wireframe   3D edge chains -> corner dwell + short blank jumps
//   MultiObject several separate closed objects -> long blank jumps
//   Particles   isolated dots, no geometry -> blank jumps dominate
//   Trails      moving dots with connected fade tails (meteors) -> like
//               Particles but the tails are short continuous chains, so a
//               reduced per-frame budget keeps every meteor drawable instead
//               of the blank overhead starving later ones. Split from
//               Particles because the two want opposite budgets: hardware
//               testing showed the pure-dot presets look correct at a high
//               blank length that truncates the trail-based ones.
enum class PresetClass : uint8_t {
    Vector      = 0,
    Smooth      = 1,
    Waves       = 2,
    Wireframe   = 3,
    MultiObject = 4,
    Particles   = 5,
    Trails      = 6,
};
PresetClass presetClassOf(Preset p);

struct PresetInfo { const char* name; const char* category; };
extern const PresetInfo PRESETS[PRESET_COUNT];

// Member list of an optimizer profile, for display in the WebUI.
// Derived from presetClassOf() over PRESETS[] -- never hand-maintained.
uint8_t     profileMemberCount(uint8_t profile);
const char* profileMemberName(uint8_t profile, uint8_t n);

size_t generate(uint8_t idx, LaserPoint* out, size_t max_pts,
                uint32_t phase, uint8_t speed, uint8_t size_val);

} // namespace presets

// Countdown Timer API — see countdown_timer.h
#include "countdown_timer.h"
