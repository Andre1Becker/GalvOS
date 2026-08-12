#pragma once
/**
 * ilda_player.h -- ILDA file player
 *
 * Supported formats:
 *   Format 0: 3D Indexed Color  (8 Byte/Punkt)
 *   Format 1: 2D Indexed Color  (6 Byte/Punkt)  ← frequentlyster
 *   Format 4: 3D True Color     (10 Byte/Punkt)
 *   Format 5: 2D True Color     (8 Byte/Punkt)  ← morner default
 *
 * Priority in the render pipeline: highest (above text, preset, DMX)
 */
#include "config.h"
#include <stdint.h>
#include <stddef.h>

namespace ilda {

/* player configuration */
struct ILDAConfig {
    int8_t    file_idx   = -1;     // -1 = no ILDA active, 0-39 = file index
    uint8_t   speed      = 128;    // playback speed 0-255 (128 = normal)
    uint8_t   size_val   = 128;    // scaling 0-255
    bool      loop       = true;   // repeat indefinitely
    bool      active     = false;  // ILDA mode active
    bool      enabled    = true;   // master on/off -- while false, loadFile()/setFromDMX() are no-ops
    bool      invert_x   = false;  // mirror X axis
    bool      invert_y   = false;  // mirror Y axis
    bool      col_override = false; // replace file's own colors with col_r/g/b
    uint8_t   col_r = 255, col_g = 255, col_b = 255;
    // P22: re-shape the file's own blank runs with Pillar 2/3's smoothstep-
    // ease(+ZV) trajectory (optimizer::reshapeBlankRuns()) instead of
    // playing them back exactly as authored. Session-only, like every other
    // field on this struct -- no NVS key, no backup/community-preset schema
    // entry (see Session M/P18: ILDA's optimizer-adjacent config is
    // deliberately hand-built, not routed through configFromLive()). ZV
    // coefficients (ring_freq_hz/ring_damping_ratio/ringing_comp_enabled)
    // still come from the single Optimizer-tab profile (gOptimizerConfig)
    // -- this bool is only the on/off gate. Default false -> existing ILDA
    // playback is byte-identical until opted in.
    bool      blank_reshape_enabled = false;
    // Statistik (read-only)
    uint16_t  total_frames = 0;
    uint16_t  current_frame = 0;
    uint32_t  total_points  = 0;
};

extern ILDAConfig gILDA;

/* Lifecycle */
void init();
bool loadFile(uint8_t idx);  // loads file into PSRAM, starts playback task
const char* errorMsg();      // reason the last loadFile() failed (valid after loadFile() returns false)
void stop();
void pause(bool paused);
bool isPaused();

/* Master enable/disable -- disabling force-stops current playback and
 * makes loadFile()/setFromDMX() no-ops until re-enabled. */
void setEnabled(bool en);

/* Get next frame (called by pattern_engine task) */
size_t getFrame(LaserPoint* out, size_t max_pts);
bool   hasNewFrame();         // true if a new frame is ready

/* DMX-controlled file selection
 * dmx_val 0    = ILDA off
 * dmx_val 1-40 = file 1-40 (0-based: idx = dmx_val - 1)
 * dmx_val 255  = last available file
 */
void setFromDMX(uint8_t dmx_val);

} // namespace ilda
