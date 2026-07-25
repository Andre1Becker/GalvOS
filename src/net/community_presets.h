#pragma once
/**
 * community_presets.h -- LittleFS storage for GitHub-hosted community presets
 *
 * A community preset bundles an optimizer tuning (OptimizerLiveConfig-shaped,
 * same field names/bounds as /api/optimizer-live) with a small set of preset
 * playback params (which built-in preset to run, color, speed, size). The
 * WebUI downloads these from raw.githubusercontent.com and POSTs them here
 * for validation + storage; activation is orchestrated by web_ui.cpp (it
 * needs gLivePreset/patterns::setPreset()/gOptimizerConfig, none of which
 * this module touches directly, same separation BackupManager keeps from
 * the AsyncWebServer routes that call it).
 *
 * Storage: one file per preset at /presets/community/<id>.json, capped at
 * MAX_FILE_BYTES. Never touches NVS -- this is LittleFS-only, like the
 * WebUI assets it lives alongside.
 */

#include <ArduinoJson.h>
#include <Arduino.h>

namespace community_presets {

constexpr uint8_t SCHEMA_VERSION = 1;
constexpr size_t  MAX_FILE_BYTES = 10 * 1024;
constexpr size_t  MAX_ID_LEN     = 64;
// Tighter than the general /api/optimizer-live ceiling (PATTERN_POINTS_MAX
// = 2048) -- a downloaded preset shouldn't be able to request a frame budget
// above the highest tuned default any built-in profile actually uses.
constexpr uint16_t MAX_PTS_PER_FRAME_CEILING = 1300;

// mkdir's /presets/community if missing. Call once, after LittleFS.begin().
void init();

// Lowercases and strips anything outside [a-z0-9-], truncated to MAX_ID_LEN.
// Returns "" if nothing valid remains -- callers must treat that as invalid.
String sanitizeId(const String& raw);

// Validates a full preset document (meta + optimizer_profile + preset_params)
// against the community-preset schema. Returns true iff every check passes;
// otherwise false with a human-readable reason. Performs no I/O and applies
// nothing -- pure validation, like BackupManager::deserializeFromJson.
bool validate(JsonDocument& doc, String& reason);

// Writes doc to /presets/community/<id>.json. Caller must validate() first.
bool save(JsonDocument& doc);

// Loads and parses /presets/community/<id>.json into doc. Returns false if
// the id is invalid, the file doesn't exist, or it fails to parse.
bool load(const String& id, JsonDocument& doc);

// Deletes /presets/community/<id>.json.
bool remove(const String& id);

// Rewrites meta.name in place (id/filename unchanged). Fails if the preset
// doesn't exist or newName is empty.
bool rename(const String& id, const String& newName);

// Appends {id, name, author, size_bytes} for every stored preset into arr.
void list(JsonArray arr);

// Fills {total_bytes, used_bytes, free_bytes, preset_count}.
void fsInfo(JsonObject obj);

} // namespace community_presets
