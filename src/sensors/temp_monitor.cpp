#include "temp_monitor.h"
#include "safety/safety.h"
#include "output/galvo_out.h"
#include "pinmap.h"
#include <Arduino.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <esp_log.h>
#include "util/log_buffer.h"
#include <Preferences.h>

namespace temp {

static const char* TAG = "temp";

// ============================================================
// default-thresholds per sensor
// Laser module strenger, Galvo-Amps toleranter
// ============================================================
TempThresholds thresholds[NUM_SENSORS] = {
    /* SENS_LASER   */ { 38.0f, 45.0f, 52.0f },
    /* SENS_DRIVER  */ { 55.0f, 65.0f, 75.0f },
    /* SENS_GALVO   */ { 60.0f, 72.0f, 85.0f },
    /* SENS_PSU     */ { 55.0f, 65.0f, 78.0f },
    /* SENS_AMBIENT */ { 38.0f, 45.0f, 55.0f },
};

TempState gTempState = {};
float sensor_offset[NUM_SENSORS] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f};

// ============================================================
// 1-Wire + DallasTemperaturee
// ============================================================
static OneWire            s_ow(PIN_ONEWIRE);
static DallasTemperature  s_sensors(&s_ow);
static DeviceAddress      s_addrs[NUM_SENSORS];
static uint8_t            s_found_count = 0;

// sensor names (RAM copy, NVS-persistent)
char sensor_names[NUM_SENSORS][SENSOR_NAME_LEN];

// Fan override: 255 = automatisch, 0-100 = manuell
static uint8_t s_fan_override[2] = {255, 255};

// ============================================================
// sensor names: NVS load / save
// ============================================================
void loadSensorNames() {
    Preferences prefs;
    prefs.begin("temp_names", true);   // read-only
    char key[8];
    bool any_set = false;
    for (uint8_t i = 0; i < NUM_SENSORS; i++) {
        snprintf(key, sizeof(key), "n%u", i);
        String stored = prefs.getString(key, "");
        if (stored.length() > 0) {
            strlcpy(sensor_names[i], stored.c_str(), SENSOR_NAME_LEN);
            any_set = true;
        } else {
            strlcpy(sensor_names[i], DEFAULT_SENSOR_NAMES[i], SENSOR_NAME_LEN);
        }
    }
    prefs.end();
    if (!any_set) {
        // first boot: write default names to NVS
        Preferences prefs_w;
        prefs_w.begin("temp_names", false);
        for (uint8_t i = 0; i < NUM_SENSORS; i++) {
            snprintf(key, sizeof(key), "n%u", i);
            prefs_w.putString(key, DEFAULT_SENSOR_NAMES[i]);
        }
        prefs_w.end();
        ESP_LOGI(TAG, "sensor names initialized (first boot)");
    }
    for (uint8_t i = 0; i < NUM_SENSORS; i++) {
        ESP_LOGI(TAG, "  sensor %u: \"%s\"", i, sensor_names[i]);
    }
}

void setSensorName(uint8_t idx, const char* name) {
    if (idx >= NUM_SENSORS || !name) return;
    strlcpy(sensor_names[idx], name, SENSOR_NAME_LEN);
    Preferences prefs;
    prefs.begin("temp_names", false);
    char key[8];
    snprintf(key, sizeof(key), "n%u", idx);
    prefs.putString(key, name);
    prefs.end();
    ESP_LOGI(TAG, "sensor %u renamed: \"%s\"", idx, name);
}

void loadSensorOffsets() {
    Preferences prefs;
    prefs.begin("temp_off", true);
    for (uint8_t i = 0; i < NUM_SENSORS; i++) {
        char key[6]; snprintf(key, sizeof(key), "o%u", i);
        sensor_offset[i] = prefs.getFloat(key, 0.0f);
    }
    prefs.end();
}

void saveSensorOffsets() {
    Preferences prefs;
    prefs.begin("temp_off", false);
    for (uint8_t i = 0; i < NUM_SENSORS; i++) {
        char key[6]; snprintf(key, sizeof(key), "o%u", i);
        prefs.putFloat(key, sensor_offset[i]);
    }
    prefs.end();
}

void setSensorOffset(uint8_t idx, float offset_c) {
    if (idx >= NUM_SENSORS) return;
    sensor_offset[idx] = constrain(offset_c, -20.0f, 20.0f);
    saveSensorOffsets();
    ESP_LOGI(TAG, "Sensor %u offset set to %.2f C", idx, sensor_offset[idx]);
}

float getSensorOffset(uint8_t idx) {
    if (idx >= NUM_SENSORS) return 0.0f;
    return sensor_offset[idx];
}


uint8_t foundSensorCount() { return s_found_count; }

