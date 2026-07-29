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
static float*       s_lutSine = nullptr;  // MOD_LUT_SIZE entries, PSRAM -- only SINE needs a LUT (see buildLut())

// ── Registry storage ─────────────────────────────────────────────────────
// Direct-indexed pointer arrays: register*() stores a POINTER to a
// static-storage-duration descriptor at s_x[d->id]. O(1) lookup, zero heap,
// same cost class as the switch statements this replaces. See
// modulator_engine.h's file header comment for why descriptor tables were
// chosen over virtual classes.
static const ModTypeDescriptor*   s_modTypes[MAX_MOD_TYPES]     = {};
static const WaveShapeDescriptor* s_waveShapes[MAX_WAVE_SHAPES] = {};
static const ModTargetDescriptor* s_modTargets[MAX_MOD_TARGETS] = {};

bool registerModType(const ModTypeDescriptor* d) {
    if (!d || d->id >= MAX_MOD_TYPES || s_modTypes[d->id]) return false;
    s_modTypes[d->id] = d;
    return true;
}
bool registerWaveShape(const WaveShapeDescriptor* d) {
    if (!d || d->id >= MAX_WAVE_SHAPES || s_waveShapes[d->id]) return false;
    s_waveShapes[d->id] = d;
    return true;
}
bool registerModTarget(const ModTargetDescriptor* d) {
    if (!d || d->id >= MAX_MOD_TARGETS || s_modTargets[d->id]) return false;
    s_modTargets[d->id] = d;
    return true;
}

const ModTypeDescriptor*   findModType(ModType id)     { return id < MAX_MOD_TYPES   ? s_modTypes[id]   : nullptr; }
const WaveShapeDescriptor* findWaveShape(WaveShape id) { return id < MAX_WAVE_SHAPES ? s_waveShapes[id] : nullptr; }
const ModTargetDescriptor* findModTarget(ModTarget id) { return id < MAX_MOD_TARGETS ? s_modTargets[id] : nullptr; }

uint8_t modTypeCount() {
    uint8_t n = 0;
    for (uint8_t i = 0; i < MAX_MOD_TYPES; i++) if (s_modTypes[i]) n++;
    return n;
}
const ModTypeDescriptor& modTypeAt(uint8_t idx) {
    uint8_t n = 0;
    for (uint8_t i = 0; i < MAX_MOD_TYPES; i++)
        if (s_modTypes[i]) { if (n == idx) return *s_modTypes[i]; n++; }
    static const ModTypeDescriptor dummy{0, "", "", nullptr, nullptr, false, false, false, false};
    return dummy;
}
uint8_t waveShapeCount() {
    uint8_t n = 0;
    for (uint8_t i = 0; i < MAX_WAVE_SHAPES; i++) if (s_waveShapes[i]) n++;
    return n;
}
const WaveShapeDescriptor& waveShapeAt(uint8_t idx) {
    uint8_t n = 0;
    for (uint8_t i = 0; i < MAX_WAVE_SHAPES; i++)
        if (s_waveShapes[i]) { if (n == idx) return *s_waveShapes[i]; n++; }
    static const WaveShapeDescriptor dummy{0, "", "", false, nullptr, false};
    return dummy;
}
uint16_t modTargetCount() {
    uint16_t n = 0;
    for (uint16_t i = 0; i < MAX_MOD_TARGETS; i++) if (s_modTargets[i]) n++;
    return n;
}
const ModTargetDescriptor& modTargetAt(uint16_t idx) {
    uint16_t n = 0;
    for (uint16_t i = 0; i < MAX_MOD_TARGETS; i++)
        if (s_modTargets[i]) { if (n == idx) return *s_modTargets[i]; n++; }
    static const ModTargetDescriptor dummy{
        0, {0, "", "", "", paramui::DataType::FLOAT, 0.f, 0.f, 0.f, 0.f, "", "", false, false, false}, 1.0f, false};
    return dummy;
}

