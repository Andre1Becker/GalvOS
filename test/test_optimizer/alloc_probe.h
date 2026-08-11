#pragma once
/**
 * alloc_probe.h -- counting allocator shim for CONTRACT.md invariant 6
 * (allocFreeSymmetric).
 *
 * Implemented with the GNU linker's --wrap mechanism (see the -Wl,--wrap
 * flags in platformio.ini's [env:native]): every malloc()/free() call made
 * from the objects on the link line -- including point_optimizer.cpp's
 * host-side heap_caps_malloc() fallback -- is routed through __wrap_malloc /
 * __wrap_free below, which count it and forward to the real CRT.
 *
 * Counting is off until arm(true) so unrelated startup / Unity / iostream
 * traffic never lands in a measured window. Keep armed windows tight: arm,
 * run the optimizer, disarm, then assert.
 */

#include <stddef.h>

namespace allocProbe {

// Zeroes live/total and disarms. Does not clear active().
void reset();

// Start / stop counting. Only calls made while armed are counted.
void arm(bool on);

// Outstanding allocations counted since the last reset() (allocations minus
// frees). Stays at 0 for a subsystem that owns no persistent heap.
long live();

// Cumulative successful allocations counted since the last reset().
unsigned long total();

// Make the next n armed malloc() calls return nullptr, simulating heap
// exhaustion. Used to exercise the optimizer's out-of-memory fallbacks,
// which is where the scratch-buffer bookkeeping is actually load-bearing.
void failNext(int n);

// True once __wrap_malloc() has been reached at least one time. False means
// the --wrap link flags are missing and every count is meaningless -- the
// tests assert on this rather than reporting a false pass.
bool active();

}  // namespace allocProbe
