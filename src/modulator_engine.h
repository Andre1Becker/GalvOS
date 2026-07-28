#pragma once
/**
 * modulator_engine.h -- LFO / Noise / Envelope / Sequencer modulation matrix
 *
 * 8 modulator slots, each producing a stateless-per-frame [-1..1] output
 * (scaled by the slot's own `level`), routed to pattern parameters through
 * up to 16 bindings (modulator -> target param, with depth + offset).
 *
 * tick() is called once per frame from galvo_out.cpp::updateSnapshot() --
 * the same per-frame hook bpm_clock::tickMs() already uses (Core 1,
 * galvoTask, NOT per point). OSCILLATOR/NOISE/SEQUENCER outputs are
 * recomputed fresh from millis() + bpm_clock::gBpm every tick (same
 * stateless-phase trick bpm_clock.h itself uses) -- no phase accumulator,
 * so no drift and no cross-core state to race on for those three types.
 * ENVELOPE is the one genuinely stateful type (event-triggered via
 * POST /api/modulators/:id/trigger), tracked by envStage/envStartMs.
 *
 * apply(target, baseValue) is called from the consuming side (currently
 * only pattern_engine.cpp's Preset-Mode render path -- the primary/default
 * interactive mode; Curve/Paint/Text/ILDA are not wired to modulators yet)
 * to fold every active binding's contribution into a live parameter value.
 *
 * Slot/binding storage and the 4x512-entry waveform LUT are PSRAM-backed
 * (ps_malloc), consistent with this project's convention for state that
 * outlives a single request (see util/ps_scratch.h).
 */

#include "config.h"
#include <ArduinoJson.h>

namespace modulator {

constexpr uint8_t  MOD_SLOTS     = 8;
constexpr uint8_t  MOD_BINDINGS  = 16;
constexpr uint8_t  MOD_SEQ_STEPS = 16;
constexpr size_t   MOD_LUT_SIZE  = 512;

enum class ModType : uint8_t { OSCILLATOR = 0, NOISE = 1, ENVELOPE = 2, SEQUENCER = 3 };
enum class WaveShape : uint8_t { SINE = 0, TRIANGLE = 1, SQUARE = 2, SAW = 3 };

// BPM-sync subdivision -- cycle length in beats: D1 = whole note (4 beats
// per cycle) .. D16 = sixteenth note (0.25 beats per cycle). Combines
// multiplicatively with Modulator::cycles for fine rate control.
enum class BpmDiv : uint8_t { D1 = 0, D2 = 1, D4 = 2, D8 = 3, D16 = 4 };

enum class ModTarget : uint8_t {
    TRANSFORM_SCALE_X  = 0,
    TRANSFORM_SCALE_Y  = 1,
    TRANSFORM_SHIFT_X  = 2,
    TRANSFORM_SHIFT_Y  = 3,
    TRANSFORM_ROTATION = 4,
    COLOR_HUE          = 5,
    COLOR_SATURATION   = 6,
    COLOR_BRIGHTNESS   = 7,
    OPT_SPEED          = 8,
    OPT_DENSITY        = 9,
    MOD_TARGET_COUNT   = 10
};

struct Modulator {
    bool      enabled      = false;   // slot in use
    ModType   type         = ModType::OSCILLATOR;
    WaveShape shape        = WaveShape::SINE;
    float     cycles       = 1.0f;    // frequency multiplier
    float     phaseOffset  = 0.0f;    // [0..1)
    float     phaseSpeed   = 1.0f;    // Hz, Free mode (bpmSync=false)
    float     level        = 1.0f;    // output scale [0..1]
    bool      bpmSync      = false;
    BpmDiv    bpmDiv       = BpmDiv::D4;
    char      name[16]     = {0};

    // ENVELOPE-only (ms). Unipolar 0->1->0 ramp, triggered externally.
    float     envAttackMs  = 50.0f;
    float     envSustainMs = 200.0f;
    float     envReleaseMs = 300.0f;

    // SEQUENCER-only. seqValues are user-set, range [-1..1]; only the
    // first seqStepCount entries are used.
    float     seqValues[MOD_SEQ_STEPS] = {0};
    uint8_t   seqStepCount = 8;

    // NOISE-only. Persisted so a slot's noise curve is stable across
    // save/reload instead of reseeding to a new random curve every boot.
    uint32_t  noiseSeed = 0;

    // -- runtime-only, not persisted / not (de)serialized --
    float output = 0.0f;   // last tick()'s value, raw output * level

    enum class EnvStage : uint8_t { IDLE, ATTACK, SUSTAIN, RELEASE };
    EnvStage envStage   = EnvStage::IDLE;
    uint32_t envStartMs = 0;
};

struct ModBinding {
    bool      active       = false;
    uint8_t   modulatorIdx = 0;
    ModTarget targetParam  = ModTarget::TRANSFORM_ROTATION;
    float     depth        = 0.0f;   // -1..1
    float     offset       = 0.0f;   // additive base, in the target's native unit
};

// Allocates PSRAM-backed slot/binding storage, builds the waveform LUT,
// and loads /modulators.json (LittleFS). Call once from web_ui::init(),
// after LittleFS.begin() -- mirrors sequencer::init()'s contract.
void init();

// Per-frame hook -- call once per frame (galvo_out.cpp::updateSnapshot()),
// NOT per point. Recomputes every enabled slot's `output` from nowMs +
// bpm_clock::gBpm. Budget: <10us total for all 8 slots (no sinf()/cosf();
// oscillator/noise/sequencer read a precomputed LUT / closed-form hash).
void tick(uint32_t nowMs);

// Sums every active binding targeting `target`
// (modulators[binding.modulatorIdx].output * binding.depth * targetScale +
// binding.offset) onto baseValue, then clamps to that target's valid
// range. Returns baseValue unchanged if no binding targets it (cheap
// early-out -- safe to call unconditionally from a render hot path).
float apply(ModTarget target, float baseValue);

// Starts an ENVELOPE-type slot's attack phase. No-op for other types or
// disabled/out-of-range slots.
void trigger(uint8_t idx);

// Bounds-checked accessors; nullptr if out of range or PSRAM alloc failed.
Modulator*  getModulator(uint8_t idx);
ModBinding* getBinding(uint8_t idx);

// Validates + writes a single slot's fields from a JSON object (partial
// update -- only keys present in obj are touched), persists. Returns false
// if idx is out of range.
bool setModulator(uint8_t idx, JsonObjectConst obj);

// Whole-array binding replace (validates/clamps each entry), persists.
bool setBindings(JsonArrayConst arr);

// Clears one slot back to defaults (enabled=false) and any bindings that
// referenced it, persists.
void clearModulator(uint8_t idx);

// Clears all slots + bindings, persists.
void resetAll();

// Serializes modulators[]/bindings[] -- shared by the GET handlers.
void fillStateJson(JsonObject& out);
void fillBindingsJson(JsonArray& out);

// LittleFS persistence at /modulators.json.
bool save();
bool load();

}  // namespace modulator
