#pragma once
/**
 * ps_scratch.h -- lazy PSRAM replacement for large `static` scratch arrays.
 *
 * Large function-local `static` buffers land in DRAM .bss and permanently
 * shrink the internal heap (v6.03.0 shipped with ~120 KB of them and only
 * ~21 KB free heap). psScratch() moves such a buffer to PSRAM on first use:
 * the pointer itself stays a small static, the storage is allocated once
 * and never freed -- identical lifetime to the static array it replaces.
 *
 * calloc semantics reproduce the zero-initialization .bss provided, so
 * state arrays guarded by "static bool seeded" flags keep their contract.
 * Falls back to internal heap if PSRAM is unavailable (e.g. gDebugNoHW on
 * a PSRAM-less devkit); callers must handle nullptr (allocation failed on
 * both heaps) by skipping the frame rather than crashing.
 */
#include <esp_heap_caps.h>
#include <stdlib.h>

template <typename T>
static inline T* psScratch(T*& slot, size_t count) {
    if (!slot) {
        slot = (T*)heap_caps_calloc(count, sizeof(T), MALLOC_CAP_SPIRAM);
        if (!slot) slot = (T*)calloc(count, sizeof(T));
    }
    return slot;
}
