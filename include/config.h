#pragma once
#include <atomic>
/**
 * config.h -- runtime configuration and shared data types
 */

#include <Arduino.h>
#include <stdint.h>

// debug mode: Galvo/Laser-Hardware ueberspringen (only ESP32, no Laser)
// Set when /api/debug-mode is enabled -- also persistent via NVS.
extern volatile bool gDebugNoHW;

constexpr uint16_t GALVO_RATE_HZ      = GALVO_SAMPLE_RATE_HZ;
constexpr size_t   PATTERN_POINTS_MAX = 2048;

// Points-Only render mode (pattern_engine.cpp::applyPointsOnlyMode)
constexpr uint8_t  POINTS_MODE_MAX_DOTS  = 50;  // UI slider ceiling
constexpr uint8_t  POINTS_MODE_MIN_DWELL = 3;   // ticks; below this a dot is invisible
constexpr uint8_t  POINTS_MODE_MAX_DWELL = 30;  // ticks; cap so few dots don't hog the whole frame

// Random Points preset (preset_patterns.cpp::p106)
constexpr uint8_t  RANDOM_PTS_MAX_COUNT = 14;   // UI slider ceiling ("Amount")

// Kaleidoscope effect (pattern_engine.cpp::applyKaleidoscope)
constexpr uint8_t  KALEIDO_SEGMENTS_MAX = 6;   // UI slider ceiling (even only)

// GalvOS v5 Point Optimizer (Pillar 1) -- runtime-tunable via WebUI slider.
// Mirrors optimizer::OptimizerConfig's persisted/tunable fields -- NOT the
// full struct. Two OptimizerConfig fields are deliberately absent here:
// galvo_kpps (set per-call from gProjection.galvo_kpps by configFromLive(),
// not a stored preference) and transform (set per-frame from
// optimizer::gLiveTransform). Per-call frame context (hasPrevPos/prevX/
// prevY/frameBudgetRemaining) isn't a config value at all and has no
// counterpart here either. Kept as a separate struct here (rather than
// including point_optimizer.h) to avoid pulling the optimizer's geometry
// types into every translation unit that already includes config.h.
//
// DEFAULT VALUES: tuned for a 30 kpps output rate (GALVO_SAMPLE_RATE_HZ,
// see platformio.ini). max_pts_per_frame=1010 -> 30000/1010 ~= 30 Hz, a
// mostly flicker-free floor at that rate (see optimize()'s frame-budget
// comment for how frameBudgetRemaining spends this across multi-call
// frames). All OPT_DEFAULT_* macros are the single source of truth;
// point_optimizer.h references them so both structs stay in sync
// automatically.
#define OPT_DEFAULT_CORNER_ANGLE_DEG            25.0f
#define OPT_DEFAULT_MIN_CORNER_PTS              2
#define OPT_DEFAULT_MAX_CORNER_PTS              8
#define OPT_DEFAULT_PTS_PER_1000_UNITS          6.0f
#define OPT_DEFAULT_BLANK_SAMPLES               16
#define OPT_DEFAULT_MAX_PTS_PER_FRAME           1010
// Not mirrored in OptimizerLiveConfig above (see the struct's own header
// comment) -- this is optimizer::OptimizerConfig::galvo_kpps' only default,
// used when a caller constructs an OptimizerConfig directly instead of
// through configFromLive(), which always overwrites it with the live
// gProjection.galvo_kpps.
#define OPT_DEFAULT_GALVO_KPPS                  30
#define OPT_DEFAULT_MIN_BLANK_SAMPLES           6
#define OPT_DEFAULT_BLANK_PTS_PER_1000_UNITS    8.0f
#define OPT_DEFAULT_MIN_INTERIOR_PTS_PER_SEG    8
#define OPT_DEFAULT_STAGE1_BLANK_TARGET         16
// RESAMPLE STAGE (Phase 2): constant point spacing. When enabled, interior
// point count for an edge is length / resample_spacing_units instead of
// length/1000 * pts_per_1000_units -- absolute, length-independent spacing
// so a 100-unit and a 1000-unit edge get the same points-per-unit density.
// Disabled by default -> edgeInteriorCount() keeps using pts_per_1000_units,
// so output stays byte-identical to the pre-resample optimizer.
#define OPT_DEFAULT_RESAMPLE_ENABLED             false
#define OPT_DEFAULT_RESAMPLE_SPACING_UNITS       160.0f
// CURVATURE-ADAPTIVE RESAMPLE (P11b): a modifier on the resample stage above.
// The constant resample spacing is a floor of density that ignores shape --
// a tight arc and a straight run get the same points-per-unit. When enabled,
// the local spacing is scaled down where the polyline actually bends: the
// discrete turn angle at an edge's endpoints (second difference of the vertex
// sequence, radians 0..PI) is read as a curvature proxy, and the effective
// spacing becomes resample_spacing_units / (1 + curvature_gain * turnAngle),
// clamped to [min_spacing_units, max_spacing_units]. A straight run (turn ~0)
// keeps the base spacing -> byte-identical to the plain resample result there;
// only curved regions densify. Feeds the SAME planSegment/budget/Stage-1
// path, so curvature density scales down under max_pts_per_frame like
// everything else. Brings continuous curve_patterns.cpp geometry (no discrete
// corners) into the optimizer via a per-turn-angle density instead of
// corner_angle_deg. Disabled by default -> resample stage unchanged.
#define OPT_DEFAULT_CURVATURE_RESAMPLE_ENABLED    false
#define OPT_DEFAULT_CURVATURE_GAIN                2.0f
#define OPT_DEFAULT_MIN_SPACING_UNITS             40.0f
#define OPT_DEFAULT_MAX_SPACING_UNITS             400.0f
// PILLAR 3: ZV (Zero Vibration) input-shaping ringing compensation on
// blank-jump moves. Disabled by default -- ring_freq_hz/ring_damping_ratio
// must be measured on real hardware (step-response capture on a scope)
// before enabling; unmeasured defaults can make ringing worse, not better.
#define OPT_DEFAULT_RINGING_COMP_ENABLED         false
#define OPT_DEFAULT_RING_FREQ_HZ                 200.0f
#define OPT_DEFAULT_RING_DAMPING_RATIO           0.15f
// VELOCITY / ACCELERATION CLAMP (Phase 4): a post-pass over the emitted lit
// point stream that protects the galvo from being commanded to move faster
// (velocity) or change speed harder (acceleration) than it can physically
// track. Disabled by default -- max_step_units / max_accel_units are galvo-
// specific (Jolooyo JY-15K-BL) and must be tuned on real hardware; unmeasured
// defaults could either over-subdivide (wasting the flicker budget) or do
// nothing. Off => output stays byte-identical to the pre-clamp optimizer.
//   max_step_units:  ceiling on per-tick position change (DAC units/sample).
//                    Long lit steps above this are subdivided by linear
//                    interpolation (position + color) so the mirror never
//                    lags a single large jump. Blank runs are exempt -- they
//                    are already eased by Pillar 2/3.
//   max_accel_units: ceiling on the per-tick change of that step magnitude
//                    (DAC units/sample^2). Limits how fast the beam is allowed
//                    to speed up, easing hard velocity ramps into corners.
#define OPT_DEFAULT_VEL_CLAMP_ENABLED            false
#define OPT_DEFAULT_MAX_STEP_UNITS               200.0f
#define OPT_DEFAULT_ACCEL_CLAMP_ENABLED          false
#define OPT_DEFAULT_MAX_ACCEL_UNITS              800.0f
// POINT DISTRIBUTION MODIFIER -- JITTER (Phase 4): a deterministic
// perpendicular-to-edge offset applied to interior (non-corner) points at
// emit time, for a hand-drawn/organic outline instead of a mathematically
// exact one. Deterministic per (edge index, point-in-edge index) -- same
// offset every frame, so a static shape gets a stable "wobble" rather than
// shimmering noise; a moving/rotating shape carries the wobble with it.
// Disabled by default -> output stays byte-identical to the pre-jitter
// optimizer. Purely a post-computation perturbation of already-planned
// point positions (see point_optimizer.cpp's emitSegment()) -- point
// counts/budget/corner severity are all computed before jitter is applied,
// so it needs no changes to planSegment()/cornerSeverity()/edgeInteriorCount().
#define OPT_DEFAULT_JITTER_ENABLED               false
#define OPT_DEFAULT_JITTER_AMOUNT_UNITS          80.0f
// SEGMENT REORDER (P20): a nearest-neighbour pass over the input segments'
// start/end points, run before geometry/budget planning, that reorders which
// segment emitAllSegments() visits when -- and, for open segments, whether it
// traverses one backwards -- to shorten the total blank-jump distance for a
// frame with several disconnected segments (wireframes, text, paint strokes).
// Purely a visitation-order change: no geometry is added or removed, closed
// segments keep their internal vertex order (their edges' color gradients are
// unaffected) but may rotate which vertex serves as the entry/exit point.
// Disabled by default -> segments are visited in the caller-supplied order,
// byte-identical to the pre-P20 optimizer.
#define OPT_DEFAULT_REORDER_SEGMENTS              false
// SEGMENT REORDER 2-OPT (P11a): an optional refinement layered on top of the
// P20 nearest-neighbour tour above. Greedy NN never revisits a decision, so it
// can leave crossings a local swap would remove; this takes the finished NN
// tour and applies bounded 2-opt block reversals, each kept only when it
// strictly shortens the total blank-jump path (open segments flip traversal
// direction with the block; closed ones keep their v0 entry). Strict-
// improvement acceptance over a symmetric metric makes it deterministic and
// never worse than the NN tour it starts from. Only meaningful when
// reorder_segments is on; skipped above the segment-count cap in
// point_optimizer.cpp to bound the per-frame O(n^2)-per-pass cost on the S3.
// Disabled by default -> byte-identical to the greedy-only reorder.
#define OPT_DEFAULT_REORDER_2OPT                  false

