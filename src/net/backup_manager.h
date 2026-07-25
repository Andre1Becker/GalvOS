#pragma once
/**
 * backup_manager.h -- full-configuration backup/restore
 *
 * Snapshots every NVS-persisted setting (galvo calibration, all optimizer
 * profiles, network config, system/projection/thermal settings) into a
 * single versioned JSON document, and restores one back into the live
 * in-RAM config structs after validating every value against the same
 * bounds the live WebUI endpoints enforce (see web_ui.cpp's
 * /api/calib-live, /api/optimizer-live, /api/projection, /api/dmx/address,
 * /api/safety/config -- BackupManager's checks are kept in sync with those
 * by hand, same as the rest of this codebase's validation).
 *
 * Restore never partially applies: every value is validated first: any
 * rejection aborts the whole restore with nothing written. Safety-critical
 * fields (galvo_kpps, per-profile max_pts_per_frame) are additionally
 * clamped to their config.h hard limits at apply time as a defense-in-depth
 * safety net, on top of (not instead of) the reject-on-out-of-range check.
 *
 * Does NOT persist to NVS or reboot -- callers (web_ui.cpp) do that, so a
 * dry-run validation is possible without side effects.
 */

#include <ArduinoJson.h>
#include <stdint.h>

class BackupManager {
public:
    static constexpr uint8_t SCHEMA_VERSION = 1;

    // Builds the full backup document from the live gConfig/gOptimizerProfiles/
    // gProjection/gSafety structs. doc must be backed by jsonAllocator()
    // (PSRAM) -- this can produce several KB with all 8 optimizer profiles.
    static void serializeToJson(JsonDocument& doc);

    // Validates every recognized key in doc, appending the dotted path of
    // any rejected one (e.g. "calib.gain_r", "optimizer.profiles[3].max_pts_per_frame")
    // to `rejected`. Returns true and applies all values to the live config
    // structs only if nothing was rejected; returns false and applies
    // nothing otherwise. Does not persist or reboot.
    static bool deserializeFromJson(JsonDocument& doc, JsonArray rejected);
};