// ── PSRAM allocation ─────────────────────────────────────────────────────
static void* psAlloc(size_t bytes) {
    void* p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    if (!p) p = malloc(bytes);  // fallback: PSRAM-less devkit (gDebugNoHW)
    return p;
}

// Only SINE gets a LUT -- TRIANGLE/SQUARE/SAW are piecewise-linear closed
// forms (see evalTriangle/evalSquare/evalSaw below), so a LUT never bought
// them anything but an extra PSRAM read. Dropping their LUTs frees 6KB
// PSRAM (was 4x512x4B=8KB, now 1x512x4B=2KB) and makes those 3 shapes
// slightly cheaper per tick().
static void buildLut() {
    s_lutSine = (float*)psAlloc(sizeof(float) * MOD_LUT_SIZE);
    if (!s_lutSine) { ESP_LOGE(TAG, "buildLut: PSRAM alloc failed for SINE"); return; }
    for (size_t i = 0; i < MOD_LUT_SIZE; i++) {
        float t = (float)i / (float)MOD_LUT_SIZE;   // [0..1)
        s_lutSine[i] = sinf(2.0f * (float)M_PI * t);
    }
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
float totalCycles(const Modulator& m, uint32_t nowMs) {
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

static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static inline int   clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

static inline float lutLookupSine(float phase01) {
    if (!s_lutSine) return 0.0f;
    size_t idx = (size_t)(phase01 * (float)MOD_LUT_SIZE);
    if (idx >= MOD_LUT_SIZE) idx = MOD_LUT_SIZE - 1;
    return s_lutSine[idx];
}

// TRIANGLE, generalized with a peak-position parameter. peak=0.5 (the
// Modulator::shapeParam default) reproduces the original fixed triangle
// exactly: -1 at t=0, +1 at t=0.5, -1 at t=1.
static float evalTriangle(float phase01, float shapeParam) {
    float peak = clampf(shapeParam, 0.02f, 0.98f);
    if (phase01 < peak) return -1.0f + 2.0f * (phase01 / peak);
    return 1.0f - 2.0f * ((phase01 - peak) / (1.0f - peak));
}

// SQUARE, generalized with a duty-cycle parameter. duty=0.5 (the
// Modulator::shapeParam default) reproduces the original fixed 50% square
// exactly.
static float evalSquare(float phase01, float shapeParam) {
    float duty = clampf(shapeParam, 0.02f, 0.98f);
    return (phase01 < duty) ? 1.0f : -1.0f;
}

// SAW deliberately ignores shapeParam in Phase 1 (ModTypeDescriptor marks
// supportsShapeParam=false for it) -- always the original fixed rising
// ramp, so an old slot with no persisted shapeParam behaves identically.
static float evalSaw(float phase01, float /*shapeParam*/) {
    return 2.0f * phase01 - 1.0f;
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

static inline float applyCurve(CurveType c, float t) {
    switch (c) {
        case CurveType::LINEAR:      return t;
        case CurveType::EASE_IN:     return t * t;
        case CurveType::EASE_OUT:    return t * (2.0f - t);
        case CurveType::EXPONENTIAL: return t * t * t;
        case CurveType::LOGARITHMIC: { float u = 1.0f - t; return 1.0f - u * u * u; }
        case CurveType::S_CURVE:     return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);  // same smootherstep as valueNoise()
        default:                     return t;
    }
}

// Evaluates the multi-point breakpoint curve at `elapsedMs` (already
// resolved for the envelope's loop mode by the caller for LOOP/PING_PONG;
// ONE_SHOT/TRIGGER just clamp to [0, totalMs]).
static float evalEnvCurve(const EnvelopeData& e, float elapsedMs) {
    uint8_t n = e.pointCount;
    if (n == 0) return 0.0f;
    if (n == 1) return e.points[0].value;
    if (elapsedMs <= e.points[0].timeMs) return e.points[0].value;
    for (uint8_t i = 1; i < n; i++) {
        if (elapsedMs <= e.points[i].timeMs) {
            const EnvPoint& p0 = e.points[i - 1];
            const EnvPoint& p1 = e.points[i];
            float span = p1.timeMs - p0.timeMs;
            float frac = span > 0.0f ? (elapsedMs - p0.timeMs) / span : 1.0f;
            frac = applyCurve(p1.curveIntoPoint, frac);
            return p0.value + (p1.value - p0.value) * frac;
        }
    }
    return e.points[n - 1].value;
}

// Legacy fixed 3-stage Attack/Sustain/Release ramp. Used verbatim, byte-
// for-byte unchanged, whenever envData.pointCount == 0 -- every slot
// loaded from a pre-Phase-1 /modulators.json before migration runs, and
// (permanently) any slot a caller deliberately leaves in legacy mode.
static float tickLegacyAdrEnvelope(Modulator& m, uint32_t nowMs) {
    if (m.envStage == Modulator::EnvStage::IDLE) return 0.0f;
    uint32_t elapsed = nowMs - m.envStartMs;

    uint32_t attack  = (uint32_t)m.envAttackMs;
    uint32_t sustain = (uint32_t)m.envSustainMs;
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

// ── ModType tick() implementations (registered as ModTypeDescriptor::tick) ──

static float tickOscillatorType(Modulator& m, uint32_t nowMs) {
    float phase = wrap01(totalCycles(m, nowMs) + m.phaseOffset);
    const WaveShapeDescriptor* sd = findWaveShape(m.shape);
    if (!sd) return 0.0f;
    if (sd->usesLut) return lutLookupSine(phase);
    return sd->eval ? sd->eval(phase, m.shapeParam) : 0.0f;
}

static float tickNoiseType(Modulator& m, uint32_t nowMs) {
    return valueNoise(m.noiseSeed, totalCycles(m, nowMs) + m.phaseOffset);
}

// ENVELOPE: dispatches between the legacy ADR ramp and the multi-point
// breakpoint curve. LOOP/PING_PONG/ONE_SHOT breakpoint modes are stateless
// (recomputed fresh from nowMs, same philosophy as OSCILLATOR/NOISE above)
// -- only TRIGGER mode (and the legacy ADR ramp, which is TRIGGER-only by
// definition) is genuinely stateful via envStage/envStartMs.
static float tickEnvelopeType(Modulator& m, uint32_t nowMs) {
    const EnvelopeData& e = m.envData;
    if (e.pointCount == 0) return tickLegacyAdrEnvelope(m, nowMs);

    float totalMs = e.points[e.pointCount - 1].timeMs;
    if (totalMs <= 0.0f) return e.points[e.pointCount - 1].value;

    switch (e.loopMode) {
        case EnvLoopMode::LOOP: {
            float t = fmodf((float)nowMs, totalMs);
            if (t < 0.0f) t += totalMs;
            return evalEnvCurve(e, t);
        }
        case EnvLoopMode::PING_PONG: {
            float cyc = fmodf((float)nowMs, totalMs * 2.0f);
            if (cyc < 0.0f) cyc += totalMs * 2.0f;
            float t = (cyc <= totalMs) ? cyc : (totalMs * 2.0f - cyc);
            return evalEnvCurve(e, t);
        }
        case EnvLoopMode::ONE_SHOT: {
            float t = (float)nowMs;
            if (t > totalMs) t = totalMs;
            return evalEnvCurve(e, t);
        }
        case EnvLoopMode::TRIGGER:
        default: {
            if (m.envStage == Modulator::EnvStage::IDLE) return 0.0f;
            float elapsed = (float)(nowMs - m.envStartMs);
            if (elapsed >= totalMs) { m.envStage = Modulator::EnvStage::IDLE; return e.points[e.pointCount - 1].value; }
            return evalEnvCurve(e, elapsed);
        }
    }
}

static float tickSequencerType(Modulator& m, uint32_t nowMs) {
    uint8_t n = m.seqStepCount;
    if (n == 0) n = 1;
    if (n > MOD_SEQ_STEPS) n = MOD_SEQ_STEPS;
    float phase = wrap01(totalCycles(m, nowMs) + m.phaseOffset);
    uint8_t step = (uint8_t)(phase * (float)n);
    if (step >= n) step = n - 1;
    return m.seqValues[step];
}

// TRIGGER-mode envelopes (legacy ADR, and multi-point with loopMode ==
// TRIGGER) start their ramp here. Auto-running modes (LOOP/PING_PONG/
// ONE_SHOT) ignore manual trigger() calls -- they're already always
// playing, driven directly by nowMs.
static void triggerEnvelopeType(Modulator& m, uint32_t nowMs) {
    if (m.envData.pointCount > 0 && m.envData.loopMode != EnvLoopMode::TRIGGER) return;
    m.envStage   = Modulator::EnvStage::ATTACK;
    m.envStartMs = nowMs;
}

// ── Built-in descriptor instances (static storage duration; only their
// addresses are stored in the registry) ────────────────────────────────
static const ModTypeDescriptor kTypeOscillator = {
    type_id::OSCILLATOR, "oscillator", "Oscillator", tickOscillatorType, nullptr, true, false, false, false};
static const ModTypeDescriptor kTypeNoise = {
    type_id::NOISE, "noise", "Noise", tickNoiseType, nullptr, false, false, false, false};
static const ModTypeDescriptor kTypeEnvelope = {
    type_id::ENVELOPE, "envelope", "Envelope", tickEnvelopeType, triggerEnvelopeType, false, true, false, false};
static const ModTypeDescriptor kTypeSequencer = {
    type_id::SEQUENCER, "sequencer", "Sequencer", tickSequencerType, nullptr, false, false, true, false};

static const WaveShapeDescriptor kShapeSine = {
    shape_id::SINE, "sine", "Sine", true, nullptr, false};
static const WaveShapeDescriptor kShapeTriangle = {
    shape_id::TRIANGLE, "triangle", "Triangle", false, evalTriangle, true};
static const WaveShapeDescriptor kShapeSquare = {
    shape_id::SQUARE, "square", "Square", false, evalSquare, true};
static const WaveShapeDescriptor kShapeSaw = {
    shape_id::SAW, "saw", "Saw", false, evalSaw, false};

// Same scale/lo/hi values as the pre-registry targetInfo() switch -- see
// modulator_engine.h's file header for why the numeric target ids are
// unchanged (zero JSON migration needed for targetParam).
static const ModTargetDescriptor kTargetScaleX = {
    target_id::TRANSFORM_SCALE_X,
    {target_id::TRANSFORM_SCALE_X, "transform_scale_x", "Scale X", "Transform", paramui::DataType::FLOAT,
     0.1f, 3.0f, 1.0f, 0.01f, "x", "Horizontal scale multiplier", true, false, true},
    1.0f, false};
static const ModTargetDescriptor kTargetScaleY = {
    target_id::TRANSFORM_SCALE_Y,
    {target_id::TRANSFORM_SCALE_Y, "transform_scale_y", "Scale Y", "Transform", paramui::DataType::FLOAT,
     0.1f, 3.0f, 1.0f, 0.01f, "x", "Vertical scale multiplier", true, false, true},
    1.0f, false};
static const ModTargetDescriptor kTargetShiftX = {
    target_id::TRANSFORM_SHIFT_X,
    {target_id::TRANSFORM_SHIFT_X, "transform_shift_x", "Shift X", "Transform", paramui::DataType::FLOAT,
     -20000.0f, 20000.0f, 0.0f, 1.0f, "", "Horizontal position offset (DAC units)", true, false, true},
    8000.0f, false};
static const ModTargetDescriptor kTargetShiftY = {
    target_id::TRANSFORM_SHIFT_Y,
    {target_id::TRANSFORM_SHIFT_Y, "transform_shift_y", "Shift Y", "Transform", paramui::DataType::FLOAT,
     -20000.0f, 20000.0f, 0.0f, 1.0f, "", "Vertical position offset (DAC units)", true, false, true},
    8000.0f, false};
static const ModTargetDescriptor kTargetRotation = {
    target_id::TRANSFORM_ROTATION,
    {target_id::TRANSFORM_ROTATION, "transform_rotation", "Rotation", "Transform", paramui::DataType::FLOAT,
     -3600.0f, 3600.0f, 0.0f, 1.0f, "deg", "Rotation offset in degrees", true, false, true},
    180.0f, false};
static const ModTargetDescriptor kTargetHue = {
    target_id::COLOR_HUE,
    {target_id::COLOR_HUE, "color_hue", "Hue", "Color", paramui::DataType::FLOAT,
     0.0f, 1.0f, 0.0f, 0.01f, "", "Hue shift (wraps instead of clamping)", true, false, true},
    0.5f, true};
static const ModTargetDescriptor kTargetSaturation = {
    target_id::COLOR_SATURATION,
    {target_id::COLOR_SATURATION, "color_saturation", "Saturation", "Color", paramui::DataType::FLOAT,
     0.0f, 2.0f, 1.0f, 0.01f, "x", "Saturation multiplier", true, false, true},
    1.0f, false};
static const ModTargetDescriptor kTargetBrightness = {
    target_id::COLOR_BRIGHTNESS,
    {target_id::COLOR_BRIGHTNESS, "color_brightness", "Brightness", "Color", paramui::DataType::FLOAT,
     0.0f, 2.0f, 1.0f, 0.01f, "x", "Brightness (value) multiplier", true, false, true},
    1.0f, false};
static const ModTargetDescriptor kTargetOptSpeed = {
    target_id::OPT_SPEED,
    {target_id::OPT_SPEED, "opt_speed", "Speed", "Optimizer", paramui::DataType::FLOAT,
     0.0f, 255.0f, 127.0f, 1.0f, "", "Animation speed", true, false, true},
    127.0f, false};
static const ModTargetDescriptor kTargetOptDensity = {
    target_id::OPT_DENSITY,
    {target_id::OPT_DENSITY, "opt_density", "Point Density", "Optimizer", paramui::DataType::FLOAT,
     0.1f, 5.0f, 1.0f, 0.01f, "x", "Optimizer output point density multiplier", true, false, true},
    1.0f, false};

static void registerBuiltins() {
    registerModType(&kTypeOscillator);
    registerModType(&kTypeNoise);
    registerModType(&kTypeEnvelope);
    registerModType(&kTypeSequencer);

    registerWaveShape(&kShapeSine);
    registerWaveShape(&kShapeTriangle);
    registerWaveShape(&kShapeSquare);
    registerWaveShape(&kShapeSaw);

    registerModTarget(&kTargetScaleX);
    registerModTarget(&kTargetScaleY);
    registerModTarget(&kTargetShiftX);
    registerModTarget(&kTargetShiftY);
    registerModTarget(&kTargetRotation);
    registerModTarget(&kTargetHue);
    registerModTarget(&kTargetSaturation);
    registerModTarget(&kTargetBrightness);
    registerModTarget(&kTargetOptSpeed);
    registerModTarget(&kTargetOptDensity);
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
    registerBuiltins();
    buildLut();
    load();  // no-op (all slots stay default/disabled) if /modulators.json is missing/invalid
    ESP_LOGI(TAG, "init: %u slot(s), %u binding(s), %u type(s), %u shape(s), %u target(s)",
             (unsigned)MOD_SLOTS, (unsigned)MOD_BINDINGS,
             (unsigned)modTypeCount(), (unsigned)waveShapeCount(), (unsigned)modTargetCount());
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
        const ModTypeDescriptor* td = findModType(m.type);
        float raw = (td && td->tick) ? td->tick(m, nowMs) : 0.0f;
        m.output = raw * m.level;
    }
    xSemaphoreGive(mtx::modulator);
}

float apply(ModTarget target, float baseValue) {
    if (!s_binds || !s_mods) return baseValue;

    const ModTargetDescriptor* td = findModTarget(target);
    float scale = td ? td->scale        : 1.0f;
    float lo    = td ? td->meta.minVal  : -1e9f;
    float hi    = td ? td->meta.maxVal  : 1e9f;
    bool  wraps = td ? td->wraps        : false;

    if (xSemaphoreTake(mtx::modulator, 0) != pdTRUE) return baseValue;
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

    if (wraps) return wrap01(value);
    if (value < lo) value = lo;
    if (value > hi) value = hi;
    return value;
}

void trigger(uint8_t idx) {
    if (!s_mods || idx >= MOD_SLOTS) return;
    LOCK_MOD();
    Modulator& m = s_mods[idx];
    if (!m.enabled) return;
    const ModTypeDescriptor* td = findModType(m.type);
    if (td && td->trigger) td->trigger(m, millis());
}

Modulator* getModulator(uint8_t idx) {
    if (!s_mods || idx >= MOD_SLOTS) return nullptr;
    return &s_mods[idx];
}

ModBinding* getBinding(uint8_t idx) {
    if (!s_binds || idx >= MOD_BINDINGS) return nullptr;
    return &s_binds[idx];
}

// Applies validated/clamped fields from `obj` onto `m` (partial update --
// only keys present in obj are touched). Caller holds mtx::modulator.
// Split out from setModulator() so load() can apply all slots under one
// lock and one save() at the end instead of a lock+flash-write per slot.
static void applyModulatorFields(Modulator& m, JsonObjectConst obj) {
    if (!obj["enabled"].isNull())     m.enabled     = obj["enabled"] | false;
    if (!obj["type"].isNull())        m.type        = (ModType)clampi((int)(obj["type"] | 0), 0, MAX_MOD_TYPES - 1);
    if (!obj["shape"].isNull())       m.shape       = (WaveShape)clampi((int)(obj["shape"] | 0), 0, MAX_WAVE_SHAPES - 1);
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
    if (!obj["shapeParam"].isNull())   m.shapeParam   = clampf(obj["shapeParam"] | 0.5f, 0.0f, 1.0f);
    if (!obj["envAttackMs"].isNull())  m.envAttackMs  = clampf(obj["envAttackMs"] | 50.0f, 0.0f, 10000.0f);
    if (!obj["envSustainMs"].isNull()) m.envSustainMs = clampf(obj["envSustainMs"] | 200.0f, 0.0f, 10000.0f);
    if (!obj["envReleaseMs"].isNull()) m.envReleaseMs = clampf(obj["envReleaseMs"] | 300.0f, 0.0f, 10000.0f);
    if (!obj["envData"].isNull()) {
        JsonObjectConst ed = obj["envData"].as<JsonObjectConst>();
        if (!ed["loopMode"].isNull())
            m.envData.loopMode = (EnvLoopMode)clampi((int)(ed["loopMode"] | (int)EnvLoopMode::TRIGGER), 0, 3);
        if (!ed["points"].isNull()) {
            JsonArrayConst pts = ed["points"].as<JsonArrayConst>();
            uint8_t n = 0;
            float lastT = 0.0f;
            for (JsonObjectConst p : pts) {
                if (n >= ENV_MAX_POINTS) break;
                float t = clampf(p["t"] | 0.0f, 0.0f, 60000.0f);
                if (n > 0 && t < lastT) t = lastT;   // enforce ascending time order
                m.envData.points[n].timeMs         = t;
                m.envData.points[n].value          = clampf(p["v"] | 0.0f, -1.0f, 1.0f);
                m.envData.points[n].curveIntoPoint = (CurveType)clampi((int)(p["c"] | 0), 0, 5);
                lastT = t;
                n++;
            }
            m.envData.pointCount = n;
        }
    }
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
    // Auto-seed a never-set (still 0) seed regardless of m.type -- keeps
    // this generic across any current/future noise-flavored ModType (built-in
    // NOISE, Phase 4's foreign NOISE2D, ...) without modulator_engine.cpp
    // needing to know their ids. Harmless no-op for types that never read
    // noiseSeed (Oscillator/Envelope/Sequencer).
    else if (m.noiseSeed == 0) m.noiseSeed = esp_random();
}

static volatile bool     s_dirty       = false;
static volatile uint32_t s_dirty_since = 0;

// See modulator_engine.h's maybeFlush() comment for why this defers save()
// instead of calling it inline: PATCH /api/modulators?idx=N runs on
// AsyncTCP's single async_tcp task, shared
// system-wide with every other connection's events (including tcp_accept
// for brand new ones); a live slider drag fires this dozens of times a
// second, and save() is a synchronous LittleFS flash write.
bool setModulator(uint8_t idx, JsonObjectConst obj) {
    if (!s_mods || idx >= MOD_SLOTS) return false;
    { LOCK_MOD(); applyModulatorFields(s_mods[idx], obj); }
    s_dirty = true; s_dirty_since = millis();
    return true;
}

void maybeFlush() {
    if (!s_dirty || millis() - s_dirty_since < 400) return;
    s_dirty = false;
    save();
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
        tmp[n].targetParam  = (ModTarget)clampi((int)(e["targetParam"] | 0), 0, (int)MAX_MOD_TARGETS - 1);
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
    // Deferred save (see maybeFlush()) -- a dragged depth/offset slider fires
    // this dozens of times/sec; a synchronous save() per call used to stall
    // the AsyncTCP task on back-to-back LittleFS writes long enough to
    // exhaust the lwIP TCP-PCB pool (tcp_accept: pcb is NULL, client-side
    // request timeout). setModulator() already avoided this; bindings didn't.
    s_dirty = true; s_dirty_since = millis();
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

void fillStateJson(JsonObject& out) {
    if (!s_mods) return;
    LOCK_MOD();
    JsonArray arr = out["modulators"].to<JsonArray>();
    for (uint8_t i = 0; i < MOD_SLOTS; i++) {
        const Modulator& m = s_mods[i];
        const ModTypeDescriptor* td = findModType(m.type);
        JsonObject o = arr.add<JsonObject>();
        o["idx"]          = i;
        o["enabled"]      = m.enabled;
        o["type"]         = (int)m.type;
        o["typeName"]     = td ? td->key : "oscillator";
        o["shape"]        = (int)m.shape;
        o["cycles"]       = m.cycles;
        o["phaseOffset"]  = m.phaseOffset;
        o["phaseSpeed"]   = m.phaseSpeed;
        o["level"]        = m.level;
        o["bpmSync"]      = m.bpmSync;
        o["bpmDiv"]       = (int)m.bpmDiv;
        o["name"]         = m.name;
        o["shapeParam"]   = m.shapeParam;
        o["envAttackMs"]  = m.envAttackMs;
        o["envSustainMs"] = m.envSustainMs;
        o["envReleaseMs"] = m.envReleaseMs;
        JsonObject ed = o["envData"].to<JsonObject>();
        ed["pointCount"] = m.envData.pointCount;
        ed["loopMode"]   = (int)m.envData.loopMode;
        JsonArray pts = ed["points"].to<JsonArray>();
        for (uint8_t p = 0; p < m.envData.pointCount; p++) {
            JsonObject po = pts.add<JsonObject>();
            po["t"] = m.envData.points[p].timeMs;
            po["v"] = m.envData.points[p].value;
            po["c"] = (int)m.envData.points[p].curveIntoPoint;
        }
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

void fillMetaJson(JsonObject& out) {
    JsonArray types = out["types"].to<JsonArray>();
    for (uint8_t i = 0; i < modTypeCount(); i++) {
        const ModTypeDescriptor& d = modTypeAt(i);
        JsonObject o = types.add<JsonObject>();
        o["id"]              = d.id;
        o["key"]              = d.key;
        o["label"]             = d.label;
        o["usesShape"]        = d.usesShape;
        o["usesEnvelope"]     = d.usesEnvelope;
        o["usesSequencer"]    = d.usesSequencer;
        o["usesShapeParam"]   = d.usesShapeParam;
        o["supportsTrigger"]  = (d.trigger != nullptr);
    }
    JsonArray shapes = out["shapes"].to<JsonArray>();
    for (uint8_t i = 0; i < waveShapeCount(); i++) {
        const WaveShapeDescriptor& d = waveShapeAt(i);
        JsonObject o = shapes.add<JsonObject>();
        o["id"]                  = d.id;
        o["key"]                  = d.key;
        o["label"]                 = d.label;
        o["supportsShapeParam"]   = d.supportsShapeParam;
    }
    JsonArray targets = out["targets"].to<JsonArray>();
    for (uint16_t i = 0; i < modTargetCount(); i++) {
        const ModTargetDescriptor& d = modTargetAt(i);
        JsonObject o = targets.add<JsonObject>();
        o["id"]          = d.id;
        o["key"]          = d.meta.key;
        o["label"]         = d.meta.label;
        o["category"]     = d.meta.category;
        o["min"]           = d.meta.minVal;
        o["max"]           = d.meta.maxVal;
        o["defaultVal"]    = d.meta.defaultVal;
        o["step"]          = d.meta.step;
        o["unit"]          = d.meta.unit;
        o["description"]  = d.meta.description;
        o["wraps"]         = d.wraps;
    }
    static const char* kCurveNames[] = {"Linear", "Ease In", "Ease Out", "Exponential", "Logarithmic", "S-Curve"};
    JsonArray curveTypes = out["curveTypes"].to<JsonArray>();
    for (uint8_t i = 0; i < 6; i++) { JsonObject o = curveTypes.add<JsonObject>(); o["id"] = i; o["label"] = kCurveNames[i]; }

    static const char* kLoopNames[] = {"One Shot", "Loop", "Ping-Pong", "Trigger"};
    JsonArray loopModes = out["loopModes"].to<JsonArray>();
    for (uint8_t i = 0; i < 4; i++) { JsonObject o = loopModes.add<JsonObject>(); o["id"] = i; o["label"] = kLoopNames[i]; }
}

bool save() {
    if (!s_mods || !s_binds) return false;
    JsonDocument doc(&jsonAllocator());
    JsonObject root = doc.to<JsonObject>();
    root["schemaVersion"] = 2;
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

    int  schemaVersion = doc["schemaVersion"] | 1;
    bool migrated = false;
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

        if (schemaVersion < 2) {
            // Pre-Phase-1 file: every ENVELOPE slot has envData.pointCount
            // == 0 (the field didn't exist yet). Synthesize an equivalent
            // 4-point breakpoint curve from the legacy 3-field ADR ramp so
            // playback stays byte-identical, now just re-expressed in the
            // new format. type/shape/targetParam ints need no remapping --
            // built-in registry ids match the pre-registry enum values.
            for (uint8_t s = 0; s < MOD_SLOTS; s++) {
                Modulator& m = s_mods[s];
                if (m.type != type_id::ENVELOPE || m.envData.pointCount != 0) continue;
                float a = m.envAttackMs, su = m.envSustainMs, r = m.envReleaseMs;
                float ts[4] = {0.0f, a, a + su, a + su + r};
                float vs[4] = {0.0f, 1.0f, 1.0f, 0.0f};
                for (uint8_t p = 0; p < 4; p++) {
                    m.envData.points[p].timeMs         = ts[p];
                    m.envData.points[p].value          = vs[p];
                    m.envData.points[p].curveIntoPoint = CurveType::LINEAR;
                }
                m.envData.pointCount = 4;
                m.envData.loopMode   = EnvLoopMode::TRIGGER;
                migrated = true;
            }
        }
    }
    // Narrow, intentional exception to "load() never saves": persist the
    // migrated schema once so subsequent boots skip the branch above.
    if (migrated) save();
    return true;
}

}  // namespace modulator