struct OptimizerLiveConfig {
    float    corner_angle_deg             = OPT_DEFAULT_CORNER_ANGLE_DEG;
    uint8_t  min_corner_pts               = OPT_DEFAULT_MIN_CORNER_PTS;
    uint8_t  max_corner_pts               = OPT_DEFAULT_MAX_CORNER_PTS;
    float    pts_per_1000_units           = OPT_DEFAULT_PTS_PER_1000_UNITS;
    uint8_t  blank_samples                = OPT_DEFAULT_BLANK_SAMPLES;
    uint16_t max_pts_per_frame            = OPT_DEFAULT_MAX_PTS_PER_FRAME;
    uint8_t  min_blank_samples            = OPT_DEFAULT_MIN_BLANK_SAMPLES;
    float    blank_pts_per_1000_units     = OPT_DEFAULT_BLANK_PTS_PER_1000_UNITS;
    uint8_t  min_interior_pts_per_segment = OPT_DEFAULT_MIN_INTERIOR_PTS_PER_SEG;
    uint8_t  stage1_blank_target          = OPT_DEFAULT_STAGE1_BLANK_TARGET;
    bool     resample_enabled             = OPT_DEFAULT_RESAMPLE_ENABLED;
    float    resample_spacing_units       = OPT_DEFAULT_RESAMPLE_SPACING_UNITS;
    bool     curvature_resample_enabled   = OPT_DEFAULT_CURVATURE_RESAMPLE_ENABLED;
    float    curvature_gain               = OPT_DEFAULT_CURVATURE_GAIN;
    float    min_spacing_units            = OPT_DEFAULT_MIN_SPACING_UNITS;
    float    max_spacing_units            = OPT_DEFAULT_MAX_SPACING_UNITS;
    bool     ringing_comp_enabled         = OPT_DEFAULT_RINGING_COMP_ENABLED;
    float    ring_freq_hz                 = OPT_DEFAULT_RING_FREQ_HZ;
    float    ring_damping_ratio           = OPT_DEFAULT_RING_DAMPING_RATIO;
    bool     vel_clamp_enabled            = OPT_DEFAULT_VEL_CLAMP_ENABLED;
    float    max_step_units               = OPT_DEFAULT_MAX_STEP_UNITS;
    bool     accel_clamp_enabled          = OPT_DEFAULT_ACCEL_CLAMP_ENABLED;
    float    max_accel_units              = OPT_DEFAULT_MAX_ACCEL_UNITS;
    bool     jitter_enabled               = OPT_DEFAULT_JITTER_ENABLED;
    float    jitter_amount_units          = OPT_DEFAULT_JITTER_AMOUNT_UNITS;
    bool     reorder_segments             = OPT_DEFAULT_REORDER_SEGMENTS;
    bool     reorder_2opt                  = OPT_DEFAULT_REORDER_2OPT;
};

// Which OptimizerNormalizeResult field(s) normalizeOptimizerConfig() had to
// correct -- so a caller with a JSON `applied` echo (web_ui.cpp) can report
// the effective value back instead of silently accepting the request's.
struct OptimizerNormalizeResult {
    bool min_blank_samples_corrected = false;
    bool min_corner_pts_corrected    = false;

    bool any() const { return min_blank_samples_corrected || min_corner_pts_corrected; }
};

// Cross-field validation for OptimizerLiveConfig. Every write path (WebUI
// live-edit, NVS-backed save, backup restore, community-preset import)
// clamps each min/max pair to its own valid range independently, so an
// inverted pair -- min greater than max -- is settable unless every one of
// those paths also enforces the RELATION between the two fields. Two pairs
// feed invariants inside the optimizer itself:
//
//   min_blank_samples <= blank_samples -- Stage 1 (point_optimizer.cpp's
//     optimize()) only reduces blanking `if (blank_samples >
//     min_blank_samples)`. Inverted, that guard never fires and a frame that
//     needs Stage 1 to fit its budget can no longer shrink.
//
//   min_corner_pts <= max_corner_pts -- cornerPointCount() interpolates
//     between the two by severity (soft corner -> min, sharp corner ->
//     max). Inverted, the interpolation runs backwards: sharp corners get
//     FEWER points than soft ones.
//
// Corrects the MIN value downward to match the MAX (never raises the max),
// so a caller's explicit max is always preserved. Call after every write to
// an OptimizerLiveConfig, regardless of source.
inline OptimizerNormalizeResult normalizeOptimizerConfig(OptimizerLiveConfig& cfg) {
    OptimizerNormalizeResult r;
    if (cfg.min_blank_samples > cfg.blank_samples) {
        cfg.min_blank_samples = cfg.blank_samples;
        r.min_blank_samples_corrected = true;
    }
    if (cfg.min_corner_pts > cfg.max_corner_pts) {
        cfg.min_corner_pts = cfg.max_corner_pts;
        r.min_corner_pts_corrected = true;
    }
    return r;
}

// ── OPTIMIZER PROFILES ──────────────────────────────────────────────────────
// Eight independent OptimizerLiveConfig profiles, one per PresetClass plus
// Text. gOptimizerConfig is always a live copy of the active profile; call
// syncOptimizerConfig() after writing to gOptimizerProfiles[n].
//
// Profiles are grouped by scanner workload, not by display category --
// see PresetClass in preset_patterns.h for the rationale.
//
//   Index 0 = Vector       closed polygons, straight runs (corner dwell)
//   Index 1 = Smooth       continuous closed curves (interior density)
//   Index 2 = Waves        open polylines, high frequency (velocity clamp)
//   Index 3 = Wireframe    3D edge chains (corner dwell + short jumps)
//   Index 4 = MultiObject  several closed objects (long blank jumps)
//   Index 5 = Particles    isolated dots (blank jumps only)
//   Index 6 = Trails       moving dots with fade tails (reduced budget)
//   Index 7 = Text         many short disconnected glyph strokes (blank
//                          jumps between strokes/letters dominate; not a
//                          PresetClass member, selected by text_renderer's
//                          caller instead of presetClassOf())
//
// NVS key suffixes are pinned per index so existing stored parameters
// migrate onto the renamed profile rather than resetting to defaults.

constexpr uint8_t OPT_PROFILE_COUNT       = 8;
constexpr uint8_t OPT_PROFILE_VECTOR      = 0;
constexpr uint8_t OPT_PROFILE_SMOOTH      = 1;
constexpr uint8_t OPT_PROFILE_WAVES       = 2;
constexpr uint8_t OPT_PROFILE_WIREFRAME   = 3;
constexpr uint8_t OPT_PROFILE_MULTIOBJECT = 4;
constexpr uint8_t OPT_PROFILE_PARTICLES   = 5;
constexpr uint8_t OPT_PROFILE_TRAILS      = 6;
constexpr uint8_t OPT_PROFILE_TEXT        = 7;

