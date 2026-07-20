#include "mem_registry.h"
#include <string.h>

namespace memreg {

static Owner  s_owners[MAX_OWNERS];
static size_t s_count = 0;

static int find(const char* name) {
    for (size_t i = 0; i < s_count; i++) {
        if (strcmp(s_owners[i].name, name) == 0) return (int)i;
    }
    return -1;
}

void track(const char* name, size_t bytes, bool psram) {
    int idx = find(name);
    if (idx >= 0) {
        s_owners[idx].bytes = bytes;
        s_owners[idx].psram = psram;
        return;
    }
    if (s_count >= MAX_OWNERS) return;
    s_owners[s_count++] = { name, bytes, psram };
}

void untrack(const char* name) {
    int idx = find(name);
    if (idx < 0) return;
    s_owners[idx] = s_owners[s_count - 1];
    s_count--;
}

size_t count() { return s_count; }

const Owner& get(size_t i) { return s_owners[i]; }

}  // namespace memreg
