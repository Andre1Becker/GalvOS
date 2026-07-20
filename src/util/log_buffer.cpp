#include "log_buffer.h"
#include "../net/ntp_client.h"
#include "mem_registry.h"
#include <Arduino.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace logbuf {

static LogEntry*         s_ring   = nullptr;
static volatile size_t   s_head   = 0;   // next Schreib-Index
static volatile size_t   s_count  = 0;   // Count vorhandener entries
static SemaphoreHandle_t s_mux    = nullptr;

static const char* LEVEL_STR[] = { "INFO", "WARN", "ERROR", "DEBUG" };
static const char* CAT_STR[]   = { "SYS", "SAFETY", "DMX", "TEMP", "GALVO", "WIFI", "USER" };

void init() {
    s_mux  = xSemaphoreCreateMutex();
    s_ring = (LogEntry*)ps_malloc(LOG_CAPACITY * sizeof(LogEntry));
    if (!s_ring) {
        ESP_LOGE("logbuf", "PSRAM alloc failed for Log-Buffer");
        return;
    }
    memreg::track("Log Buffer", LOG_CAPACITY * sizeof(LogEntry), true);
    memset(s_ring, 0, LOG_CAPACITY * sizeof(LogEntry));
    // Ersten entry direkt schreiben
    log(LVL_INFO, CAT_SYSTEM, "Log buffer initialized (%u entries, PSRAM)",
        (unsigned)LOG_CAPACITY);
}

void log(LogLevel level, LogCat cat, const char* fmt, ...) {
    if (!s_ring || !s_mux) return;

    LogEntry entry;
    entry.ts_ms    = millis();
    entry.level    = level;
    entry.category = cat;

    va_list args;
    va_start(args, fmt);
    vsnprintf(entry.msg, LOG_MSG_LEN, fmt, args);
    va_end(args);

    // Also mirror to ESP log
    switch (level) {
        case LVL_WARN:  ESP_LOGW(CAT_STR[cat], "%s", entry.msg); break;
        case LVL_ERROR: ESP_LOGE(CAT_STR[cat], "%s", entry.msg); break;
        default:        ESP_LOGI(CAT_STR[cat], "%s", entry.msg); break;
    }

    if (xSemaphoreTake(s_mux, pdMS_TO_TICKS(5)) != pdTRUE) return;

    s_ring[s_head] = entry;
    s_head = (s_head + 1) % LOG_CAPACITY;
    if (s_count < LOG_CAPACITY) s_count++;

    xSemaphoreGive(s_mux);
}

size_t toJson(char* buf, size_t buf_len, uint32_t after_ts, size_t max_entries) {
    if (!s_ring || !s_mux || buf_len < 16) return 0;

    if (xSemaphoreTake(s_mux, pdMS_TO_TICKS(50)) != pdTRUE) {
        strlcpy(buf, "[]", buf_len);
        return 0;
    }

    size_t written = 0;
    size_t count = 0;
    char* p = buf;
    char* end = buf + buf_len - 2;

    // Startposition: aeltester entry
    size_t start = (s_count < LOG_CAPACITY) ? 0 : s_head;

    *p++ = '[';
    bool first = true;

    for (size_t i = 0; i < s_count && count < max_entries; i++) {
        size_t idx = (start + i) % LOG_CAPACITY;
        const LogEntry& e = s_ring[idx];

        if (e.ts_ms < after_ts) continue;   // already seen (strict <, so last entry is not re-sent)

        // Estimate whether space is sufficient (~200 bytes per entry)
        if (p + 220 >= end) break;

        if (!first) *p++ = ',';
        first = false;

        // Escape msg (einfache Version: " → \")
        char escaped[LOG_MSG_LEN * 2];
        const char* src = e.msg;
        char* dst = escaped;
        while (*src && dst - escaped < (int)sizeof(escaped) - 2) {
            if (*src == '"')      { *dst++ = '\\'; *dst++ = '"'; }
            else if (*src == '\\') { *dst++ = '\\'; *dst++ = '\\'; }
            else if (*src == '\n') { *dst++ = '\\'; *dst++ = 'n'; }
            else                   { *dst++ = *src; }
            src++;
        }
        *dst = 0;

        // Wall-clock time if NTP synced
        char wall[10] = "--:--:--";
        if (ntp_client::isSynced()) {
            time_t epoch = ntp_client::nowEpoch();
            // Adjust for entry age: entry was at millis() - e.ts_ms ago
            long age_s = (long)((millis() - e.ts_ms) / 1000UL);
            time_t entry_epoch = epoch - age_s;
            struct tm t;
            localtime_r(&entry_epoch, &t);
            snprintf(wall, sizeof(wall), "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
        }

        int n = snprintf(p, end - p,
            "{\"ts\":%lu,\"wall\":\"%s\",\"lvl\":\"%s\",\"cat\":\"%s\",\"msg\":\"%s\"}",
            (unsigned long)e.ts_ms,
            wall,
            LEVEL_STR[e.level < 4 ? e.level : 0],
            CAT_STR[e.category < 7 ? e.category : 0],
            escaped);

        if (n > 0 && p + n < end) { p += n; count++; }
        else break;
    }

    *p++ = ']';
    *p   = 0;
    written = count;

    xSemaphoreGive(s_mux);
    return written;
}

size_t count() { return s_count; }
bool   isFull() { return s_count >= LOG_CAPACITY; }
void   clear()  {
    if (xSemaphoreTake(s_mux, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_head = s_count = 0;
        xSemaphoreGive(s_mux);
    }
}

}  // namespace logbuf
