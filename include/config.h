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
constexpr uint8_t  POINTS_MODE_MAX_DOTS  = 80;  // UI slider ceiling
constexpr uint8_t  POINTS_MODE_MIN_DWELL = 3;   // ticks; below this a dot is invisible
constexpr uint8_t  POINTS_MODE_MAX_DWELL = 30;  // ticks; cap so few dots don't hog the whole frame

// Random Points preset (preset_patterns.cpp::p106)
constexpr uint8_t  RANDOM_PTS_MAX_COUNT = 14;   // UI slider ceiling ("Amount")

// Kaleidoscope effect (pattern_engine.cpp::applyKaleidoscope)
constexpr uint8_t  KALEIDO_SEGMENTS_MAX = 16;  // UI slider ceiling

// WebUI output-parameter preview animation (Point Optimizer, Galvo
// Calibration live, Pattern Parameters tabs). Static UI layout constant --
// mirrored by hand in data/index.html as --param-preview-size, same
// convention as the POINTS_MODE_* constants above (no live API sync).
constexpr uint8_t  UI_PARAM_PREVIEW_SIZE = 80;  // px, square

// GalvOS v5 Point Optimizer (Pillar 1) -- runtime-tunable via WebUI slider.
// Mirrors optimizer::OptimizerConfig field-for-field; kept as a separate
// struct here (rather than including point_optimizer.h) to avoid pulling
// the optimizer's geometry types into every translation unit that already
// includes config.h.
//
// DEFAULT VALUES: tuned for 30kpps output rate (GALVO_SAMPLE_RATE_HZ=45000).
// max_pts_per_frame=750 -> 30000/1010 = 30 Hz mostly flicker-free floor.
// All OPT_DEFAULT_* macros are the single source of truth; point_optimizer.h
// references them so both structs stay in sync automatically.
#define OPT_DEFAULT_CORNER_ANGLE_DEG            25.0f
#define OPT_DEFAULT_MIN_CORNER_PTS              2
#define OPT_DEFAULT_MAX_CORNER_PTS              8
#define OPT_DEFAULT_PTS_PER_1000_UNITS          6.0f
#define OPT_DEFAULT_MIN_SEGMENT_PTS             2
#define OPT_DEFAULT_BLANK_SAMPLES               16
#define OPT_DEFAULT_MAX_PTS_PER_FRAME           1010
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

struct OptimizerLiveConfig {
    float    corner_angle_deg             = OPT_DEFAULT_CORNER_ANGLE_DEG;
    uint8_t  min_corner_pts               = OPT_DEFAULT_MIN_CORNER_PTS;
    uint8_t  max_corner_pts               = OPT_DEFAULT_MAX_CORNER_PTS;
    float    pts_per_1000_units           = OPT_DEFAULT_PTS_PER_1000_UNITS;
    uint8_t  min_segment_pts              = OPT_DEFAULT_MIN_SEGMENT_PTS;
    uint8_t  blank_samples                = OPT_DEFAULT_BLANK_SAMPLES;
    uint16_t max_pts_per_frame            = OPT_DEFAULT_MAX_PTS_PER_FRAME;
    uint8_t  min_blank_samples            = OPT_DEFAULT_MIN_BLANK_SAMPLES;
    float    blank_pts_per_1000_units     = OPT_DEFAULT_BLANK_PTS_PER_1000_UNITS;
    uint8_t  min_interior_pts_per_segment = OPT_DEFAULT_MIN_INTERIOR_PTS_PER_SEG;
    uint8_t  stage1_blank_target          = OPT_DEFAULT_STAGE1_BLANK_TARGET;
    bool     resample_enabled             = OPT_DEFAULT_RESAMPLE_ENABLED;
    float    resample_spacing_units       = OPT_DEFAULT_RESAMPLE_SPACING_UNITS;
    bool     ringing_comp_enabled         = OPT_DEFAULT_RINGING_COMP_ENABLED;
    float    ring_freq_hz                 = OPT_DEFAULT_RING_FREQ_HZ;
    float    ring_damping_ratio           = OPT_DEFAULT_RING_DAMPING_RATIO;
    bool     vel_clamp_enabled            = OPT_DEFAULT_VEL_CLAMP_ENABLED;
    float    max_step_units               = OPT_DEFAULT_MAX_STEP_UNITS;
    bool     accel_clamp_enabled          = OPT_DEFAULT_ACCEL_CLAMP_ENABLED;
    float    max_accel_units              = OPT_DEFAULT_MAX_ACCEL_UNITS;
};

extern OptimizerLiveConfig gOptimizerConfig;

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

enum ControlSource : uint8_t {
    SRC_NONE = 0, SRC_DMX, SRC_ARTNET, SRC_ETHERDREAM,
    SRC_HELIOS, SRC_INTERNAL, SRC_WEBUI
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
    std::atomic<uint32_t> dmx_frame_count   {0};
    std::atomic<uint32_t> last_dmx_ms       {0};
    // UI Override: WebUI takes priority over DMX/Art-Net when active
    // ui_master_dimmer is always applied; ui_override also blocks DMX source
    std::atomic<bool>     ui_override       {false};  // true = ignore DMX, use WebUI
    std::atomic<uint8_t>  ui_master_dimmer  {0};      // 0 = follow DMX CH1, 1-255 = forced
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
    MIRROR_X       = 1,  // flip horizontal (negate X)
    MIRROR_Y       = 2,  // flip vertical (negate Y)
    MIRROR_RADIAL4 = 3,  // 4-fold rotational copy, no reflection
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
    volatile uint8_t     col_anim_speed = 128;  // 0-255
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
    volatile uint8_t  points_count         = 24;    // 2..POINTS_MODE_MAX_DOTS dots
    volatile bool     points_fade_in_on    = true;  // false = hard on, no ramp
    volatile bool     points_fade_out_on   = true;  // false = hard off, no ramp
    volatile uint16_t points_fade_in_ms    = 400;   // fade-in duration, ms
    volatile uint16_t points_fade_out_ms   = 400;   // fade-out duration, ms
    volatile uint8_t  points_fade_dir      = FADE_DIR_IN_OUT;
    volatile bool     points_static_on     = false; // true = full brightness, no fade cycle
    // Random Points preset (preset_patterns.cpp::p106) -- Amount/Speed
    // reuse size_val/speed above, Duration needed its own field.
    volatile uint16_t random_pts_hold_ms   = 500;   // 50..5000, hold time per dot, ms
    // Kaleidoscope effect (global toggle, Preset + Curve mode)
    volatile bool     kaleido_enabled   = false;
    volatile uint8_t  kaleido_segments  = 6;      // 2..KALEIDO_SEGMENTS_MAX
    volatile bool     kaleido_mirror_h  = false;  // alternate segments: flip X
    volatile bool     kaleido_mirror_v  = false;  // alternate segments: flip Y
    // Mirror effect (separate from Kaleidoscope) -- Off/X/Y/Radial4
    volatile uint8_t  mirror_mode = MIRROR_OFF;
    // Auto-Scaling: oscillates size between 0 and size_val, speed-driven
    volatile uint8_t  autoscaleSpeed  = 0;   // 0..100%, 0 = off
    volatile uint8_t  autoscaleMode   = AUTOSCALE_SMALL_BIG_SMALL;
    volatile float    autoscalePhase  = 0.f; // internal running phase 0..1
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
extern PaintConfig gPaint;

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