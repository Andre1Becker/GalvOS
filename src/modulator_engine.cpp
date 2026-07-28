#include "modulator_engine.h"
#include "mutex.h"
#include "bpm_clock.h"
#include "json_alloc.h"
#include <LittleFS.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <math.h>
#include <new>
#include <string.h>

namespace modulator {

static const char* TAG  = "modulator";
static const char* PATH = "/modulators.json";

static Modulator*  s_mods  = nullptr;   // [MOD_SLOTS],   PSRAM, placement-new'd
static ModBinding*  s_binds = nullptr;   // [MOD_BINDINGS], PSRAM, placement-new'd
static float*       s_lut[4] = {nullptr, nullptr, nullptr, nullptr};  // per WaveShape, PSRAM

// ── PSRAM allocation ─────────────────────────────────────────────────────
static void* psAlloc(size_t bytes) {
    void* p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    if (!p) p = malloc(bytes);  // fallback: PSRAM-less devkit (gDebugNoHW)
    return p;
}

static void buildLut() {
    for (int s = 0; s < 4; s++) {
        s_lut[s] = (float*)psAlloc(sizeof(float) * MOD_LUT_SIZE);
        if (!s_lut[s]) { ESP_LOGE(TAG, "buildLut: PSRAM alloc failed for shape %d", s); continue; }
        for (size_t i = 0; i < MOD_LUT_SIZE; i++) {
            float t = (float)i / (float)MOD_LUT_SIZE;   // [0..1)
            float v;
            switch ((WaveShape)s) {
                case WaveShape::SINE:     v = sinf(2.0f * (float)M_PI * t); break;
                case WaveShape::TRIANGLE: v = (t < 0.5f) ? (4.0f * t - 1.0f) : (3.0f - 4.0f * t); break;
                case WaveShape::SQUARE:   v = (t < 0.5f) ? 1.0f : -1.0f; break;
                case WaveShape::SAW:      v = 2.0f * t - 1.0f; break;
                default:                  v = 0.0f; break;
            }
            s_lut[s][i] = v;
        }
    }
}

void init() {
    if (!s_mods) {
        void* p = psAlloc(sizeof(Modulator) * MOD_SLOTS);
        if (!p) { ESP_LOGE(TAG, "init: PSRAM alloc failed for modulators[]"); return; }
        s_mods = new (p) Modulator[MOD_SLOTS];
    }
    if (!s_binds) {
        void* p = psAlloc(sizeof(ModBinding) * MOD_BINDINGS);
        if (!p) { ESP_LOGE(TAG, "init: PSRAM alloc failed for bindings[]"); return; }
        s_binds = new (p) ModBinding[MOD_BINDINGS];
    }
    buildLut();
    load();  // no-op (all slots stay default/disabled) if /modulators.json is missing/invalid
    ESP_LOGI(TAG, "init: %u slot(s), %u binding(s) loaded", (unsigned)MOD_SLOTS, (unsigned)MOD_BINDINGS);
}

// ── Waveform / noise / envelope math ─────────────────────────────────────

static inline float bpmDivBeats(BpmDiv d) {
    switch (d) {
        case BpmDiv::D1:  return 4.0f;
        case BpmDiv::D2:  return 2.0f;
        case BpmDiv::D4:  return 1.0f;
        case BpmDiv::D8:  return 0.5f;
        case BpmDiv::D16: return 0.25f;
        default:          return 1.0f;
    }
}

// Continuous, unwrapped cycle count since boot -- stateless (recomputed
// fresh from nowMs every call, same trick bpm_clock::tickMs() uses for its
// own phase_ms), so there is no accumulator to drift or race across cores.
static inline float totalCycles(const Modulator& m, uint32_t nowMs) {
    if (m.bpmSync) {
        float bpm = bpm_clock::gBpm.bpm;
        if (bpm < 1.0f) bpm = 1.0f;
        float totalBeats = ((float)nowMs / 60000.0f) * bpm;
        return (totalBeats / bpmDivBeats(m.bpmDiv)) * m.cycles;
    }
    return ((float)nowMs / 1000.0f) * m.phaseSpeed * m.cycles;
}

static inline float wrap01(float t) {
    t = fmodf(t, 1.0f);
    if (t < 0.0f) t += 1.0f;
    return t;
}

static inline float lutLookup(WaveShape shape, float phase01) {
    uint8_t s = (uint8_t)shape;
    if (s > 3 || !s_lut[s]) return 0.0f;
    size_t idx = (size_t)(phase01 * (float)MOD_LUT_SIZE);
    if (idx >= MOD_LUT_SIZE) idx = MOD_LUT_SIZE - 1;
    return s_lut[s][idx];
}

// Deterministic integer hash -> pseudo-random float in [-1..1]. No runtime
// state: same (seed, lattice index) always produces the same value, which
// is what makes valueNoise() below stateless/race-free.
static inline float hashNoise(uint32_t seed, int32_t i) {
    uint32_t h = (uint32_t)i * 2654435761u + seed * 0x9E3779B1u;
    h ^= h >> 15; h *= 0x85EBCA6Bu;
    h ^= h >> 13; h *= 0xC2B2AE35u;
    h ^= h >> 16;
    return ((float)h / 4294967295.0f) * 2.0f - 1.0f;
}

// 1D value noise, Perlin-style smootherstep interpolation between
// hash-derived lattice points. `pos` is the unwrapped totalCycles() value,
// so the noise curve never repeats on a short, audible/visible period the
// way wrapping phase01 would.
static inline float valueNoise(uint32_t seed, float pos) {
    int32_t i0 = (int32_t)floorf(pos);
    float   f  = pos - (float)i0;
    float   a  = hashNoise(seed, i0);
    float   b  = hashNoise(seed, i0 + 1);
    float   s  = f * f * f * (f * (f * 6.0f - 15.0f) + 10.0f);
    return a + (b - a) * s;
}

static float tickEnvelope(Modulator& m, uint32_t nowMs) {
    if (m.envStage == Modulator::EnvStage::IDLE) return 0.0f;
    uint32_t elapsed = nowMs - m.envStartMs;

    uint32_t attack  = (uint32_t)m.envAttackMs;
    uint32_t sustain  = (uint32_t)m.envSustainMs;
    uint32_t release = (uint32_t)m.envReleaseMs;

    if (m.envStage == Modulator::EnvStage::ATTACK) {
        if (attack == 0 || elapsed >= attack) {
            m.envStage = Modulator::EnvStage::SUSTAIN;
            m.envStartMs = nowMs;
            elapsed = 0;
        } else {
            return (float)elapsed / (float)attack;
        }
    }
    if (m.envStage == Modulator::EnvStage::SUSTAIN) {
        if (elapsed >= sustain) {
            m.envStage = Modulator::EnvStage::RELEASE;
            m.envStartMs = nowMs;
            elapsed = 0;
        } else {
            return 1.0f;
        }
    }
    if (m.envStage == Modulator::EnvStage::RELEASE) {
        if (release == 0 || elapsed >= release) {
            m.envStage = Modulator::EnvStage::IDLE;
            return 0.0f;
        }
        return 1.0f - (float)elapsed / (float)release;
    }
    return 0.0f;
}

void tick(uint32_t nowMs) {
    if (!s_mods) return;
    // Non-blocking: galvoTask (Core 1) must never stall on a lock a Core 0
    // web handler might be holding -- same convention as updateSnapshot()'s
    // xSemaphoreTake(mtx::zone, 0). On contention, slots simply keep last
    // frame's output for one frame.
    if (xSemaphoreTake(mtx::modulator, 0) != pdTRUE) return;
    for (uint8_t i = 0; i < MOD_SLOTS; i++) {
        Modulator& m = s_mods[i];
        if (!m.enabled) { m.output = 0.0f; continue; }

        float raw;
        switch (m.type) {
            case ModType::OSCILLATOR: {
                float phase = wrap01(totalCycles(m, nowMs) + m.phaseOffset);
                raw = lutLookup(m.shape, phase);
                break;
            }
            case ModType::NOISE:
                raw = valueNoise(m.noiseSeed, totalCycles(m, nowMs) + m.phaseOffset);
                break;
            case ModType::ENVELOPE:
                raw = tickEnvelope(m, nowMs);
                break;
            case ModType::SEQUENCER: {
                uint8_t n = m.seqStepCount;
                if (n == 0) n = 1;
                if (n > MOD_SEQ_STEPS) n = MOD_SEQ_STEPS;
                float phase = wrap01(totalCycles(m, nowMs) + m.phaseOffset);
                uint8_t step = (uint8_t)(phase * (float)n);
                if (step >= n) step = n - 1;
                raw = m.seqValues[step];
                break;
            }
            default:
                raw = 0.0f;
                break;
        }
        m.output = raw * m.level;
    }
    xSemaphoreGive(mtx::modulator);
}

// Per-target scale (native units per raw [-1..1] modulator unit) + valid
// range. baseValue is expected to already be in the target's native units
// (e.g. 1.0 for a neutral scale multiplier, 0.0 for a neutral shift/degree).
static void targetInfo(ModTarget t, float& scale, float& lo, float& hi) {
    switch (t) {
        case ModTarget::TRANSFORM_SCALE_X:  scale = 1.0f;    lo = 0.1f;    hi = 3.0f;    break;
        case ModTarget::TRANSFORM_SCALE_Y:  scale = 1.0f;    lo = 0.1f;    hi = 3.0f;    break;
        case ModTarget::TRANSFORM_SHIFT_X:  scale = 8000.0f; lo = -20000.0f; hi = 20000.0f; break;
        case ModTarget::TRANSFORM_SHIFT_Y:  scale = 8000.0f; lo = -20000.0f; hi = 20000.0f; break;
        case ModTarget::TRANSFORM_ROTATION: scale = 180.0f;  lo = -3600.0f; hi = 3600.0f; break;
        case ModTarget::COLOR_HUE:          scale = 0.5f;    lo = 0.0f;    hi = 1.0f;    break;  // wrapped, not clamped
        case ModTarget::COLOR_SATURATION:   scale = 1.0f;    lo = 0.0f;    hi = 2.0f;    break;
        case ModTarget::COLOR_BRIGHTNESS:   scale = 1.0f;    lo = 0.0f;    hi = 2.0f;    break;
        case ModTarget::OPT_SPEED:          scale = 127.0f;  lo = 0.0f;    hi = 255.0f;  break;
        case ModTarget::OPT_DENSITY:        scale = 1.0f;    lo = 0.1f;    hi = 5.0f;    break;
        default:                            scale = 1.0f;    lo = -1e9f;   hi = 1e9f;    break;
    }
}

float apply(ModTarget target, float baseValue) {
    if (!s_binds || !s_mods) return baseValue;
    if (xSemaphoreTake(mtx::modulator, 0) != pdTRUE) return baseValue;

    float scale, lo, hi;
    targetInfo(target, scale, lo, hi);
    float value = baseValue;
    bool  any = false;
    for (uint8_t i = 0; i < MOD_BINDINGS; i++) {
        const ModBinding& b = s_binds[i];
        if (!b.active || b.targetParam != target) continue;
        if (b.modulatorIdx >= MOD_SLOTS || !s_mods[b.modulatorIdx].enabled) continue;
        value += s_mods[b.modulatorIdx].output * b.depth * scale + b.offset;
        any = true;
    }
    xSemaphoreGive(mtx::modulator);
    if (!any) return baseValue;

    if (target == ModTarget::COLOR_HUE) return wrap01(value);
    if (value < lo) value = lo;
    if (value > hi) value = hi;
    return value;
}

void trigger(uint8_t idx) {
    if (!s_mods || idx >= MOD_SLOTS) return;
    LOCK_MOD();
    Modulator& m = s_mods[idx];
    if (!m.enabled || m.type != ModType::ENVELOPE) return;
    m.envStage   = Modulator::EnvStage::ATTACK;
    m.envStartMs = millis();
}

Modulator* getModulator(uint8_t idx) {
    if (!s_mods || idx >= MOD_SLOTS) return nullptr;
    return &s_mods[idx];
}

ModBinding* getBinding(uint8_t idx) {
    if (!s_binds || idx >= MOD_BINDINGS) return nullptr;
    return &s_binds[idx];
}

static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static int   clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Applies validated/clamped fields from `obj` onto `m` (partial update --
// only keys present in obj are touched). Caller holds mtx::modulator.
// Split out from setModulator() so load() can apply all slots under one
// lock and one save() at the end instead of a lock+flash-write per slot.
static void applyModulatorFields(Modulator& m, JsonObjectConst obj) {
    if (!obj["enabled"].isNull())     m.enabled     = obj["enabled"] | false;
    if (!obj["type"].isNull())        m.type        = (ModType)clampi((int)(obj["type"] | 0), 0, 3);
    if (!obj["shape"].isNull())       m.shape       = (WaveShape)clampi((int)(obj["shape"] | 0), 0, 3);
    if (!obj["cycles"].isNull())      m.cycles      = clampf(obj["cycles"] | 1.0f, 0.01f, 32.0f);
    if (!obj["phaseOffset"].isNull()) m.phaseOffset = wrap01(obj["phaseOffset"] | 0.0f);
    if (!obj["phaseSpeed"].isNull())  m.phaseSpeed  = clampf(obj["phaseSpeed"] | 1.0f, 0.01f, 20.0f);
    if (!obj["level"].isNull())       m.level       = clampf(obj["level"] | 1.0f, 0.0f, 1.0f);
    if (!obj["bpmSync"].isNull())     m.bpmSync     = obj["bpmSync"] | false;
    if (!obj["bpmDiv"].isNull())      m.bpmDiv      = (BpmDiv)clampi((int)(obj["bpmDiv"] | 0), 0, 4);
    if (!obj["name"].isNull()) {
        const char* n = obj["name"] | "";
        strncpy(m.name, n, sizeof(m.name) - 1);
        m.name[sizeof(m.name) - 1] = 0;
    }
    if (!obj["envAttackMs"].isNull())  m.envAttackMs  = clampf(obj["envAttackMs"] | 50.0f, 0.0f, 10000.0f);
    if (!obj["envSustainMs"].isNull()) m.envSustainMs = clampf(obj["envSustainMs"] | 200.0f, 0.0f, 10000.0f);
    if (!obj["envReleaseMs"].isNull()) m.envReleaseMs = clampf(obj["envReleaseMs"] | 300.0f, 0.0f, 10000.0f);
    if (!obj["seqStepCount"].isNull()) m.seqStepCount = (uint8_t)clampi((int)(obj["seqStepCount"] | 8), 1, MOD_SEQ_STEPS);
    if (!obj["seqValues"].isNull()) {
        JsonArrayConst arr = obj["seqValues"].as<JsonArrayConst>();
        uint8_t i = 0;
        for (JsonVariantConst v : arr) {
            if (i >= MOD_SEQ_STEPS) break;
            m.seqValues[i] = clampf(v.as<float>(), -1.0f, 1.0f);
            i++;
        }
    }
    if (!obj["noiseSeed"].isNull()) m.noiseSeed = obj["noiseSeed"] | 0;
    else if (m.noiseSeed == 0 && m.type == ModType::NOISE) m.noiseSeed = esp_random();
}

bool setModulator(uint8_t idx, JsonObjectConst obj) {
    if (!s_mods || idx >= MOD_SLOTS) return false;
    { LOCK_MOD(); applyModulatorFields(s_mods[idx], obj); }
    save();
    return true;
}

// Caller holds mtx::modulator. Split out so load() can apply bindings under
// the same lock it already holds for the modulator slots, without an extra
// save() in between (see setBindings()/load()).
static void applyBindingsArray(JsonArrayConst arr) {
    ModBinding tmp[MOD_BINDINGS];
    uint8_t n = 0;
    for (JsonObjectConst e : arr) {
        if (n >= MOD_BINDINGS) break;
        tmp[n].active       = e["active"] | true;
        tmp[n].modulatorIdx = (uint8_t)clampi((int)(e["modulatorIdx"] | 0), 0, MOD_SLOTS - 1);
        tmp[n].targetParam  = (ModTarget)clampi((int)(e["targetParam"] | 0), 0, (int)ModTarget::MOD_TARGET_COUNT - 1);
        tmp[n].depth        = clampf(e["depth"] | 0.0f, -1.0f, 1.0f);
        tmp[n].offset       = e["offset"] | 0.0f;
        n++;
    }
    for (uint8_t i = 0; i < n; i++) s_binds[i] = tmp[i];
    for (uint8_t i = n; i < MOD_BINDINGS; i++) s_binds[i] = ModBinding();
}

bool setBindings(JsonArrayConst arr) {
    if (!s_binds) return false;
    { LOCK_MOD(); applyBindingsArray(arr); }
    save();
    return true;
}

void clearModulator(uint8_t idx) {
    if (!s_mods || idx >= MOD_SLOTS) return;
    {
        LOCK_MOD();
        s_mods[idx] = Modulator();
        if (s_binds) {
            for (uint8_t i = 0; i < MOD_BINDINGS; i++)
                if (s_binds[i].modulatorIdx == idx) s_binds[i] = ModBinding();
        }
    }
    save();
}

void resetAll() {
    {
        LOCK_MOD();
        if (s_mods)  for (uint8_t i = 0; i < MOD_SLOTS;    i++) s_mods[i]  = Modulator();
        if (s_binds) for (uint8_t i = 0; i < MOD_BINDINGS; i++) s_binds[i] = ModBinding();
    }
    save();
}

static const char* typeName(ModType t) {
    switch (t) { case ModType::OSCILLATOR: return "oscillator"; case ModType::NOISE: return "noise";
                 case ModType::ENVELOPE: return "envelope"; case ModType::SEQUENCER: return "sequencer";
                 default: return "oscillator"; }
}

void fillStateJson(JsonObject& out) {
    if (!s_mods) return;
    LOCK_MOD();
    JsonArray arr = out["modulators"].to<JsonArray>();
    for (uint8_t i = 0; i < MOD_SLOTS; i++) {
        const Modulator& m = s_mods[i];
        JsonObject o = arr.add<JsonObject>();
        o["idx"]          = i;
        o["enabled"]      = m.enabled;
        o["type"]         = (int)m.type;
        o["typeName"]     = typeName(m.type);
        o["shape"]        = (int)m.shape;
        o["cycles"]       = m.cycles;
        o["phaseOffset"]  = m.phaseOffset;
        o["phaseSpeed"]   = m.phaseSpeed;
        o["level"]        = m.level;
        o["bpmSync"]      = m.bpmSync;
        o["bpmDiv"]       = (int)m.bpmDiv;
        o["name"]         = m.name;
        o["envAttackMs"]  = m.envAttackMs;
        o["envSustainMs"] = m.envSustainMs;
        o["envReleaseMs"] = m.envReleaseMs;
        o["seqStepCount"] = m.seqStepCount;
        JsonArray sv = o["seqValues"].to<JsonArray>();
        for (uint8_t s = 0; s < MOD_SEQ_STEPS; s++) sv.add(m.seqValues[s]);
        o["noiseSeed"]    = m.noiseSeed;
        o["output"]       = m.output;   // live value, read-only
    }
}

void fillBindingsJson(JsonArray& out) {
    if (!s_binds) return;
    LOCK_MOD();
    for (uint8_t i = 0; i < MOD_BINDINGS; i++) {
        const ModBinding& b = s_binds[i];
        JsonObject o = out.add<JsonObject>();
        o["idx"]          = i;
        o["active"]       = b.active;
        o["modulatorIdx"] = b.modulatorIdx;
        o["targetParam"]  = (int)b.targetParam;
        o["depth"]        = b.depth;
        o["offset"]       = b.offset;
    }
}

bool save() {
    if (!s_mods || !s_binds) return false;
    JsonDocument doc(&jsonAllocator());
    JsonObject root = doc.to<JsonObject>();
    fillStateJson(root);                        // takes/releases LOCK_MOD() itself
    JsonArray binds = root["bindings"].to<JsonArray>();
    fillBindingsJson(binds);                    // takes/releases LOCK_MOD() itself

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

    if (!s_mods || !s_binds) return false;
    {
        LOCK_MOD();
        JsonArrayConst mods = doc["modulators"].as<JsonArrayConst>();
        uint8_t i = 0;
        for (JsonObjectConst o : mods) {
            if (i >= MOD_SLOTS) break;
            applyModulatorFields(s_mods[i], o);
            i++;
        }
        applyBindingsArray(doc["bindings"].as<JsonArrayConst>());
    }
    return true;   // no save() -- state was just read FROM /modulators.json
}

}  // namespace modulator
