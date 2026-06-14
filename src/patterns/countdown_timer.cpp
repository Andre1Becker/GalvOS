#include "countdown_timer.h"
#include <Arduino.h>

namespace countdown_timer {

static volatile uint32_t s_remaining_s = 0;
static volatile bool     s_running     = false;
static volatile bool     s_expired     = false;
static uint32_t          s_last_ms     = 0;

void set(uint32_t seconds) {
    s_remaining_s = seconds;
    s_running     = false;
    s_expired     = false;
}

void start() {
    if (s_remaining_s > 0) {
        s_running = true;
        s_last_ms = millis();
        s_expired = false;
    }
}

void pause() { s_running = false; }

void stop() {
    s_running     = false;
    s_remaining_s = 0;
    s_expired     = false;
}

void reset(uint32_t seconds) { set(seconds); }

void tick() {
    if (!s_running || s_remaining_s == 0) return;
    uint32_t now = millis();
    uint32_t dt  = now - s_last_ms;
    if (dt < 1000) return;
    uint32_t dec = dt / 1000;
    s_last_ms    = now - (dt % 1000);
    if (dec >= s_remaining_s) {
        s_remaining_s = 0;
        s_running     = false;
        s_expired     = true;
    } else {
        s_remaining_s -= dec;
    }
}

uint32_t remaining() { return s_remaining_s; }
bool     running()   { return s_running;     }
bool     expired()   { return s_expired;     }

} // namespace countdown_timer
