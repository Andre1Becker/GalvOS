#pragma once
#include <stdint.h>

/**
 * countdown_timer.h
 * Shared countdown timer state — used by both the galvo pattern renderer
 * (preset_patterns.cpp / p100) and the HTTP API (web_ui.cpp).
 */
namespace countdown_timer {

void     set(uint32_t seconds);   // Load time, stop if running
void     start();                 // Begin counting down
void     pause();                 // Freeze without resetting
void     stop();                  // Stop + reset to 0
void     reset(uint32_t seconds); // Set new time, stop

void     tick();                  // Call every ~100 ms from any task

uint32_t remaining();             // Seconds left
bool     running();               // True while counting
bool     expired();               // True when reached 0 naturally

} // namespace countdown_timer