// ── PER-PROFILE TUNED DEFAULTS ──────────────────────────────────────────────
// The OPT_DEFAULT_* macros above are the GENERIC fallback (and the reset
// target for a single parameter). The table below is the per-profile tuning:
// every profile used to boot from the same generic defaults, which made the
// six-profile split inert -- Smooth and Particles ran identical parameters
// despite having opposite bottlenecks.
//
// Derived by sweeping the real optimizer against each class's actual geometry
// at a 1300-point frame budget (~23 Hz at 30 kpps), scoring worst-case lit
// step size (the galvo-velocity proxy) subject to every class member fitting
// the budget. Key findings encoded here:
//
//   Smooth      no true corners -> max_corner_pts down to 3, budget spent on
//               interior density instead. Worst-case step 146 -> 89 units.
//   Vector      Star 8 is the binding member; p/1k=9 puts it at ~1284.
//   Wireframe/  budget-bound, NOT density-bound: interior density is scaled
//   MultiObject back by the optimizer's Stage 2 regardless of what is asked
//               for, so the real lever is blanking. Lower blank_samples and
//               stage1_blank_target return points to lit geometry.
//   Particles   lit count is fixed (one dwell per dot); >90% of the frame is
//               blanking, so only the blank parameters matter.
//   Text        every glyph is a handful of short pen-up/pen-down strokes
//               (a "G" is one stroke, an "A" is two, ...), so -- like
//               Particles -- the frame is blank-dominated rather than
//               density-dominated. Unlike Particles, jumps are not all the
//               same length (short intra-glyph lifts vs. longer letter-to-
//               letter advances), so blank_samples keeps a modest ceiling
//               instead of Particles' aggressive 10. text_renderer.cpp
//               additionally hard-floors min_interior_pts_per_segment>=1
//               itself, so the low profile default here just avoids
//               reserving MORE than that floor needs. Both endpoints of a
//               short stroke (an E's crossbar, a T's bar) are kept by the
//               corner dwell, which emits min_corner_pts at every vertex
//               regardless of interior density. corner_angle_deg/max_corner_pts stay
//               close to Vector since glyphs (M, W, K, Z, ...) have real
//               sharp corners, just smaller and more numerous than Vector's
//               shapes, so fewer points per corner are needed.
//
// blank_samples / min_blank_samples / blank_pts_per_1000_units for Wireframe,
// MultiObject and Text (P21, see docs/optimizer-refactor/DECISIONS.md
// 2026-08-11 Session P): blankCountForDistance() is
// round(dist/1000 * blank_pts_per_1000_units), clamped to
// [min_blank_samples, blank_samples]. The three rows below shipped with
// blank_pts_per_1000_units in [8,10] and blank_samples in [10,12], i.e. a
// proportional window of ~450-1200 DAC units -- while real jumps in these
// three classes (measured off each class's actual generator code: wireframe
// polyhedra at their real projected scale, Concentric Rings / Nested Squares /
// Solar System / Starburst's actual radii, and the stroke font's real glyph
// coordinates) run 1,700-33,000 units. Every real jump therefore clamped to
// the ceiling, making Pillar 2 a constant per-jump cost for these three
// classes regardless of distance. Re-centered the window on each class's
// measured distribution (ceiling widened, floor lowered) so short and long
// jumps within a class now cost different amounts:
//   Wireframe:   12/6/10.0 -> 20/4/0.8   (window  450-1500 ->  5,000-25,000)
//   MultiObject: 12/6/10.0 -> 18/4/1.5   (window  450-1500 ->  2,700-12,000)
//   Text:        10/4/9.0  -> 16/4/1.0   (window  400-1100 ->  4,000-16,000)
// stage1_blank_target (10/10/7) is untouched -- it already sits between each
// new floor and ceiling, so it stays a valid Stage-1 shrink target with more
// headroom above it than before. min_corner_pts/max_corner_pts/pts_per_1000_
// units/max_pts_per_frame are untouched. Particles (index 5) was flagged in
// that same session as "looks like the same issue" but out of scope -- fixed
// below.
//
// Particles (6.65.1): flagged-not-fixed by Session P, confirmed by a bug
// report of Starfield/RandomPoints/PointSpread/ConfettiBurst/BouncingPoints/
// ExplosionSpread/Fireworks/MilkyWay all drawing connecting streaks instead
// of isolated dots. Root cause was exactly Session P's class of bug: shipped
// window was blank_samples=10, min_blank_samples=6, blppu=12.0 -> 500-833
// DAC units. These presets scatter points across the FULL canvas (SC=18000,
// so up to ~48,000 units corner-to-corner for Starfield/RandomPoints; several
// of them -- RandomPoints, ConfettiBurst's launch order, PointSpread at low
// N -- visit points in an order uncorrelated with position, so the jump is
// effectively a random chord across the whole canvas, not a short hop between
// neighbours). Every real jump clamped to the 10-tick ceiling: at 30 kpps
// that is 333us total (166us of actual travel) commanded to cross up to
// 48,000 units -- physically nowhere close, so the beam was still mid-flight
// when the laser re-armed, painting the "connect the dots" streak. Widened
// ceiling+slope (min_blank_samples left at 6 -- Starfield's own tight
// clustered-hop case, the profile's original tuning target per
// pattern_engine.cpp's applyPointsOnlyMode() comment, is still served by the
// same floor):
//   Particles: 10/6/12.0 -> 40/6/0.9   (window  500-833 -> 6,667-44,444)
// Members that self-cap point count against cfg.blank_samples (Starfield's
// nStars, RandomPoints has no such cap) will now show fewer simultaneous
// points at maxed-out Size sliders -- correct dim/sparse dots beat bright
// streaks. stage1_blank_target (8) untouched, same rationale as Session P.
//
// PROFILE_DEFAULTS is indexed by OPT_PROFILE_* and consumed by loadConfig()
// as the NVS fallback, so a user's stored per-profile values still win.
struct OptimizerProfileDefaults {
    float    corner_angle_deg;
    uint8_t  min_corner_pts;
    uint8_t  max_corner_pts;
    float    pts_per_1000_units;
    uint8_t  blank_samples;
    uint8_t  min_blank_samples;
    uint8_t  stage1_blank_target;
    float    blank_pts_per_1000_units;
    uint8_t  min_interior_pts_per_segment;
    uint16_t max_pts_per_frame;   // per-profile frame budget (was a single
                                  // global default). Trails needs a smaller
                                  // budget than the rest so blank overhead
                                  // does not starve later meteors.
};

// Order MUST match OPT_PROFILE_* indices.
static const OptimizerProfileDefaults OPT_PROFILE_DEFAULTS[OPT_PROFILE_COUNT] = {
    // cad, mincp, maxcp, p/1k, blank, minbl, s1tgt, blppu, minip, maxppf
    {  30.f,   2,     8,    9.f,   16,    6,    12,    8.f,    8,   1300 },  // 0 Vector
    {  60.f,   2,     3,   11.f,   16,    6,    12,    8.f,    8,   1300 },  // 1 Smooth
    {  35.f,   2,     6,    8.f,   16,    6,    12,    8.f,    8,   1300 },  // 2 Waves
    {  25.f,   2,     8,    6.f,   20,    4,    10,    0.8f,   6,   1300 },  // 3 Wireframe
    {  25.f,   2,     6,    5.f,   18,    4,    10,    1.5f,   6,   1300 },  // 4 MultiObject
    {  25.f,   2,     4,    6.f,   40,    6,     8,    0.9f,   4,   1300 },  // 5 Particles
    {  60.f,   3,     3,   11.f,   16,    6,    12,    8.f,    8,    880 },  // 6 Trails
    {  28.f,   2,     5,    6.f,   16,    4,     7,    1.0f,   1,   1300 },  // 7 Text
};

extern OptimizerLiveConfig gOptimizerProfiles[OPT_PROFILE_COUNT];
extern OptimizerLiveConfig gOptimizerConfig;   // live copy of active profile
extern volatile uint8_t    gActiveOptimizerProfile;

// Copy gOptimizerProfiles[gActiveOptimizerProfile] → gOptimizerConfig.
// Call whenever the active profile index or its contents change.
inline void syncOptimizerConfig() {
    gOptimizerConfig = gOptimizerProfiles[gActiveOptimizerProfile];
}

// Pattern cache invalidation counter (Phase 2). Bumped whenever a change
// makes previously-cached static-preset geometry stale: any optimizer-live
// write and any galvo_kpps change. preset_patterns.cpp compares the value it
// cached against the current one and regenerates on mismatch. A plain
// uint32_t is sufficient -- it's written only from the (single) web-server
// task and read only from the pattern task; a stale read costs at most one
// extra regeneration, never wrong geometry.
extern volatile uint32_t gPatternCacheGen;

