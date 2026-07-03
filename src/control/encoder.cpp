#include "encoder.h"
#include "pinmap.h"
#include "config.h"
#include "patterns/calib_patterns.h"
#include "patterns/preset_patterns.h"
#include "patterns/pattern_engine.h"
#include "storage/sd_card.h"
#include "ilda/ilda_player.h"
#include <Arduino.h>
#include <esp_log.h>

namespace encoder {

static const char* TAG = "enc";

enum EncoderMode : uint8_t {
    MODE_PRESET  = 0,   // Drehen = Preset switch
    MODE_DIMMER  = 1,   // Drehen = Master-Dimmer
    MODE_ILDA    = 2,   // rotate = ILDA file
    MODE_CALIB   = 3,   // Drehen = calibration-Pattern
    MODE_COUNT   = 4
};

static const char* MODE_NAMES[MODE_COUNT] = {
    "Preset", "Dimmer", "ILDA", "Calib."
};

static volatile int32_t s_pos      = 0;
static volatile int32_t s_last_pos = 0;
static EncoderMode       s_mode    = MODE_PRESET;

// Entprell-State
static uint32_t s_btn_pressed_ms   = 0;
static bool     s_btn_was_pressed  = false;
static uint32_t s_last_click_ms    = 0;
static uint8_t  s_click_count      = 0;

static volatile int8_t s_enc_a_last = 0;

static void IRAM_ATTR enc_isr(void*) {
    int8_t a = digitalRead(PIN_ENC_A);
    int8_t b = digitalRead(PIN_ENC_B);
    if (a != s_enc_a_last) {
        s_pos += (a == b) ? 1 : -1;
        s_enc_a_last = a;
    }
}

void init() {
    pinMode(PIN_ENC_A,   INPUT_PULLUP);
    pinMode(PIN_ENC_B,   INPUT_PULLUP);
    pinMode(PIN_ENC_BTN, INPUT_PULLUP);  // GPIO41 free — encoder button re-enabled
    s_enc_a_last = digitalRead(PIN_ENC_A);
    attachInterruptArg(PIN_ENC_A, enc_isr, nullptr, CHANGE);
    ESP_LOGI(TAG, "Encor init GPIO A=%d B=%d BTN=%d",
             PIN_ENC_A, PIN_ENC_B, PIN_ENC_BTN);
}

static void applyDelta(int32_t delta) {
    if (delta == 0) return;
    switch (s_mode) {
        case MODE_PRESET: {
            // cycle preset index, activate via pattern engine
            static uint8_t enc_preset = 0;
            enc_preset = (uint8_t)constrain((int)enc_preset + delta, 0, presets::PRESET_COUNT - 1);
            gLivePreset.pattern_idx = enc_preset;
            patterns::setPreset((int8_t)enc_preset);
            break;
        }
        case MODE_DIMMER: {
            int d = (int)gState.master_dimmer.load() + delta * 5;
            gState.master_dimmer.store((uint8_t)constrain(d, 0, 255));
            break;
        }
        case MODE_ILDA:
            // next/previous ILDA file
            if (sd_card::fileCount() > 0) {
                int idx = (int)ilda::gILDA.file_idx + delta;
                idx = ((idx % (int)sd_card::fileCount()) + sd_card::fileCount())
                      % sd_card::fileCount();
                ilda::loadFile((uint8_t)idx);
            }
            break;
        case MODE_CALIB: {
            int idx = (int)gState.calib_idx + delta;
            idx = ((idx % calib_patterns::CALIB_PATTERN_COUNT)
                   + calib_patterns::CALIB_PATTERN_COUNT)
                  % calib_patterns::CALIB_PATTERN_COUNT;
            gState.calib_idx = (uint8_t)idx;
            gState.calib_active = true;
            break;
        }
        default: break;
    }
}

void task(void*) {
    const uint32_t LONG_PRESS_MS   = 800;
    const uint32_t DOUBLE_CLICK_MS = 350;

    for (;;) {
        // ── Encor-Delta auswerten ───────────────────────────
        int32_t pos = s_pos;
        int32_t delta = pos - s_last_pos;
        if (delta != 0) {
            s_last_pos = pos;
            applyDelta(delta);
        }

        // ── Button auswerten ────────────────────────────────────
        bool btn = (digitalRead(PIN_ENC_BTN) == LOW);
        uint32_t now = millis();

        if (btn && !s_btn_was_pressed) {
            s_btn_pressed_ms = now;
            s_btn_was_pressed = true;
        } else if (!btn && s_btn_was_pressed) {
            uint32_t held = now - s_btn_pressed_ms;
            s_btn_was_pressed = false;

            if (held >= LONG_PRESS_MS) {
                gState.master_dimmer.store(gState.master_dimmer.load() > 0 ? 0 : 200);
                ESP_LOGI(TAG, "LongPress → Dimmer=%d", gState.master_dimmer.load());
            } else {
                if (now - s_last_click_ms < DOUBLE_CLICK_MS) {
                    s_click_count++;
                } else {
                    s_click_count = 1;
                }
                s_last_click_ms = now;
            }
        }

        if (s_click_count > 0 &&
            (now - s_last_click_ms) > DOUBLE_CLICK_MS &&
            !s_btn_was_pressed) {

            if (s_click_count >= 2) {
                s_mode = (EncoderMode)((s_mode + 1) % MODE_COUNT);
                ESP_LOGI(TAG, "Mode → %s", MODE_NAMES[s_mode]);
            } else {
                if (s_mode == MODE_CALIB) {
                    gState.calib_active = false;
                }
            }
            s_click_count = 0;
        }

        if (s_btn_was_pressed && (now - s_btn_pressed_ms) > 3000) {
            ESP_LOGW(TAG, "Long-hold >3s → Safety-Reset-Request");
            s_btn_pressed_ms = now;
        }

        vTaskDelay(pdMS_TO_TICKS(10));  // 100 Hz Poll-Rate
    }
}

} // namespace encor
