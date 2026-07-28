#pragma once
/**
 * layers.h -- Layer Engine data model (Phase 1 of the Layer Engine, see
 * plans/mutable-dreaming-candy.md)
 *
 * gLayerStack (config.h) holds up to LAYER_MAX parametric layers, each with
 * a Shape (layer_shapes::ShapeType + param), Color (HSL/HSV), and Transform
 * (scale/shift/rotate) -- rendered by pattern_engine.cpp's Layer mode when
 * gLayerStack.active is true. Mirrors sequencer.h/.cpp's structure: global
 * struct guarded by mtx::state, LittleFS JSON persistence at /layers.json.
 *
 * Two update granularities, matching the split already used elsewhere in
 * this codebase (sequencer.cpp's whole-playlist setPlaylist() vs.
 * modulator_engine.cpp's per-slot setModulator()): setStack() replaces the
 * whole layer array (add/remove/reorder, infrequent), setLayer() partially
 * updates one existing layer's fields (frequent, e.g. a live slider drag) --
 * cheap enough to call on every debounced oninput without round-tripping
 * the whole array.
 */
#include "config.h"
#include <ArduinoJson.h>

namespace layers {

// Loads /layers.json (LittleFS). Leaves gLayerStack.active = false
// regardless of what was saved -- same fail-safe-off rule as
// sequencer::init(). Call once, after LittleFS.begin() (see web_ui::init()).
void init();

// Replaces the whole layer array (validates/clamps each entry) plus the
// active/selected fields, persists. Returns false (state left untouched) if
// arr is malformed.
bool setStack(JsonArrayConst layersArr, bool active, uint8_t selected);

// Partial field update for one existing layer (only keys present in obj are
// touched, validated/clamped same as modulator::setModulator()), persists.
// Returns false if idx >= gLayerStack.count.
bool setLayer(uint8_t idx, JsonObjectConst obj);

// Toggles Layer mode on/off without touching layer contents, persists.
void setActive(bool active);

// Empties the stack, turns Layer mode off, persists.
void clear();

// Serializes the full current state (active/selected/layers[]) -- shared by
// GET /api/layers and any future status consumer.
void fillStateJson(JsonObject& out);

// LittleFS persistence at /layers.json.
bool save();
bool load();

}  // namespace layers
