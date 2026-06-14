#include "ntp_client.h"
#include "config.h"
#include "util/log_buffer.h"
#include <Arduino.h>
#include <WiFi.h>
#include <esp_sntp.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <time.h>
#include <string.h>

static const char* TAG = "ntp";

namespace ntp_client {

static volatile bool     s_synced  = false;
static volatile bool     s_inited  = false;   // latches true; never reset
static SemaphoreHandle_t s_mutex   = nullptr;

static void sntp_cb(struct timeval* tv) {
    s_synced = true;
    char buf[24];
    formatTime(buf, sizeof(buf), true);
    ESP_LOGI(TAG, "NTP synced: %s UTC", buf);
    LOG_I(logbuf::CAT_WIFI, "NTP synced: %s UTC", buf);
}

void init() {
    if (WiFi.status() != WL_CONNECTED) return;

    // Create mutex on first call
    if (!s_mutex) s_mutex = xSemaphoreCreateMutex();

    // Serialise concurrent calls (boot path + watchdog task race)
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(200)) != pdTRUE) return;

    if (s_inited) {
        // Already running — safe to restart sync without touching operating mode
        xSemaphoreGive(s_mutex);
        if (!s_synced) {
            esp_sntp_restart();
            ESP_LOGI(TAG, "NTP restart (re-sync)");
        }
        return;
    }

    // First-time init only:
    // Do NOT call esp_sntp_setoperatingmode() — default is already POLL
    // and calling it while SNTP is running triggers an IDF assertion.
    const char* srv = (gConfig.ntp_server[0] != '\0')
                      ? gConfig.ntp_server : "pool.ntp.org";
    esp_sntp_setservername(0, srv);
    sntp_set_time_sync_notification_cb(sntp_cb);
    esp_sntp_init();
    s_inited = true;

    setenv("TZ", gConfig.ntp_tz[0] ? gConfig.ntp_tz : "UTC0", 1);
    tzset();

    xSemaphoreGive(s_mutex);

    ESP_LOGI(TAG, "NTP init: server=%s", srv);
    LOG_I(logbuf::CAT_WIFI, "NTP init: %s", srv);
}

void task(void*) {
    // Wait for potential boot-path init to finish, then monitor sync
    vTaskDelay(pdMS_TO_TICKS(5000));
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(30000));
        if (!s_inited && WiFi.status() == WL_CONNECTED) {
            init();
        } else if (s_inited && !s_synced) {
            // Already inited but not synced yet — trigger a restart safely
            esp_sntp_restart();
        }
    }
}

bool    isSynced()  { return s_synced; }
bool    isInited()  { return s_inited; }

time_t nowEpoch() {
    if (!s_synced) return 0;
    return time(nullptr);
}

void formatTime(char* buf, size_t len, bool date) {
    time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);
    if (date)
        strftime(buf, len, "%Y-%m-%d %H:%M:%S", &t);
    else
        strftime(buf, len, "%H:%M:%S", &t);
}

} // namespace ntp_client
