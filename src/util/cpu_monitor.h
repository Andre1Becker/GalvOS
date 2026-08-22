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

// Measured idle time as a percentage of the last window, per core. This is
// the raw quantity load0()/load1() are derived from (load = 100 - idle),
// published so a load figure can be checked against its own measurement
// instead of taken on faith -- see the note in cpu_monitor.cpp.
float idlePercent(uint8_t core);

// SoC internal temperature sensor (°C). NAN until the first ~2s reading.
float cpuTemp();

} // namespace cpu_mon