enum DmxChannel : uint8_t {
    // ── default pattern control (CH 1-16) ────────────────────────────
    DMX_MASTER = 0,     // CH 1:  master dimmer 0-255
    DMX_COLOR,          // CH 2:  Color Preset 0-255
    DMX_COLOR_SPEED,    // CH 3:  color animation speed
    DMX_PATTERN_GROUP,  // CH 4:  Pattern Group (0=Geometry, 1=Waves...)
    DMX_PATTERN_SELECT, // CH 5:  pattern within group 0-255
    DMX_DYN_EFFECT,     // CH 6:  Dynamic effect (Rotation, Pulse...)
    DMX_EFFECT_SPEED,   // CH 7:  Effect speed
    DMX_SIZE,           // CH 8:  size/scaling 0-255
    DMX_AUTO_SCALE,     // CH 9:  auto-scaling on/off
    DMX_ROTATION,       // CH 10: Rotation 0-255 (0-360)
    DMX_HFLIP,          // CH 11: Horizontal flip (0=normal, 128+=flip)
    DMX_VFLIP,          // CH 12: Vertical flip
    DMX_HMOVE,          // CH 13: Horizontal position
    DMX_VMOVE,          // CH 14: Vertical position
    DMX_WAVE_AMP,       // CH 15: Wave Amplitude (for wave patterns)
    DMX_WAVE_FREQ,      // CH 16: Wave Frequency

    // ── ILDA SD-Card Player (CH 17-22) ──────────────────────────────
    // Appended to existing DMX table, no gaps
    DMX_ILDA_SELECT,    // CH 17: 0=off, 1-40=file 1-40, 255=last
    DMX_ILDA_SPEED,     // CH 18: Playback speed 0-255
    DMX_ILDA_SIZE,      // CH 19: scaling 0-255 (128=original)
    DMX_ILDA_LOOP,      // CH 20: 0=once, 1-255=loop
    DMX_ILDA_BRIGHT,    // CH 21: brightness override 0-255 (255=use dimmer)
    DMX_ILDA_REPEAT,    // CH 22: Frame repeat 0=normal, 1-255=slower

// ── Color animation (CH 23-25) ──────────────────────────────────
    DMX_COL_ANIM_TYPE,  // CH 23: 0=off, 1=gradient, 2=chase, 3=strobe, 4=pulse, 5=twinkle, 6=flip
    DMX_COL_ANIM_SEQ,   // CH 24: sequence index 0-9
    DMX_COL_ANIM_SPEED, // CH 25: animation speed 0-255

    DMX_CHANNELS_USED = 25  // total: 25 DMX channels
};

// DMX channel names (for WebUI and documentation)
static const char* DMX_CHANNEL_NAMES[25] = {
    "Master Dimmer",        // 1
    "Color Preset",          // 2
    "Color Speed",           // 3
    "Pattern Group",       // 4
    "Pattern Select",      // 5
    "Effect Mode",         // 6
    "Effect Speed",         // 7
    "Size",              // 8
    "Auto-Scale",           // 9
    "Rotation",             // 10
    "H-Flip",               // 11
    "V-Flip",               // 12
    "H-Position",           // 13
    "V-Position",           // 14
    "Wave Amplitude",       // 15
    "Wave Frequency",       // 16
    "ILDA File (0-40)",     // 17
    "ILDA Speed",           // 18
    "ILDA Size",            // 19
    "ILDA Loop",            // 20
    "ILDA Brightness",      // 21
    "ILDA Frame Repeat",    // 22
    "Color Anim Type",      // 23
    "Color Anim Sequence",  // 24
    "Color Anim Speed",     // 25
};

// SRC_HELIOS: repurposed for the network Helios emulation (helios_net.cpp) --
// the USB stub (helios_usb.cpp) never wired this up. SRC_SACN/SRC_OSC appended
// at the end so existing numeric values (persisted nowhere, but mirrored in
// data/index.html's srcNames[]) stay stable.
enum ControlSource : uint8_t {
    SRC_NONE = 0, SRC_DMX, SRC_ARTNET, SRC_ETHERDREAM,
    SRC_HELIOS, SRC_INTERNAL, SRC_WEBUI, SRC_SACN, SRC_OSC
};

struct __attribute__((packed)) LaserPoint {
    int16_t  x, y;          // 4 bytes: galvo position ±32767
    uint8_t  r, g, b;       // 3 bytes: color 0-255
    uint8_t  blank;         // 1 byte:  1 = beam off (blanking)

    // Constructor for brace-initialization (packed prevents aggregate-init)
    LaserPoint() : x(0), y(0), r(0), g(0), b(0), blank(0) {}
    LaserPoint(int16_t x, int16_t y, uint8_t r, uint8_t g, uint8_t b, uint8_t blank)
        : x(x), y(y), r(r), g(g), b(b), blank(blank) {}
};                          // = exactly 8 bytes
static_assert(sizeof(LaserPoint) == 8, "LaserPoint padding check");

struct RuntimeConfig {
    uint8_t   version = 2;
    uint16_t  dmx_address    = DEFAULT_DMX_ADDRESS;
    uint16_t  artnet_universe = DEFAULT_DMX_UNIVERSE;

    // Per-interface enable toggles (WebUI Config tab). Sockets/servers are
    // still opened at boot (matches the existing Art-Net/EtherDream
    // lifecycle, see main.cpp) -- disabling a feature stops it from acting
    // on received data rather than tearing down its socket.
    bool      osc_enabled        = true;
    bool      sacn_enabled       = true;
    bool      helios_net_enabled = true;
    bool      artnet_enabled     = true;
    bool      etherdream_enabled = true;

    // Per-protocol verbose logging (WebUI Config tab). Off by default --
    // logs every received command/frame to Serial and the WebUI log buffer,
    // which is too noisy to leave on permanently at frame rate.
    bool      debug_log_dmx        = false;
    bool      debug_log_artnet     = false;
    bool      debug_log_etherdream = false;
    bool      debug_log_helios_net = false;
    bool      debug_log_osc        = false;
    bool      debug_log_sacn       = false;

    int16_t   galvo_x_offset = 0;
    int16_t   galvo_y_offset = 0;
    int16_t   galvo_x_gain   = 32767;
    int16_t   galvo_y_gain   = 32767;
    bool      swap_xy        = false;
    bool      invert_x       = false;
    bool      invert_y       = false;

    // DAC output limiting: clamps the final 16-bit DAC codes for X/Y to
    // [dac_limit_min, dac_limit_max] before writing to the DAC8562.
    // Default ~95% of full range (0x0666..0xF999) to keep the OPA4134
    // differential-amp output within +/-5.5V (galvo input rated +/-5V,
    // hardware gain is 2.2x). Symmetric around 0x8000 by default.
    uint16_t  dac_limit_min  = 0x0666;
    uint16_t  dac_limit_max  = 0xF999;

    // Proportional pre-scale applied to X/Y about center (0x8000) so a
    // pattern's extreme corners shrink into the galvo's linear range instead
    // of being flattened by the dac_limit_min/max clamp above. 1.0 = no
    // scale (rely on clamp only). Default 0.91 derived from the OPA4134's
    // 2.2x hardware gain (~1/2.2*2.0 of usable code range) -- see
    // docs/HARDWARE.md's DAC->Galvo transfer function. The dac_limit clamp
    // stays in place as the final safety net after this scale is applied.
    float     outputScale    = 0.91f;

    // white balance — calculated from laser specification:
    // R=1000mW × sens(638nm,0.265) = 265 mW_vis
    // G=1000mW × sens(520nm,0.710) = 710 mW_vis
    // B=3000mW × sens(445nm,0.040) = 120 mW_vis  <- weakest
    // Normalized to 120 mW_vis:
    uint8_t   gain_r = 115;   // 1000mW × 45% × 0.265 ≈ 120 mW_vis ✓
    uint8_t   gain_g =  43;   // 1000mW × 17% × 0.710 ≈ 120 mW_vis ✓
    uint8_t   gain_b = 255;   // 3000mW ×100% × 0.040 = 120 mW_vis ✓
    bool      gamma_enable = true;   // perceptual brightness correction (CIE 1931)

    // Visibility threshold ("Basiswert") per color: lowest final PWM duty
    // at which the laser diode driver actually emits visible light -- below
    // this the beam is physically dark regardless of duty. Measured per
    // channel via the Calib tab (White Balance pattern). The logical 0-255
    // color range is remapped onto [thresh_x..255] so 0-100% always spans
    // the full visible range instead of wasting it on a dead zone.
    // See galvo_out.cpp::mapVisibleRange().
    uint8_t   thresh_r = 143;
    uint8_t   thresh_g = 144;
    uint8_t   thresh_b = 169;

