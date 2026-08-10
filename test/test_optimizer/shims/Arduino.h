#pragma once
/**
 * Arduino.h -- host (native) build shim for the optimizer Contract tests.
 *
 * include/config.h includes <Arduino.h> for its basic integer types only; it
 * declares no Arduino API and calls none. The host Contract build therefore
 * needs nothing more than the standard headers those declarations rely on,
 * which is exactly what this shim provides.
 *
 * Deliberately minimal: if a future config.h change starts using a real
 * Arduino symbol, the host build must fail loudly rather than silently
 * compile against a fake. Do not grow this file into an Arduino emulation --
 * stub the specific symbol in test/optimizer/shims/ instead.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <algorithm>

// ESP-IDF placement attribute: puts a function in internal RAM instead of
// flash. Purely a placement hint, no semantics the host build has to model.
#ifndef IRAM_ATTR
#define IRAM_ATTR
#endif
