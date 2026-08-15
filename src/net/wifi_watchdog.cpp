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

static volatile uint32_t s_last_ok_ms          = 0;
static volatile bool     s_ever_ok             = false;  // true once any gateway ping has ever replied
static volatile uint32_t s_soft_recovery_count = 0;
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

uint32_t softRecoveryCount() { return s_soft_recovery_count; }

void task(void*) {
    s_last_ok_ms = millis();  // don't trip before the first check ever ran
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(20000));  // check every 20s

        if (!gConfig.wifi_watchdog_reboot_enabled) {
            s_last_ok_ms = millis();
            continue;
        }

        bool ok = (WiFi.status() == WL_CONNECTED) && pingGatewayOnce();
        if (ok) {
            s_last_ok_ms = millis();
            s_ever_ok = true;
            continue;
        }

        if (!s_ever_ok) {
            // Never had a working link yet (boot, AP-only fallback, no
            // configured SSID in range) -- indefinite tolerance, there is
            // nothing to "recover" back to. Keep the age timer from
            // accumulating so this state alone can never trigger a reboot.
            s_last_ok_ms = millis();
            continue;
        }

        // age deliberately does NOT reset just because WiFi.status() flips
        // away from WL_CONNECTED here (e.g. mid soft-reconnect below) --
        // only a genuine successful ping (the `ok` branch above) resets it,
        // so a soft reconnect that never actually comes back still reaches
        // the hard-reboot tier instead of resetting the clock forever.
        uint32_t age = millis() - s_last_ok_ms;

        // Tier 1: soft WiFi-level reconnect. Re-associates the STA link
        // only -- laser stays armed, pattern engine/DMX/Art-Net keep
        // running uninterrupted. Retried every cycle (~20s, this loop's own
        // pace) rather than once -- a single attempt can plausibly fail to
        // re-associate outright on a marginal link (this is what actually
        // happened live: the one-shot version left the radio fully
        // disconnected -- worse than the zombie state it started from --
        // for the rest of the outage instead of continuing to try), and
        // repeated attempts are exactly what WiFi.setAutoReconnect(true)
        // would already be doing if the driver had noticed the drop itself.
        if (age >= gConfig.wifi_watchdog_soft_timeout_ms &&
            age < gConfig.wifi_watchdog_timeout_ms) {
            s_soft_recovery_count++;
            ESP_LOGW(TAG, "Gateway unreachable for %u ms -- soft WiFi reconnect (attempt #%u)",
                     (unsigned)age, (unsigned)s_soft_recovery_count);
            LOG_W(logbuf::CAT_WIFI, "Gateway unreachable %us -- soft WiFi reconnect",
                  age / 1000);
            WiFi.disconnect();
            WiFi.reconnect();
            continue;
        }

        // Tier 2: soft reconnect didn't restore reachability -- true last
        // resort, per the project's own "no auto-reboot mid-show except as
        // an absolute last measure" rule.
        if (age >= gConfig.wifi_watchdog_timeout_ms) {
            ESP_LOGE(TAG, "Gateway unreachable for %u ms (limit %u) -- failsafe reboot",
                     (unsigned)age, (unsigned)gConfig.wifi_watchdog_timeout_ms);
            LOG_E(logbuf::CAT_WIFI, "Gateway unreachable %us -- failsafe reboot", age / 1000);
            safety::failsafeReboot("GATEWAY_UNREACHABLE");
        }
    }
}

} // namespace wifi_watchdog
