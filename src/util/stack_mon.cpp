#include "stack_mon.h"
#include <esp_log.h>
#include "util/log_buffer.h"

namespace stackMon {

static const char* TAG = "stack_mon";
static constexpr size_t      MAX_TASKS  = 16;
static constexpr UBaseType_t WARN_WORDS = 256;  // 256 * 4B = 1024B free

struct Entry {
    TaskHandle_t handle;
    const char*  name;
    bool         warned;
};

static Entry  sTasks[MAX_TASKS];
static size_t sCount = 0;

void watch(TaskHandle_t h, const char* name) {
    if (!h || sCount >= MAX_TASKS) return;
    sTasks[sCount++] = { h, name, false };
}

void update() {
    for (size_t i = 0; i < sCount; i++) {
        Entry& e = sTasks[i];
        if (e.warned) continue;
        UBaseType_t words = uxTaskGetStackHighWaterMark(e.handle);
        if (words < WARN_WORDS) {
            e.warned = true;
            unsigned freeBytes = (unsigned)(words * sizeof(StackType_t));
            ESP_LOGW(TAG, "%s stack margin low: %u B free", e.name, freeBytes);
            LOG_W(logbuf::CAT_SYSTEM, "%s stack low: %u B free", e.name, freeBytes);
        }
    }
}

} // namespace stackMon