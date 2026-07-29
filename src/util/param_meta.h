#pragma once
/**
 * param_meta.h -- generic parameter metadata descriptor.
 *
 * Describes a single animatable/modulatable parameter (id, label, range,
 * unit, ...) independent of which subsystem owns it. Used by
 * modulator_engine.h's ModTargetDescriptor today; intended for reuse by
 * future Camera/Duplicator modules so the WebUI can build parameter pickers
 * generically instead of hardcoding a per-module control list.
 */

#include <stdint.h>

namespace paramui {

enum class DataType : uint8_t { FLOAT, INT, BOOL, ENUM };

struct ParamMeta {
    uint16_t    id;              // stable, persisted numeric id (0 = invalid)
    const char* key;              // JSON key / internal name
    const char* label;            // UI display label
    const char* category;         // "Transform" | "Color" | "Optimizer" | ...
    DataType    dataType;
    float       minVal, maxVal, defaultVal, step;
    const char* unit;              // "", "deg", "Hz", "%"
    const char* description;
    bool        animatable   : 1;  // may be bound as a modulator TARGET
    bool        modulatable  : 1;  // reserved: may itself carry a modulator
    bool        serializable : 1;
};

}  // namespace paramui
