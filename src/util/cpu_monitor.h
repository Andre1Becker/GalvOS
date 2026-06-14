#pragma once
#include <stdint.h>

namespace cpu_mon {

// Call once from setup() after scheduler has started
void init();

// Call periodically (every ~500 ms) from a task to update measurements
// Returns immediately — non-blocking
void update();

// Returns load 0..100 for each core (last measurement window)
uint8_t load0();   // Core 0 — network / DMX / UI
uint8_t load1();   // Core 1 — galvo output (usually near 100% by design)

} // namespace cpu_mon
