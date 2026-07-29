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
 * only pattern_engine.cpp's Preset-Mode render path and
 * preset_patterns.cpp's OPT_DENSITY optimizer-config hook -- the primary/
 * default interactive mode; Curve/Paint/Text/ILDA are not wired to
 * modulators yet) to fold every active binding's contribution into a live
 * parameter value.
 *
 * Slot/binding storage and the waveform LUT are PSRAM-backed (ps_malloc),
 * consistent with this project's convention for state that outlives a
 * single request (see util/ps_scratch.h).
 *
 * -- Extensibility (registry) --
 * ModType/WaveShape/ModTarget are plain integer id-spaces, not enum class,
 * so a future module (e.g. a Camera or Duplicator living in its own .cpp)
 * can define its own type/shape/target constants and register them via
 * registerModType()/registerWaveShape()/registerModTarget() from its own
 * init() -- called after modulator::init() -- without ever editing this
 * file again. Built-in ids (the type_id/shape_id/target_id namespaces
 * below) are numerically identical to the pre-registry enum values they
 * replace, so persisted /modulators.json ints need no migration.
 */

#include "config.h"
#include "util/param_meta.h"
#include <ArduinoJson.h>

namespace modulator {

constexpr uint8_t  MOD_SLOTS     = 8;
constexpr uint8_t  MOD_BINDINGS  = 16;
constexpr uint8_t  MOD_SEQ_STEPS = 16;
constexpr size_t   MOD_LUT_SIZE  = 512;

// -- Registry id-spaces (plain integers, extensible) --
using ModType   = uint8_t;
using WaveShape = uint8_t;
using ModTarget = uint16_t;   // widened vs. legacy uint8_t: headroom for future targets

namespace type_id {
    constexpr ModType OSCILLATOR = 0, NOISE = 1, ENVELOPE = 2, SEQUENCER = 3;
}
namespace shape_id {
    constexpr WaveShape SINE = 0, TRIANGLE = 1, SQUARE = 2, SAW = 3;
}
namespace target_id {
    constexpr ModTarget TRANSFORM_SCALE_X  = 0;
    constexpr ModTarget TRANSFORM_SCALE_Y  = 1;
    constexpr ModTarget TRANSFORM_SHIFT_X  = 2;
    constexpr ModTarget TRANSFORM_SHIFT_Y  = 3;
    constexpr ModTarget TRANSFORM_ROTATION = 4;
    constexpr ModTarget COLOR_HUE          = 5;
    constexpr ModTarget COLOR_SATURATION   = 6;
    constexpr ModTarget COLOR_BRIGHTNESS   = 7;
    constexpr ModTarget OPT_SPEED          = 8;
    constexpr ModTarget OPT_DENSITY        = 9;
}

constexpr uint8_t  MAX_MOD_TYPES   = 16;
constexpr uint8_t  MAX_WAVE_SHAPES = 8;
constexpr uint16_t MAX_MOD_TARGETS = 64;

// BPM-sync subdivision -- cycle length in beats: D1 = whole note (4 beats
// per cycle) .. D16 = sixteenth note (0.25 beats per cycle). Combines
// multiplicatively with Modulator::cycles for fine rate control.
enum class BpmDiv : uint8_t { D1 = 0, D2 = 1, D4 = 2, D8 = 3, D16 = 4 };

// -- Multi-point envelope (replaces the old fixed 3-stage Attack/Sustain/
// Release ramp for slots that opt in by setting pointCount > 0; slots with
// pointCount == 0 -- every slot loaded from a pre-Phase-1 /modulators.json
// -- keep running the legacy envAttackMs/envSustainMs/envReleaseMs ramp
// byte-for-byte unchanged, see modulator_engine.cpp's tickEnvelope()).
enum class CurveType   : uint8_t { LINEAR = 0, EASE_IN = 1, EASE_OUT = 2, EXPONENTIAL = 3, LOGARITHMIC = 4, S_CURVE = 5 };
enum class EnvLoopMode : uint8_t { ONE_SHOT = 0, LOOP = 1, PING_PONG = 2, TRIGGER = 3 };  // TRIGGER = today's externally-gated semantics

constexpr uint8_t ENV_MAX_POINTS = 8;

struct EnvPoint {
    float     timeMs         = 0.0f;   // ascending, absolute ms from envelope start
    float     value          = 0.0f;   // [-1..1], matches oscillator/noise output range
    CurveType curveIntoPoint = CurveType::LINEAR;  // shape of the segment ENDING at this point
};

struct EnvelopeData {
    EnvPoint    points[ENV_MAX_POINTS];
    uint8_t     pointCount = 0;   // 0 = "not set" -> legacy 3-field ADR ramp is used instead
    EnvLoopMode loopMode   = EnvLoopMode::TRIGGER;
};

struct Modulator {
    bool      enabled      = false;   // slot in use
    ModType   type         = type_id::OSCILLATOR;
    WaveShape shape        = shape_id::SINE;
    float     cycles       = 1.0f;    // frequency multiplier
    float     phaseOffset  = 0.0f;    // [0..1)
    float     phaseSpeed   = 1.0f;    // Hz, Free mode (bpmSync=false)
    float     level        = 1.0f;    // output scale [0..1]
    bool      bpmSync      = false;
    BpmDiv    bpmDiv       = BpmDiv::D4;
    char      name[16]     = {0};

    // OSCILLATOR-only. Meaning depends on shape: SQUARE = duty cycle
    // [0..1], TRIANGLE/SAW = saw<->triangle<->ramp morph [0..1]. Ignored
    // by SINE in Phase 1 (reserved for future wavefolding).
    float     shapeParam   = 0.5f;