// ============================================================
// PWM-Fan control
// ESP32-S3: LEDC-Peripherie, 25 kHz (above fan whine)
// ============================================================
static void fanPwmInit() {
    // Arduino Core 2.x (espressif32 6.x) API: ledcSetup + ledcAttachPin
    // channel 0 = Fan 1, channel 1 = Fan 2
    ledcSetup(0, 25000, 8);          // channel 0: 25 kHz, 8-Bit
    ledcSetup(1, 25000, 8);          // channel 1: 25 kHz, 8-Bit
    ledcAttachPin(PIN_FAN1_PWM, 0);  // GPIO → channel 0
    ledcAttachPin(PIN_FAN2_PWM, 1);  // GPIO → channel 1
    // At startup: fans to 100% until first temperature reading
    ledcWrite(0, 255);
    ledcWrite(1, 255);
    gTempState.fan1_duty = 255;
    gTempState.fan2_duty = 255;
}

static void setFanDuty(uint8_t fan, uint8_t duty) {
    uint8_t safe_duty = (duty == 0) ? 0 : max((uint8_t)38, duty);
    if (fan == 0) {
        ledcWrite(0, safe_duty);   // channel 0 = Fan 1
        gTempState.fan1_duty = safe_duty;
    } else {
        ledcWrite(1, safe_duty);   // channel 1 = Fan 2
        gTempState.fan2_duty = safe_duty;
    }
}

void setFanOverride(uint8_t fan, uint8_t percent) {
    if (fan > 1) return;
    s_fan_override[fan] = (percent > 100) ? 255 : percent;
    ESP_LOGI(TAG, "Fan %u override: %u%%", fan, percent);
}

