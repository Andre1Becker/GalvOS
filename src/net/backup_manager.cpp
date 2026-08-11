#include "backup_manager.h"
#include "config.h"
#include "mutex.h"
#include <Arduino.h>
#include <IPAddress.h>
#include <string.h>

namespace {

inline bool inRangeI(long v, long lo, long hi) { return v >= lo && v <= hi; }
inline bool inRangeF(float v, float lo, float hi) { return v >= lo && v <= hi; }

inline bool strLenOk(const char* s, size_t maxLen) {
    return s != nullptr && strlen(s) <= maxLen;
}

// Empty string = "unset", always accepted. Non-empty must parse as an IPv4
// address -- these get handed straight to WiFi.config() on boot.
inline bool ipFieldOk(const char* s) {
    if (!s || strlen(s) >= 16) return false;
    if (s[0] == '\0') return true;
    IPAddress tmp;
    return tmp.fromString(s);
}

String profKey(uint8_t i, const char* field) {
    char buf[48];
    snprintf(buf, sizeof(buf), "optimizer.profiles[%u].%s", (unsigned)i, field);
    return String(buf);
}

} // namespace

void BackupManager::serializeToJson(JsonDocument& doc) {
    doc["fw"]     = LASER_FW_VERSION;
    doc["schema"] = SCHEMA_VERSION;

    JsonObject calib = doc["calib"].to<JsonObject>();
    calib["galvo_x_offset"] = gConfig.galvo_x_offset;
    calib["galvo_y_offset"] = gConfig.galvo_y_offset;
    calib["galvo_x_gain"]   = gConfig.galvo_x_gain;
    calib["galvo_y_gain"]   = gConfig.galvo_y_gain;
    calib["swap_xy"]        = gConfig.swap_xy;
    calib["invert_x"]       = gConfig.invert_x;
    calib["invert_y"]       = gConfig.invert_y;
    calib["dac_limit_min"]  = gConfig.dac_limit_min;
    calib["dac_limit_max"]  = gConfig.dac_limit_max;
    calib["gain_r"]         = gConfig.gain_r;
    calib["gain_g"]         = gConfig.gain_g;
    calib["gain_b"]         = gConfig.gain_b;
    calib["thresh_r"]       = gConfig.thresh_r;
    calib["thresh_g"]       = gConfig.thresh_g;
    calib["thresh_b"]       = gConfig.thresh_b;
    calib["gamma_enable"]   = gConfig.gamma_enable;

    JsonObject optimizer = doc["optimizer"].to<JsonObject>();
    optimizer["active_profile"] = gActiveOptimizerProfile;
    JsonArray profiles = optimizer["profiles"].to<JsonArray>();
    for (uint8_t i = 0; i < OPT_PROFILE_COUNT; i++) {
        const OptimizerLiveConfig& p = gOptimizerProfiles[i];
        JsonObject o = profiles.add<JsonObject>();
        o["corner_angle_deg"]             = p.corner_angle_deg;
        o["min_corner_pts"]               = p.min_corner_pts;
        o["max_corner_pts"]               = p.max_corner_pts;
        o["pts_per_1000_units"]           = p.pts_per_1000_units;
        o["min_segment_pts"]              = p.min_segment_pts;
        o["blank_samples"]                = p.blank_samples;
        o["max_pts_per_frame"]            = p.max_pts_per_frame;
        o["min_blank_samples"]            = p.min_blank_samples;
        o["blank_pts_per_1000_units"]     = p.blank_pts_per_1000_units;
        o["min_interior_pts_per_segment"] = p.min_interior_pts_per_segment;
        o["stage1_blank_target"]          = p.stage1_blank_target;
        o["resample_enabled"]             = p.resample_enabled;
        o["resample_spacing_units"]       = p.resample_spacing_units;
        o["ringing_comp_enabled"]         = p.ringing_comp_enabled;
        o["ring_freq_hz"]                 = p.ring_freq_hz;
        o["ring_damping_ratio"]           = p.ring_damping_ratio;
        o["jitter_enabled"]               = p.jitter_enabled;
        o["jitter_amount_units"]          = p.jitter_amount_units;
        o["vel_clamp_enabled"]            = p.vel_clamp_enabled;
        o["max_step_units"]               = p.max_step_units;
        o["accel_clamp_enabled"]          = p.accel_clamp_enabled;
        o["max_accel_units"]              = p.max_accel_units;
    }

    JsonObject net = doc["net"].to<JsonObject>();
    net["dmx_address"]     = gConfig.dmx_address;
    net["artnet_universe"] = gConfig.artnet_universe;
    net["hostname"]        = gConfig.hostname;
    net["wifi_ssid"]       = gConfig.wifi_ssid;
    net["wifi_pass"]       = gConfig.wifi_pass;
    net["wifi_static"]     = gConfig.wifi_static;
    net["wifi_ip"]         = gConfig.wifi_ip;
    net["wifi_gw"]         = gConfig.wifi_gw;
    net["wifi_mask"]       = gConfig.wifi_mask;
    net["wifi_dns"]        = gConfig.wifi_dns;
    net["ntp_server"]      = gConfig.ntp_server;
    net["ntp_tz"]          = gConfig.ntp_tz;

    JsonObject sys = doc["sys"].to<JsonObject>();
    sys["galvo_kpps"]          = gProjection.galvo_kpps;
    sys["galvo_rated_kpps"]    = gProjection.galvo_rated_kpps;
    sys["scan_angle_mech_deg"] = gProjection.scan_angle_mech_deg;
    sys["exit_angle_deg"]      = gProjection.exit_angle_deg;
    sys["ilda_test_angle_deg"] = gProjection.ilda_test_angle_deg;
    sys["power_r_mw"]          = gProjection.power_r_mw;
    sys["power_g_mw"]          = gProjection.power_g_mw;
    sys["power_b_mw"]          = gProjection.power_b_mw;
    sys["distance_m"]          = gProjection.distance_m;
    sys["temp_warn_c"]         = gSafety.temp_warn_c;
    sys["temp_reduce_c"]       = gSafety.temp_reduce_c;
    sys["temp_shutdown_c"]     = gSafety.temp_shutdown_c;
    sys["fan_min_pct"]         = gSafety.fan_min_pct;
    sys["fan_auto"]            = gSafety.fan_auto;
}

