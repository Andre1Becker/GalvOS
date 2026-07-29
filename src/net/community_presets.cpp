#include "community_presets.h"
#include "json_alloc.h"
#include "patterns/preset_patterns.h"
#include <LittleFS.h>
#include <esp_log.h>
#include <string.h>

namespace community_presets {

static const char* TAG = "community";
static const char* DIR = "/presets/community";

static String pathFor(const String& id) {
    return String(DIR) + "/" + id + ".json";
}

void init() {
    // LittleFS's mkdir() is not recursive -- "/presets" must exist before
    // "/presets/community" can be created, unlike open(..., create=true).
    if (!LittleFS.exists("/presets")) {
        if (!LittleFS.mkdir("/presets")) ESP_LOGE(TAG, "mkdir /presets failed");
    }
    if (!LittleFS.exists(DIR)) {
        if (!LittleFS.mkdir(DIR)) ESP_LOGE(TAG, "mkdir %s failed", DIR);
    }
}

String sanitizeId(const String& raw) {
    String out;
    out.reserve(raw.length());
    for (size_t i = 0; i < raw.length() && out.length() < MAX_ID_LEN; i++) {
        char c = raw[i];
        if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-') out += c;
    }
    return out;
}

// Bounds mirror applyOptimizerOverrides() in web_ui.cpp (kept in sync by
// hand, same convention BackupManager documents for its own field list) --
// out-of-range here means a downloaded preset gets rejected outright rather
// than silently clamped, since it was never tuned against this hardware.
static bool checkOptimizerProfile(JsonObjectConst op, String& reason) {
    if (op.isNull()) { reason = "optimizer_profile missing"; return false; }

    struct FieldF { const char* key; float lo; float hi; };
    struct FieldI { const char* key; int lo; int hi; };
    struct FieldB { const char* key; };

    static const FieldF floats[] = {
        {"corner_angle_deg", 0.0f, 180.0f},
        {"pts_per_1000_units", 0.1f, 50.0f},
        {"blank_pts_per_1000_units", 0.1f, 50.0f},
        {"resample_spacing_units", 10.0f, 2000.0f},
        {"ring_freq_hz", 1.0f, 2000.0f},
        {"ring_damping_ratio", 0.0f, 0.9f},
        {"jitter_amount_units", 0.0f, 2000.0f},
        {"max_step_units", 50.0f, 32767.0f},
        {"max_accel_units", 10.0f, 32767.0f},
    };
    static const FieldI ints[] = {
        {"min_corner_pts", 1, 20},
        {"max_corner_pts", 1, 20},
        {"min_segment_pts", 2, 20},
        {"blank_samples", 1, 100},
        {"max_pts_per_frame", 50, (int)MAX_PTS_PER_FRAME_CEILING},
        {"min_blank_samples", 1, 100},
        {"min_interior_pts_per_segment", 0, 50},
        {"stage1_blank_target", 1, 100},
    };
    static const FieldB bools[] = {
        {"resample_enabled"}, {"ringing_comp_enabled"}, {"jitter_enabled"},
        {"vel_clamp_enabled"}, {"accel_clamp_enabled"},
    };

    for (const auto& f : floats) {
        if (!op[f.key].is<float>()) { reason = String("optimizer_profile.") + f.key + " missing/not a number"; return false; }
        float v = op[f.key];
        if (v < f.lo || v > f.hi) { reason = String("optimizer_profile.") + f.key + " out of range"; return false; }
    }
    for (const auto& f : ints) {
        if (!op[f.key].is<int>()) { reason = String("optimizer_profile.") + f.key + " missing/not an integer"; return false; }
        int v = op[f.key];
        if (v < f.lo || v > f.hi) { reason = String("optimizer_profile.") + f.key + " out of range"; return false; }
    }
    for (const auto& f : bools) {
        if (!op[f.key].is<bool>()) { reason = String("optimizer_profile.") + f.key + " missing/not a boolean"; return false; }
    }
    return true;
}

// name/author must be printable ASCII (0x20-0x7E) only, non-empty, capped at
// MAX_META_LEN -- no silent truncation, a violation is an outright reject.
static bool checkTextField(const char* value, const char* fieldName, String& reason) {
    size_t len = strlen(value);
    if (len == 0) { reason = String(fieldName) + " missing"; return false; }
    if (len > MAX_META_LEN) { reason = String(fieldName) + " exceeds max length " + String(MAX_META_LEN); return false; }
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)value[i];
        if (c < 0x20 || c > 0x7E) { reason = String(fieldName) + " must be printable ASCII only"; return false; }
    }
    return true;
}

