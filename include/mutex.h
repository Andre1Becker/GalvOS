#pragma once
/**
 * mutex.h — Zentrale Mutex-Definitionen for thread-sichere Zugriffe
 *
 * All global state objects (gConfig, gState, gLivePreset, etc.)
 * MUST be accessed via the corresponding guard macros.
 *
 * Verwendung:
 *   { GRD_CONFIG auto _g = lock_config();  gConfig.gain_r = x; }
 *   { GRD_STATE  auto _g = lock_state();   gState.master_dimmer = x; }
 *   { GRD_SD                               sd_card::scanFiles(); }
 */
#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace mtx {

// Mutex-Handles — in main.cpp initialisiert
extern SemaphoreHandle_t config;   // gConfig, gSafety
extern SemaphoreHandle_t state;    // gState, gLivePreset, gTextConfig
extern SemaphoreHandle_t sd;       // SD card (all SD.open / scanFiles)
extern SemaphoreHandle_t preview;  // gPreview, Websocket-Frame

void init();  // create all mutexes

// RAII-Guard: automatisch sperren/entsperren
struct Guard {
    SemaphoreHandle_t mx;
    bool ok;
    Guard(SemaphoreHandle_t m, TickType_t timeout = pdMS_TO_TICKS(50))
        : mx(m), ok(xSemaphoreTake(m, timeout) == pdTRUE) {}
    ~Guard() { if (ok) xSemaphoreGive(mx); }
    explicit operator bool() const { return ok; }
};

} // namespace mtx

// Convenience-Makros
#define LOCK_CONFIG()  mtx::Guard _cfg_guard(mtx::config)
#define LOCK_STATE()   mtx::Guard _st_guard(mtx::state)
#define LOCK_SD()      mtx::Guard _sd_guard(mtx::sd)
#define LOCK_PREVIEW() mtx::Guard _pv_guard(mtx::preview)