bool BackupManager::deserializeFromJson(JsonDocument& doc, JsonArray rejected) {
    bool ok = true;

    // Schema gate -- only version 1 exists so far, no migration path yet.
    // Without a known schema nothing else in the document can be trusted.
    if (!doc["schema"].is<int>() || (int)doc["schema"] != SCHEMA_VERSION) {
        rejected.add("schema");
        return false;
    }

    JsonObjectConst calib     = doc["calib"];
    JsonObjectConst optimizer = doc["optimizer"];
    JsonObjectConst net       = doc["net"];
    JsonObjectConst sys       = doc["sys"];

    // ── calib ────────────────────────────────────────────────────────────
    #define CCHECK(field, cond) \
        if (!calib[field].isNull()) { \
            if (!(cond)) { rejected.add("calib." field); ok = false; } \
        }
    if (!calib.isNull()) {
        CCHECK("galvo_x_offset", calib["galvo_x_offset"].is<int>() && inRangeI(calib["galvo_x_offset"].as<int>(), -32768, 32767));
        CCHECK("galvo_y_offset", calib["galvo_y_offset"].is<int>() && inRangeI(calib["galvo_y_offset"].as<int>(), -32768, 32767));
        CCHECK("galvo_x_gain",   calib["galvo_x_gain"].is<int>()   && inRangeI(calib["galvo_x_gain"].as<int>(),   -32768, 32767));
        CCHECK("galvo_y_gain",   calib["galvo_y_gain"].is<int>()   && inRangeI(calib["galvo_y_gain"].as<int>(),   -32768, 32767));
        CCHECK("swap_xy",        calib["swap_xy"].is<bool>());
        CCHECK("invert_x",       calib["invert_x"].is<bool>());
        CCHECK("invert_y",       calib["invert_y"].is<bool>());
        CCHECK("dac_limit_min",  calib["dac_limit_min"].is<int>() && inRangeI(calib["dac_limit_min"].as<int>(), 0, 65535));
        CCHECK("dac_limit_max",  calib["dac_limit_max"].is<int>() && inRangeI(calib["dac_limit_max"].as<int>(), 0, 65535));
        CCHECK("gain_r",         calib["gain_r"].is<int>() && inRangeI(calib["gain_r"].as<int>(), 0, 255));
        CCHECK("gain_g",         calib["gain_g"].is<int>() && inRangeI(calib["gain_g"].as<int>(), 0, 255));
        CCHECK("gain_b",         calib["gain_b"].is<int>() && inRangeI(calib["gain_b"].as<int>(), 0, 255));
        CCHECK("thresh_r",       calib["thresh_r"].is<int>() && inRangeI(calib["thresh_r"].as<int>(), 0, 255));
        CCHECK("thresh_g",       calib["thresh_g"].is<int>() && inRangeI(calib["thresh_g"].as<int>(), 0, 255));
        CCHECK("thresh_b",       calib["thresh_b"].is<int>() && inRangeI(calib["thresh_b"].as<int>(), 0, 255));
        CCHECK("gamma_enable",   calib["gamma_enable"].is<bool>());
        if (!calib["dac_limit_min"].isNull() && !calib["dac_limit_max"].isNull() &&
            calib["dac_limit_min"].is<int>() && calib["dac_limit_max"].is<int>()) {
            if ((int)calib["dac_limit_min"] >= (int)calib["dac_limit_max"]) {
                rejected.add("calib.dac_limit_min");
                rejected.add("calib.dac_limit_max");
                ok = false;
            }
        }
    }
    #undef CCHECK

    // ── optimizer ────────────────────────────────────────────────────────
    if (!optimizer.isNull()) {
        if (!optimizer["active_profile"].isNull()) {
            if (!(optimizer["active_profile"].is<int>() &&
                  inRangeI(optimizer["active_profile"].as<int>(), 0, OPT_PROFILE_COUNT - 1))) {
                rejected.add("optimizer.active_profile");
                ok = false;
            }
        }
        JsonArrayConst profiles = optimizer["profiles"];
        if (!profiles.isNull()) {
            if (profiles.size() > OPT_PROFILE_COUNT) {
                rejected.add("optimizer.profiles");
                ok = false;
            } else {
                #define PCHECK(field, cond) \
                    if (!o[field].isNull()) { \
                        if (!(cond)) { rejected.add(profKey(i, field)); ok = false; } \
                    }
                for (uint8_t i = 0; i < profiles.size(); i++) {
                    JsonObjectConst o = profiles[i];
                    PCHECK("corner_angle_deg",             o["corner_angle_deg"].is<float>() && inRangeF(o["corner_angle_deg"].as<float>(), 0.0f, 180.0f));
                    PCHECK("min_corner_pts",                o["min_corner_pts"].is<int>() && inRangeI(o["min_corner_pts"].as<int>(), 1, 20));
                    PCHECK("max_corner_pts",                o["max_corner_pts"].is<int>() && inRangeI(o["max_corner_pts"].as<int>(), 1, 20));
                    PCHECK("pts_per_1000_units",            o["pts_per_1000_units"].is<float>() && inRangeF(o["pts_per_1000_units"].as<float>(), 0.1f, 50.0f));
                    PCHECK("min_segment_pts",               o["min_segment_pts"].is<int>() && inRangeI(o["min_segment_pts"].as<int>(), 2, 20));
                    PCHECK("blank_samples",                 o["blank_samples"].is<int>() && inRangeI(o["blank_samples"].as<int>(), 1, 100));
                    PCHECK("max_pts_per_frame",             o["max_pts_per_frame"].is<int>() && inRangeI(o["max_pts_per_frame"].as<int>(), 50, (int)PATTERN_POINTS_MAX));
                    PCHECK("min_blank_samples",             o["min_blank_samples"].is<int>() && inRangeI(o["min_blank_samples"].as<int>(), 1, 100));
                    PCHECK("blank_pts_per_1000_units",      o["blank_pts_per_1000_units"].is<float>() && inRangeF(o["blank_pts_per_1000_units"].as<float>(), 0.1f, 50.0f));
                    PCHECK("min_interior_pts_per_segment",  o["min_interior_pts_per_segment"].is<int>() && inRangeI(o["min_interior_pts_per_segment"].as<int>(), 0, 50));
                    PCHECK("stage1_blank_target",           o["stage1_blank_target"].is<int>() && inRangeI(o["stage1_blank_target"].as<int>(), 1, 100));
                    PCHECK("resample_enabled",              o["resample_enabled"].is<bool>());
                    PCHECK("resample_spacing_units",        o["resample_spacing_units"].is<float>() && inRangeF(o["resample_spacing_units"].as<float>(), 10.0f, 2000.0f));
                    PCHECK("ringing_comp_enabled",           o["ringing_comp_enabled"].is<bool>());
                    PCHECK("ring_freq_hz",                  o["ring_freq_hz"].is<float>() && inRangeF(o["ring_freq_hz"].as<float>(), 1.0f, 2000.0f));
                    PCHECK("ring_damping_ratio",             o["ring_damping_ratio"].is<float>() && inRangeF(o["ring_damping_ratio"].as<float>(), 0.0f, 0.9f));
                    PCHECK("jitter_enabled",                 o["jitter_enabled"].is<bool>());
                    PCHECK("jitter_amount_units",            o["jitter_amount_units"].is<float>() && inRangeF(o["jitter_amount_units"].as<float>(), 0.0f, 2000.0f));
                    PCHECK("vel_clamp_enabled",              o["vel_clamp_enabled"].is<bool>());
                    PCHECK("max_step_units",                o["max_step_units"].is<float>() && inRangeF(o["max_step_units"].as<float>(), 50.0f, 32767.0f));
                    PCHECK("accel_clamp_enabled",            o["accel_clamp_enabled"].is<bool>());
                    PCHECK("max_accel_units",               o["max_accel_units"].is<float>() && inRangeF(o["max_accel_units"].as<float>(), 10.0f, 32767.0f));
                }
                #undef PCHECK
            }
        }
    }

    // ── net ──────────────────────────────────────────────────────────────
    #define NCHECK(field, cond) \
        if (!net[field].isNull()) { \
            if (!(cond)) { rejected.add("net." field); ok = false; } \
        }
    if (!net.isNull()) {
        NCHECK("dmx_address",     net["dmx_address"].is<int>() && inRangeI(net["dmx_address"].as<int>(), 1, 512));
        NCHECK("artnet_universe", net["artnet_universe"].is<int>() && inRangeI(net["artnet_universe"].as<int>(), 0, 65535));
        NCHECK("hostname",        net["hostname"].is<const char*>() && strLenOk(net["hostname"], 31));
        NCHECK("wifi_ssid",       net["wifi_ssid"].is<const char*>() && strLenOk(net["wifi_ssid"], 32));
        NCHECK("wifi_pass",       net["wifi_pass"].is<const char*>() && strLenOk(net["wifi_pass"], 64));
        NCHECK("wifi_static",     net["wifi_static"].is<bool>());
        NCHECK("wifi_ip",         net["wifi_ip"].is<const char*>() && ipFieldOk(net["wifi_ip"]));
        NCHECK("wifi_gw",         net["wifi_gw"].is<const char*>() && ipFieldOk(net["wifi_gw"]));
        NCHECK("wifi_mask",       net["wifi_mask"].is<const char*>() && ipFieldOk(net["wifi_mask"]));
        NCHECK("wifi_dns",        net["wifi_dns"].is<const char*>() && ipFieldOk(net["wifi_dns"]));
        NCHECK("ntp_server",      net["ntp_server"].is<const char*>() && strLenOk(net["ntp_server"], 63));
        NCHECK("ntp_tz",          net["ntp_tz"].is<const char*>() && strLenOk(net["ntp_tz"], 47));
    }
    #undef NCHECK

    // ── sys ──────────────────────────────────────────────────────────────
    #define SCHECK(field, cond) \
        if (!sys[field].isNull()) { \
            if (!(cond)) { rejected.add("sys." field); ok = false; } \
        }
    if (!sys.isNull()) {
        SCHECK("galvo_kpps",          sys["galvo_kpps"].is<int>() && inRangeI(sys["galvo_kpps"].as<int>(), 12, 60));
        SCHECK("galvo_rated_kpps",    sys["galvo_rated_kpps"].is<int>() && inRangeI(sys["galvo_rated_kpps"].as<int>(), 1, 100));
        SCHECK("scan_angle_mech_deg", sys["scan_angle_mech_deg"].is<float>() && inRangeF(sys["scan_angle_mech_deg"].as<float>(), 0.01f, 45.0f));
        SCHECK("exit_angle_deg",      sys["exit_angle_deg"].is<float>() && inRangeF(sys["exit_angle_deg"].as<float>(), 0.01f, 45.0f));
        SCHECK("ilda_test_angle_deg", sys["ilda_test_angle_deg"].is<float>() && inRangeF(sys["ilda_test_angle_deg"].as<float>(), 0.01f, 20.0f));
        SCHECK("power_r_mw",          sys["power_r_mw"].is<float>() && inRangeF(sys["power_r_mw"].as<float>(), 0.0f, 10000.0f));
        SCHECK("power_g_mw",          sys["power_g_mw"].is<float>() && inRangeF(sys["power_g_mw"].as<float>(), 0.0f, 10000.0f));
        SCHECK("power_b_mw",          sys["power_b_mw"].is<float>() && inRangeF(sys["power_b_mw"].as<float>(), 0.0f, 10000.0f));
        SCHECK("distance_m",          sys["distance_m"].is<float>() && inRangeF(sys["distance_m"].as<float>(), 0.1f, 100.0f));
        SCHECK("temp_warn_c",         sys["temp_warn_c"].is<int>() && inRangeI(sys["temp_warn_c"].as<int>(), 0, 100));
        SCHECK("temp_reduce_c",       sys["temp_reduce_c"].is<int>() && inRangeI(sys["temp_reduce_c"].as<int>(), 0, 100));
        SCHECK("temp_shutdown_c",     sys["temp_shutdown_c"].is<int>() && inRangeI(sys["temp_shutdown_c"].as<int>(), 0, 100));
        SCHECK("fan_min_pct",         sys["fan_min_pct"].is<int>() && inRangeI(sys["fan_min_pct"].as<int>(), 0, 100));
        SCHECK("fan_auto",            sys["fan_auto"].is<bool>());
    }
    #undef SCHECK

    if (!ok) return false;

    // ── Apply (only reached when every field above validated clean) ────────
    {
        LOCK_CONFIG();
        if (!calib.isNull()) {
            if (!calib["galvo_x_offset"].isNull()) gConfig.galvo_x_offset = calib["galvo_x_offset"];
            if (!calib["galvo_y_offset"].isNull()) gConfig.galvo_y_offset = calib["galvo_y_offset"];
            if (!calib["galvo_x_gain"].isNull())   gConfig.galvo_x_gain   = calib["galvo_x_gain"];
            if (!calib["galvo_y_gain"].isNull())   gConfig.galvo_y_gain   = calib["galvo_y_gain"];
            if (!calib["swap_xy"].isNull())        gConfig.swap_xy  = calib["swap_xy"];
            if (!calib["invert_x"].isNull())       gConfig.invert_x = calib["invert_x"];
            if (!calib["invert_y"].isNull())       gConfig.invert_y = calib["invert_y"];
            if (!calib["dac_limit_min"].isNull())  gConfig.dac_limit_min = calib["dac_limit_min"];
            if (!calib["dac_limit_max"].isNull())  gConfig.dac_limit_max = calib["dac_limit_max"];
            if (!calib["gain_r"].isNull())         gConfig.gain_r = calib["gain_r"];
            if (!calib["gain_g"].isNull())         gConfig.gain_g = calib["gain_g"];
            if (!calib["gain_b"].isNull())         gConfig.gain_b = calib["gain_b"];
            if (!calib["thresh_r"].isNull())       gConfig.thresh_r = calib["thresh_r"];
            if (!calib["thresh_g"].isNull())       gConfig.thresh_g = calib["thresh_g"];
            if (!calib["thresh_b"].isNull())       gConfig.thresh_b = calib["thresh_b"];
            if (!calib["gamma_enable"].isNull())   gConfig.gamma_enable = calib["gamma_enable"];
        }
        if (!net.isNull()) {
            if (!net["dmx_address"].isNull())     gConfig.dmx_address     = net["dmx_address"];
            if (!net["artnet_universe"].isNull()) gConfig.artnet_universe = net["artnet_universe"];
            if (!net["hostname"].isNull())    strlcpy(gConfig.hostname,  net["hostname"],  sizeof(gConfig.hostname));
            if (!net["wifi_ssid"].isNull())   strlcpy(gConfig.wifi_ssid, net["wifi_ssid"], sizeof(gConfig.wifi_ssid));
            if (!net["wifi_pass"].isNull())   strlcpy(gConfig.wifi_pass, net["wifi_pass"], sizeof(gConfig.wifi_pass));
            if (!net["wifi_static"].isNull()) gConfig.wifi_static = net["wifi_static"];
            if (!net["wifi_ip"].isNull())     strlcpy(gConfig.wifi_ip,   net["wifi_ip"],   sizeof(gConfig.wifi_ip));
            if (!net["wifi_gw"].isNull())     strlcpy(gConfig.wifi_gw,   net["wifi_gw"],   sizeof(gConfig.wifi_gw));
            if (!net["wifi_mask"].isNull())   strlcpy(gConfig.wifi_mask, net["wifi_mask"], sizeof(gConfig.wifi_mask));
            if (!net["wifi_dns"].isNull())    strlcpy(gConfig.wifi_dns,  net["wifi_dns"],  sizeof(gConfig.wifi_dns));
            if (!net["ntp_server"].isNull())  strlcpy(gConfig.ntp_server, net["ntp_server"], sizeof(gConfig.ntp_server));
            if (!net["ntp_tz"].isNull())      strlcpy(gConfig.ntp_tz,     net["ntp_tz"],     sizeof(gConfig.ntp_tz));
        }
        if (!sys.isNull()) {
            if (!sys["temp_warn_c"].isNull())     gSafety.temp_warn_c     = sys["temp_warn_c"];
            if (!sys["temp_reduce_c"].isNull())   gSafety.temp_reduce_c   = sys["temp_reduce_c"];
            if (!sys["temp_shutdown_c"].isNull()) gSafety.temp_shutdown_c = sys["temp_shutdown_c"];
            if (!sys["fan_min_pct"].isNull())     gSafety.fan_min_pct     = sys["fan_min_pct"];
            if (!sys["fan_auto"].isNull())        gSafety.fan_auto        = sys["fan_auto"];
        }
    } // LOCK_CONFIG

    if (!optimizer.isNull()) {
        JsonArrayConst profiles = optimizer["profiles"];
        if (!profiles.isNull()) {
            for (uint8_t i = 0; i < profiles.size(); i++) {
                JsonObjectConst o = profiles[i];
                OptimizerLiveConfig& P = gOptimizerProfiles[i];
                if (!o["corner_angle_deg"].isNull())            P.corner_angle_deg = o["corner_angle_deg"];
                if (!o["min_corner_pts"].isNull())              P.min_corner_pts = o["min_corner_pts"];
                if (!o["max_corner_pts"].isNull())              P.max_corner_pts = o["max_corner_pts"];
                if (!o["pts_per_1000_units"].isNull())          P.pts_per_1000_units = o["pts_per_1000_units"];
                if (!o["min_segment_pts"].isNull())             P.min_segment_pts = o["min_segment_pts"];
                if (!o["blank_samples"].isNull())               P.blank_samples = o["blank_samples"];
                // Safety-critical: re-clamp on top of the already-passed range check
                // above, so a future bug in that check can't turn into an out-of-range write.
                if (!o["max_pts_per_frame"].isNull())
                    P.max_pts_per_frame = (uint16_t)constrain((int)o["max_pts_per_frame"], 50, (int)PATTERN_POINTS_MAX);
                if (!o["min_blank_samples"].isNull())            P.min_blank_samples = o["min_blank_samples"];
                if (!o["blank_pts_per_1000_units"].isNull())     P.blank_pts_per_1000_units = o["blank_pts_per_1000_units"];
                if (!o["min_interior_pts_per_segment"].isNull()) P.min_interior_pts_per_segment = o["min_interior_pts_per_segment"];
                if (!o["stage1_blank_target"].isNull())         P.stage1_blank_target = o["stage1_blank_target"];
                if (!o["resample_enabled"].isNull())            P.resample_enabled = o["resample_enabled"];
                if (!o["resample_spacing_units"].isNull())      P.resample_spacing_units = o["resample_spacing_units"];
                if (!o["ringing_comp_enabled"].isNull())        P.ringing_comp_enabled = o["ringing_comp_enabled"];
                if (!o["ring_freq_hz"].isNull())                P.ring_freq_hz = o["ring_freq_hz"];
                if (!o["ring_damping_ratio"].isNull())          P.ring_damping_ratio = o["ring_damping_ratio"];
                if (!o["jitter_enabled"].isNull())              P.jitter_enabled = o["jitter_enabled"];
                if (!o["jitter_amount_units"].isNull())         P.jitter_amount_units = o["jitter_amount_units"];
                if (!o["vel_clamp_enabled"].isNull())           P.vel_clamp_enabled = o["vel_clamp_enabled"];
                if (!o["max_step_units"].isNull())              P.max_step_units = o["max_step_units"];
                if (!o["accel_clamp_enabled"].isNull())         P.accel_clamp_enabled = o["accel_clamp_enabled"];
                if (!o["max_accel_units"].isNull())             P.max_accel_units = o["max_accel_units"];
                // Every field above passed its own range check, but the two
                // range checks don't see each other -- an inverted min/max pair
                // (min_blank_samples > blank_samples, min_corner_pts >
                // max_corner_pts) survives untouched otherwise. See
                // normalizeOptimizerConfig()'s doc comment in config.h.
                normalizeOptimizerConfig(P);
            }
        }
        if (!optimizer["active_profile"].isNull())
            gActiveOptimizerProfile = (uint8_t)(int)optimizer["active_profile"];
        syncOptimizerConfig();
        gPatternCacheGen++;
    }

    if (!sys.isNull()) {
        if (!sys["galvo_kpps"].isNull())
            // Safety-critical: re-clamp (see max_pts_per_frame comment above).
            gProjection.galvo_kpps = (uint16_t)constrain((int)sys["galvo_kpps"], 12, 60);
        if (!sys["galvo_rated_kpps"].isNull()) gProjection.galvo_rated_kpps    = sys["galvo_rated_kpps"];
        if (!sys["scan_angle_mech_deg"].isNull()) gProjection.scan_angle_mech_deg = sys["scan_angle_mech_deg"];
        if (!sys["exit_angle_deg"].isNull())      gProjection.exit_angle_deg      = sys["exit_angle_deg"];
        if (!sys["ilda_test_angle_deg"].isNull()) gProjection.ilda_test_angle_deg = sys["ilda_test_angle_deg"];
        if (!sys["power_r_mw"].isNull())          gProjection.power_r_mw = sys["power_r_mw"];
        if (!sys["power_g_mw"].isNull())          gProjection.power_g_mw = sys["power_g_mw"];
        if (!sys["power_b_mw"].isNull())          gProjection.power_b_mw = sys["power_b_mw"];
        if (!sys["distance_m"].isNull())          gProjection.distance_m = sys["distance_m"];
        if (!sys["galvo_kpps"].isNull() || !sys["scan_angle_mech_deg"].isNull() ||
            !sys["exit_angle_deg"].isNull())
            gPatternCacheGen++;
    }

    return true;
}