static bool checkPresetParams(JsonObjectConst pp, String& reason) {
    if (pp.isNull()) { reason = "preset_params missing"; return false; }
    if (!pp["preset_idx"].is<int>()) { reason = "preset_params.preset_idx missing/not an integer"; return false; }
    int idx = pp["preset_idx"];
    if (idx < 0 || idx >= (int)presets::PRESET_COUNT) { reason = "preset_params.preset_idx out of range"; return false; }

    // Project rule: preset color channels are on/off only -- intermediate
    // values only ever come from col_override, never a stored preset.
    static const char* colorKeys[] = {"col_r", "col_g", "col_b"};
    for (const char* k : colorKeys) {
        if (pp[k].is<int>()) {
            int v = pp[k];
            if (v != 0 && v != 255) { reason = String("preset_params.") + k + " must be 0 or 255"; return false; }
        }
    }
    if (pp["speed"].is<int>()) {
        int v = pp["speed"];
        if (v < 0 || v > 255) { reason = "preset_params.speed out of range"; return false; }
    }
    if (pp["size_val"].is<int>()) {
        int v = pp["size_val"];
        if (v < 0 || v > 255) { reason = "preset_params.size_val out of range"; return false; }
    }
    return true;
}

bool validate(JsonDocument& doc, String& reason) {
    JsonObjectConst meta = doc["meta"];
    if (meta.isNull()) { reason = "meta missing"; return false; }
    if (!meta["schema_version"].is<int>() || (int)meta["schema_version"] != SCHEMA_VERSION) {
        reason = "meta.schema_version must be " + String(SCHEMA_VERSION); return false;
    }
    const char* rawId = meta["id"] | "";
    String id(rawId);
    if (id.length() == 0 || id.length() > MAX_ID_LEN || sanitizeId(id) != id) {
        reason = "meta.id must match [a-z0-9-]+, max " + String(MAX_ID_LEN) + " chars"; return false;
    }
    const char* name = meta["name"] | "";
    if (!checkTextField(name, "meta.name", reason)) return false;

    // author is optional -- only validate it when the key is actually present.
    if (!meta["author"].isNull()) {
        const char* author = meta["author"] | "";
        if (!checkTextField(author, "meta.author", reason)) return false;
    }

    if (!checkOptimizerProfile(doc["optimizer_profile"], reason)) return false;
    if (!checkPresetParams(doc["preset_params"], reason)) return false;

    // Reject anything the schema doesn't know about rather than silently
    // ignoring it -- keeps the on-disk format from drifting unnoticed.
    for (JsonPairConst kv : doc.as<JsonObjectConst>()) {
        const char* k = kv.key().c_str();
        if (strcmp(k, "meta") != 0 && strcmp(k, "optimizer_profile") != 0 && strcmp(k, "preset_params") != 0) {
            reason = String("unknown top-level key: ") + k;
            return false;
        }
    }
    return true;
}

