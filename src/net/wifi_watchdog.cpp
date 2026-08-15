#include "wifi_watchdog.h"
#include "config.h"
#include "safety/safety.h"
#include "util/log_buffer.h"
#include <Arduino.h>
#include <WiFi.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <ping/ping_sock.h>
#include <lwip/ip_addr.h>

static const char* TAG = "wifi_wd";

namespace wifi_watchdog {

static volatile uint32_t s_last_ok_ms = 0;
static SemaphoreHandle_t s_reply_sem  = nullptr;

static void onPingSuccess(esp_ping_handle_t, void*) {
    s_last_ok_ms = millis();
    if (s_reply_sem) xSemaphoreGive(s_reply_sem);
}

// Pings the current default gateway once, blocking up to ~1.5s for a reply.
// Returns true on reply. False on timeout, no gateway, or session error.
static bool pingGatewayOnce() {
    IPAddress gwIp = WiFi.gatewayIP();
    if (gwIp == IPAddress(0, 0, 0, 0)) return false;

    ip_addr_t target;
    if (!ipaddr_aton(gwIp.toString().c_str(), &target)) return false;

    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.target_addr = target;
    cfg.count       = 1;
    cfg.timeout_ms  = 1000;

    esp_ping_callbacks_t cbs = {};
    cbs.on_ping_success = onPingSuccess;

    esp_ping_handle_t ping;
    if (esp_ping_new_session(&cfg, &cbs, &ping) != ESP_OK) return false;

    if (!s_reply_sem) s_reply_sem = xSemaphoreCreateBinary();
    xSemaphoreTake(s_reply_sem, 0);  // drain any stale signal

    esp_ping_start(ping);
    bool got = xSemaphoreTake(s_reply_sem, pdMS_TO_TICKS(cfg.timeout_ms + 500)) == pdTRUE;

    esp_ping_stop(ping);
    esp_ping_delete_session(ping);
    return got;
}

uint32_t msSinceLastReply() {
    if (s_last_ok_ms == 0) return UINT32_MAX;
    return millis() - s_last_ok_ms;
}

void task(void*) {
    s_last_ok_ms = millis();  // don't trip before the first check ever ran
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(20000));  // check every 20s

        // Disabled, or nothing to ping yet (boot / AP-only fallback / mid
        // reconnect) -- keep the timer fresh so re-enabling or reconnecting
        // doesn't immediately fire on stale age.
        if (!gConfig.wifi_watchdog_reboot_enabled || WiFi.status() != WL_CONNECTED) {
            s_last_ok_ms = millis();
            continue;
        }

        if (pingGatewayOnce()) continue;  // s_last_ok_ms updated by callback

        uint32_t age = millis() - s_last_ok_ms;
        if (age >= gConfig.wifi_watchdog_timeout_ms) {
            ESP_LOGE(TAG, "Gateway unreachable for %u ms (limit %u) -- failsafe reboot",
                     (unsigned)age, (unsigned)gConfig.wifi_watchdog_timeout_ms);
            LOG_E(logbuf::CAT_WIFI, "Gateway unreachable %us -- failsafe reboot", age / 1000);
            safety::failsafeReboot("GATEWAY_UNREACHABLE");
        }
    }
}

} // namespace wifi_watchdog
