// Diagnostic snapshot for the 2026-09-14 shutdown-boundary review.
// Reproduces UNSAFE baseline decisions; passing is NOT a safety acceptance gate.
// allOk() and emergencyStop() are copied verbatim from b4385e3 safety.cpp.
// GPIO, health and logging are host stubs; no firmware/RTOS/electrical test.
#include <atomic>
#include <cassert>
#include <iostream>

struct State {
    std::atomic<bool> estop_ok{true}, scanfail_ok{true}, laser_armed{true};
} gState;
struct Config { bool safety_override = false; } gConfig;
static volatile bool s_user_arm_request = true;
static bool wd = true, subs = true;
static int enable_output = 1, estop_input = 1;
constexpr int PIN_LASER_ENABLE = 38, PIN_ESTOP = 47, LOW = 0, HIGH = 1;
bool watchdogOk() { return wd; }
bool subsystemsOk() { return subs; }
void digitalWrite(int pin, int value) {
    assert(pin == PIN_LASER_ENABLE);
    enable_output = value;
}
int digitalRead(int pin) {
    assert(pin == PIN_ESTOP);
    return estop_input;
}
#define ESP_LOGW(...) ((void)0)
#define LOG_W(...) ((void)0)

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

void sampleEstop() {
    gState.estop_ok.store(digitalRead(PIN_ESTOP) == HIGH);
}

int main() {
    for (bool override_value : {false, true}) {
        for (int fault = 0; fault < 4; ++fault) {
            gConfig.safety_override = override_value;
            s_user_arm_request = true;
            gState.estop_ok = (fault != 0);
            gState.scanfail_ok = (fault != 1);
            wd = (fault != 2);
            subs = (fault != 3);
            assert(allOk() == override_value);
            std::cout << "fault " << fault << " override " << override_value
                      << " enable_decision " << allOk();
            // The task's input/decision path does not clear the ARM request.
            gState.estop_ok = true;
            gState.scanfail_ok = true;
            wd = subs = true;
            assert(s_user_arm_request && allOk());
            std::cout << " after_fault_clears " << allOk() << "\n";
        }
        s_user_arm_request = false;
        assert(!allOk());
        s_user_arm_request = true;
        gState.laser_armed = true;
        enable_output = HIGH;
        emergencyStop();
        assert(enable_output == LOW && !gState.laser_armed.load());
        assert(s_user_arm_request == override_value);
        assert(allOk() == override_value);
        std::cout << "stop override " << override_value
                  << " immediate_output " << enable_output
                  << " arm_request " << s_user_arm_request
                  << " next_decision " << allOk() << "\n";
    }
    estop_input = HIGH;
    sampleEstop();
    assert(gState.estop_ok);
    estop_input = LOW;
    sampleEstop();
    assert(!gState.estop_ok);
    std::cout << "estop: HIGH accepted; LOW rejected\n";
}

