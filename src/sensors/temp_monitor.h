#pragma once
/**
 * temp_monitor.h -- DS18B20 Temperaturee monitoring + PWM-Fan control
 *
 * Hardware:
 *   - 5× DS18B20 waterproof probes, all on one 1-Wire bus (PIN_ONEWIRE)
 *   - 2× Fan via HW-517 MOSFET module (PWM on PIN_FAN1_PWM, PIN_FAN2_PWM)
 *
 * sensor positions:
 *   0 = Laser module MN-W5000       (most critical point)
 *   1 = Laser driver MN-1W5AT     (Buck-Stages)
 *   2 = Galvo board TDA2030A        (power output stages)
 *   3 = PSU HY-60W             (transformer)
 *   4 = Ambient (enclosure center)     (reference / fan-fail detect)
 *
 * Protection cascade:
 *   >= WARN_C  → fans to 100%, WebUI-Warning
 *   >= ALERT_C → Laser-ARM disarmed (safety::requestArm(false))
 *   >= CRIT_C  → immediate shutdown: safety::emergencyStop()
 */

#include "config.h"
#include <stdint.h>

namespace temp {

// Number of sensors
static constexpr uint8_t NUM_SENSORS = 5;

// sensor index constants
static constexpr uint8_t SENS_LASER   = 0;
static constexpr uint8_t SENS_DRIVER  = 1;
static constexpr uint8_t SENS_GALVO   = 2;
static constexpr uint8_t SENS_PSU     = 3;
static constexpr uint8_t SENS_AMBIENT = 4;

// Names for WebUI / logging — konfigurierbar via WebUI, NVS-persistent
static constexpr uint8_t SENSOR_NAME_LEN = 24;
extern char sensor_names[NUM_SENSORS][SENSOR_NAME_LEN];  // RAM copy

// default names (written to NVS on first boot)
static const char* const DEFAULT_SENSOR_NAMES[NUM_SENSORS] = {
    "Laser driver",
    "Galvo board",
    "Buck-Converter",
    "Ambient",
    "sensor 5"
};

// thresholds per sensor [C]
struct TempThresholds {
    float warn;   // Fan 100%, WebUI-Warning
    float alert;  // ARM disarmed
    float crit;   // immediate shutdown
};

// Configurable thresholds (can be adjusted via WebUI)
extern TempThresholds thresholds[NUM_SENSORS];

// Per-sensor calibration offset (added to raw reading, persisted in NVS)
extern float sensor_offset[NUM_SENSORS];   // default 0.0

// Livedata
struct TempState {
    float    temp_c[NUM_SENSORS];        // Corrected temperature (raw + offset)
    float    temp_raw[NUM_SENSORS];      // Raw reading before offset
    bool     sensor_ok[NUM_SENSORS];     // sensor present and readable
    bool     warn_active[NUM_SENSORS];   // warning threshold exceeded
    bool     alert_active[NUM_SENSORS];  // Alert-threshold exceeded
    bool     crit_active[NUM_SENSORS];   // Kritisch-threshold exceeded
    uint8_t  fan1_duty;                  // Fan 1 Duty-Cycle 0..255
    uint8_t  fan2_duty;                  // Fan 2 Duty-Cycle 0..255
    bool     any_alert;                  // Irgendein Alert active
    bool     any_crit;                   // Irgendein Crit active
    uint32_t last_read_ms;
};

extern TempState gTempState;

// Initialization (after galvo::init() call)
void init();

// FreeRTOS task (Core 0, priority 3)
void task(void*);

// Manually override fans (0-100%, 255=auto)
void setFanOverride(uint8_t fan, uint8_t percent);

// All sensors scan once and log addresses (at startup)
void scanAndLogSensors();

// sensor names from NVS load
void loadSensorNames();

// Einen sensor names setzen and in NVS save
void setSensorName(uint8_t idx, const char* name);

// Number of actually detected sensors
uint8_t foundSensorCount();

// Calibration offset per sensor (+/- degrees C, persisted in NVS)
void    setSensorOffset(uint8_t idx, float offset_c);
float   getSensorOffset(uint8_t idx);
void    loadSensorOffsets();
void    saveSensorOffsets();

}  // namespace temp