    uint16_t  scanfail_timeout_ms = 50;
    uint16_t  watchdog_period_ms  = 500;

    char      wifi_ssid[33] = {0};
    char      wifi_pass[65] = {0};
    char      hostname[32]  = "galvOS";

    // NTP
    char      ntp_server[64] = "pool.ntp.org";
    char      ntp_tz[48]     = "UTC0";           // POSIX TZ string

    // Safety
    // Largest free internal (DRAM) block -- catches heap fragmentation,
    // not just total free heap. esp_restart() if below this threshold.
    // Calibrated 2026-07-10 on real hardware post-WS-removal (5.34.x):
    // idle largest=28660, single-client browser load-peak largest=11764
    // (lowest observed in normal operation), settled largest=13812.
    // No remaining internal-heap allocation exceeds a few KB (JSON/log
    // buffers moved to PSRAM); 6144 gives ~2x margin below the measured
    // peak for a second client/tab or slower WiFi timing, while still
    // catching real fragmentation well before allocation failure.
    uint32_t  heap_critical_bytes = 6144;
    bool      safety_override = false;
    bool      dac_debug_log   = false;  // log DAC8562 writes (hex) to Serial+UI, rate-limited

    // network: DHCP or static
    bool      wifi_static   = false;
    char      wifi_ip[16]   = {0};      // e.g. "192.168.1.100"
    char      wifi_gw[16]   = {0};      // e.g. "192.168.1.1"
    char      wifi_mask[16] = {0};      // e.g. "255.255.255.0"
    char      wifi_dns[16]  = {0};      // e.g. "8.8.8.8"

    // SHA-256 hex (64 chars) of the password. Default: empty = "laser"
    char      auth_hash[65] = {0};
};

extern RuntimeConfig gConfig;

struct RuntimeState {
    // ── safety-critical flags → std::atomic (FIX: race condition) ──
    // atomic<> guarantees atomic read/write operations without a mutex.
    // No lock needed, no overhead -- ideal for frequently read flags.
    std::atomic<bool>     laser_armed       {false};
    std::atomic<bool>     estop_ok          {false};
    std::atomic<bool>     scanfail_ok       {false};
    std::atomic<uint8_t>  source            {0};      // ControlSource
    std::atomic<uint8_t>  master_dimmer     {0};
    std::atomic<uint8_t>  thermal_power_scale {255};  // 255=100%; set by temp::task() via gSafety.temp_reduce_c
    std::atomic<uint32_t> points_per_sec    {0};
    std::atomic<uint32_t> fps               {0};      // drawn frames/sec, see galvo::fps()
    std::atomic<uint32_t> frame_n           {0};      // total points in last rendered frame
    std::atomic<uint32_t> frame_lit         {0};      // lit (non-blank) points in last frame
    std::atomic<uint32_t> frame_blank       {0};      // blank points in last frame
    // DAC output-limiting clip diagnostic (galvo_out.cpp galvoTask()): counts
    // points whose pre-clamp DAC code fell outside [dac_limit_min, dac_limit_max]
    // for the last fully-consumed frame, i.e. saturation the optimizer/pattern
    // layer is blind to (clamping happens after they've already run). Diagnostic
    // only -- nothing reads these to auto-correct anything.
    std::atomic<uint32_t> dacClipCountX     {0};      // points clamped on X, last frame
    std::atomic<uint32_t> dacClipCountY     {0};      // points clamped on Y, last frame
    std::atomic<uint32_t> dacClipCountAny   {0};      // points clamped on X and/or Y, last frame (for dacClipPct)
    std::atomic<uint32_t> dacClipTotalPts   {0};      // total points in that frame
    std::atomic<uint32_t> dmx_frame_count   {0};
    std::atomic<uint32_t> last_dmx_ms       {0};
    // Text Mode ran out of frame-buffer budget and dropped part of the
    // string/glyph -- see textrender::wasTruncated(). Surfaced on
    // /api/status as text_truncated so the WebUI can warn instead of the
    // laser just silently drawing an incomplete word.
    std::atomic<bool>     text_truncated    {false};
    // UI Override: WebUI takes priority over DMX/Art-Net when active
    // ui_master_dimmer is always applied; ui_override also blocks DMX source
    std::atomic<bool>     ui_override       {false};  // true = ignore DMX, use WebUI
    std::atomic<uint8_t>  ui_master_dimmer  {0};      // 0 = follow DMX CH1, 1-255 = forced
    // Refreshed (millis() + margin) on every chunk of a large static HTTP
    // response (e.g. index.html.gz on a hard reload) -- see web_ui.cpp's
    // serveIndexGz. Lets other internal-DRAM-sensitive subsystems (the
    // EtherDream discovery beacon) skip non-essential allocations while a
    // transfer is actively pressuring the shared lwIP pool, without
    // touching the safety-critical HEAP_CRITICAL failsafe itself.
    std::atomic<uint32_t> heavy_io_until_ms {0};
    // calibration pattern mode (less time-critical, volatile is sufficient)
    volatile bool         calib_active      = false;
    volatile uint8_t      calib_idx         = 0;
    volatile uint8_t      calib_bright      = 255;   // WebUI slider removed; Master Dimmer is now the sole intensity control
    volatile uint8_t      calib_channel     = 0;

    // Basiswert-Kalibrierung ("Visibility threshold" test beam): static
    // low-level beam, bypasses gain/gamma/dimmer entirely -- see
    // galvo_out.cpp galvoTask() and mapVisibleRange(). Toggled by the
    // Start/Stop button in the Calib tab's Parameter card.
    volatile bool          calib_thresh_test = false;
    volatile uint8_t       calib_thresh_ch   = 0;      // 0=RGB,1=R,2=G,3=B
    // Three Circles gain-matching pattern: skip mapVisibleRange() so gain
    // changes are not masked by the threshold floor. Set by /api/calib-pattern
    // when idx==6, cleared on stop or any other pattern selection.
    volatile bool          calib_no_thresh   = false;
    // Color-ramp linearity patterns (calibRampR/G/B, idx 18-20): skip
    // dimmer/thermal-scale/gain AND gamma AND threshold entirely -- the
    // commanded duty must equal the PWM duty with nothing in between. See
    // galvo_out.cpp galvoTask()'s calib_raw_duty branch. Set by
    // /api/calib-pattern when idx is one of the ramp indices, cleared on
    // stop or any other pattern selection.
    volatile bool          calib_raw_duty    = false;
};

extern RuntimeState gState;

struct WebOverride {
    volatile bool     active = false;
    volatile uint8_t  values[DMX_CHANNELS_USED] = {
        0,    // CH1  DMX_MASTER       (0 = off)
        0,    // CH2  DMX_COLOR        (0 = white)
        0,    // CH3  DMX_COLOR_SPEED
        0,    // CH4  DMX_PATTERN_GROUP
        0,    // CH5  DMX_PATTERN_SELECT
        0,    // CH6  DMX_DYN_EFFECT
        0,    // CH7  DMX_EFFECT_SPEED
        128,  // CH8  DMX_SIZE         (128 = 50% = default size)
        0,    // CH9  DMX_AUTO_SCALE
        0,    // CH10 DMX_ROTATION     (0 = no rotation)
        0,    // CH11 DMX_HFLIP
        0,    // CH12 DMX_VFLIP
        128,  // CH13 DMX_HMOVE        (128 = center)
        128,  // CH14 DMX_VMOVE        (128 = center)
        0,    // CH15 DMX_WAVE_AMP
        0,    // CH16 DMX_WAVE_FREQ
        0,    // CH17 DMX_ILDA_SELECT
        0,    // CH18 DMX_ILDA_SPEED
        128,  // CH19 DMX_ILDA_SIZE    (128 = original size)
        0,    // CH20 DMX_ILDA_LOOP
        255,  // CH21 DMX_ILDA_BRIGHT  (255 = use dimmer)
        0,    // CH22 DMX_ILDA_REPEAT
    };
};

extern WebOverride gOverride;

/* ============================================================
 * Live-Preset-Controls
 * Changed via WebUI in real time -- no restart required.
 * ============================================================ */
