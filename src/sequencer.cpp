#include "sequencer.h"
#include "patterns/pattern_engine.h"
#include "bpm_clock.h"
#include "mutex.h"
#include "json_alloc.h"
#include <LittleFS.h>
#include <esp_log.h>

namespace sequencer {

static const char* TAG  = "sequencer";
static const char* PATH = "/sequencer.json";

// Beat-edge bookkeeping. Written only from tick() and the transport
// functions below, all of which take LOCK_STATE() -- see mutex.h, same
// convention gLivePreset.rot_angle_* uses for its own per-frame RMW state.
static uint16_t s_prev_phase    = 0;
static uint8_t  s_beats_in_step = 0;
static volatile bool s_blanking = false;

// Index of the next *enabled* step from `from`, walking circularly in
// direction dir (+1/-1) through raw array indices. Falls back to `from`
// itself if it's the only enabled step. Returns -1 if none are enabled.
// Caller already holds LOCK_STATE().
static int8_t findStepInDirection(uint8_t from, int dir) {
    int n = gSequencer.stepCount;
    if (n == 0) return -1;
    int idx = from;
    for (int i = 0; i < n; i++) {
        idx = ((idx + dir) % n + n) % n;
        if (gSequencer.steps[idx].enabled) return (int8_t)idx;
    }
    return -1;
}

static int8_t firstEnabledStep() {
    for (uint8_t i = 0; i < gSequencer.stepCount; i++)
        if (gSequencer.steps[i].enabled) return (int8_t)i;
    return -1;
}

// Switches the active preset via the normal path (optimizer profile switch +
// gPatternCacheGen bump happen inside setPreset() itself, never duplicated
// here) and resets beat bookkeeping for the new step. Caller holds LOCK_STATE().
static void applyStep(uint8_t idx) {
    gSequencer.currentStep = idx;
    s_beats_in_step = 0;
    s_blanking = false;
    patterns::setPreset(presets::presetFromIndex(gSequencer.steps[idx].presetIdx));
}

void init() {
    load();  // no-op (running stays false) if /sequencer.json is missing/invalid
    ESP_LOGI(TAG, "init: %u step(s) loaded, running=false", gSequencer.stepCount);
}

void tick() {
    LOCK_STATE();
    if (!gSequencer.running || gSequencer.stepCount == 0) { s_blanking = false; return; }

    uint16_t phase = (uint16_t)bpm_clock::gBpm.phase_ms;
    bool edge = phase < s_prev_phase;
    s_prev_phase = phase;

    const SequencerStep& cur = gSequencer.steps[gSequencer.currentStep];
    uint8_t dur = cur.beats ? cur.beats : 1;

    if (edge && s_beats_in_step < 255) s_beats_in_step++;

    if (s_beats_in_step >= dur) {
        int8_t nxt = findStepInDirection(gSequencer.currentStep, +1);
        if (nxt < 0) { // every step got disabled out from under a running sequence
            ESP_LOGW(TAG, "tick: no enabled steps left, stopping");
            gSequencer.running = false;
            s_blanking = false;
            return;
        }
        // A wrap in raw index space (landed at or before where we started,
        // including the single-enabled-step case where nxt == current)
        // means we just completed a full pass over the playlist.
        bool wrapped = nxt <= (int8_t)gSequencer.currentStep;
        if (wrapped && !gSequencer.loop) {
            ESP_LOGI(TAG, "tick: reached end, loop=false, stopping");
            gSequencer.running = false;
            s_blanking = false;
            return;
        }
        applyStep((uint8_t)nxt);
        return;
    }

    uint8_t remaining = dur - s_beats_in_step;
    s_blanking = cur.transitionBeats > 0 && remaining <= cur.transitionBeats;
}

bool isBlanking() { return s_blanking; }

void start() {
    LOCK_STATE();
    if (gSequencer.stepCount == 0) { ESP_LOGW(TAG, "start(): empty playlist"); return; }
    int8_t idx = firstEnabledStep();
    if (idx < 0) { ESP_LOGW(TAG, "start(): no enabled steps"); return; }
    gSequencer.running = true;
    s_prev_phase = (uint16_t)bpm_clock::gBpm.phase_ms;
    applyStep((uint8_t)idx);
}

void stop() {
    LOCK_STATE();
    gSequencer.running = false;
    s_blanking = false;
}

void next() {
    LOCK_STATE();
    if (gSequencer.stepCount == 0) return;
    int8_t idx = findStepInDirection(gSequencer.currentStep, +1);
    if (idx >= 0) applyStep((uint8_t)idx);
}

void prev() {
    LOCK_STATE();
    if (gSequencer.stepCount == 0) return;
    int8_t idx = findStepInDirection(gSequencer.currentStep, -1);
    if (idx >= 0) applyStep((uint8_t)idx);
}

void jumpTo(uint8_t step) {
    LOCK_STATE();
    if (step >= gSequencer.stepCount) { ESP_LOGW(TAG, "jumpTo(%u): out of range", step); return; }
    applyStep(step);
}

bool setPlaylist(JsonArrayConst arr, bool loopAll) {
    SequencerStep tmp[SEQUENCER_MAX_STEPS];
    uint8_t n = 0;
    for (JsonObjectConst e : arr) {
        if (n >= SEQUENCER_MAX_STEPS) break;
        int beats  = e["beats"]  | 4;
        int transB = e["transitionBeats"] | 0;
        beats  = constrain(beats, 1, 32);
        transB = constrain(transB, 0, beats);
        tmp[n].presetIdx       = (uint8_t)constrain((int)(e["presetIdx"] | 0), 0, 255);
        tmp[n].beats           = (uint8_t)beats;
        tmp[n].transitionBeats = (uint8_t)transB;
        tmp[n].enabled         = e["enabled"] | true;
        n++;
    }

    {
        LOCK_STATE();
        for (uint8_t i = 0; i < n; i++) gSequencer.steps[i] = tmp[i];
        gSequencer.stepCount   = n;
        gSequencer.loop        = loopAll;
        gSequencer.currentStep = 0;
        gSequencer.running     = false;
        s_beats_in_step = 0;
        s_blanking      = false;
    }
    save();
    return true;
}

void clear() {
    {
        LOCK_STATE();
        gSequencer.stepCount   = 0;
        gSequencer.currentStep = 0;
        gSequencer.running     = false;
        s_beats_in_step = 0;
        s_blanking      = false;
    }
    save();
}

void fillStateJson(JsonObject& out) {
    LOCK_STATE();
    out["running"]     = gSequencer.running;
    out["loop"]        = gSequencer.loop;
    out["currentStep"] = gSequencer.currentStep;
    out["stepCount"]   = gSequencer.stepCount;
    JsonArray arr = out["steps"].to<JsonArray>();
    for (uint8_t i = 0; i < gSequencer.stepCount; i++) {
        JsonObject s = arr.add<JsonObject>();
        s["presetIdx"]       = gSequencer.steps[i].presetIdx;
        s["beats"]           = gSequencer.steps[i].beats;
        s["transitionBeats"] = gSequencer.steps[i].transitionBeats;
        s["enabled"]         = gSequencer.steps[i].enabled;
    }
}

bool save() {
    JsonDocument doc(&jsonAllocator());
    JsonObject root = doc.to<JsonObject>();
    fillStateJson(root);  // takes/releases LOCK_STATE() itself -- fine, not nested

    File f = LittleFS.open(PATH, "w");
    if (!f) { ESP_LOGE(TAG, "save(): open for write failed"); return false; }
    size_t written = serializeJson(doc, f);
    f.close();
    if (written == 0) { ESP_LOGE(TAG, "save(): write failed"); return false; }
    return true;
}

bool load() {
    if (!LittleFS.exists(PATH)) return false;
    File f = LittleFS.open(PATH, "r");
    if (!f) return false;

    JsonDocument doc(&jsonAllocator());
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) { ESP_LOGE(TAG, "load(): JSON error: %s", err.c_str()); return false; }

    setPlaylist(doc["steps"].as<JsonArrayConst>(), doc["loop"] | true);
    return true;
}

} // namespace sequencer
