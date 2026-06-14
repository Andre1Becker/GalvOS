#pragma once
/**
 * ilda_player.h -- ILDA file player
 *
 * Unterstuetzte Formate:
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

/* Konfiguration des Abspielers */
struct ILDAConfig {
    int8_t    file_idx   = -1;     // -1 = no ILDA active, 0-39 = file index
    uint8_t   speed      = 128;    // Abspielgeschwindigkeit 0-255 (128 = normal)
    uint8_t   size_val   = 128;    // scaling 0-255
    bool      loop       = true;   // Endlos-Wiederholen
    bool      active     = false;  // ILDA mode active
    // Statistik (read-only)
    uint16_t  total_frames = 0;
    uint16_t  current_frame = 0;
    uint32_t  total_points  = 0;
};

extern ILDAConfig gILDA;

/* Lifecycle */
void init();
bool loadFile(uint8_t idx);  // Laedt file in PSRAM, startet Playback-Task
void stop();
void pause(bool paused);

/* Get next frame (called by pattern_engine task) */
size_t getFrame(LaserPoint* out, size_t max_pts);
bool   hasNewFrame();         // true if neuer Frame bereit

/* DMX-gesteuerte fileauswahl
 * dmx_val 0   = ILDA off
 * dmx_val 1-40 = file 1-40 (0-basiert: idx = dmx_val - 1)
 * dmx_val 255  = letzte availablee file
 */
void setFromDMX(uint8_t dmx_val);

} // namespace ilda