    // ENVELOPE-only, legacy fallback (ms). Unipolar 0->1->0 ramp, triggered
    // externally. Kept permanently -- see envData below.
    float     envAttackMs  = 50.0f;
    float     envSustainMs = 200.0f;
    float     envReleaseMs = 300.0f;

    // ENVELOPE-only, multi-point breakpoint curve. Takes precedence over
    // the legacy 3 fields above when pointCount > 0.
    EnvelopeData envData;

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
    ModTarget targetParam  = target_id::TRANSFORM_ROTATION;
    float     depth        = 0.0f;   // -1..1
    float     offset       = 0.0f;   // additive base, in the target's native unit
};

// -- Registry descriptors --
// Plain-data, function-pointer-based (no virtual dispatch -- see file
// header comment). Each is a `static const` struct owned by whichever .cpp
// defines it; register*() stores a POINTER into a fixed-size, direct-
// indexed array, so lookup stays O(1)/zero-heap, same cost class as the
// switch statements it replaces.
struct ModTypeDescriptor {
    ModType     id;
    const char* key;      // "oscillator" -- JSON typeName, replaces old switch
    const char* label;
    float (*tick)(Modulator& m, uint32_t nowMs);     // raw [-1..1], pre-`level`
    void  (*trigger)(Modulator& m, uint32_t nowMs);   // nullptr if unsupported
    bool  usesShape, usesEnvelope, usesSequencer;     // UI hints
    // Type-level UI hint: show the generic shapeParam slider for this type
    // regardless of usesShape/wave-shape (Oscillator's shapeParam slider is
    // instead gated per-WaveShape via WaveShapeDescriptor::supportsShapeParam
    // -- this flag is for a type with no shape dropdown at all, e.g. Phase 4's
    // NOISE2D, that still wants shapeParam exposed). Defaults false for all
    // Phase 1-3 built-ins (trailing aggregate-init field -- see
    // modulator_engine.cpp's kTypeOscillator/kTypeNoise/kTypeEnvelope/
    // kTypeSequencer, each updated with an explicit trailing `false`).
    //
    // NOTE: no default member initializer here on purpose -- PlatformIO/
    // Arduino-ESP32 builds under -std=gnu++11, where a struct with a default
    // member initializer is no longer an aggregate and positional brace-init
    // (`ModTypeDescriptor{...}`) stops compiling (see point_optimizer.h's
    // PathVertex/PathSegment header comment for the same landmine). Every
    // aggregate-init site (4 built-ins + the 2 dummy fallbacks in
    // modulator_engine.cpp) must list this field explicitly.
    bool  usesShapeParam;
};

struct WaveShapeDescriptor {
    WaveShape   id;
    const char* key;
    const char* label;
    bool        usesLut;                               // true only for SINE
    float (*eval)(float phase01, float shapeParam);     // closed-form (non-LUT shapes); nullptr if usesLut
    bool        supportsShapeParam;                     // UI hint: show Slope/Shape slider
};

struct ModTargetDescriptor {
    ModTarget          id;
    paramui::ParamMeta meta;
    float              scale;   // native units per raw modulator unit
    bool               wraps;   // true only for COLOR_HUE (wrap01 vs clamp)
};

// Registers a descriptor into the corresponding fixed-size table. Returns
// false if the id is already registered or the table is full. Descriptors
// must have static storage duration (only the pointer is stored).
bool registerModType  (const ModTypeDescriptor*   d);
bool registerWaveShape(const WaveShapeDescriptor* d);
bool registerModTarget(const ModTargetDescriptor* d);

const ModTypeDescriptor*   findModType  (ModType id);
const WaveShapeDescriptor* findWaveShape(WaveShape id);
const ModTargetDescriptor* findModTarget(ModTarget id);

// Continuous, unwrapped cycle count since boot (BPM-synced or free-running
// per m.bpmSync, scaled by m.cycles) -- the same stateless time-phase base
// Oscillator/Noise/Sequencer tick() implementations use. Exported so a
// foreign ModType producer (e.g. Phase 4's spatial_noise.cpp) can share the
// exact same BPM-sync semantics instead of reimplementing them.
float totalCycles(const Modulator& m, uint32_t nowMs);

uint8_t  modTypeCount();
const ModTypeDescriptor& modTypeAt(uint8_t i);
uint8_t  waveShapeCount();
const WaveShapeDescriptor& waveShapeAt(uint8_t i);
uint16_t modTargetCount();
const ModTargetDescriptor& modTargetAt(uint16_t i);

// Serializes the full registry (types/shapes/targets) for the WebUI's
// boot-time /api/modulators/meta fetch, so a foreign module's registered
// target/type/shape shows up in the editor without any WebUI code change.
void fillMetaJson(JsonObject& out);

// Allocates PSRAM-backed slot/binding storage, registers the 4 built-in
// modulator types + 4 built-in wave shapes + 10 built-in targets, builds
// the waveform LUT, and loads /modulators.json (LittleFS). Call once from
// web_ui::init(), after LittleFS.begin() -- mirrors sequencer::init()'s
// contract. Foreign modules (e.g. a future camera.cpp) must call their own
// register*() calls AFTER this.
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

// Flushes a pending save() queued by setModulator() once ~400ms have passed
// since the last field write, so a continuous slider drag serializes at
// most one flash write instead of one per PATCH (setModulator() used to
// call save() synchronously on the AsyncTCP request path, which stalls the
// whole system-wide TCP pcb pool for as long as the flash write takes).
// Call periodically from a task that is neither the network stack nor the
// render loop (currently safety::task()).
void maybeFlush();

}  // namespace modulator
