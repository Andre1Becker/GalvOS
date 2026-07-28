#pragma once
/**
 * sequencer.h -- BPM-synced preset playlist ("Preset Sequencer")
 *
 * Walks gSequencer.steps[] in order, advancing one step every time the
 * current step's beat count elapses. Beat detection piggybacks on the
 * existing bpm_clock module (see bpm_clock.h) by watching its wall-clock
 * phase_ms for a high->low wrap -- there is no beat-tick event/callback
 * elsewhere in the codebase, so this is the first consumer of that kind.
 *
 * tick() must be called once per patterns::task() iteration (Core 0) so
 * beat detection and patterns::setPreset() calls stay on the same thread
 * that already dispatches Preset mode -- no additional locking is needed
 * for the read side beyond the existing gSequencer/mtx::state convention.
 */
#include "config.h"
#include <ArduinoJson.h>

namespace sequencer {

// Loads the saved playlist from /sequencer.json (LittleFS). Leaves
// gSequencer.running = false regardless of what was saved -- playback
// resuming on its own the instant the device boots would run against this
// project's fail-safe-off philosophy for a Class 4 laser. Call once, after
// LittleFS.begin() (see web_ui::init()).
void init();

// Per-iteration beat/step advance. No-op unless gSequencer.running.
void tick();

// True while a transitionBeats blank window is active -- pattern_engine.cpp
// keeps rendering/animating normally (so color/rotation/modulator state
// doesn't freeze) but forces the frame's points blank right before push, so
// the beam output alone goes dark for the transition.
bool isBlanking();

// Transport. All hard-cut immediately (no transition blank) except the
// beat-driven advance inside tick().
void start();   // jumps to step 0, sets running = true
void stop();    // running = false; current preset stays on screen
void next();    // manual advance, ignores beat/transition
void prev();    // manual step back, ignores beat/transition
void jumpTo(uint8_t step);

// Replaces the whole playlist (validating/clamping each step) and persists
// it. Returns false (playlist left untouched) if arr is malformed.
bool setPlaylist(JsonArrayConst steps, bool loopAll);

// Empties the playlist, stops playback, persists the (now empty) state.
void clear();

// Serializes the full current state (loop/running/currentStep/steps[]) --
// shared by GET /api/sequencer and buildStateJson() so both stay in sync.
void fillStateJson(JsonObject& out);

// LittleFS persistence at /sequencer.json. save() is called by setPlaylist()
// and clear(); load() is called once by init(). Exposed mainly so init()
// (declared above it in this header) resolves without a forward-decl dance.
bool save();
bool load();

} // namespace sequencer