// Color animation types (firmware-side, also DMX-controllable)
enum ColAnimType : uint8_t {
    COL_ANIM_OFF      = 0,
    COL_ANIM_GRADIENT = 1,
    COL_ANIM_CHASE    = 2,
    COL_ANIM_STROBE   = 3,
    COL_ANIM_PULSE    = 4,
    COL_ANIM_TWINKLE  = 5,
    COL_ANIM_FLIP     = 6,
    COL_ANIM_SEGMENT  = 7,  // per-point segment coloring with phase travel
};

enum FadeDirection : uint8_t {
    FADE_DIR_IN_OUT     = 0,  // Inside -> Outside
    FADE_DIR_OUT_IN     = 1,  // Outside -> Inside
    FADE_DIR_LEFT_RIGHT = 2,  // Left -> Right
    FADE_DIR_RIGHT_LEFT = 3,  // Right -> Left
    FADE_DIR_TOP_BOTTOM = 4,  // Top -> Bottom
    FADE_DIR_BOTTOM_TOP = 5,  // Bottom -> Top
};

enum AutoScaleMode : uint8_t {
    AUTOSCALE_SMALL_BIG_SMALL = 0, // 0 -> size_val -> 0
    AUTOSCALE_SMALL_BIG       = 1, // 0 -> size_val, then reset
    AUTOSCALE_BIG_SMALL       = 2, // size_val -> 0, then reset
};

// Mirror effect modes -- same set as Paint-by-Finger's mirror brush
// (see data/index.html::paintMirrorPoints()), independent of Kaleidoscope.
enum MirrorMode : uint8_t {
    MIRROR_OFF     = 0,
    MIRROR_X       = 1,  // negate Y -> symmetric across the X axis (top/bottom)
    MIRROR_Y       = 2,  // negate X -> symmetric across the Y axis (left/right)
    MIRROR_RADIAL4 = 3,  // 4-fold quadrant reflection
};

// Kaleidoscope modes (pattern_engine.cpp::applyKaleidoscope)
enum KaleidoMode : uint8_t {
    KALEIDO_MODE_RADIAL = 0,  // plain rotational copy (cyclic group C_S)
    KALEIDO_MODE_MIRROR = 1,  // true dihedral fold+repeat (group D_S)
};

struct LivePresetControls {
    volatile uint8_t  speed        = 0;
    volatile uint8_t  size_val     = 255;
    volatile uint8_t  col_r        = 255;
    volatile uint8_t  col_g        = 0;
    volatile uint8_t  col_b        = 0;
    volatile bool     col_override  = false;
    volatile ColAnimType col_anim_type  = COL_ANIM_OFF;
    volatile uint8_t     col_anim_seq   = 0;    // 0-9 sequence index
    volatile uint8_t     col_anim_speed = 1;    // 0-255 (lowest visible speed, not 0/frozen)
    volatile bool         col_anim_bpm_sync = false; // true = phase locked to BPM clock beat, col_anim_speed ignored
    volatile uint8_t     col_seg_count  = 4;    // 1-10 color segments
    volatile int8_t      col_seg_dir    = 1;    // +1 forward, -1 reverse
    volatile int16_t  rotation   = 0;   // Z-axis (degrees)
    volatile bool     rot_x      = false;  // X-axis active
    volatile bool     rot_y      = false;  // Y-axis active
    volatile bool     rot_z      = false;  // Z-axis — off by default
    volatile float    rot_speed_x = 0.015f;
    volatile float    rot_speed_y = 0.018f;
    volatile float    rot_speed_z = 0.020f;
    volatile float    rot_angle_x = 0.f;
    volatile float    rot_angle_y = 0.f;
    volatile float    rot_angle_z = 0.f;
    volatile uint8_t  trail      = 0;
    volatile uint8_t  pattern_idx = 0;   // encoder: current preset index
    // Wave parameters (apply to all wave patterns #35-52)
    volatile float    wave_amp   = 1.0f;  // 0.1 – 2.0  (amplitude factor)
    volatile float    wave_freq  = 1.0f;  // 0.25 – 4.0 (frequency multiplier)
    // Points-Only render mode (global toggle, all presets)
    volatile bool     points_mode_enabled  = false;
    volatile uint8_t  points_count         = 12;    // 2..POINTS_MODE_MAX_DOTS dots
    volatile bool     points_fade_in_on    = true;  // false = hard on, no ramp
    volatile bool     points_fade_out_on   = true;  // false = hard off, no ramp
    volatile uint16_t points_fade_in_ms    = 400;   // fade-in duration, ms
    volatile uint16_t points_fade_out_ms   = 400;   // fade-out duration, ms
    volatile uint8_t  points_fade_dir      = FADE_DIR_IN_OUT;
    volatile bool     points_static_on     = false; // true = full brightness, no fade cycle
    volatile bool     points_bpm_sync      = false; // true = fade cycle locked to BPM clock beat (blink on beat)
    // Random Points preset (preset_patterns.cpp::p106) -- Amount/Speed
    // reuse size_val/speed above, Duration needed its own field.
    volatile uint16_t random_pts_hold_ms   = 500;   // 50..5000, hold time per dot, ms
    // Bouncing Points preset (preset_patterns.cpp::p_bouncing)
    volatile uint8_t  bp_trail_len     = 6;     // 0=off, 1..12 trail ghost steps
    volatile bool     bp_endless        = true;  // true=loop forever, false=time-limited
    volatile uint16_t bp_duration_sec   = 30;   // 1..90 s, ignored when bp_endless=true
    // Endless Spiral preset (preset_patterns.cpp::p_endless_spiral)
    volatile uint8_t  spiral_arms        = 2;    // 1..6 spiral arms
    // Endless Tunnel preset (preset_patterns.cpp::p_endless_tunnel)
    volatile uint8_t  tunnel_rings       = 6;    // 3..12 concentric rings
    volatile uint8_t  tunnel_sides       = 4;    // 3..10 polygon sides (0-ish maps to circle)
    // Explosion Spread preset (preset_patterns.cpp::p_explosion)
    volatile uint8_t  explosion_rays     = 16;   // 4..40 rays
    // Fireworks preset (preset_patterns.cpp::p_fireworks)
    volatile uint8_t  fw_max_shells      = 2;    // 1..3 concurrent shells
    volatile bool     fw_glitter         = true; // sparkle overlay on/off
    // Milky Way preset (preset_patterns.cpp::p_milkyway)
    volatile uint8_t  mw_dots            = 60;   // 10..60 star dots
    volatile uint8_t  mw_tilt            = 60;   // 20..80 % projection tilt (top-down slant)
    // Kaleidoscope effect (global toggle, Preset + Curve mode)
    volatile bool     kaleido_enabled   = false;
    volatile uint8_t  kaleido_mode      = KALEIDO_MODE_MIRROR;
    volatile uint8_t  kaleido_segments  = 4;      // 2..6, even only
    volatile bool     kaleido_mirror_h  = false;  // Radial mode only: alternate segments flip X
    volatile bool     kaleido_mirror_v  = false;  // Radial mode only: alternate segments flip Y
    // Mirror effect (separate from Kaleidoscope) -- Off/X/Y/Radial4
    volatile uint8_t  mirror_mode = MIRROR_OFF;
    // Auto-Scaling: oscillates size between 0 and size_val, speed-driven
    volatile uint8_t  autoscaleSpeed  = 0;   // 0..100%, 0 = off
    volatile uint8_t  autoscaleMode   = AUTOSCALE_SMALL_BIG_SMALL;
    volatile float    autoscalePhase  = 0.f; // internal running phase 0..1
    // Per-segment colors for line-based presets (polygons, stars, etc.)
    volatile bool    seg_colors_enabled = false;
    volatile uint8_t seg_col_r[10]      = {255,0,0,255,0,255,255,255,128,0};
    volatile uint8_t seg_col_g[10]      = {0,255,0,255,255,0,128,255,0,128};
    volatile uint8_t seg_col_b[10]      = {0,0,255,0,255,255,0,128,255,255};
};

extern LivePresetControls gLivePreset;

/* ============================================================
 * Text configuration
 * ============================================================ */
enum TextFont   : uint8_t { FONT_SIMPLE=0, FONT_BOLD=1, FONT_OUTLINE=2 };
enum TextAnim   : uint8_t {
    TANIM_STATIC=0,    TANIM_SCROLL_L=1,  TANIM_SCROLL_R=2,
    TANIM_BOUNCE=3,    TANIM_TYPEWRITER=4, TANIM_WAVE=5,
    TANIM_PULSE=6,     TANIM_ROTATE=7,    TANIM_ZOOM=8,
    TANIM_3D_EXT=9,    TANIM_ORBIT=10,    TANIM_STARWARS=11
};

