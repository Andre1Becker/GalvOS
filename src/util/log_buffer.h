#pragma once
/**
 * log_buffer.h -- PSRAM-basierter Ring-Log seit Boot
 *
 * Stores entries in PSRAM (no LittleFS wear!).
 * Kapazitaet: LOG_CAPACITY entries x ~128 bytes = ~512 KB at 4096 entries.
 * When the buffer is full the oldest entries are overwritten.
 *
 * Kategorien: INFO, WARN, ERROR, TEMP, SAFETY, DMX, GALVO
 */

#include <stdint.h>
#include <stddef.h>

namespace logbuf {

constexpr size_t LOG_CAPACITY    = 2048;   // entries in ring
constexpr size_t LOG_MSG_LEN     = 100;    // Max. Nachrichtenlaenge

enum LogLevel : uint8_t { LVL_INFO=0, LVL_WARN=1, LVL_ERROR=2, LVL_DEBUG=3 };
enum LogCat   : uint8_t { CAT_SYSTEM=0, CAT_SAFETY=1, CAT_DMX=2,
                           CAT_TEMP=3, CAT_GALVO=4, CAT_WIFI=5, CAT_USER=6 };

struct LogEntry {
    uint32_t ts_ms;                 // millis() seit Boot
    LogLevel level;
    LogCat   category;
    char     msg[LOG_MSG_LEN];
};

void init();                        // PSRAM allozieren (nach psramInit!)
void log(LogLevel, LogCat, const char* fmt, ...);

// Convenience-Makros
#define LOG_I(cat, ...) logbuf::log(logbuf::LVL_INFO,  (cat), __VA_ARGS__)
#define LOG_W(cat, ...) logbuf::log(logbuf::LVL_WARN,  (cat), __VA_ARGS__)
#define LOG_E(cat, ...) logbuf::log(logbuf::LVL_ERROR, (cat), __VA_ARGS__)

// JSON-Serialisierung for WebAPI
// Gibt maximal `max_entries` entries ab `after_ts` zurueck
// Gibt Count serialisierter entries zurueck
size_t toJson(char* buf, size_t buf_len, uint32_t after_ts, size_t max_entries);

// Statistik
size_t count();      // Count vorhandener entries
bool   isFull();     // Ring voll?
void   clear();      // Log delete

}  // namespace logbuf
