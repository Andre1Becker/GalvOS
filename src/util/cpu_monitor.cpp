/**
 * cpu_monitor.cpp
 *
 * Per-core CPU load, measured as real idle TIME via the FreeRTOS idle hook
 * (esp_register_freertos_idle_hook_for_cpu()).
 * update() is called from web_ui::task() every ~1s.
 * init() is called once at the top of setup().
 */
#include "cpu_monitor.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_freertos_hooks.h>
#include <esp_cpu.h>
#include <esp_log.h>
#include <Arduino.h>
#include <math.h>

static const char* TAG = "cpu_mon";

namespace cpu_mon {

// ── Why cycles and not idle-hook invocations (v6.83.0) ───────────────────
// The original implementation counted idle-hook CALLS per window and divided
// by a baseline count captured once at startup. Two independent defects:
//
// 1. The baseline was the RAW count of the first window, treated as if that
//    window were 500 ms. init() runs at the top of setup() but the first
//    update() comes from web_ui::task, created only after LittleFS/SD/DAC
//    bring-up and the blocking WiFi connect -- so the baseline window was
//    seconds long and its count inflated by exactly that ratio. Every later
//    reading was then 1 - (500 ms of idle)/(several seconds of idle), a
//    number set by how long boot happened to take. Measured on this board: a
//    genuinely idle Core 0 (disarmed, no preset, dimmer 0) reported 73%, and
//    the whole true 0-100% range was compressed into roughly 73-100%.
//
// 2. Even with a correct baseline, the invocation COUNT is not proportional
//    to idle time, because the cost of one idle-loop iteration is not
//    constant: it is served from the flash cache, which both cores share.
//    Measured live on this board, same idle state, minutes apart: 437 vs 869
//    invocations/ms -- a 2x swing in the supposed reference, i.e. cache
//    pressure alone moves the reading by ~50 percentage points.
//
// Accumulating the CCOUNT delta between consecutive hook calls fixes both:
// it measures elapsed idle time directly, in absolute units, so there is no
// reference to calibrate and iteration cost drops out entirely.
//
// Deltas larger than kIdleGapMaxCycles are dropped -- those are the
// boundaries where the idle task was preempted, and counting them would
// score another task's runtime as idle. The trade-off is that a preemption
// SHORTER than that threshold is also counted as idle: the threshold is set
// well above the normal idle-loop iteration period (~1-3 us here, so a
// cache-slow iteration is never mistaken for preemption) and well below the
// FreeRTOS tick, which puts the residual error at a fraction of a percent
// and biases it towards under-reporting load rather than inventing it.
// Interrupts serviced during an idle stretch are likewise counted as idle;
// they are short, and no cheaper measurement distinguishes them.
static constexpr uint32_t kIdleGapMaxCycles = 20 * 240;  // ~20 us @ 240 MHz

static volatile uint32_t s_idleCycles[2] = {0, 0};
static uint32_t          s_lastCcount[2] = {0, 0};
static volatile uint8_t  s_load[2]       = {0, 0};
static float             s_idlePct[2]    = {0.0f, 0.0f};
static uint32_t          s_snap_ms   = 0;
static bool              s_inited    = false;
static float             s_cpu_temp  = NAN;
static uint32_t          s_temp_ms   = 0;

static inline bool IRAM_ATTR idleHook(int core) {
    uint32_t c = esp_cpu_get_ccount();
    uint32_t d = c - s_lastCcount[core];      // wraps cleanly; deltas are tiny
    s_lastCcount[core] = c;
    if (d < kIdleGapMaxCycles) s_idleCycles[core] += d;
    return false;                              // idle task may still sleep
}

static bool IRAM_ATTR hook0() { return idleHook(0); }
static bool IRAM_ATTR hook1() { return idleHook(1); }

void init() {
    if (s_inited) return;
    s_inited = true;

    esp_register_freertos_idle_hook_for_cpu(hook0, 0);
    esp_register_freertos_idle_hook_for_cpu(hook1, 1);

    s_idleCycles[0] = s_idleCycles[1] = 0;
    s_snap_ms = millis();
    ESP_LOGI(TAG, "CPU monitor hooks registered");
}

void update() {
    if (!s_inited) init();

    uint32_t now = millis();

    // SoC internal temp sensor — cheap but not free (start/stop toggles the
    // analog block), so it runs on its own slower cadence independent of the
    // CPU-load window below.
    if (now - s_temp_ms >= 2000) {
        s_temp_ms  = now;
        s_cpu_temp = temperatureRead();
    }

    uint32_t dt = now - s_snap_ms;
    if (dt < 500) return;    // minimum window

    uint32_t idle0 = s_idleCycles[0];
    uint32_t idle1 = s_idleCycles[1];
    s_idleCycles[0] = s_idleCycles[1] = 0;
    s_snap_ms = now;

    // Cycles available per core over the window. getCpuFrequencyMhz() is read
    // per update rather than cached: the load figure must stay correct if the
    // clock is ever changed at runtime.
    const float windowCycles = (float)dt * (float)getCpuFrequencyMhz() * 1000.0f;
    if (windowCycles <= 0.0f) return;

    const uint32_t idle[2] = { idle0, idle1 };
    for (int c = 0; c < 2; c++) {
        float frac = (float)idle[c] / windowCycles;
        if (frac > 1.0f) frac = 1.0f;          // rounding / clock-change slack
        s_idlePct[c] = frac * 100.0f;
        int load = (int)lroundf((1.0f - frac) * 100.0f);
        s_load[c] = (uint8_t)(load < 0 ? 0 : load > 100 ? 100 : load);
    }
}

float idlePercent(uint8_t core) { return core < 2 ? s_idlePct[core] : 0.0f; }

uint8_t load0() { return s_load[0]; }
uint8_t load1() { return s_load[1]; }
float   cpuTemp() { return s_cpu_temp; }

} // namespace cpu_mon