bool save(JsonDocument& doc, String& reason) {
    const char* rawId = doc["meta"]["id"] | "";
    String id(rawId);
    if (id.length() == 0) { reason = "meta.id missing"; return false; }

    String path = pathFor(id);
    // Updates to an already-stored id don't count against the cap -- only
    // reject when this would add a brand-new file past MAX_COMMUNITY_PRESETS.
    if (!LittleFS.exists(path)) {
        uint8_t count = 0;
        File dir = LittleFS.open(DIR);
        if (dir && dir.isDirectory()) {
            File entry = dir.openNextFile();
            while (entry) {
                if (!entry.isDirectory()) {
                    String fname = entry.name();
                    int slash = fname.lastIndexOf('/');
                    if (slash >= 0) fname = fname.substring(slash + 1);
                    if (fname.endsWith(".json")) count++;
                }
                entry.close();
                entry = dir.openNextFile();
            }
            dir.close();
        }
        if (count >= MAX_COMMUNITY_PRESETS) {
            reason = "preset limit reached (" + String(MAX_COMMUNITY_PRESETS) + ")";
            return false;
        }
    }

    File f = LittleFS.open(path, "w");
    if (!f) { reason = "open for write failed"; ESP_LOGE(TAG, "open for write failed: %s", id.c_str()); return false; }
    size_t written = serializeJson(doc, f);
    f.close();
    if (written == 0) { reason = "write failed"; ESP_LOGE(TAG, "write failed: %s", id.c_str()); return false; }
    return true;
}

bool load(const String& rawId, JsonDocument& doc) {
    String id = sanitizeId(rawId);
    if (id.length() == 0 || id != rawId) return false;

    File f = LittleFS.open(pathFor(id), "r");
    if (!f) return false;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    return err == DeserializationError::Ok;
}

bool remove(const String& rawId) {
    String id = sanitizeId(rawId);
    if (id.length() == 0 || id != rawId) return false;
    return LittleFS.remove(pathFor(id));
}

bool rename(const String& rawId, const String& newName) {
    if (newName.length() == 0) return false;
    JsonDocument doc(&jsonAllocator());
    if (!load(rawId, doc)) return false;
    doc["meta"]["name"] = newName;
    String reason;
    return save(doc, reason);
}

void list(JsonArray arr) {
    File dir = LittleFS.open(DIR);
    if (!dir || !dir.isDirectory()) return;
    File entry = dir.openNextFile();
    while (entry) {
        if (!entry.isDirectory()) {
            // arduino-esp32's LittleFS returns the full path from openNextFile()
            // (core-version dependent) -- strip any directory component so `id`
            // is always the bare basename used by pathFor()/load()/remove().
            String fname = entry.name();
            int slash = fname.lastIndexOf('/');
            if (slash >= 0) fname = fname.substring(slash + 1);
            if (fname.endsWith(".json")) {
                String id = fname.substring(0, fname.length() - 5);
                JsonDocument doc(&jsonAllocator());
                if (deserializeJson(doc, entry) == DeserializationError::Ok) {
                    JsonObject p = arr.add<JsonObject>();
                    p["id"]          = id;
                    p["name"]        = doc["meta"]["name"] | id;
                    p["author"]      = doc["meta"]["author"] | "";
                    p["size_bytes"]  = entry.size();
                }
            }
        }
        entry.close();
        entry = dir.openNextFile();
    }
    dir.close();
}

void fsInfo(JsonObject obj) {
    obj["total_bytes"] = LittleFS.totalBytes();
    obj["used_bytes"]  = LittleFS.usedBytes();
    obj["free_bytes"]  = LittleFS.totalBytes() - LittleFS.usedBytes();

    uint16_t count = 0;
    File dir = LittleFS.open(DIR);
    if (dir && dir.isDirectory()) {
        File entry = dir.openNextFile();
        while (entry) {
            if (!entry.isDirectory()) {
                String fname = entry.name();
                int slash = fname.lastIndexOf('/');
                if (slash >= 0) fname = fname.substring(slash + 1);
                if (fname.endsWith(".json")) count++;
            }
            entry.close();
            entry = dir.openNextFile();
        }
        dir.close();
    }
    obj["preset_count"] = count;
}

} // namespace community_presets
