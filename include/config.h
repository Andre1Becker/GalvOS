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

// GalvOS v5 Point Optimizer (Pillar 1) -- runtime-tunable via WebUI slider.
// Mirrors optimizer::OptimizerConfig field-for-field; kept as a separate
// struct here (rather than including point_optimizer.h) to avoid pulling
// the optimizer's geometry types into every translation unit that already
// includes config.h.
struct OptimizerLiveConfig {
    float   corner_angle_deg   = 25.0f;
    uint8_t min_corner_pts     = 1;
    uint8_t max_corner_pts     = 6;
    float   pts_per_1000_units = 4.0f;
    uint8_t min_segment_pts    = 2;
    uint8_t blank_samples      = 40;
};
extern OptimizerLiveConfig gOptimizerConfig;

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

    DMX_CHANNELS_USED = 22  // total: 22 DMX channels
};

// DMX channel names (for WebUI and documentation)
static const char* DMX_CHANNEL_NAMES[22] = {
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
    "Wave Amplitude",     // 15
    "Wave Frequency",      // 16
    "ILDA File (0-40)",    // 17  ← ILDA
    "ILDA Speed",           // 18
    "ILDA Size",         // 19
    "ILDA Loop",            // 20
    "ILDA Brightness",      // 21
    "ILDA Frame Repeat",    // 22
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
    bool      gamma_enable = true;   // gamma correction γ=2.2
    float     gamma_val    = 2.2f;   // adjustable 1.0–3.0 via WebUI

    uint16_t  scanfail_timeout_ms = 50;
    uint16_t  watchdog_period_ms  = 500;

    char      wifi_ssid[33] = {0};
    char      wifi_pass[65] = {0};
    char      hostname[32]  = "galvOS";

    // NTP
    char      ntp_server[64] = "pool.ntp.org";
    char      ntp_tz[48]     = "UTC0";           // POSIX TZ string

    // Safety
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
    std::atomic<uint32_t> points_per_sec    {0};
    std::atomic<uint32_t> dmx_frame_count   {0};
    std::atomic<uint32_t> last_dmx_ms       {0};
    // UI Override: WebUI takes priority over DMX/Art-Net when active
    // ui_master_dimmer is always applied; ui_override also blocks DMX source
    std::atomic<bool>     ui_override       {false};  // true = ignore DMX, use WebUI
    std::atomic<uint8_t>  ui_master_dimmer  {0};      // 0 = follow DMX CH1, 1-255 = forced
    // calibration pattern mode (less time-critical, volatile is sufficient)
    volatile bool         calib_active      = false;
    volatile uint8_t      calib_idx         = 0;
    volatile uint8_t      calib_bright      = 200;
    volatile uint8_t      calib_channel     = 0;
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

// Pattern buffer snapshot for preview (atomically filled by pattern task)
struct PreviewSnapshot {
    SemaphoreHandle_t mux;
    LaserPoint        points[512];   // sampled subset
    size_t            count;
};

extern PreviewSnapshot gPreview;

/* ============================================================
 * Live-Preset-Controls
 * Changed via WebUI in real time -- no restart required.
 * ============================================================ */
struct LivePresetControls {
    volatile uint8_t  speed      = 80;
    volatile uint8_t  size_val   = 128;
    volatile uint8_t  col_r      = 255;
    volatile uint8_t  col_g      = 255;
    volatile uint8_t  col_b      = 255;
    volatile bool     col_override = false;
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
    volatile bool     mirror_x   = false;
    volatile bool     mirror_y   = false;
    volatile uint8_t  trail      = 0;
    volatile uint8_t  pattern_idx = 0;   // encoder: current preset index
    // Wave parameters (apply to all wave patterns #35-52)
    volatile float    wave_amp   = 1.0f;  // 0.1 – 2.0  (amplitude factor)
    volatile float    wave_freq  = 1.0f;  // 0.25 – 4.0 (frequency multiplier)
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
    bool      active      = false;   // text mode active (overrides preset + DMX)
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

// ── Projection & Galvo Rate Configuration ───────────────────────────────────
struct ProjectionConfig {
    // Galvo sample rate — user-adjustable at runtime
    uint16_t galvo_kpps          = 20;     // 12..60 kpps (kilo-points-per-second)

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