struct TextConfig {
    char      text[128]   = {0};
    TextFont  font        = FONT_SIMPLE;
    TextAnim  animation   = TANIM_SCROLL_L;
    uint8_t   speed       = 80;
    uint8_t   size_val    = 128;
    uint8_t   col_r       = 255;
    uint8_t   col_g       = 255;
    uint8_t   col_b       = 255;
    bool      rainbow     = false;
    bool      flip_x      = false;   // mirror text horizontally (negate X)
    bool      flip_y      = false;   // mirror text vertically   (negate Y)
    bool      orbit_reverse = false; // Orbit anim: spin the sphere the other way
    volatile bool      active      = false;   // text mode active (overrides preset + DMX)
};

extern TextConfig gTextConfig;

/* ============================================================
 * ILDA SD-card player configuration
 * defined in ilda_player.cpp as ilda::gILDA
 * ============================================================ */
// -> #include "ilda/ilda_player.h" for access


/* ============================================================
 * Safety configuration (Feature 5) — temperature-based
 * ============================================================ */
struct SafetyConfig {
    uint8_t  temp_warn_c     = 45;   // C → fan 100%
    uint8_t  temp_reduce_c   = 55;   // C → laser power 50%
    uint8_t  temp_shutdown_c = 70;   // C → immediate shutdown
    uint8_t  fan_min_pct     = 15;   // % minimum PWM for startup
    bool     fan_auto        = true; // automatic fan speed
};
extern SafetyConfig gSafety;

/* ============================================================
 * Playlist-entry (Feature 4)
 * ============================================================ */
#define PLAYLIST_MAX_ENTRIES  32
struct PlaylistEntry {
    uint8_t  file_idx;      // SD-file-Index
    uint8_t  loop_count;    // 0 = infinite loop
    uint16_t pause_ms;      // pause after this entry
};
struct PlaylistConfig {
    bool          active      = false;
    bool          loop_all    = true;
    uint8_t       count       = 0;
    uint8_t       current     = 0;
    PlaylistEntry entries[PLAYLIST_MAX_ENTRIES];
};
extern PlaylistConfig gPlaylist;

/* ============================================================
 * Preset Sequencer -- BPM-synced preset playlist (sequencer.cpp)
 * ============================================================ */
#define SEQUENCER_MAX_STEPS 32
struct SequencerStep {
    uint8_t presetIdx       = 0;     // index into presets::PRESETS[]
    uint8_t beats           = 4;     // step duration in beats (UI offers 1/2/4/8/16/32)
    uint8_t transitionBeats = 0;     // blank window before next step, 0 = hard cut
    bool    enabled         = true;
};
struct SequencerConfig {
    SequencerStep steps[SEQUENCER_MAX_STEPS];
    uint8_t       stepCount   = 0;
    uint8_t       currentStep = 0;
    bool          running     = false;
    bool          loop        = true;
};
extern SequencerConfig gSequencer;

// ── Mathematical Curve Mode ──────────────────────────────────────────────────
struct CurveConfig {
    int8_t  active_curve = -1;             // -1 = off, 0..8 = curve index
    struct Params {
        float   p[5];
        uint8_t r, g, b;
    } params[9];                           // one set per curve
    bool    initialized = false;
};
extern CurveConfig gCurves;

// ── Paint-by-Finger Canvas ───────────────────────────────────────────────────
// Freeform/shape drawing composed in the WebUI, projected as an optimized
// point cloud (see paint_patterns.cpp). Vertices are pre-simplified
// client-side; count/stroke_count guard iteration so a partially-filled
// canvas never reads stale geometry. Written via /api/paint/set, guarded by
// mtx::paint (dedicated mutex -- same write-tear fix pattern as gZone).
#define PAINT_STROKES_MAX      12   // max strokes/shapes per canvas
#define PAINT_VERTS_PER_STROKE 96   // max vertices per stroke (client-simplified)

struct PaintStroke {
    uint16_t count  = 0;             // vertices used in x[]/y[]
    bool     closed = false;         // true = polygon (rect/triangle/circle), false = open path
    uint8_t  r = 255, g = 255, b = 255;
    float    x[PAINT_VERTS_PER_STROKE];
    float    y[PAINT_VERTS_PER_STROKE];
};

struct PaintConfig {
    volatile bool    active       = false;  // Paint mode active (overrides curve+preset+DMX)
    volatile uint8_t stroke_count = 0;      // strokes used in strokes[]
    PaintStroke      strokes[PAINT_STROKES_MAX];
};
// Reference, not object: the ~9 KB canvas is placement-new'd into PSRAM at
// static-init time (see main.cpp) instead of costing DRAM .bss. Safe because
// CONFIG_SPIRAM_BOOT_INIT registers the PSRAM heap before global ctors run.
extern PaintConfig& gPaint;

// ── Laser Welding Effect ─────────────────────────────────────────────────────
// Alternative renderer of the same gPaint stroke list (see weld_patterns.cpp):
// a bright torch head travels the drawn path, trailing an afterglow and throwing
// ballistic sparks. RAM-only live control (not persisted to NVS), same as the
// other Paint live controls. Active only while gPaint.active && gWeld.enabled.
#define WELD_SPARK_COUNT_MIN  4
#define WELD_SPARK_COUNT_MAX 10

enum WeldDirection : uint8_t { WELD_DIR_FORWARD = 0, WELD_DIR_REVERSE = 1, WELD_DIR_PINGPONG = 2 };

struct WeldConfig {
    volatile bool     enabled       = false;
    volatile uint8_t  direction     = WELD_DIR_FORWARD;
    volatile uint16_t speed_units   = 6000;   // path units per second
    volatile uint16_t glow_units    = 4000;   // afterglow length in path units
    volatile uint8_t  spark_count   = 6;
    volatile uint16_t spark_life_ms = 260;
    volatile uint8_t  head_r  = 255, head_g  = 255, head_b = 255;  // white hot
    volatile uint8_t  glow_r  = 255, glow_g  = 0,   glow_b = 0;    // cooling red
    volatile uint8_t  spark_r = 255, spark_g = 255, spark_b = 0;   // yellow
};
extern WeldConfig gWeld;

// ── Projection & Galvo Rate Configuration ───────────────────────────────────
struct ProjectionConfig {
    // Galvo sample rate — user-adjustable at runtime
    uint16_t galvo_kpps          = 20;     // 12..60 kpps (kilo-points-per-second)

    // Galvo rated speed (kpps) from the datasheet, measured at the ILDA test
    // angle (±8° optical). This is the physical capability of the scanner and
    // the basis for deriving PPS-dependent optimizer parameters (interior
    // density + velocity/acceleration clamps) -- see liveOptimizerConfig().
    // Distinct from galvo_kpps, which is the *chosen* output rate: the ratio
    // (galvo_rated_kpps / galvo_kpps) is what scales the derived params, so at
    // full-rate output (galvo_kpps == galvo_rated_kpps) the ratio is 1 and the
    // tuned base values are used unchanged.
    uint16_t galvo_rated_kpps    = 15;     // 1..100 kpps, datasheet value (UI default GALVO-15K)

    // Galvo angular specs (mechanical half-angle in degrees)
    float    scan_angle_mech_deg = 25.0f;  // galvo mechanical half-angle (full sweep ±25° = 50°)
    float    exit_angle_deg      = 20.0f;  // housing aperture half-angle (often smaller)
    float    ilda_test_angle_deg = 8.0f;   // ILDA rating angle (standard = ±8° optical)

    // Per-channel laser power (actual module output in mW)
    float    power_r_mw          = 1000.f; // Red   638 nm — V(λ)=0.235
    float    power_g_mw          = 1000.f; // Green 520 nm — V(λ)=0.710
    float    power_b_mw          = 3000.f; // Blue  445 nm — V(λ)=0.040, B(λ)=0.220 (!)

    // Wavelength-dependent factors (IEC 60825-1)
    // V(λ): luminous efficiency  — for photometric power and white balance
    // B(λ): blue-light hazard    — for photochemical retinal injury (peaks ~445 nm)
    static constexpr float V_R = 0.235f;  // V(638 nm)
    static constexpr float V_G = 0.710f;  // V(520 nm)
    static constexpr float V_B = 0.040f;  // V(445 nm)
    static constexpr float B_R = 0.000f;  // B(638 nm) — negligible
    static constexpr float B_G = 0.001f;  // B(520 nm) — very low
    static constexpr float B_B = 0.220f;  // B(445 nm) — HIGH: photochemical hazard!

