#pragma once
/**
 * mem_registry.h -- named-owner registry for long-lived heap/PSRAM buffers
 *
 * ESP-IDF has no per-caller heap accounting, so this is manual bookkeeping:
 * each module calls track() once, right after a successful ps_malloc() /
 * heap_caps_malloc() for a buffer that lives for the rest of boot (or until
 * untrack() on free/reload). Powers the WebUI Log tab's memory viewer.
 */

#include <stddef.h>
#include <stdint.h>

namespace memreg {

constexpr size_t MAX_OWNERS = 24;

struct Owner {
    const char* name;
    size_t      bytes;
    bool        psram;
};

// Register (or update, if `name` already exists -- pointer compared by
// string content) a named allocation. Call once per buffer, right after
// the allocation succeeds.
void track(const char* name, size_t bytes, bool psram);

// Remove a previously tracked allocation (e.g. ILDA buffers freed on
// stop/reload) so the viewer doesn't keep counting freed memory.
void untrack(const char* name);

size_t       count();
const Owner& get(size_t i);

}  // namespace memreg
