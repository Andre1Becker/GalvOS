#include "bpm_clock.h"
#include <Arduino.h>
#include <Preferences.h>
#include <math.h>
#include "control/dmx_in.h"

namespace bpm_clock {

static const float BPM_MIN = 20.0f;
static const float BPM_MAX = 300.0f;
static const uint32_t TAP_RESET_GAP_MS = 3000;

BpmClock gBpm;

static volatile float    s_manual_bpm  = 120.0f;
static volatile uint16_t s_dmx_channel = 237;

// Tap tempo ring: fixed 4-slot buffer, oldest dropped on overflow. Written
// only from web_ui's request task (Core 0), read only from tickMs() (also
// only ever called from galvoTask on Core 1) -- s_tap_n is bumped last so a
// torn read at worst sees one fewer/older tap for a single frame.
static volatile uint32_t s_tap_ts[4] = {0};
static volatile uint8_t  s_tap_n     = 0;
static volatile float    s_tap_bpm   = 0.0f;

// DMX Beat-Stop (v==0) state, read+written only from tickMs() (Core 1).
// s_dmx_paused: true while frozen, so a v>=1 tick can tell "resuming from a
// freeze" apart from "already running". s_phase_offset_ms: additive shift
// applied to millis() before the beatMs modulo -- normally 0 (unchanged
// behavior); set on resume so the beat continues from the frozen fractional
// position instead of snapping to whatever millis() % beatMs happens to be.
static bool  s_dmx_paused      = false;
static float s_phase_offset_ms = 0.0f;

static float clampBpm(float b) {
    if (b < BPM_MIN) return BPM_MIN;
    if (b > BPM_MAX) return BPM_MAX;
    return b;
}

void init() {
    Preferences p;
    p.begin("bpm", true);
    s_manual_bpm  = p.getFloat("manual", 120.0f);
    s_dmx_channel = p.getUShort("dmx_ch", 237);
    p.end();
    gBpm.bpm = s_manual_bpm;
}

void setManualBpm(float bpm) {
    s_manual_bpm = clampBpm(bpm);
    Preferences p;
    p.begin("bpm", false);
    p.putFloat("manual", s_manual_bpm);
    p.end();
}

float manualBpm() { return s_manual_bpm; }

void tap() {
    uint32_t now = millis();
    // Gap too long since the previous tap -- start a fresh sequence.
    if (s_tap_n > 0 && (now - s_tap_ts[s_tap_n - 1]) > TAP_RESET_GAP_MS) {
        s_tap_n = 0;
    }
    if (s_tap_n >= 4) {
        s_tap_ts[0] = s_tap_ts[1];
        s_tap_ts[1] = s_tap_ts[2];
        s_tap_ts[2] = s_tap_ts[3];
        s_tap_n = 3;
    }
    s_tap_ts[s_tap_n] = now;
    s_tap_n++;

    if (s_tap_n >= 2) {
        uint32_t sum = 0;
        for (uint8_t i = 1; i < s_tap_n; i++) sum += s_tap_ts[i] - s_tap_ts[i - 1];
        float avgIntervalMs = (float)sum / (float)(s_tap_n - 1);
        if (avgIntervalMs > 0.0f) s_tap_bpm = clampBpm(60000.0f / avgIntervalMs);
    }
}

void setDmxChannel(uint16_t channel) {
    if (channel < 1) channel = 1;
    if (channel > 512) channel = 512;
    s_dmx_channel = channel;
    Preferences p;
    p.begin("bpm", false);
    p.putUShort("dmx_ch", s_dmx_channel);
    p.end();
}

uint16_t dmxChannel() { return s_dmx_channel; }

uint32_t tickMs() {
    float  bpm;
    Source src;

    if (dmx_in::isReceiving()) {
        uint8_t v = dmx_in::getChannel(s_dmx_channel);
        if (v == 0) {
            // Beat-Stop: freeze -- keep bpm/source/phase exactly as they
            // are, do NOT fall through to Tap/Manual, do NOT recompute
            // phase from millis(). Idempotent across repeated 0-ticks.
            s_dmx_paused = true;
            gBpm.source  = SRC_DMX;
            return gBpm.phase_ms;
        }
        bpm = (float)v;   // 1:1, no BPM_MIN/BPM_MAX rescale on the DMX path
        src = SRC_DMX;
        if (s_dmx_paused) {
            // Seamless resume: re-derive the fractional beat position the
            // freeze left off at (bpm-agnostic: phase_ms/1000 of a cycle),
            // then set the offset so this tick's elapsedMs lands exactly
            // there instead of jumping to millis() % beatMs.
            float beatMs = 60000.0f / bpm;
            float frozenElapsedMs = (gBpm.phase_ms / 1000.0f) * beatMs;
            s_phase_offset_ms = frozenElapsedMs - (float)millis();
            s_dmx_paused = false;
        }
    } else {
        s_dmx_paused = false;   // no DMX signal at all -- not a Beat-Stop freeze
        if (s_tap_n >= 2) {
            // Tap tempo holds until a fresh tap sequence overwrites s_tap_bpm
            // or DMX takes over above -- TAP_RESET_GAP_MS only governs when a
            // new tap() call starts a fresh sequence instead of averaging
            // into the old one, it is not a timeout for staying on SRC_TAP.
            bpm = s_tap_bpm;
            src = SRC_TAP;
        } else {
            bpm = s_manual_bpm;
            src = SRC_MANUAL;
        }
    }

    gBpm.bpm    = bpm;
    gBpm.source = src;

    float beatMs = 60000.0f / bpm;
    float elapsedMs = fmodf((float)millis() + s_phase_offset_ms, beatMs);
    if (elapsedMs < 0.0f) elapsedMs += beatMs;
    uint32_t phase = (uint32_t)((elapsedMs / beatMs) * 1000.0f);
    if (phase > 999) phase = 999;
    gBpm.phase_ms = phase;
    return phase;
}

float phaseNormalized() {
    return gBpm.phase_ms / 1000.0f;
}

}  // namespace bpm_clock