    // Projection geometry
    float    distance_m          = 3.0f;   // throw distance to projection surface (m)

    // Derived (calculated, not stored): visible power and hazard power
    float visPowerMw() const {
        return power_r_mw * V_R + power_g_mw * V_G + power_b_mw * V_B;
    }
    float totalPowerMw() const { return power_r_mw + power_g_mw + power_b_mw; }
    float blueLightHazardMw() const {
        return power_r_mw * B_R + power_g_mw * B_G + power_b_mw * B_B;
    }
    // Auto white balance: gain values (0-255) to equalise visible output
    void autoWhiteBalance(uint8_t& gr, uint8_t& gg, uint8_t& gb) const {
        float vr = power_r_mw * V_R;
        float vg = power_g_mw * V_G;
        float vb = power_b_mw * V_B;
        float weakest = vr < vg ? (vr < vb ? vr : vb) : (vg < vb ? vg : vb);
        if (weakest < 0.001f) { gr = gg = gb = 255; return; }
        gr = (uint8_t)((weakest / vr) * 255.f + 0.5f);
        gg = (uint8_t)((weakest / vg) * 255.f + 0.5f);
        gb = (uint8_t)((weakest / vb) * 255.f + 0.5f);
    }
};
extern ProjectionConfig gProjection;
// ── Projection Zone (touch-defined safe scan area) ──────────────────────────
// User-defined polygon. Lit points outside the polygon are blanked in the
// galvo output path (laser OFF, mirror position retained). Coordinates are
// signed galvo units (-32767..+32767), same space as LaserPoint.x/y.
#define ZONE_POINTS_MAX 16

struct ZoneConfig {
    volatile bool    enabled = false;            // master clip on/off
    volatile uint8_t count   = 4;                // active vertex count (3..ZONE_POINTS_MAX)
    int16_t          x[ZONE_POINTS_MAX] = { -24000,  24000,  24000, -24000 };
    int16_t          y[ZONE_POINTS_MAX] = { -24000, -24000,  24000,  24000 };

    // Ray-casting point-in-polygon test (integer, IRAM-safe, no float).
    // Returns true if (px,py) lies inside the active polygon.
    bool IRAM_ATTR contains(int16_t px, int16_t py) const {
        uint8_t c = count; if (c < 3) return true;   // <3 pts = no clipping
        bool inside = false;
        for (uint8_t i = 0, j = c - 1; i < c; j = i++) {
            int32_t yi = y[i], yj = y[j];
            if ((yi > py) != (yj > py)) {
                // x-coordinate of the edge at scanline py
                int64_t xint = (int64_t)(x[j] - x[i]) * (py - yi);
                int64_t yd   = (yj - yi);
                // px < xi + (xj-xi)*(py-yi)/(yj-yi)  -> cross multiply, keep sign
                if (yd > 0) { if ((int64_t)(px - x[i]) * yd < xint) inside = !inside; }
                else        { if ((int64_t)(px - x[i]) * yd > xint) inside = !inside; }
            }
        }
        return inside;
    }
};
extern ZoneConfig gZone;

// ── Camera Closed-Loop Keystone (geometric warp correction) ─────────────────
// N x N control-point grid mapping normalized [-1..1] pattern-space
// coordinates onto normalized [-1..1] projected-surface coordinates via
// bilinear interpolation between the surrounding four control points. N=2
// is exactly a 4-corner quad warp (classic "keystone" correction); N>2 adds
// interior control points for barrel/pincushion-style correction, same
// bilinear code path. Applied in the optimizer pipeline right after the
// Transform stage (see point_optimizer.cpp's pipeline-order comment), so
// resample/corner-dwell/blank-jump geometry all see the warped shape.
#define WARP_GRID_MAX 5

struct WarpConfig {
    volatile bool    enabled  = false;
    volatile uint8_t gridSize = 2;    // 2..WARP_GRID_MAX
    // points[row][col] = target position (normalized [-1..1]) for the
    // control point whose IDENTITY position is
    //   (-1 + col*2/(gridSize-1), -1 + row*2/(gridSize-1)).
    // Only the top-left gridSize x gridSize block is meaningful; the rest of
    // this fixed-size array is unused padding. Grid is tiny (max 5*5*2
    // floats) -> plain DRAM member, no PSRAM needed.
    float points[WARP_GRID_MAX][WARP_GRID_MAX][2];

    WarpConfig() { resetIdentity(); }

    // Rebuilds points[][][] as the identity grid for the CURRENT gridSize.
    void resetIdentity() {
        uint8_t n = gridSize;
        if (n < 2) n = 2;
        if (n > WARP_GRID_MAX) n = WARP_GRID_MAX;
        for (uint8_t r = 0; r < WARP_GRID_MAX; r++) {
            for (uint8_t c = 0; c < WARP_GRID_MAX; c++) {
                points[r][c][0] = (n > 1) ? (-1.0f + (2.0f * c) / (n - 1)) : 0.0f;
                points[r][c][1] = (n > 1) ? (-1.0f + (2.0f * r) / (n - 1)) : 0.0f;
            }
        }
    }
};
extern WarpConfig gWarp;

// ── Per-Segment Brightness Compensation (Prompt 7c) ──────────────────────────
// N x N gain grid -- same dimensions/gridSize bound (WARP_GRID_MAX) and
// normalized [-1..1] space as WarpConfig above, so it reuses warp::
// sampleGrid()'s bilinear cell/weight computation (see warpGrid.h) instead of
// duplicating it. Independent grid STATE from gWarp though: this corrects
// projector throw-distance/angle vignetting (a radiometric effect -- scan
// speed, and therefore exposure per unit path length, varies across the
// surface), warp corrects geometric keystone -- two unrelated physical
// effects that only happen to share the same grid-editor math/UX.
struct BrightnessConfig {
    volatile bool    enabled  = false;
    volatile uint8_t gridSize = 2;    // 2..WARP_GRID_MAX
    // gain[row][col]: 0..255 maps to 0.0..1.0, default 255 (identity, no
    // attenuation). Only the top-left gridSize x gridSize block is
    // meaningful. uint8_t (not float, unlike WarpConfig::points) -- this is
    // a straight multiplier on an already-8-bit RGB channel, no precision is
    // gained by storing it wider.
    uint8_t gain[WARP_GRID_MAX][WARP_GRID_MAX];

    BrightnessConfig() { resetIdentity(); }
    void resetIdentity() {
        for (uint8_t r = 0; r < WARP_GRID_MAX; r++)
            for (uint8_t c = 0; c < WARP_GRID_MAX; c++)
                gain[r][c] = 255;
    }
};
extern BrightnessConfig gBrightness;

// ── Model-Based Inverse Filtering / Galvo Deconvolution (Prompt 12b) ────────
// Per-axis regularized inverse of the galvo's measured 2nd-order mechanical
// resonance, pre-applied to the emitted point stream so the OPTICAL output
// tracks the commanded trajectory instead of ringing at it. See
// docs/feature-prompts/DECISIONS.md, Prompt 12b for the full model/
// discretization derivation and rationale.
//
// Deliberately NOT the same fields as Pillar 3's ring_freq_hz/
// ring_damping_ratio (point_optimizer.h): those are a single value SHARED
// across X/Y, used only to time the ZV-shaped blank-jump. This is a genuine
// per-axis trajectory pre-filter applied to every emitted point -- the two
// galvos are physically distinct and can have different resonance, and the
// two mechanisms (input-shaped blank jump vs. full inverse-filtered
// trajectory) are independent, both still gate-able separately.
struct InverseFilterAxisModel {
    // Measured undamped natural frequency (Hz) / damping ratio (0..~0.9).
    // wnHz <= 0 means "unmeasured" -- that axis is passed through unfiltered
    // even when InverseFilterConfig::enabled is true.
    float wnHz = 0.0f;
    float zeta = 0.0f;
};

struct InverseFilterConfig {
    volatile bool enabled = false;
    // Regularization: sets the inverse filter's rolloff corner as a multiple
    // of wn (see inverseFilter.cpp -- H_inv(s)'s denominator is
    // (1 + regAlpha*s/wn)^2). Smaller = stronger correction of the fastest
    // dynamics but more high-frequency gain (noise/jitter amplification);
    // larger = safer/weaker. The resulting filter is BIBO-stable for any
    // regAlpha > 0 regardless of zeta -- see inverseFilter.cpp's header
    // comment for why.
    float regAlpha = 0.35f;
    InverseFilterAxisModel x;
    InverseFilterAxisModel y;
};
extern InverseFilterConfig gInverseFilter;