// ============================================================
// sensor-Scan at startup (determine and log addresses)
// ============================================================
void scanAndLogSensors() {
    // Retry up to 3 times — 1-Wire needs the bus to settle after power-on.
    // Wiring checklist logged on failure:
    //   Data pin = GPIO18, 4.7kΩ pull-up between Data and 3.3V
    //   VCC = 3.3V (NOT 5V), GND = GND
    //   Do NOT use parasite-power mode (VCC must be wired separately)
    for (uint8_t attempt = 0; attempt < 3; attempt++) {
        s_sensors.begin();
        s_found_count = s_sensors.getDeviceCount();
        if (s_found_count > 0) break;
        ESP_LOGW(TAG, "DS18B20 scan attempt %u: 0 sensors — retrying in 500ms", attempt+1);
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    if (s_found_count == 0) {
        ESP_LOGE(TAG, "DS18B20: NO sensors found on GPIO%d!", PIN_ONEWIRE);
        ESP_LOGE(TAG, "  Checklist:");
        ESP_LOGE(TAG, "  [1] 4.7kΩ pull-up between GPIO18 (Data) and 3.3V?");
        ESP_LOGE(TAG, "  [2] DS18B20 VCC pin wired to 3.3V (not 5V)?");
        ESP_LOGE(TAG, "  [3] DS18B20 GND pin wired to GND?");
        ESP_LOGE(TAG, "  [4] Data pin connected to GPIO18?");
        LOG_E(logbuf::CAT_SYSTEM,
              "DS18B20: 0 sensors on GPIO%d — check 4.7k pull-up to 3.3V!", PIN_ONEWIRE);
        return;
    }

    ESP_LOGI(TAG, "DS18B20 Scan: %u sensor(s) found on GPIO%d", s_found_count, PIN_ONEWIRE);
    LOG_I(logbuf::CAT_SYSTEM, "DS18B20: %u sensor(s) found", s_found_count);

    for (uint8_t i = 0; i < min((uint8_t)NUM_SENSORS, s_found_count); i++) {
        if (s_sensors.getAddress(s_addrs[i], i)) {
            ESP_LOGI(TAG, "  sensor %u [%s]: %02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X",
                     i, sensor_names[i],
                     s_addrs[i][0], s_addrs[i][1], s_addrs[i][2], s_addrs[i][3],
                     s_addrs[i][4], s_addrs[i][5], s_addrs[i][6], s_addrs[i][7]);
            s_sensors.setResolution(s_addrs[i], 11);
        }
    }
    ESP_LOGI(TAG, "DS18B20: %u/%u sensors active.", s_found_count, NUM_SENSORS);
}

// ============================================================
// Fan regulation (proportional to highest temperature)
// ============================================================
static uint8_t calcFanDuty() {
    // Use the most critical sensor as control variable
    float worst_ratio = 0.0f;
    for (uint8_t i = 0; i < NUM_SENSORS; i++) {
        if (!gTempState.sensor_ok[i]) continue;
        float t = gTempState.temp_c[i];
        float w = thresholds[i].warn;
        float a = thresholds[i].alert;
        // Ratio: 0.0 at t <= warn, 1.0 at t >= alert
        float ratio = (t - w) / (a - w);
        if (ratio > worst_ratio) worst_ratio = ratio;
    }
    worst_ratio = constrain(worst_ratio, 0.0f, 1.0f);

    // Unter Warn-threshold: 35% minimum speed (quiet but safely running)
    // At warning threshold: 70%
    // At alert threshold: 100%
    uint8_t duty = (uint8_t)(38.0f + worst_ratio * 217.0f);  // 38..255
    return duty;
}

// ============================================================
// main task
// ============================================================
void init() {
    loadSensorNames();
    loadSensorOffsets();
    fanPwmInit();
    scanAndLogSensors();
}

void task(void*) {
    uint32_t last_request = 0;
    bool     conversion_pending = false;

    // DS18B20 run in async mode:
    // requestTemperaturees() starts conversion, 375ms read later
    // This way the task does not block for 375ms at once.

    for (;;) {
        uint32_t now = millis();

        galvo::logDacDebugIfPending();

        // ---- request conversion (every 2 seconds) ----
        if (!conversion_pending && now - last_request >= 2000) {
            s_sensors.setWaitForConversion(false);
            s_sensors.requestTemperatures();
            last_request = now;
            conversion_pending = true;
        }

        // ---- fetch result (375ms after request) ----
        if (conversion_pending && now - last_request >= 400) {
            conversion_pending = false;
            gTempState.any_alert = false;
            gTempState.any_crit  = false;

            for (uint8_t i = 0; i < NUM_SENSORS; i++) {
                float t = DEVICE_DISCONNECTED_C;
                if (i < s_found_count) {
                    t = s_sensors.getTempC(s_addrs[i]);
                }
                bool ok = (t != DEVICE_DISCONNECTED_C && t > -40.0f && t < 120.0f);
                gTempState.sensor_ok[i] = ok;
                gTempState.temp_raw[i]  = ok ? t : 0.0f;
                float t_cal = ok ? (t + sensor_offset[i]) : 0.0f;
                gTempState.temp_c[i]    = t_cal;

                if (ok) {
                    gTempState.warn_active[i]  = (t_cal >= thresholds[i].warn);
                    gTempState.alert_active[i] = (t_cal >= thresholds[i].alert);
                    gTempState.crit_active[i]  = (t_cal >= thresholds[i].crit);

                    if (gTempState.alert_active[i]) {
                        gTempState.any_alert = true;
                        LOG_W(logbuf::CAT_TEMP, "ALERT %s: %.1fC", sensor_names[i], t); ESP_LOGW(TAG, "ALERT %s: %.1fC >= %.1fC",
                                 sensor_names[i], t, thresholds[i].alert);
                    }
                    if (gTempState.crit_active[i]) {
                        gTempState.any_crit = true;
                        LOG_E(logbuf::CAT_TEMP, "KRIT %s: %.1fC", sensor_names[i], t); ESP_LOGE(TAG, "CRITICAL %s: %.1fC >= %.1fC — EMERGENCY SHUTDOWN",
                                 sensor_names[i], t, thresholds[i].crit);
                    }
                } else {
                    // sensor missing = caution: treat as warning
                    gTempState.warn_active[i]  = true;
                    gTempState.alert_active[i] = false;
                    gTempState.crit_active[i]  = false;
                    ESP_LOGD(TAG, "sensor %u unreadable", i);
                }
            }

            gTempState.last_read_ms = now;

            // ---- Protection action ----
            if (gTempState.any_crit) {
                safety::requestArm(false);
                ESP_LOGE(TAG, "Thermal emergency shutdown triggered!");
            } else if (gTempState.any_alert) {
                safety::requestArm(false);
                ESP_LOGW(TAG, "Laser disarmed due to temperature alert");
            }

            // ---- Fan regulation ----
            uint8_t auto_duty = calcFanDuty();
            // on alert/crit: always 100%
            if (gTempState.any_alert || gTempState.any_crit) auto_duty = 255;

            for (uint8_t f = 0; f < 2; f++) {
                if (s_fan_override[f] == 255) {
                    setFanDuty(f, auto_duty);
                } else {
                    setFanDuty(f, (uint8_t)(s_fan_override[f] * 255 / 100));
                }
            }

            // ---- compact status log every 30s + heap watermark ----
            static uint32_t last_log = 0;
            if (now - last_log >= 30000) {
                last_log = now;
                // Heap-warning thresholdn
                const uint32_t heap  = ESP.getFreeHeap();
                const uint32_t psram = ESP.getFreePsram();
                if (heap  < 30000)  ESP_LOGE(TAG, "⚠ LOW HEAP:  %u B", heap);
                if (psram < 200000 && psram > 0)
                                    ESP_LOGW(TAG, "⚠ LOW PSRAM: %u B", psram);
                ESP_LOGI(TAG, "Temps | Laser:%.1f Drv:%.1f Galvo:%.1f PSU:%.1f Amb:%.1f | "
                              "Fan1:%u%% Fan2:%u%% | Heap:%uK PSRAM:%uK",
                         gTempState.temp_c[0], gTempState.temp_c[1],
                         gTempState.temp_c[2], gTempState.temp_c[3],
                         gTempState.temp_c[4],
                         gTempState.fan1_duty * 100 / 255,
                         gTempState.fan2_duty * 100 / 255,
                         heap / 1024, psram / 1024);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

}  // namespace temp
