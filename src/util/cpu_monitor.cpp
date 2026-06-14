/**
 * cpu_monitor.cpp
 *
 * CPU load via esp_register_freertos_idle_hook_for_cpu().
 * update() is called from web_ui::task() every ~1s.
 * init() is called once after the scheduler is running (from web_ui::init
 * or the first update() call).
 */
#include "cpu_monitor.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_freertos_hooks.h>
#include <esp_log.h>
#include <Arduino.h>

static const char* TAG = "cpu_mon";

namespace cpu_mon {

static volatile uint32_t s_idle[2]   = {0, 0};
static volatile uint8_t  s_load[2]   = {0, 0};
static uint32_t          s_base[2]   = {0, 0};
static uint32_t          s_snap_ms   = 0;
static bool              s_inited    = false;
static bool              s_calibrated = false;

static bool hook0() { s_idle[0]++; return false; }
static bool hook1() { s_idle[1]++; return false; }

void init() {
    if (s_inited) return;
    s_inited = true;

    esp_register_freertos_idle_hook_for_cpu(hook0, 0);
    esp_register_freertos_idle_hook_for_cpu(hook1, 1);

    // Reset counters and let them accumulate for 600ms before first update()
    s_idle[0] = s_idle[1] = 0;
    s_snap_ms = millis();
    ESP_LOGI(TAG, "CPU monitor hooks registered");
}

void update() {
    if (!s_inited) init();

    uint32_t now = millis();
    uint32_t dt  = now - s_snap_ms;
    if (dt < 500) return;    // minimum window

    uint32_t cnt0 = s_idle[0];
    uint32_t cnt1 = s_idle[1];
    s_idle[0] = s_idle[1] = 0;
    s_snap_ms = now;

    if (!s_calibrated) {
        // First window after init — use as baseline (system mostly idle)
        if (cnt0 > 20 && dt >= 500) {
            s_base[0] = cnt0;
            s_base[1] = cnt1;
            s_calibrated = true;
            ESP_LOGI(TAG, "Calibrated: base0=%lu base1=%lu dt=%lu ms",
                     (unsigned long)cnt0, (unsigned long)cnt1, (unsigned long)dt);
        }
        // Don't report load yet
        return;
    }

    // Scale baseline to actual window
    float scale = (float)dt / 500.0f;
    auto clamp = [](int v) -> uint8_t {
        return (uint8_t)(v < 0 ? 0 : v > 100 ? 100 : v);
    };

    float b0 = s_base[0] * scale;
    float b1 = s_base[1] * scale;
    if (b0 < 1) b0 = 1;
    if (b1 < 1) b1 = 1;

    s_load[0] = clamp((int)((1.0f - cnt0/b0) * 100.0f));
    s_load[1] = clamp((int)((1.0f - cnt1/b1) * 100.0f));
}

uint8_t load0() { return s_load[0]; }
uint8_t load1() { return s_load[1]; }

} // namespace cpu_mon
