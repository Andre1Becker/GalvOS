#include "safety.h"
#include <Arduino.h>
#include <esp_log.h>
#include <esp_attr.h>
#include <esp_heap_caps.h>
#include <string.h>
#include "util/log_buffer.h"

namespace safety {

static const char* TAG = "safety";

static volatile bool     s_user_arm_request = false;
static volatile uint32_t s_last_heartbeat_ms = 0;

void init() {
    pinMode(PIN_ESTOP,        INPUT_PULLUP);
    // PIN_SCAN_FAIL_IN (GPIO39): NE555 not yet populated — held HIGH via pull-up.
    // scanfail_ok is permanently true until NE555 is wired.
    pinMode(PIN_SCAN_FAIL_IN, INPUT_PULLUP);
    gState.scanfail_ok.store(true);
    pinMode(PIN_LASER_ENABLE, OUTPUT);
    pinMode(PIN_WATCHDOG_OUT, OUTPUT);
    digitalWrite(PIN_LASER_ENABLE, LOW);
    digitalWrite(PIN_WATCHDOG_OUT, LOW);
    s_last_heartbeat_ms = millis();  // prevent watchdog timeout before loop() starts
    ESP_LOGI(TAG, "Safety initialized -- laser disabled at boot");
}

void heartbeat() {
    s_last_heartbeat_ms = millis();
}

// ── Per-subsystem heartbeats ──────────────────────────────────────────────────
// Each subsystem calls its heartbeat() variant. allOk() requires all active
// subsystems to be recently alive. Pattern engine missing → laser off.
enum Subsystem { SYS_PATTERN = 0, SYS_GALVO = 1, SYS_COUNT = 2 };

static volatile uint32_t s_sub_hb[SYS_COUNT] = {0, 0};
static volatile bool     s_sub_active[SYS_COUNT] = {false, false};

void subsystemHeartbeat(int sys) {
    if (sys >= 0 && sys < SYS_COUNT) {
        s_sub_hb[sys] = millis();
        s_sub_active[sys] = true;
    }
}

void requestArm(bool v) {
    s_user_arm_request = v;
    if (!v) { ESP_LOGW(TAG, "User DISARM");       LOG_W(logbuf::CAT_SAFETY, "DISARM"); }
    else    { ESP_LOGI(TAG, "User ARM request");   LOG_I(logbuf::CAT_SAFETY, "ARM requested"); }
}

bool watchdogOk() {
    uint32_t age = millis() - s_last_heartbeat_ms;
    return age < gConfig.watchdog_period_ms;
}

bool subsystemsOk() {
    uint32_t now = millis();
    for (int i = 0; i < SYS_COUNT; i++) {
        if (!s_sub_active[i]) continue;  // not registered yet — skip
        if (now - s_sub_hb[i] > 1000) return false;  // 1s timeout per subsystem
    }
    return true;
}

bool userArmRequest() { return s_user_arm_request; }

bool allOk() {
    if (gConfig.safety_override) {
        return s_user_arm_request;  // bypass HW/watchdog/subsystem checks
    }
    return gState.estop_ok.load() &&
           gState.scanfail_ok.load() &&
           watchdogOk() &&
           subsystemsOk() &&
           s_user_arm_request;
}

static RTC_NOINIT_ATTR char     s_failsafe_reason[24];
static RTC_NOINIT_ATTR uint32_t s_failsafe_magic;
constexpr uint32_t FAILSAFE_MAGIC = 0x46534652;  // "FSFR"

const char* lastFailsafeReason() {
    return (s_failsafe_magic == FAILSAFE_MAGIC) ? s_failsafe_reason : "";
}

static void failsafeReboot(const char* reason) {
    strncpy(s_failsafe_reason, reason, sizeof(s_failsafe_reason) - 1);
    s_failsafe_reason[sizeof(s_failsafe_reason) - 1] = '\0';
    s_failsafe_magic = FAILSAFE_MAGIC;
    digitalWrite(PIN_LASER_ENABLE, LOW);
    gState.laser_armed.store(false);
    ESP_LOGE(TAG, "FAILSAFE REBOOT: %s", reason);
    LOG_E(logbuf::CAT_SAFETY, "Failsafe reboot: %s", reason);
    delay(200);  // let log/WS flush
    esp_restart();
}

void emergencyStop() {
    // v4.5.15: do not revoke ARM when safety_override is active —
    // otherwise every pushFrame timeout triggers a permanent DISARM
    // that the user cannot recover from without a page reload.
    if (!gConfig.safety_override) {
        s_user_arm_request = false;
    }
    digitalWrite(PIN_LASER_ENABLE, LOW);
    gState.laser_armed.store(false);
    LOG_W(logbuf::CAT_SAFETY, "EMERGENCY STOP");
    ESP_LOGW(TAG, "Emergency stop triggered");
}

void task(void*) {
    bool last_state = false;
    uint32_t toggle = 0;

    for (;;) {
        // Inputs are LOW-active in this wiring (reed switch closes to GND)
        gState.estop_ok.store(digitalRead(PIN_ESTOP) == HIGH);  // E-Stop open = OK
        // scanfail_ok: NE555 not yet populated, INPUT_PULLUP holds HIGH = OK
        gState.scanfail_ok.store(digitalRead(PIN_SCAN_FAIL_IN) == HIGH);
        { static bool _last_scan=true;
          bool _scan = gState.scanfail_ok.load();
          if (_scan != _last_scan) {
            ESP_LOGW(TAG, "SCAN_FAIL changed: %s", _scan ? "OK" : "FAIL");
            _last_scan = _scan; } }
        bool now_armed = allOk();
        digitalWrite(PIN_LASER_ENABLE, now_armed ? HIGH : LOW);
        gState.laser_armed.store(now_armed);  // atomic store

        // watchdog pulse: square wave to external NE555 retrigger or SSR
        // If this task hangs, the toggle stops -> SSR drops out
        toggle = (toggle + 1) & 1;
        digitalWrite(PIN_WATCHDOG_OUT, toggle);

        if (now_armed != last_state) {
            ESP_LOGW(TAG, "Laser %s | ESTOP:%d SCAN:%d WD:%d SUB:%d ARM:%d OVR:%d",
                     now_armed ? "ARMED" : "DISARMED",
                     (int)gState.estop_ok.load(), (int)gState.scanfail_ok.load(),
                     (int)watchdogOk(), (int)subsystemsOk(),
                     (int)s_user_arm_request, (int)gConfig.safety_override);
            last_state = now_armed;
        }

        // Heap-critical failsafe: largest free internal block, not just total
        // free heap (fragmentation-aware). Source-independent -- fires the
        // same whether output is driven by DMX, Art-Net, WebUI or SD/ILDA.
        // Own counter, not `toggle` (toggle only alternates 0/1 for the
        // watchdog square wave -- `toggle & 0x1F` never rate-limited).
        static uint32_t heap_check_ctr = 0;
        if (++heap_check_ctr >= 125) {  // ~2.5s @ 50 Hz
            heap_check_ctr = 0;
            size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
            if (largest < gConfig.heap_critical_bytes) {
                failsafeReboot("HEAP_CRITICAL");
            }
        }

        vTaskDelay(pdMS_TO_TICKS(20));  // 50 Hz safety loop
    }
}

}  // namespace safety