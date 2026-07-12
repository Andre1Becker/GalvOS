#include "web_ui.h"
#include "config.h"
#include "safety/safety.h"
#include "output/galvo_out.h"
#include "patterns/pattern_engine.h"
#include "patterns/curve_patterns.h"
#include "patterns/preset_patterns.h"
#include "patterns/countdown_timer.h"
#include "patterns/text_renderer.h"
#include "ilda/ilda_player.h"
#include "patterns/calib_patterns.h"
#include "mutex.h"
#include "storage/playlist.h"
#include "net/ota_update.h"
#include "net/etherdream.h"
#include "storage/sd_card.h"
#include "sensors/temp_monitor.h"
#include "util/log_buffer.h"
#include "util/cpu_monitor.h"
#include "util/stack_mon.h"
#include "net/ntp_client.h"
#include "patterns/preset_patterns.h"
#include "patterns/countdown_timer.h"
#include <Arduino.h>
#include <math.h>
#include <string.h>
#include <memory>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <ArduinoJson.h>
#include "json_alloc.h"
#include <LittleFS.h>
#include <SD.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_log.h>

AsyncWebServer s_server(80);  // file-scope: ota_update sieht es

namespace web_ui {

static const char* TAG = "web";

// ── PSRAM-backed JSON response ────────────────────────────────────────────────
// serializeJson() into an Arduino String allocates on the internal DRAM heap,
// which is the scarce resource shared with lwIP/WiFi. On the polled /api/state
// path this repeatedly pressured internal heap (root cause of the low-heap /
// WiFiUdp ENOMEM spiral). This serializes straight into a PSRAM buffer and
// streams it chunked; the shared_ptr deleter frees the buffer whether the
// response completes or the client aborts mid-stream.
static void sendJsonPsram(AsyncWebServerRequest* req, const JsonDocument& doc) {
    size_t json_len = measureJson(doc);
    size_t buf_len  = json_len + 1;
    std::shared_ptr<char> buf(
        (char*)heap_caps_malloc(buf_len, MALLOC_CAP_SPIRAM),
        [](char* p) { heap_caps_free(p); });
    if (!buf) { req->send(503, "text/plain", "OOM"); return; }

    serializeJson(doc, buf.get(), buf_len);

    AsyncWebServerResponse* resp = req->beginChunkedResponse(
        "application/json",
        [buf, json_len](uint8_t* out, size_t maxLen, size_t index) -> size_t {
            if (index >= json_len) return 0;
            size_t n = std::min(maxLen, json_len - index);
            memcpy(out, buf.get() + index, n);
            return n;
        });
    req->send(resp);
}
// ── Session Auth Token ────────────────────────────────────────────────────────
static char s_auth_token[17] = {0};

static void generateAuthToken() {
    uint64_t mac  = ESP.getEfuseMac();
    uint32_t rnd  = esp_random();
    snprintf(s_auth_token, sizeof(s_auth_token), "%08X%08X",
             (uint32_t)(mac & 0xFFFFFFFF) ^ rnd,
             (uint32_t)(mac >> 32) ^ ~rnd);
}

static bool isAuthorised(AsyncWebServerRequest* req) {
    if (req->hasHeader("X-Auth")) {
        const AsyncWebHeader* h = req->getHeader("X-Auth");
        if (h && strcmp(h->value().c_str(), s_auth_token) == 0) return true;
    }
    if (req->hasParam("token")) {
        if (strcmp(req->getParam("token")->value().c_str(), s_auth_token) == 0)
            return true;
    }
    return false;
}

static void denyUnauth(AsyncWebServerRequest* req) {
    req->send(401, "application/json",
        "{\"error\":\"Unauthorized\",\"hint\":\"Send X-Auth: <token> header\"}");
}

// WebSocket removed in 5.34.0 (unused, caused internal-heap exhaustion).
// WiFi-Scan Status
static volatile bool   s_scan_running = false;
static volatile int    s_scan_results = 0;

// ── /api/paint/set chunked-body buffer (fixed PSRAM, no per-request heap alloc) ──
// Previous impl used `new String()` per request, freed only on the success
// path. An aborted/dropped upload (never reaching index+len==total) leaked
// it permanently -- root cause of the post-5.21.0 heap exhaustion.
static const size_t PAINT_BODY_CAP  = 32768;
static char*        s_paint_body    = nullptr;
static size_t       s_paint_body_len = 0;

/* ============================================================
 * Config Persistence
 * ============================================================ */
static Preferences s_prefs;

static void persistConfig() {
    s_prefs.begin("laser", false);
    s_prefs.putUShort("dmx_addr",   gConfig.dmx_address);
    s_prefs.putUShort("artnet_uni", gConfig.artnet_universe);
    s_prefs.putShort("xoff",        gConfig.galvo_x_offset);
    s_prefs.putShort("yoff",        gConfig.galvo_y_offset);
    s_prefs.putShort("xgain",       gConfig.galvo_x_gain);
    s_prefs.putShort("ygain",       gConfig.galvo_y_gain);
    s_prefs.putUShort("dac_lim_lo", gConfig.dac_limit_min);
    s_prefs.putUShort("dac_lim_hi", gConfig.dac_limit_max);
    s_prefs.putBool("swap",         gConfig.swap_xy);
    s_prefs.putBool("invx",         gConfig.invert_x);
    s_prefs.putBool("invy",         gConfig.invert_y);
    s_prefs.putUChar("gain_r",      gConfig.gain_r);
    s_prefs.putUChar("gain_g",      gConfig.gain_g);
    s_prefs.putUChar("gain_b",      gConfig.gain_b);
    s_prefs.putBool ("gamma_en",    gConfig.gamma_enable);
    s_prefs.putFloat("gamma_val",   gConfig.gamma_val);
    s_prefs.putUChar("gain_g",      gConfig.gain_g);
    s_prefs.putUChar("gain_b",      gConfig.gain_b);
    s_prefs.putString("ssid",       gConfig.wifi_ssid);
    s_prefs.putString("pass",       gConfig.wifi_pass);
    s_prefs.putString("host",       gConfig.hostname);
    s_prefs.putBool("static_ip",    gConfig.wifi_static);
    s_prefs.putString("ip",         gConfig.wifi_ip);
    s_prefs.putString("gw",         gConfig.wifi_gw);
    s_prefs.putString("mask",       gConfig.wifi_mask);
    s_prefs.putString("dns",        gConfig.wifi_dns);
    s_prefs.putFloat ("opt_cad",    gOptimizerConfig.corner_angle_deg);
    s_prefs.putUChar ("opt_mincp",  gOptimizerConfig.min_corner_pts);
    s_prefs.putUChar ("opt_maxcp",  gOptimizerConfig.max_corner_pts);
    s_prefs.putFloat ("opt_ppu",    gOptimizerConfig.pts_per_1000_units);
    s_prefs.putUChar ("opt_minsp",  gOptimizerConfig.min_segment_pts);
    s_prefs.putUChar ("opt_blank",  gOptimizerConfig.blank_samples);
    s_prefs.putUShort("opt_maxppf", gOptimizerConfig.max_pts_per_frame);
    s_prefs.putUChar ("opt_minbl",  gOptimizerConfig.min_blank_samples);
    s_prefs.putFloat ("opt_blppu",  gOptimizerConfig.blank_pts_per_1000_units);
    s_prefs.putUChar ("opt_minip",  gOptimizerConfig.min_interior_pts_per_segment);
    s_prefs.putUChar ("opt_s1tgt",  gOptimizerConfig.stage1_blank_target);
    s_prefs.putBool  ("opt_rngen",  gOptimizerConfig.ringing_comp_enabled);
    s_prefs.putFloat ("opt_rngfq",  gOptimizerConfig.ring_freq_hz);
    s_prefs.putFloat ("opt_rngdr",  gOptimizerConfig.ring_damping_ratio);
    s_prefs.putBool ("zone_en",   gZone.enabled);
    s_prefs.putUChar("zone_cnt",  gZone.count);
    s_prefs.putBytes("zone_x",    (const void*)gZone.x, sizeof(gZone.x));
    s_prefs.putBytes("zone_y",    (const void*)gZone.y, sizeof(gZone.y));
    s_prefs.end();
}
static void loadZone() {
    s_prefs.begin("laser", true);
    gZone.enabled = s_prefs.getBool ("zone_en",  gZone.enabled);
    gZone.count   = s_prefs.getUChar("zone_cnt", gZone.count);
    if (gZone.count < 3)               gZone.count = 3;
    if (gZone.count > ZONE_POINTS_MAX) gZone.count = ZONE_POINTS_MAX;
    s_prefs.getBytes("zone_x", (void*)gZone.x, sizeof(gZone.x));
    s_prefs.getBytes("zone_y", (void*)gZone.y, sizeof(gZone.y));
    s_prefs.end();
}

/* ============================================================
 * JSON Builders
 * ============================================================ */
static void buildStateJson(JsonDocument& doc) {
    doc["estop_ok"]        = gState.estop_ok.load();
    doc["scanfail_ok"]     = gState.scanfail_ok.load();
    doc["laser_armed"]     = gState.laser_armed.load();
    doc["watchdog_ok"]     = safety::watchdogOk();
    doc["subsystems_ok"]   = safety::subsystemsOk();
    doc["last_failsafe"]   = safety::lastFailsafeReason();
    doc["arm_requested"]   = safety::userArmRequest();
    doc["calib_active"]    = gState.calib_active;
    doc["ilda_active"]     = ilda::gILDA.active;
    doc["playlist_active"] = playlist::isActive();
    doc["safety_override"] = gConfig.safety_override;
    doc["source"]          = (int)gState.source;
    { JsonArray off = doc["temp_offsets"].to<JsonArray>();
      JsonArray raw = doc["temp_raw"].to<JsonArray>();
      for (uint8_t i = 0; i < temp::NUM_SENSORS; i++) {
          off.add(temp::getSensorOffset(i));
          raw.add(temp::gTempState.temp_raw[i]);
      } }
    doc["sd_ready"]        = sd_card::isReady();
    doc["sd_free_kb"]      = sd_card::freeKB();
    doc["sd_total_kb"]     = sd_card::totalKB();
    doc["sd_error"]        = sd_card::errorMsg();
    doc["sd_fs_type"]      = sd_card::fsType();
    doc["sd_file_count"]   = sd_card::fileCount();
    doc["dac_ok"]          = galvo::dacOk();
    doc["no_hw_mode"]      = galvo::noHwMode();
    doc["preset_idx"]      = patterns::getPreset();
    doc["dmx_frame_count"] = gState.dmx_frame_count.load();
    doc["master_dimmer"]   = gState.master_dimmer.load();
    doc["points_per_sec"]  = galvo::pointsPerSec();
    doc["buffer_fill"]     = galvo::bufferFillLevel();
    uint32_t age = millis() - gState.last_dmx_ms.load();
    doc["last_dmx_age_ms"] = (gState.last_dmx_ms.load() == 0) ? -1 : (int32_t)age;
    doc["hostname"]        = gConfig.hostname;
    doc["ntp_server"]      = gConfig.ntp_server;
    doc["ntp_tz"]          = gConfig.ntp_tz;
    doc["ntp_synced"]      = ntp_client::isSynced();
    doc["ip"]              = WiFi.localIP().toString();
    doc["rssi"]            = WiFi.RSSI();
    doc["uptime_s"]        = millis() / 1000;
    doc["heap"]            = ESP.getFreeHeap();
    doc["free_heap"]       = ESP.getFreeHeap();     // alias for dashboard
    doc["cpu0"]            = cpu_mon::load0();
    doc["cpu1"]            = cpu_mon::load1();
    doc["psram"]           = ESP.getFreePsram();
    doc["free_psram"]      = ESP.getFreePsram();    // alias for dashboard
    // Dashboard extras
    doc["fw_version"]      = LASER_FW_VERSION;
    doc["ntp_synced"]      = ntp_client::isSynced();
    { char pw[12]; snprintf(pw, sizeof(pw), "%08X", (uint32_t)(ESP.getEfuseMac()>>16));
      doc["ota_pass"] = pw; }
    doc["auth_token"]  = s_auth_token;
    // Temperaturees + Namen + founde sensors
    doc["found"] = temp::foundSensorCount();
    JsonArray temps = doc["temps"].to<JsonArray>();
    JsonArray names = doc["names"].to<JsonArray>();
    JsonArray ok_arr = doc["ok"].to<JsonArray>();
    for (int i = 0; i < temp::NUM_SENSORS; i++) {
        if (temp::gTempState.sensor_ok[i]) temps.add(temp::gTempState.temp_c[i]);
        else temps.add(nullptr);
        names.add(temp::sensor_names[i]);
        ok_arr.add(temp::gTempState.sensor_ok[i]);
    }
    doc["fan1_duty"]  = temp::gTempState.fan1_duty;
    doc["fan2_duty"]  = temp::gTempState.fan2_duty;
    doc["temp_alert"] = temp::gTempState.any_alert;
    doc["temp_crit"]  = temp::gTempState.any_crit;
}

static void buildConfigJson(JsonDocument& doc) {
    doc["dmx_address"]     = gConfig.dmx_address;
    doc["artnet_universe"] = gConfig.artnet_universe;
    doc["galvo_x_offset"]  = gConfig.galvo_x_offset;
    doc["galvo_y_offset"]  = gConfig.galvo_y_offset;
    doc["galvo_x_gain"]    = gConfig.galvo_x_gain;
    doc["galvo_y_gain"]    = gConfig.galvo_y_gain;
    doc["swap_xy"]         = gConfig.swap_xy;
    doc["invert_x"]        = gConfig.invert_x;
    doc["invert_y"]        = gConfig.invert_y;
    doc["gain_r"]          = gConfig.gain_r;
    doc["gain_g"]          = gConfig.gain_g;
    doc["gain_b"]          = gConfig.gain_b;
    doc["hostname"]        = gConfig.hostname;
    doc["ntp_server"]      = gConfig.ntp_server;
    doc["ntp_tz"]          = gConfig.ntp_tz;
    doc["ntp_synced"]      = ntp_client::isSynced();
    doc["wifi_ssid"]       = gConfig.wifi_ssid;
    doc["wifi_static"]     = gConfig.wifi_static;
    doc["wifi_ip"]         = gConfig.wifi_ip;
    doc["wifi_gw"]         = gConfig.wifi_gw;
    doc["wifi_mask"]       = gConfig.wifi_mask;
    doc["wifi_dns"]        = gConfig.wifi_dns;
    doc["wifi_connected"]  = (WiFi.status() == WL_CONNECTED);
    doc["wifi_ip_current"] = WiFi.localIP().toString();
    doc["dac_debug_log"]   = gConfig.dac_debug_log;
    doc["dac_limit_min"]   = gConfig.dac_limit_min;
    doc["dac_limit_max"]   = gConfig.dac_limit_max;
    doc["opt_corner_angle_deg"]   = gOptimizerConfig.corner_angle_deg;
    doc["opt_min_corner_pts"]     = gOptimizerConfig.min_corner_pts;
    doc["opt_max_corner_pts"]     = gOptimizerConfig.max_corner_pts;
    doc["opt_pts_per_1000_units"] = gOptimizerConfig.pts_per_1000_units;
    doc["opt_min_segment_pts"]    = gOptimizerConfig.min_segment_pts;
    doc["opt_blank_samples"]      = gOptimizerConfig.blank_samples;
    doc["opt_max_pts_per_frame"]  = gOptimizerConfig.max_pts_per_frame;
    doc["opt_min_blank_samples"]  = gOptimizerConfig.min_blank_samples;
    doc["opt_blank_pts_per_1000_units"] = gOptimizerConfig.blank_pts_per_1000_units;
    doc["opt_min_interior_pts_per_segment"] = gOptimizerConfig.min_interior_pts_per_segment;
    doc["opt_stage1_blank_target"] = gOptimizerConfig.stage1_blank_target;
    doc["opt_ringing_comp_enabled"] = gOptimizerConfig.ringing_comp_enabled;
    doc["opt_ring_freq_hz"]         = gOptimizerConfig.ring_freq_hz;
    doc["opt_ring_damping_ratio"]   = gOptimizerConfig.ring_damping_ratio;
}

/* ============================================================
 * WiFi scan task (background, blocking ~3s)
 * ============================================================ */
static void wifiScanTask(void*) {
    s_scan_running = true;
    s_scan_results = 0;
    // Synchronous scan -- blocks ~2-3s, runs in its own task
    int n = WiFi.scanNetworks(false, false);
    s_scan_results = (n < 0) ? 0 : n;
    s_scan_running = false;
    ESP_LOGI(TAG, "WiFi-Scan: %d networks found", s_scan_results);
    vTaskDelete(nullptr);
}

/* ============================================================
 * Init -- register all endpoints
 * ============================================================ */
void init() {
    generateAuthToken();
    loadZone();

    s_paint_body = (char*)ps_malloc(PAINT_BODY_CAP);
    if (!s_paint_body) ESP_LOGE(TAG, "PSRAM alloc failed for paint body buffer");

    if (!LittleFS.begin(true))
        ESP_LOGE(TAG, "LittleFS mount failed");

    // ---- Statische SPA ----
    // register serveStatic at end -- API routes take precedence
    // (called further below, directly before s_server.begin())

    // ---- GET /api/state ----
    s_server.on("/api/state", HTTP_GET, [](AsyncWebServerRequest* req) {
        JsonDocument doc(&jsonAllocator()); buildStateJson(doc);
        sendJsonPsram(req, doc);
    });

    // ---- GET /api/config ----
    s_server.on("/api/config", HTTP_GET, [](AsyncWebServerRequest* req) {
        JsonDocument doc(&jsonAllocator()); buildConfigJson(doc);
        sendJsonPsram(req, doc);
    });

    // ---- POST /api/config ----
    s_server.on("/api/config", HTTP_POST,
        [](AsyncWebServerRequest* req) {},
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            JsonDocument doc(&jsonAllocator());
            if (deserializeJson(doc, data, len)) { req->send(400, "text/plain", "bad json"); return; }
            if (doc["dmx_address"].is<int>())      gConfig.dmx_address     = doc["dmx_address"];
            if (doc["artnet_universe"].is<int>())  gConfig.artnet_universe = doc["artnet_universe"];
            if (doc["hostname"].is<const char*>()) strlcpy(gConfig.hostname, doc["hostname"], sizeof(gConfig.hostname));
            if (doc["wifi_ssid"].is<const char*>()) strlcpy(gConfig.wifi_ssid, doc["wifi_ssid"], sizeof(gConfig.wifi_ssid));
            if (doc["wifi_pass"].is<const char*>()) strlcpy(gConfig.wifi_pass, doc["wifi_pass"], sizeof(gConfig.wifi_pass));
            if (doc["wifi_static"].is<bool>())     gConfig.wifi_static = doc["wifi_static"];
            if (doc["wifi_ip"].is<const char*>())  strlcpy(gConfig.wifi_ip,   doc["wifi_ip"],   sizeof(gConfig.wifi_ip));
            if (doc["wifi_gw"].is<const char*>())  strlcpy(gConfig.wifi_gw,   doc["wifi_gw"],   sizeof(gConfig.wifi_gw));
            if (doc["wifi_mask"].is<const char*>()) strlcpy(gConfig.wifi_mask, doc["wifi_mask"], sizeof(gConfig.wifi_mask));
            if (doc["wifi_dns"].is<const char*>())  strlcpy(gConfig.wifi_dns,  doc["wifi_dns"],  sizeof(gConfig.wifi_dns));
            if (doc["dac_debug_log"].is<bool>())    gConfig.dac_debug_log = doc["dac_debug_log"];
            if (doc["dac_limit_min"].is<int>() && doc["dac_limit_max"].is<int>()) {
                int lo = constrain((int)doc["dac_limit_min"], 0, 65535);
                int hi = constrain((int)doc["dac_limit_max"], 0, 65535);
                if (lo < hi) {
                    gConfig.dac_limit_min = (uint16_t)lo;
                    gConfig.dac_limit_max = (uint16_t)hi;
                } else {
                    req->send(400, "application/json",
                        "{\"error\":\"dac_limit_min must be < dac_limit_max\"}");
                    return;
                }
            }
            persistConfig();
            req->send(200, "text/plain", "OK");
        });

    // ---- POST /api/arm ----
    s_server.on("/api/arm", HTTP_POST,
        [](AsyncWebServerRequest* req) {},
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            bool arm = (len > 0 && data[0] == '1');
            safety::requestArm(arm);
            req->send(200, "text/plain", arm ? "ARMED" : "DISARMED");
        });

    // ---- POST /api/dmx-override ----
    s_server.on("/api/dmx-override", HTTP_POST,
        [](AsyncWebServerRequest* req) {},
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            JsonDocument doc(&jsonAllocator());
            if (deserializeJson(doc, data, len)) { req->send(400, "text/plain", "bad json"); return; }
            JsonArrayConst arr = doc["values"];
            if (arr.size() != DMX_CHANNELS_USED) { req->send(400, "text/plain", "need 16 values"); return; }
            for (int i = 0; i < DMX_CHANNELS_USED; i++) gOverride.values[i] = arr[i].as<uint8_t>();
            req->send(200, "text/plain", "OK");
        });

    // ---- POST /api/override-mode ----
    s_server.on("/api/override-mode", HTTP_POST,
        [](AsyncWebServerRequest* req) {},
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            JsonDocument doc(&jsonAllocator());
            if (deserializeJson(doc, data, len)) { req->send(400, "text/plain", "bad json"); return; }
            gOverride.active = doc["active"] | false;
            if (gOverride.active) gState.source.store(SRC_WEBUI);
            req->send(200, "text/plain", "OK");
        });

    // ---- POST /api/calib-live ----
    s_server.on("/api/calib-live", HTTP_POST,
        [](AsyncWebServerRequest* req) {},
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            JsonDocument doc(&jsonAllocator());
            if (deserializeJson(doc, data, len)) { req->send(400, "text/plain", "bad json"); return; }
            if (doc["xoff"].is<int>())   gConfig.galvo_x_offset = doc["xoff"];
            if (doc["yoff"].is<int>())   gConfig.galvo_y_offset = doc["yoff"];
            if (doc["xgain"].is<int>())  gConfig.galvo_x_gain   = doc["xgain"];
            if (doc["ygain"].is<int>())  gConfig.galvo_y_gain   = doc["ygain"];
            if (doc["swap"].is<bool>())  gConfig.swap_xy        = doc["swap"];
            if (doc["invx"].is<bool>())  gConfig.invert_x       = doc["invx"];
            if (doc["invy"].is<bool>())  gConfig.invert_y       = doc["invy"];
            { LOCK_CONFIG();
            if (doc["gain_r"].is<int>()) gConfig.gain_r = doc["gain_r"];
            if (doc["gain_g"].is<int>()) gConfig.gain_g = doc["gain_g"];
            if (doc["gain_b"].is<int>()) gConfig.gain_b = doc["gain_b"];
            if (doc["gamma_enable"].is<bool>()) gConfig.gamma_enable = doc["gamma_enable"];
            if (doc["gamma_val"].is<float>()) {
                gConfig.gamma_val = constrain((float)doc["gamma_val"], 1.0f, 3.0f);
                galvo::rebuildGammaLut(gConfig.gamma_val);
            }
            if (doc["dac_limit_min"].is<int>() && doc["dac_limit_max"].is<int>()) {
                int lo = constrain((int)doc["dac_limit_min"], 0, 65535);
                int hi = constrain((int)doc["dac_limit_max"], 0, 65535);
                if (lo < hi) {
                    gConfig.dac_limit_min = (uint16_t)lo;
                    gConfig.dac_limit_max = (uint16_t)hi;
                }
            }
            } // LOCK_CONFIG
            req->send(200, "text/plain", "OK");
        });

    // ---- POST /api/optimizer-live ---- (apply immediately, no persist)
    s_server.on("/api/optimizer-live", HTTP_POST,
        [](AsyncWebServerRequest* req) {},
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            JsonDocument doc(&jsonAllocator());
            if (deserializeJson(doc, data, len)) { req->send(400, "text/plain", "bad json"); return; }
            if (doc["corner_angle_deg"].is<float>())
                gOptimizerConfig.corner_angle_deg = constrain((float)doc["corner_angle_deg"], 0.0f, 180.0f);
            if (doc["min_corner_pts"].is<int>())
                gOptimizerConfig.min_corner_pts = constrain((int)doc["min_corner_pts"], 1, 20);
            if (doc["max_corner_pts"].is<int>())
                gOptimizerConfig.max_corner_pts = constrain((int)doc["max_corner_pts"], 1, 20);
            if (doc["pts_per_1000_units"].is<float>())
                gOptimizerConfig.pts_per_1000_units = constrain((float)doc["pts_per_1000_units"], 0.1f, 50.0f);
            if (doc["min_segment_pts"].is<int>())
                gOptimizerConfig.min_segment_pts = constrain((int)doc["min_segment_pts"], 2, 20);
            if (doc["blank_samples"].is<int>())
                gOptimizerConfig.blank_samples = constrain((int)doc["blank_samples"], 1, 100);
            if (doc["max_pts_per_frame"].is<int>())
                gOptimizerConfig.max_pts_per_frame = constrain((int)doc["max_pts_per_frame"], 50, (int)PATTERN_POINTS_MAX);
            if (doc["min_blank_samples"].is<int>())
                gOptimizerConfig.min_blank_samples = constrain((int)doc["min_blank_samples"], 1, 100);
            if (doc["blank_pts_per_1000_units"].is<float>())
                gOptimizerConfig.blank_pts_per_1000_units = constrain((float)doc["blank_pts_per_1000_units"], 0.1f, 50.0f);
            if (doc["min_interior_pts_per_segment"].is<int>())
                gOptimizerConfig.min_interior_pts_per_segment = constrain((int)doc["min_interior_pts_per_segment"], 0, 50);
            if (doc["stage1_blank_target"].is<int>())
                gOptimizerConfig.stage1_blank_target = constrain((int)doc["stage1_blank_target"], 1, 100);
            if (doc["ringing_comp_enabled"].is<bool>())
                gOptimizerConfig.ringing_comp_enabled = (bool)doc["ringing_comp_enabled"];
            if (doc["ring_freq_hz"].is<float>())
                gOptimizerConfig.ring_freq_hz = constrain((float)doc["ring_freq_hz"], 1.0f, 2000.0f);
            if (doc["ring_damping_ratio"].is<float>())
                gOptimizerConfig.ring_damping_ratio = constrain((float)doc["ring_damping_ratio"], 0.0f, 0.9f);
            req->send(200, "text/plain", "OK");
        });

    // ---- POST /api/optimizer-save ---- (persist current values to NVS)
    s_server.on("/api/optimizer-save", HTTP_POST,
        [](AsyncWebServerRequest* req) {},
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t*, size_t, size_t, size_t) {
            persistConfig();
            req->send(200, "text/plain", "saved");
        });
        // ---- GET /api/zone ----
    s_server.on("/api/zone", HTTP_GET, [](AsyncWebServerRequest* req) {
        JsonDocument doc(&jsonAllocator());
        doc["enabled"] = gZone.enabled;
        doc["count"]   = gZone.count;
        JsonArray zx = doc["x"].to<JsonArray>();
        JsonArray zy = doc["y"].to<JsonArray>();
        for (uint8_t i = 0; i < gZone.count; i++) { zx.add(gZone.x[i]); zy.add(gZone.y[i]); }
        sendJsonPsram(req, doc);
    });

    // ---- POST /api/zone/enable ---- (toggle clipping without re-sending polygon)
    s_server.on("/api/zone/enable", HTTP_POST,
        [](AsyncWebServerRequest* req) {},
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            JsonDocument doc(&jsonAllocator());
            if (deserializeJson(doc, data, len)) { req->send(400, "text/plain", "bad json"); return; }
            { LOCK_ZONE(); gZone.enabled = doc["enabled"] | false; }
            persistConfig();
            req->send(200, "text/plain", gZone.enabled ? "ENABLED" : "DISABLED");
        });

    // ---- POST /api/zone/preview ---- (project zone outline = last calib pattern)
    s_server.on("/api/zone/preview", HTTP_POST,
        [](AsyncWebServerRequest* req) {},
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            bool on = !(len > 0 && data[0] == '0');
            if (on) {
                gState.calib_idx     = calib_patterns::CALIB_PATTERN_COUNT - 1;  // zone_outline
                gState.calib_bright  = 200;
                gState.calib_channel = 0;
                gState.calib_active  = true;
            } else {
                gState.calib_active = false;
            }
            req->send(200, "text/plain", on ? "PREVIEW" : "STOP");
        });

    // ---- POST /api/zone ---- (apply + persist polygon). Registered LAST so
    // the more-specific /api/zone/enable and /api/zone/preview match first.
    s_server.on("/api/zone", HTTP_POST,
        [](AsyncWebServerRequest* req) {},
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            JsonDocument doc(&jsonAllocator());
            if (deserializeJson(doc, data, len)) { req->send(400, "text/plain", "bad json"); return; }
            JsonArrayConst ax = doc["x"];
            JsonArrayConst ay = doc["y"];
            if (ax.isNull() || ay.isNull() || ax.size() != ay.size() ||
                ax.size() < 3 || ax.size() > ZONE_POINTS_MAX) {
                req->send(400, "application/json",
                    "{\"error\":\"need x[] and y[] of equal length, 3..16 points\"}");
                return;
            }
            uint8_t cnt = ax.size();
            { LOCK_ZONE();
                for (uint8_t i = 0; i < cnt; i++) {
                    gZone.x[i] = (int16_t)constrain((int)ax[i].as<int>(), -32767, 32767);
                    gZone.y[i] = (int16_t)constrain((int)ay[i].as<int>(), -32767, 32767);
                }
                gZone.count = cnt;
                if (doc["enabled"].is<bool>()) gZone.enabled = doc["enabled"];
            }
            persistConfig();
            req->send(200, "text/plain", "OK");
        });
    // ---- POST /api/calib-save ----
    s_server.on("/api/calib-save", HTTP_POST,
        [](AsyncWebServerRequest* req) {},
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t*, size_t, size_t, size_t) {
            persistConfig();
            req->send(200, "text/plain", "saved");
        });

    // ---- POST /api/test-pattern ----
    s_server.on("/api/test-pattern", HTTP_POST,
        [](AsyncWebServerRequest* req) {},
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            JsonDocument doc(&jsonAllocator());
            if (deserializeJson(doc, data, len)) { req->send(400, "text/plain", "bad json"); return; }
            const char* name = doc["pattern"] | "";
            patterns::triggerTestPattern(name);
            req->send(200, "text/plain", "OK");
        });

    // ---- GET /api/presets ---- all 40 preset names
    s_server.on("/api/presets", HTTP_GET, [](AsyncWebServerRequest* req) {
        JsonDocument doc(&jsonAllocator());
        JsonArray arr = doc.to<JsonArray>();
        for (uint8_t i = 0; i < presets::PRESET_COUNT; i++) {
            JsonObject p = arr.add<JsonObject>();
            p["idx"]  = i;
            p["name"] = presets::PRESETS[i].name;
            p["cat"]  = presets::PRESETS[i].category;
        }
        sendJsonPsram(req, doc);
    });

    // ---- POST /api/preset ---- activate/deactivate preset
    s_server.on("/api/preset", HTTP_POST,
        [](AsyncWebServerRequest* req) {},
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            JsonDocument doc(&jsonAllocator());
            if (deserializeJson(doc, data, len)) { req->send(400, "text/plain", "bad json"); return; }
            int8_t idx = doc["idx"] | -1;
            patterns::setPreset(idx);
            req->send(200, "text/plain", "OK");
        });

    // ---- POST /api/fan-override ----
    s_server.on("/api/fan-override", HTTP_POST,
        [](AsyncWebServerRequest* req) {},
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            JsonDocument doc(&jsonAllocator());
            if (deserializeJson(doc, data, len)) { req->send(400, "text/plain", "bad json"); return; }
            if (doc["fan0"].is<int>()) temp::setFanOverride(0, doc["fan0"]);
            if (doc["fan1"].is<int>()) temp::setFanOverride(1, doc["fan1"]);
            req->send(200, "text/plain", "OK");
        });

    // ---- POST /api/temp-thresholds ----
    s_server.on("/api/temp-thresholds", HTTP_POST,
        [](AsyncWebServerRequest* req) {},
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            JsonDocument doc(&jsonAllocator());
            if (deserializeJson(doc, data, len)) { req->send(400, "text/plain", "bad json"); return; }
            JsonArrayConst arr = doc["thresholds"];
            for (size_t i = 0; i < min((size_t)temp::NUM_SENSORS, arr.size()); i++) {
                if (arr[i]["warn"].is<float>())  temp::thresholds[i].warn  = arr[i]["warn"];
                if (arr[i]["alert"].is<float>()) temp::thresholds[i].alert = arr[i]["alert"];
                if (arr[i]["crit"].is<float>())  temp::thresholds[i].crit  = arr[i]["crit"];
            }
            req->send(200, "text/plain", "OK");
        });

    // ---- POST /api/preset-live ---- real-time parameters for running preset
    s_server.on("/api/preset-live", HTTP_POST,
        [](AsyncWebServerRequest* req) {},
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            JsonDocument doc(&jsonAllocator());
            if (deserializeJson(doc, data, len)) { req->send(400, "text/plain", "bad json"); return; }
            if (doc["speed"].is<int>())     gLivePreset.speed     = doc["speed"];
            if (doc["size"].is<int>())      gLivePreset.size_val  = doc["size"];
            if (doc["autoscaleSpeed"].is<int>()) gLivePreset.autoscaleSpeed = (uint8_t)constrain((int)doc["autoscaleSpeed"], 0, 100);
            if (doc["autoscaleMode"].is<int>())  gLivePreset.autoscaleMode  = (uint8_t)constrain((int)doc["autoscaleMode"], 0, 2);
            if (doc["col_r"].is<int>())          gLivePreset.col_r          = doc["col_r"];
            if (doc["col_g"].is<int>())          gLivePreset.col_g          = doc["col_g"];
            if (doc["col_b"].is<int>())          gLivePreset.col_b          = doc["col_b"];
            if (doc["col_override"].is<bool>())  gLivePreset.col_override   = doc["col_override"];
            if (doc["col_anim_type"].is<int>())  gLivePreset.col_anim_type  = (ColAnimType)(uint8_t)(int)doc["col_anim_type"];
            if (doc["col_anim_seq"].is<int>())   gLivePreset.col_anim_seq   = (uint8_t)(int)doc["col_anim_seq"];
            if (doc["col_anim_speed"].is<int>()) gLivePreset.col_anim_speed = (uint8_t)(int)doc["col_anim_speed"];
            if (doc["col_seg_count"].is<int>())  gLivePreset.col_seg_count  = (uint8_t)(int)doc["col_seg_count"];
            if (doc["col_seg_dir"].is<int>())    gLivePreset.col_seg_dir    = (int8_t)(int)doc["col_seg_dir"];
            if (doc["rotation"].is<int>())     gLivePreset.rotation    = (int16_t)(int)doc["rotation"];
            if (doc["kaleido_enabled"].is<bool>())  gLivePreset.kaleido_enabled  = doc["kaleido_enabled"];
            if (doc["kaleido_segments"].is<int>())  gLivePreset.kaleido_segments = (uint8_t)constrain((int)doc["kaleido_segments"], 2, KALEIDO_SEGMENTS_MAX);
            if (doc["kaleido_mirror_h"].is<bool>()) gLivePreset.kaleido_mirror_h = doc["kaleido_mirror_h"];
            if (doc["kaleido_mirror_v"].is<bool>()) gLivePreset.kaleido_mirror_v = doc["kaleido_mirror_v"];
            if (doc["mirror_mode"].is<int>())       gLivePreset.mirror_mode      = (uint8_t)constrain((int)doc["mirror_mode"], 0, 3);
            { LOCK_STATE();
                if (doc["rot_x"].is<bool>()) { gLivePreset.rot_x = doc["rot_x"]; gLivePreset.rot_angle_x = 0; }
                if (doc["rot_y"].is<bool>()) { gLivePreset.rot_y = doc["rot_y"]; gLivePreset.rot_angle_y = 0; }
                if (doc["rot_z"].is<bool>()) { gLivePreset.rot_z = doc["rot_z"]; gLivePreset.rot_angle_z = 0; }
            }
            if (doc["rot_speed"].is<float>())  {
                float rs = doc["rot_speed"];
                gLivePreset.rot_speed_z = rs;
                gLivePreset.rot_speed_y = rs * 0.9f;
                gLivePreset.rot_speed_x = rs * 0.75f;
            }
            if (doc["wave_amp"].is<float>())  gLivePreset.wave_amp  = constrain((float)doc["wave_amp"],  0.1f, 2.0f);
            if (doc["wave_freq"].is<float>()) gLivePreset.wave_freq = constrain((float)doc["wave_freq"], 0.25f, 4.0f);
            if (doc["points_mode_enabled"].is<bool>()) gLivePreset.points_mode_enabled = doc["points_mode_enabled"];
            if (doc["points_count"].is<int>())  gLivePreset.points_count  = (uint8_t)constrain((int)doc["points_count"], 2, POINTS_MODE_MAX_DOTS);
            if (doc["points_fade_in_on"].is<bool>())  gLivePreset.points_fade_in_on  = doc["points_fade_in_on"];
            if (doc["points_fade_out_on"].is<bool>()) gLivePreset.points_fade_out_on = doc["points_fade_out_on"];
            if (doc["points_fade_in_ms"].is<int>())   gLivePreset.points_fade_in_ms  = (uint16_t)constrain((int)doc["points_fade_in_ms"], 0, 10000);
            if (doc["points_fade_out_ms"].is<int>())  gLivePreset.points_fade_out_ms = (uint16_t)constrain((int)doc["points_fade_out_ms"], 0, 10000);
            if (doc["points_fade_dir"].is<int>())     gLivePreset.points_fade_dir    = (uint8_t)constrain((int)doc["points_fade_dir"], 0, 5);
            if (doc["points_static_on"].is<bool>())   gLivePreset.points_static_on   = doc["points_static_on"];
            if (doc["random_pts_hold_ms"].is<int>())  gLivePreset.random_pts_hold_ms = (uint16_t)constrain((int)doc["random_pts_hold_ms"], 50, 5000);
            req->send(200, "text/plain", "OK");
        });

    // ---- POST /api/text ---- text mode
    // ── /api/curves GET — curve state + param definitions ───────────────────
    s_server.on("/api/curves", HTTP_GET, [](AsyncWebServerRequest* req) {
        // Initialize defaults on first access
        if (!gCurves.initialized) {
            for (uint8_t i = 0; i < curves::CURVE_COUNT; i++) {
                curves::CurveParams tmp;
                curves::initDefaultParams(i, tmp);
                for (int j = 0; j < 5; j++) gCurves.params[i].p[j] = tmp.p[j];
                gCurves.params[i].r = tmp.r;
                gCurves.params[i].g = tmp.g;
                gCurves.params[i].b = tmp.b;
            }
            gCurves.initialized = true;
        }
        JsonDocument doc(&jsonAllocator());
        doc["active"] = gCurves.active_curve;
        doc["count"]  = (int)curves::CURVE_COUNT;

        JsonArray defs = doc["defs"].to<JsonArray>();
        for (uint8_t ci = 0; ci < curves::CURVE_COUNT; ci++) {
            const curves::CurveDef& d = curves::CURVE_DEFS[ci];
            JsonObject def = defs.add<JsonObject>();
            def["name"] = d.name;
            def["desc"] = d.description;
            JsonArray params = def["params"].to<JsonArray>();
            for (int pi = 0; pi < 5; pi++) {
                const curves::ParamDef& pd = d.params[pi];
                JsonObject p = params.add<JsonObject>();
                p["label"] = pd.label;
                p["min"]   = pd.min_val;
                p["max"]   = pd.max_val;
                p["def"]   = pd.def_val;
                p["step"]  = pd.step;
            }
            def["dr"] = d.def_r;
            def["dg"] = d.def_g;
            def["db"] = d.def_b;
        }

        JsonArray params = doc["params"].to<JsonArray>();
        for (uint8_t ci = 0; ci < curves::CURVE_COUNT; ci++) {
            const CurveConfig::Params& cp = gCurves.params[ci];
            JsonObject po = params.add<JsonObject>();
            JsonArray p = po["p"].to<JsonArray>();
            for (int pi = 0; pi < 5; pi++) p.add(cp.p[pi]);
            po["r"] = cp.r;
            po["g"] = cp.g;
            po["b"] = cp.b;
        }

        sendJsonPsram(req, doc);
    });

    // ── /api/curves POST — select curve + update params ──────────────────────
    s_server.on("/api/curves", HTTP_POST, [](AsyncWebServerRequest* req){},
    nullptr,
    [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
        JsonDocument doc(&jsonAllocator());
        if (deserializeJson(doc, data, len) != DeserializationError::Ok) {
            req->send(400, "application/json", "{\"error\":\"bad json\"}");
            return;
        }
        // Select curve (-1 = off)
        if (!doc["curve"].isNull()) {
            int8_t idx = doc["curve"] | -1;
            patterns::setCurve(idx);
        }
        // Update params for a specific curve
        if (!doc["ci"].isNull() && !doc["params"].isNull()) {
            uint8_t ci = doc["ci"] | 255;
            if (ci < curves::CURVE_COUNT) {
                JsonArray pa = doc["params"].as<JsonArray>();
                for (int pi = 0; pi < 5 && pi < (int)pa.size(); pi++) {
                    float v = pa[pi];
                    const curves::ParamDef& pd = curves::CURVE_DEFS[ci].params[pi];
                    gCurves.params[ci].p[pi] = std::max(pd.min_val, std::min(pd.max_val, v));
                }
                if (!doc["r"].isNull()) gCurves.params[ci].r = doc["r"] | 255;
                if (!doc["g"].isNull()) gCurves.params[ci].g = doc["g"] | 0;
                if (!doc["b"].isNull()) gCurves.params[ci].b = doc["b"] | 128;
            }
        }
        req->send(200, "application/json", "{\"ok\":true}");
    });

        s_server.on("/api/text", HTTP_POST,
        [](AsyncWebServerRequest* req) {},
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            JsonDocument doc(&jsonAllocator());
            if (deserializeJson(doc, data, len)) { req->send(400, "text/plain", "bad json"); return; }
            { LOCK_STATE();
                if (doc["text"].is<const char*>())   strlcpy(gTextConfig.text, doc["text"], sizeof(gTextConfig.text));
                if (doc["font"].is<int>())           gTextConfig.font      = (TextFont)(int)doc["font"];
                if (doc["anim"].is<int>())           gTextConfig.animation = (TextAnim)(int)doc["anim"];
                if (doc["speed"].is<int>())          gTextConfig.speed     = doc["speed"];
                if (doc["size"].is<int>())           gTextConfig.size_val  = doc["size"];
                if (doc["col_r"].is<int>())          gTextConfig.col_r     = doc["col_r"];
                if (doc["col_g"].is<int>())          gTextConfig.col_g     = doc["col_g"];
                if (doc["col_b"].is<int>())          gTextConfig.col_b     = doc["col_b"];
                if (doc["rainbow"].is<bool>())       gTextConfig.rainbow   = doc["rainbow"];
                if (doc["flip_x"].is<bool>())        gTextConfig.flip_x    = doc["flip_x"];
                if (doc["flip_y"].is<bool>())        gTextConfig.flip_y    = doc["flip_y"];
                if (doc["active"].is<bool>())        gTextConfig.active    = doc["active"];
            }
            req->send(200, "text/plain", "OK");
        });

    // ---- GET /api/text/vertices ---- raw glyph outline paths for the
    // Paint by Finger "Text" tool (see textrender::glyphOutlinePaths).
    // Query: text=<string, max 24 chars>, size=<0-255, default 128>
    // NOTE: must be registered BEFORE GET /api/text -- ESPAsyncWebServer
    // matched this URI to the shorter, earlier-registered /api/text
    // handler (returned TextConfig JSON instead of glyph paths).
    s_server.on("/api/text/vertices", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (!req->hasParam("text")) {
            req->send(400, "application/json", "{\"error\":\"missing text param\"}");
            return;
        }
        String text = req->getParam("text")->value();
        if (text.length() > 24) text = text.substring(0, 24);

        uint8_t size_val = 128;
        if (req->hasParam("size")) {
            size_val = (uint8_t)constrain(atoi(req->getParam("size")->value().c_str()), 0, 255);
        }
        const float scale = 40.f + (size_val / 255.f) * 800.f;

        textrender::GlyphSubpath paths[textrender::TEXT_VERTICES_MAX_PATHS];
        size_t n = textrender::glyphOutlinePaths(text.c_str(), scale, paths, textrender::TEXT_VERTICES_MAX_PATHS);

        JsonDocument doc(&jsonAllocator());
        JsonArray arr = doc["paths"].to<JsonArray>();
        for (size_t i = 0; i < n; i++) {
            JsonObject o = arr.add<JsonObject>();
            JsonArray xa = o["x"].to<JsonArray>();
            JsonArray ya = o["y"].to<JsonArray>();
            for (uint8_t v = 0; v < paths[i].count; v++) { xa.add(paths[i].x[v]); ya.add(paths[i].y[v]); }
        }
        doc["count"] = (int)n;
        sendJsonPsram(req, doc);
    });

    // ---- GET /api/text ----
    s_server.on("/api/text", HTTP_GET, [](AsyncWebServerRequest* req) {
        JsonDocument doc(&jsonAllocator());
        doc["text"]    = gTextConfig.text;
        doc["font"]    = (int)gTextConfig.font;
        doc["anim"]    = (int)gTextConfig.animation;
        doc["speed"]   = gTextConfig.speed;
        doc["size"]    = gTextConfig.size_val;
        doc["col_r"]   = gTextConfig.col_r;
        doc["col_g"]   = gTextConfig.col_g;
        doc["col_b"]   = gTextConfig.col_b;
        doc["rainbow"] = gTextConfig.rainbow;
        doc["flip_x"]  = gTextConfig.flip_x;
        doc["flip_y"]  = gTextConfig.flip_y;
        doc["active"]  = gTextConfig.active;
        sendJsonPsram(req, doc);
    });

    // ---- POST /api/text/off ----
    s_server.on("/api/text/off", HTTP_POST,
        [](AsyncWebServerRequest* req) { gTextConfig.active = false; req->send(200,"text/plain","OK"); });

    // ---- GET /api/paint ---- current canvas (reload / multi-client sync)
    s_server.on("/api/paint", HTTP_GET, [](AsyncWebServerRequest* req) {
        JsonDocument doc(&jsonAllocator());
        doc["active"] = gPaint.active;
        JsonArray arr = doc["strokes"].to<JsonArray>();
        { LOCK_PAINT();
            for (uint8_t s = 0; s < gPaint.stroke_count; s++) {
                const PaintStroke& st = gPaint.strokes[s];
                JsonObject o = arr.add<JsonObject>();
                o["closed"] = st.closed;
                o["r"] = st.r; o["g"] = st.g; o["b"] = st.b;
                JsonArray xa = o["x"].to<JsonArray>();
                JsonArray ya = o["y"].to<JsonArray>();
                for (uint16_t i = 0; i < st.count; i++) { xa.add(st.x[i]); ya.add(st.y[i]); }
            }
        }
        sendJsonPsram(req, doc);
    });

    // ---- POST /api/paint/set ---- replace canvas (full strokes[] upload)
    // Body: {active?:bool, strokes:[{x:[...],y:[...],closed:bool,r,g,b}, ...]}
    s_server.on("/api/paint/set", HTTP_POST,
        [](AsyncWebServerRequest* req) {},
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total) {
            // Body may arrive across multiple TCP chunks (e.g. several Circle
            // strokes, 41 vertices each) -- buffer until fully received.
            if (!s_paint_body) { req->send(500, "text/plain", "no body buffer"); return; }
            if (index == 0) s_paint_body_len = 0;
            if (total >= PAINT_BODY_CAP) { req->send(400, "text/plain", "body too large"); return; }
            if (s_paint_body_len + len < PAINT_BODY_CAP) {
                memcpy(s_paint_body + s_paint_body_len, data, len);
                s_paint_body_len += len;
            }
            if (index + len != total) return;
            s_paint_body[s_paint_body_len] = 0;
            JsonDocument doc(&jsonAllocator());
            DeserializationError jerr = deserializeJson(doc, s_paint_body, s_paint_body_len);
            if (jerr) { req->send(400, "text/plain", "bad json"); return; }
            JsonArrayConst strokesArr = doc["strokes"];
            if (strokesArr.isNull() || strokesArr.size() > PAINT_STROKES_MAX) {
                req->send(400, "application/json",
                    "{\"error\":\"need strokes[], max 12\"}");
                return;
            }
            { LOCK_PAINT();
                uint8_t sc = 0;
                for (JsonObjectConst so : strokesArr) {
                    JsonArrayConst ax = so["x"];
                    JsonArrayConst ay = so["y"];
                    if (ax.isNull() || ay.isNull() || ax.size() != ay.size() || ax.size() < 2) continue;
                    uint16_t cnt = (uint16_t)min((size_t)PAINT_VERTS_PER_STROKE, ax.size());
                    PaintStroke& st = gPaint.strokes[sc];
                    for (uint16_t i = 0; i < cnt; i++) {
                        st.x[i] = ax[i].as<float>();
                        st.y[i] = ay[i].as<float>();
                    }
                    st.count  = cnt;
                    st.closed = so["closed"] | false;
                    st.r = (uint8_t)(so["r"] | 255);
                    st.g = (uint8_t)(so["g"] | 255);
                    st.b = (uint8_t)(so["b"] | 255);
                    sc++;
                }
                gPaint.stroke_count = sc;
            }
            if (doc["active"].is<bool>()) patterns::setPaintActive(doc["active"]);
            req->send(200, "text/plain", "OK");
        });

    // ---- POST /api/paint/clear ---- empty canvas, keeps active state ----
    s_server.on("/api/paint/clear", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            { LOCK_PAINT(); gPaint.stroke_count = 0; }
            req->send(200, "text/plain", "OK");
        });

    // ---- POST /api/paint/off ---- deactivate Paint mode, keep strokes ----
    s_server.on("/api/paint/off", HTTP_POST,
        [](AsyncWebServerRequest* req) { patterns::setPaintActive(false); req->send(200,"text/plain","OK"); });

    // ---- GET /api/sd ---- SD cardn-Status and file list
    s_server.on("/api/sd", HTTP_GET, [](AsyncWebServerRequest* req) {
        JsonDocument doc(&jsonAllocator());
        doc["ready"]     = sd_card::isReady();
        doc["file_count"]= sd_card::fileCount();
        doc["free_kb"]   = sd_card::freeKB();
        doc["total_kb"]  = sd_card::totalKB();
        JsonArray files  = doc["files"].to<JsonArray>();
        for (uint8_t i = 0; i < sd_card::fileCount(); i++) {
            JsonObject fo = files.add<JsonObject>();
            fo["idx"]  = i;
            fo["name"] = sd_card::fileName(i);
            fo["path"] = sd_card::filePath(i);
            fo["dmx"]  = i + 1;  // DMX-value = Index + 1
        }
        doc["ilda_active"]  = ilda::gILDA.active;
        doc["ilda_file"]    = ilda::gILDA.file_idx;
        doc["ilda_frame"]   = ilda::gILDA.current_frame;
        doc["ilda_total"]   = ilda::gILDA.total_frames;
        doc["ilda_points"]  = ilda::gILDA.total_points;
        sendJsonPsram(req, doc);
    });


    // ---- GET /api/sd/info ---- detailed SD card info ----
    s_server.on("/api/sd/info", HTTP_GET, [](AsyncWebServerRequest* req) {
        JsonDocument doc(&jsonAllocator());
        doc["ready"]      = sd_card::isReady();
        doc["fs_type"]    = sd_card::fsType();
        doc["total_kb"]   = sd_card::totalKB();
        doc["free_kb"]    = sd_card::freeKB();
        doc["used_kb"]    = sd_card::totalKB() - sd_card::freeKB();
        doc["file_count"] = sd_card::fileCount();
        doc["error"]      = sd_card::errorMsg();
        if (sd_card::totalKB() > 0) {
            doc["used_pct"] = (int)(100UL * (sd_card::totalKB() - sd_card::freeKB())
                                   / sd_card::totalKB());
        } else {
            doc["used_pct"] = 0;
        }
        char buf[512];
        serializeJson(doc, buf, sizeof(buf));
        req->send(200, "application/json", buf);
    });

    // ---- POST /api/sd/scan ---- SD neu scannen
    s_server.on("/api/sd/scan", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            uint8_t n = sd_card::scanFiles();
            req->send(200, "application/json",
                      String("{\"file_count\":") + n + "}");
        });

    // ---- POST /api/sd/remount ---- unmount + remount (hot-swap)
    s_server.on("/api/sd/remount", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            bool ok = sd_card::remount();
            req->send(ok ? 200 : 500, "application/json",
                ok ? "{\"ok\":true}"
                   : "{\"ok\":false,\"error\":\"Mount failed\"}");
        });

    // ---- POST /api/sd/eject ---- safe eject (flush + unmount)
    s_server.on("/api/sd/eject", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            sd_card::eject();
            req->send(200, "application/json", "{\"ok\":true}");
        });

    // ---- POST /api/ilda/play ---- play ILDA file
    s_server.on("/api/ilda/play", HTTP_POST,
        [](AsyncWebServerRequest* req) {},
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            JsonDocument doc(&jsonAllocator());
            if (deserializeJson(doc, data, len)) { req->send(400); return; }
            uint8_t idx = doc["idx"] | 255;
            if (idx == 255) { req->send(400, "text/plain", "idx required"); return; }
            bool ok = ilda::loadFile(idx);
            if (doc["speed"].is<int>())    ilda::gILDA.speed    = doc["speed"];
            if (doc["size"].is<int>())     ilda::gILDA.size_val = doc["size"];
            if (doc["loop"].is<bool>())    ilda::gILDA.loop     = doc["loop"];
            req->send(ok ? 200 : 500, "text/plain", ok ? "OK" : "Error");
        });

    // ---- POST /api/ilda/stop ---- stop ILDA
    s_server.on("/api/ilda/stop", HTTP_POST,
        [](AsyncWebServerRequest* req) { ilda::stop(); req->send(200,"text/plain","OK"); });

    // ---- POST /api/ilda/pause ---- pause ILDA
    s_server.on("/api/ilda/pause", HTTP_POST,
        [](AsyncWebServerRequest* req) {},
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            JsonDocument doc(&jsonAllocator());
            if (!deserializeJson(doc, data, len)) ilda::pause((bool)doc["paused"]);
            req->send(200,"text/plain","OK");
        });

    // ---- GET /api/ilda/status ----
    s_server.on("/api/ilda/status", HTTP_GET, [](AsyncWebServerRequest* req) {
        JsonDocument doc(&jsonAllocator());
        doc["active"]  = ilda::gILDA.active;
        doc["file_idx"]= ilda::gILDA.file_idx;
        doc["frame"]   = ilda::gILDA.current_frame;
        doc["total"]   = ilda::gILDA.total_frames;
        doc["speed"]   = ilda::gILDA.speed;
        doc["size"]    = ilda::gILDA.size_val;
        doc["loop"]    = ilda::gILDA.loop;
        if (ilda::gILDA.file_idx >= 0 && ilda::gILDA.file_idx < sd_card::fileCount())
            doc["name"] = sd_card::fileName(ilda::gILDA.file_idx);
        sendJsonPsram(req, doc);
    });

    // ---- GET /api/dmx/channels ---- DMX-channel-Dokumentation
    s_server.on("/api/dmx/channels", HTTP_GET, [](AsyncWebServerRequest* req) {
        JsonDocument doc(&jsonAllocator());
        JsonArray arr = doc["channels"].to<JsonArray>();
        for (int i = 0; i < DMX_CHANNELS_USED; i++) {
            JsonObject ch = arr.add<JsonObject>();
            ch["ch"]   = i + 1;
            ch["name"] = DMX_CHANNEL_NAMES[i];
            ch["ilda"] = (i >= DMX_ILDA_SELECT);
        }
        sendJsonPsram(req, doc);
    });

    // ── calibration-Pattern API ──────────────────────────────────
    // NOTE: specific routes (/stop, /list) must be registered BEFORE
    // the bare /api/calib-pattern route — ESPAsyncWebServer matches
    // the first registered handler whose prefix matches the URL.

    // POST /api/calib-pattern/stop  (registered first — avoids prefix match)
    s_server.on("/api/calib-pattern/stop", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            gState.calib_active = false;
            // Release calib-forced dimmer only if no real DMX source active
            if (gState.master_dimmer.load() == 0)
                gState.ui_master_dimmer.store(0);
            req->send(200, "text/plain", "OK");
        });

    // GET /api/calib-pattern/list
    s_server.on("/api/calib-pattern/list", HTTP_GET,
        [](AsyncWebServerRequest* req) {
            JsonDocument doc(&jsonAllocator());
            JsonArray arr = doc["patterns"].to<JsonArray>();
            for (uint8_t i = 0; i < calib_patterns::CALIB_PATTERN_COUNT; i++) {
                JsonObject o = arr.add<JsonObject>();
                o["idx"]   = i;
                o["name"]  = calib_patterns::CALIB_INFO[i].name;
                o["desc"]  = calib_patterns::CALIB_INFO[i].desc;
                o["check"] = calib_patterns::CALIB_INFO[i].what_to_check;
            }
            doc["active"]  = gState.calib_active;
            doc["idx"]     = gState.calib_idx;
            doc["bright"]  = gState.calib_bright;
            doc["channel"] = gState.calib_channel;
            sendJsonPsram(req, doc);
        });

    // POST /api/calib-pattern {"idx":0,"bright":200,"channel":0,"active":true}
    s_server.on("/api/calib-pattern", HTTP_POST,
        [](AsyncWebServerRequest* req) {},
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            JsonDocument doc(&jsonAllocator());
            if (deserializeJson(doc, data, len)) { req->send(400); return; }
            if (doc["active"].is<bool>())   gState.calib_active  = doc["active"];
            if (doc["idx"].is<int>())       gState.calib_idx     = constrain((int)doc["idx"], 0, calib_patterns::CALIB_PATTERN_COUNT-1);
            if (doc["bright"].is<int>())    gState.calib_bright  = doc["bright"];
            if (doc["channel"].is<int>())   gState.calib_channel = constrain((int)doc["channel"], 0, 3);
            // Calib mode enabled -> disable ILDA and text
           if (gState.calib_active) {
                ilda::stop();
                gTextConfig.active = false;
                // Ensure beam is active during calibration even without DMX
                if (gState.ui_master_dimmer.load() == 0)
                    gState.ui_master_dimmer.store(200);
            }
            req->send(200, "text/plain", "OK");
        });

    // POST /api/calib-pattern/stop
    s_server.on("/api/calib-pattern/stop", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            gState.calib_active = false;
            req->send(200, "text/plain", "OK");
        });


    // ── Feature 5: safety configuration ──────────────────────
    s_server.on("/api/safety/config", HTTP_GET, [](AsyncWebServerRequest* req) {
        JsonDocument doc(&jsonAllocator());
        doc["temp_warn"]     = gSafety.temp_warn_c;
        doc["temp_reduce"]   = gSafety.temp_reduce_c;
        doc["temp_shutdown"] = gSafety.temp_shutdown_c;
        doc["fan_min_pct"]   = gSafety.fan_min_pct;
        doc["fan_auto"]      = gSafety.fan_auto;
        sendJsonPsram(req, doc);
    });
    s_server.on("/api/safety/config", HTTP_POST,
        [](AsyncWebServerRequest* req){},
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            JsonDocument doc(&jsonAllocator());
            if (deserializeJson(doc, data, len)) { req->send(400); return; }
            if (doc["temp_warn"].is<int>())     gSafety.temp_warn_c     = doc["temp_warn"];
            if (doc["temp_reduce"].is<int>())   gSafety.temp_reduce_c   = doc["temp_reduce"];
            if (doc["temp_shutdown"].is<int>()) gSafety.temp_shutdown_c = doc["temp_shutdown"];
            if (doc["fan_min_pct"].is<int>())   gSafety.fan_min_pct     = doc["fan_min_pct"];
            if (doc["fan_auto"].is<bool>())     gSafety.fan_auto        = doc["fan_auto"];
            // Safety-Config in NVS save
            Preferences p; p.begin("laser", false);
            p.putUChar("t_warn",  gSafety.temp_warn_c);
            p.putUChar("t_red",   gSafety.temp_reduce_c);
            p.putUChar("t_shut",  gSafety.temp_shutdown_c);
            p.putUChar("fan_min", gSafety.fan_min_pct);
            p.putBool ("fan_auto",gSafety.fan_auto);
            p.end();
            req->send(200, "text/plain", "OK");
        });

    // ── Feature 7: DMX-Startadresse (persistent) ──────────────
    // dmx_address is already in gConfig and set in /api/config.
    // Beim SET is es sofort in NVS saved.
    // Endpoint for explicit read/write:
    s_server.on("/api/dmx/address", HTTP_GET, [](AsyncWebServerRequest* req) {
        JsonDocument doc(&jsonAllocator());
        doc["dmx_address"]     = gConfig.dmx_address;
        doc["artnet_universe"] = gConfig.artnet_universe;
        sendJsonPsram(req, doc);
    });
    s_server.on("/api/dmx/address", HTTP_POST,
        [](AsyncWebServerRequest* req){},
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            JsonDocument doc(&jsonAllocator());
            if (deserializeJson(doc, data, len)) { req->send(400); return; }
            if (doc["dmx_address"].is<int>()) {
                gConfig.dmx_address = constrain((int)doc["dmx_address"], 1, 512);
            }
            if (doc["artnet_universe"].is<int>()) {
                gConfig.artnet_universe = doc["artnet_universe"];
            }
            // Immediate persistent save (feature 7: last value retained)
            Preferences p; p.begin("laser", false);
            p.putUShort("dmx_addr",   gConfig.dmx_address);
            p.putUShort("artnet_uni", gConfig.artnet_universe);
            p.end();
            req->send(200, "application/json",
                String("{\"dmx_address\":") + gConfig.dmx_address +
                ",\"artnet_universe\":" + gConfig.artnet_universe + "}");
        });

    // ── Feature 8: Art-Net Status ──────────────────────────────
    // /api/safety-override — simple boolean toggle for UI Safety Override card
    // ── Countdown Timer API ────────────────────────────────────────────
    s_server.on("/api/timer/set", HTTP_POST, [](AsyncWebServerRequest* req){},
        nullptr, [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            JsonDocument doc(&jsonAllocator());
            if (deserializeJson(doc, data, len) != DeserializationError::Ok)
                return req->send(400);
            uint32_t secs = doc["seconds"] | 0u;
            countdown_timer::set(secs);
            req->send(200,"application/json","{\"ok\":true}");
        });
    s_server.on("/api/timer/start", HTTP_POST,
        [](AsyncWebServerRequest* r){ countdown_timer::start(); r->send(200,"application/json","{\"ok\":true}"); });
    s_server.on("/api/timer/pause", HTTP_POST,
        [](AsyncWebServerRequest* r){ countdown_timer::pause(); r->send(200,"application/json","{\"ok\":true}"); });
    s_server.on("/api/timer/stop",  HTTP_POST,
        [](AsyncWebServerRequest* r){ countdown_timer::stop();  r->send(200,"application/json","{\"ok\":true}"); });
    s_server.on("/api/timer/reset", HTTP_POST, [](AsyncWebServerRequest* req){},
        nullptr, [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            JsonDocument doc(&jsonAllocator());
            if (deserializeJson(doc, data, len) == DeserializationError::Ok)
                countdown_timer::reset(doc["seconds"] | countdown_timer::remaining());
            req->send(200,"application/json","{\"ok\":true}");
        });
    s_server.on("/api/timer/state", HTTP_GET, [](AsyncWebServerRequest* req) {
        char buf[128];
        snprintf(buf, sizeof(buf),
            "{\"remaining\":%lu,\"running\":%s,\"expired\":%s}",
            (unsigned long)countdown_timer::remaining(),
            countdown_timer::running() ? "true" : "false",
            countdown_timer::expired() ? "true" : "false");
        req->send(200, "application/json", buf);
    });

    s_server.on("/api/safety-override", HTTP_POST, [](AsyncWebServerRequest* req){},
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            JsonDocument doc(&jsonAllocator());
            if (deserializeJson(doc, data, len) == DeserializationError::Ok) {
                bool en = doc["enabled"] | false;
                gConfig.safety_override = en;
                LOG_W(logbuf::CAT_SAFETY, "Safety override %s via WebUI",
                      en ? "ENABLED" : "disabled");
                req->send(200, "application/json", en ? "{\"ok\":true,\"enabled\":true}"
                                                      : "{\"ok\":true,\"enabled\":false}");
            } else {
                req->send(400, "application/json", "{\"error\":\"bad json\"}");
            }
        });

    s_server.on("/api/artnet/status", HTTP_GET, [](AsyncWebServerRequest* req) {
        JsonDocument doc(&jsonAllocator());
        doc["enabled"]         = true;
        doc["universe"]        = gConfig.artnet_universe;
        doc["dmx_address"]     = gConfig.dmx_address;
        doc["etherdream_connected"] = etherdream::isConnected();
        doc["etherdream_playing"]   = etherdream::isPlaying();
        sendJsonPsram(req, doc);
    });

    // ── Feature 4: Playlist ───────────────────────────────────
    s_server.on("/api/playlist", HTTP_GET, [](AsyncWebServerRequest* req) {
        JsonDocument doc(&jsonAllocator());
        doc["active"]   = gPlaylist.active;
        doc["count"]    = gPlaylist.count;
        doc["current"]  = gPlaylist.current;
        doc["loop_all"] = gPlaylist.loop_all;
        JsonArray arr = doc["entries"].to<JsonArray>();
        for (uint8_t i = 0; i < gPlaylist.count; i++) {
            JsonObject e = arr.add<JsonObject>();
            e["file_idx"]   = gPlaylist.entries[i].file_idx;
            e["loop_count"] = gPlaylist.entries[i].loop_count;
            e["pause_ms"]   = gPlaylist.entries[i].pause_ms;
            if (sd_card::fileName(gPlaylist.entries[i].file_idx))
                e["name"] = sd_card::fileName(gPlaylist.entries[i].file_idx);
        }
        sendJsonPsram(req, doc);
    });
    s_server.on("/api/playlist/start", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            playlist::start();
            req->send(200, "text/plain", "OK");
        });
    s_server.on("/api/playlist/stop", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            playlist::stop();
            req->send(200, "text/plain", "OK");
        });
    s_server.on("/api/playlist/reload", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            bool ok = playlist::loadFromSD();
            req->send(ok ? 200 : 404, "text/plain", ok ? "OK" : "no playlist.json");
        });
    s_server.on("/api/playlist", HTTP_POST,
        [](AsyncWebServerRequest* req){},
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            JsonDocument doc(&jsonAllocator());
            if (deserializeJson(doc, data, len)) { req->send(400); return; }
            gPlaylist.count = 0;
            JsonArray arr = doc["entries"].as<JsonArray>();
            for (JsonObject e : arr) {
                if (gPlaylist.count >= PLAYLIST_MAX_ENTRIES) break;
                gPlaylist.entries[gPlaylist.count].file_idx   = e["file_idx"]   | 0;
                gPlaylist.entries[gPlaylist.count].loop_count = e["loop_count"] | 1;
                gPlaylist.entries[gPlaylist.count].pause_ms   = e["pause_ms"]   | 0;
                gPlaylist.count++;
            }
            gPlaylist.loop_all = doc["loop_all"] | true;
            req->send(200, "application/json",
                String("{\"count\":") + gPlaylist.count + "}");
        });

    // ── Feature 11: ILDA file upload via HTTP ────────────────
    s_server.on("/api/ilda/upload", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            bool ok = !req->hasParam("error");
            req->send(ok ? 200 : 500, "application/json",
                ok ? "{\"status\":\"ok\",\"rescan\":true}" : "{\"error\":\"upload failed\"}");
            if (ok) sd_card::scanFiles();  // SD neu scannen
        },
        [](AsyncWebServerRequest* req, String filename, size_t index,
           uint8_t* data, size_t len, bool final) {
            static File s_upload_file;
            if (index == 0) {
                String path = "/ilda/" + filename;
                ESP_LOGI("upload", "Start: %s", path.c_str());
                { LOCK_SD(); s_upload_file = SD.open(path, FILE_WRITE); }
                if (!s_upload_file) {
                    ESP_LOGE("upload", "could not create file");
                    req->params();  // setze error-flag
                }
            }
            if (s_upload_file && len)
                s_upload_file.write(data, len);
            if (final && s_upload_file) {
                s_upload_file.close();
                ESP_LOGI("upload", "Done: %s (%u bytes)", filename.c_str(), index+len);
            }
        });

    // /api/log and /api/log/clear are registered below with full pagination support


    // ---- GET /api/wifi-scan ----
    // Gibt {status:"scanning"} or {status:"done", networks:[...]} zurueck
    s_server.on("/api/wifi-scan", HTTP_GET, [](AsyncWebServerRequest* req) {
        JsonDocument doc(&jsonAllocator());
        if (s_scan_running) {
            doc["status"] = "scanning";
            sendJsonPsram(req, doc);
            return;
        }
        int n = s_scan_results;
        if (n <= 0) {
            // No scan yet -- start immediately
            WiFi.scanDelete();
            xTaskCreatePinnedToCore(wifiScanTask, "wifi_scan", 4096, nullptr, 2, nullptr, 0);
            doc["status"] = "scanning";
            sendJsonPsram(req, doc);
            return;
        }
        // Ergebnisse liefern
        doc["status"] = "done";
        JsonArray arr = doc["networks"].to<JsonArray>();
        for (int i = 0; i < n; i++) {
            JsonObject net = arr.add<JsonObject>();
            net["ssid"]    = WiFi.SSID(i);
            net["rssi"]    = WiFi.RSSI(i);
            net["secure"]  = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
            net["channel"] = WiFi.channel(i);
        }
        WiFi.scanDelete();
        s_scan_results = 0;
        sendJsonPsram(req, doc);
    });

    // ---- POST /api/wifi-scan (neuen Scan erzwingen) ----
    s_server.on("/api/wifi-scan", HTTP_POST,
        [](AsyncWebServerRequest* req) {},
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t*, size_t, size_t, size_t) {
            if (!s_scan_running) {
                WiFi.scanDelete();
                s_scan_results = 0;
                xTaskCreatePinnedToCore(wifiScanTask, "wifi_scan", 4096, nullptr, 2, nullptr, 0);
            }
            req->send(202, "text/plain", "scan started");
        });

    // ---- GET /api/wifi-status ----
    s_server.on("/api/wifi-status", HTTP_GET, [](AsyncWebServerRequest* req) {
        JsonDocument doc(&jsonAllocator());
        doc["connected"] = (WiFi.status() == WL_CONNECTED);
        doc["ssid"]      = WiFi.SSID();
        doc["ip"]        = WiFi.localIP().toString();
        doc["rssi"]      = WiFi.RSSI();
        doc["mode"]      = gConfig.wifi_static ? "static" : "dhcp";
        sendJsonPsram(req, doc);
    });

    // ---- POST /api/wifi-connect ----
    s_server.on("/api/wifi-connect", HTTP_POST,
        [](AsyncWebServerRequest* req) {},
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            JsonDocument doc(&jsonAllocator());
            if (deserializeJson(doc, data, len)) { req->send(400, "text/plain", "bad json"); return; }
            const char* ssid = doc["ssid"] | "";
            const char* pass = doc["pass"] | "";
            if (strlen(ssid) == 0) { req->send(400, "text/plain", "ssid required"); return; }
            strlcpy(gConfig.wifi_ssid, ssid, sizeof(gConfig.wifi_ssid));
            strlcpy(gConfig.wifi_pass, pass, sizeof(gConfig.wifi_pass));
            persistConfig();
            WiFi.disconnect();
            delay(100);
            WiFi.begin(ssid, pass);
            req->send(200, "text/plain", "connecting");
        });

    // ---- POST /api/reboot ----
    s_server.on("/api/log", HTTP_GET, [](AsyncWebServerRequest* req) {
        uint32_t after_ts  = 0;
        size_t   max_ent   = 200;
        if (req->hasParam("after")) after_ts = (uint32_t)atol(req->getParam("after")->value().c_str());
        if (req->hasParam("max"))   max_ent  = (size_t)  atoi(req->getParam("max")->value().c_str());
        if (max_ent > 500) max_ent = 500;

        // PSRAM buffer -- up to ~120 KB for 500 entries, must stay off internal heap.
        // shared_ptr deleter frees it whether the chunked response completes
        // normally or the client aborts mid-stream (same leak class as the
        // pre-5.21.0 /api/paint/set String leak).
        size_t buf_len = max_ent * 220 + 32;
        std::shared_ptr<char> buf(
            (char*)heap_caps_malloc(buf_len, MALLOC_CAP_SPIRAM),
            [](char* p) { heap_caps_free(p); });
        if (!buf) { req->send(503, "text/plain", "OOM"); return; }

        logbuf::toJson(buf.get(), buf_len, after_ts, max_ent);
        size_t json_len = strlen(buf.get());

        AsyncWebServerResponse* resp = req->beginChunkedResponse(
            "application/json",
            [buf, json_len](uint8_t* out, size_t maxLen, size_t index) -> size_t {
                if (index >= json_len) return 0;
                size_t n = std::min(maxLen, json_len - index);
                memcpy(out, buf.get() + index, n);
                return n;
            });
        req->send(resp);
    });

    // ---- POST /api/log/clear ----
    s_server.on("/api/log/clear", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            logbuf::clear();
            LOG_I(logbuf::CAT_USER, "Log cleared by browser");
            req->send(200, "text/plain", "cleared");
        });

    // ---- GET /api/log/stats ----
    s_server.on("/api/log/stats", HTTP_GET, [](AsyncWebServerRequest* req) {
        char buf[80];
        snprintf(buf, sizeof(buf),
                 "{\"count\":%u,\"capacity\":%u,\"full\":%s}",
                 (unsigned)logbuf::count(),
                 (unsigned)logbuf::LOG_CAPACITY,
                 logbuf::isFull() ? "true" : "false");
        req->send(200, "application/json", buf);
    });

    // ---- POST /api/factory-reset ----
    s_server.on("/api/factory-reset", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            s_prefs.begin("laser", false); s_prefs.clear(); s_prefs.end();
            req->send(200, "text/plain", "reset"); delay(500); ESP.restart();
        });

    // WebSocket removed in 5.34.0 — unused (state is polled via /api/state).
    // Idle Chrome kept a /ws client open whose AsyncTCP framebuffers sat on
    // internal DRAM, and the onerror->close->reconnect loop leaked a fresh
    // client each round -> HEAP_CRITICAL. No server-side WS producer existed.

    // ═══ POST /api/debug/hw — Hardware debug: Galvo + Laser direkt setzen ════
    // Allowed: laser_armed=true OR gDebugNoHW=true
    // Body JSON: {x, y, r, g, b}  — x/y: -32767..32767, r/g/b: 0..255
    // Sonderbefehle: {cmd:"center"}, {cmd:"off"}, {cmd:"sweep_x"}, {cmd:"sweep_y"}
    //               {cmd:"corners"} -- move to all 4 corners in sequence
    s_server.on("/api/debug/hw", HTTP_POST,
        [](AsyncWebServerRequest* req) {},
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            if (!gState.laser_armed.load() && !gDebugNoHW) {
                req->send(403, "application/json",
                    "{\"error\":\"Laser not armed and no debug mode\"}");
                return;
            }
            JsonDocument doc(&jsonAllocator());
            if (deserializeJson(doc, data, len)) {
                req->send(400, "application/json", "{\"error\":\"JSON invalid\"}");
                return;
            }

            // Sonder-Kommandos
            if (doc["cmd"].is<const char*>()) {
                const char* cmd = doc["cmd"];
                if (strcmp(cmd, "off") == 0) {
                    galvo::clearDebugOutput();
                    req->send(200, "application/json", "{\"ok\":true,\"cmd\":\"off\"}");
                    return;
                }
                if (strcmp(cmd, "exit") == 0) {
                    galvo::clearDebugOutput();
                    req->send(200, "application/json", "{\"ok\":true,\"cmd\":\"exit\"}");
                    return;
                }
                if (strcmp(cmd, "center") == 0) {
                    galvo::setDebugOutput(0, 0, 0, 0, 0);
                    req->send(200, "application/json", "{\"ok\":true,\"cmd\":\"center\"}");
                    return;
                }
                // Simple sweep commands: injected as preset into pattern engine
                // Here only return as note (pattern task handles sweep)
                req->send(200, "application/json",
                    "{\"ok\":true,\"info\":\"Sweep via pattern_engine — verwende /api/preset\"}");
                return;
            }

            // Direkte X/Y/R/G/B valuee
            int16_t x = doc["x"] | 0;
            int16_t y = doc["y"] | 0;
            uint8_t r = doc["r"] | 0;
            uint8_t g2= doc["g"] | 0;
            uint8_t b = doc["b"] | 0;
            // Grenzen sichern
            x = (int16_t)constrain((int)x, -32767, 32767);
            y = (int16_t)constrain((int)y, -32767, 32767);
            galvo::setDebugOutput(x, y, r, g2, b);

            char buf[128];
            snprintf(buf, sizeof(buf),
                "{\"ok\":true,\"x\":%d,\"y\":%d,\"r\":%u,\"g\":%u,\"b\":%u}",
                x, y, r, g2, b);
            req->send(200, "application/json", buf);
        });

    // ═══ GET /api/debug/hw — aktuellen Debug-Zustand abrufen ════════════
    s_server.on("/api/debug/hw", HTTP_GET, [](AsyncWebServerRequest* req) {
        char buf[160];
        snprintf(buf, sizeof(buf),
            "{\"active\":%d,\"armed\":%d,\"debug_mode\":%d}",
            (int)galvo::isDebugOutputActive(),
            (int)gState.laser_armed.load(),
            (int)gDebugNoHW);
        req->send(200, "application/json", buf);
    });

    // ═══ POST /api/debug/dac-cmd — raw DAC8562 command / hold-value test ═
    // Requires laser_armed (same guard as /api/debug/hw).
    // Body: {"op":"reset"}                          -- software reset (full)
    //       {"op":"powerup"}                        -- clear power-down on both channels
    //       {"op":"hold","ch":0|1,"code":0..65535,"ms":1..60000}
    s_server.on("/api/debug/dac-cmd", HTTP_POST,
        [](AsyncWebServerRequest* req) {},
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            if (!gState.laser_armed.load() && !gDebugNoHW) {
                req->send(403, "application/json",
                    "{\"error\":\"laser not armed\"}");
                return;
            }
            JsonDocument doc(&jsonAllocator());
            if (deserializeJson(doc, data, len)) {
                req->send(400, "application/json", "{\"error\":\"bad json\"}");
                return;
            }
            const char* op = doc["op"] | "";
            bool ok = false;

            if (strcmp(op, "reset") == 0) {
                // CMD=101, DB0=1 -> full reset (all registers to power-on defaults)
                ok = galvo::sendRawCommand(0b101, 0b000, 0x0001);
            } else if (strcmp(op, "powerup") == 0) {
                // CMD=100, ADDR=111 (both channels), DB5/DB4=00 (normal mode),
                // DB1/DB0=11 (apply to DAC-A and DAC-B)
                ok = galvo::sendRawCommand(0b100, 0b111, 0x0003);
            } else if (strcmp(op, "hold") == 0) {
                int ch    = doc["ch"]   | 0;
                int code  = doc["code"] | 0x8000;
                int ms    = doc["ms"]   | 2000;
                ch   = constrain(ch, 0, 1);
                code = constrain(code, 0, 65535);
                ms   = constrain(ms, 1, 60000);
                galvo::holdChannelValue((uint8_t)ch, (uint16_t)code, (uint32_t)ms);
                ok = true;
            } else {
                req->send(400, "application/json", "{\"error\":\"unknown op\"}");
                return;
            }

            req->send(ok ? 200 : 500, "application/json",
                ok ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"SPI transfer failed\"}");
        });

    // ---- GET /api/status ---- dashboard state (browser polls every 1-2s) ----
    s_server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest* req) {
        // No JsonDocument/serializeJson — direct sprintf saves heap + CPU
        static char buf[512];
        // OTA password for dashboard (chip-ID based)
        char ota_pw[12];
        snprintf(ota_pw, sizeof(ota_pw), "%08X",
                 (uint32_t)(ESP.getEfuseMac() >> 16));
        char fw_ver[32];
        snprintf(fw_ver, sizeof(fw_ver), "%s", LASER_FW_VERSION);
        // Network info
        String ip_str   = WiFi.localIP().toString();
        String host_str = WiFi.getHostname() ? WiFi.getHostname() : gConfig.hostname;
        int32_t rssi    = WiFi.RSSI();
        uint32_t uptime = millis() / 1000;
        // DMX last activity age
        uint32_t dmx_age = gState.last_dmx_ms.load()
                         ? (millis() - gState.last_dmx_ms.load()) : 0xFFFFFFFF;

        snprintf(buf, sizeof(buf),
            "{\"estop_ok\":%d,\"scanfail_ok\":%d,\"laser_armed\":%d,"
            "\"source\":%d,\"master_dimmer\":%d,\"points_per_sec\":%lu,"
            "\"buffer_fill\":%d,\"debug_mode\":%d,"
            "\"ui_override\":%d,\"ui_master_dimmer\":%d,"
            "\"fw_version\":\"%s\",\"ota_pass\":\"%s\","
            "\"free_heap\":%u,\"free_psram\":%u,"
            "\"hostname\":\"%s\",\"ip\":\"%s\","
            "\"rssi\":%d,\"uptime_s\":%u,"
            "\"last_dmx_age_ms\":%u}",
            (int)gState.estop_ok.load(),
            (int)gState.scanfail_ok.load(),
            (int)gState.laser_armed.load(),
            (int)gState.source.load(),
            (int)gState.master_dimmer.load(),
            (unsigned long)galvo::pointsPerSec(),
            (int)galvo::bufferFillLevel(),
            (int)gDebugNoHW,
            (int)gState.ui_override.load(),
            (int)gState.ui_master_dimmer.load(),
            fw_ver, ota_pw,
            (unsigned)ESP.getFreeHeap(),
            (unsigned)ESP.getFreePsram(),
            host_str.c_str(), ip_str.c_str(),
            (int)rssi, (unsigned)uptime,
            (unsigned)dmx_age);
        req->send(200, "application/json", buf);
    });

    // ---- POST /api/debug-mode ---- Safety-Bypass for Hardware-freien Test ----
    s_server.on("/api/debug-mode", HTTP_POST,
        [](AsyncWebServerRequest* req) {},
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            JsonDocument doc(&jsonAllocator());
            if (deserializeJson(doc, data, len)) { req->send(400, "text/plain", "bad json"); return; }
            bool enabled = doc["enabled"] | false;
            gDebugNoHW = enabled;
            // NVS persistent save (ueberlebt Reboot)
            { Preferences p; p.begin("laser",false);
              p.putBool("dbg_nohw", enabled); p.end(); }
            if (enabled) {
                gState.master_dimmer.store(200);
                gState.laser_armed.store(true);
                gState.estop_ok.store(true);
                gState.scanfail_ok.store(true);
                gOverride.active = true;
                gOverride.values[DMX_MASTER] = 200;
                ESP_LOGW("web", "DEBUG NO-HW MODE ON");
            } else {
                gOverride.active = false;
                gState.master_dimmer.store(0);
                gState.laser_armed.store(false);
                ESP_LOGI("web", "Debug mode OFF");
            }
            req->send(200, "application/json", enabled ? "{\"debug\":true}" : "{\"debug\":false}");
        });

    // Static files: registered right before begin() (see fix below) --
    // routes defined after this point used to be shadowed by serveStatic's
    // catch-all GET/HEAD match (e.g. GET /api/projection), same class of
    // ESPAsyncWebServer ordering bug as /api/text vs /api/text/vertices.
    // 404 handler: do not forward API paths to LittleFS
    s_server.onNotFound([](AsyncWebServerRequest* req) {
        // API-Pfade: JSON-Error instead of HTML-404
        if (req->url().startsWith("/api/")) {
            req->send(404, "application/json", "{\"error\":\"not found\"}");
        } else {
            req->send(404, "text/plain", "Not found");
        }
    });

    // ── /api/temp/name — rename sensor ──────────────────────────────────────

    // ---- POST /api/ui-control ---- UI Override + Master Dimmer ----
    // body: {"ui_override": true/false, "master_dimmer": 0-255}
    // ui_override=true: WebUI takes priority over DMX/Art-Net
    // master_dimmer>0: forces dimmer globally (overrides DMX CH1)
    // master_dimmer=0: follow DMX CH1 as normal
    s_server.on("/api/ui-control", HTTP_POST,
        [](AsyncWebServerRequest* req) {},
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            if (!isAuthorised(req)) { denyUnauth(req); return; }
            JsonDocument doc(&jsonAllocator());
            if (deserializeJson(doc, data, len)) { req->send(400, "text/plain", "bad json"); return; }
            bool ui_override   = doc["ui_override"] | false;
            uint8_t master_dim = (uint8_t)(doc["master_dimmer"] | 0);
            bool prev_override = gState.ui_override.load();
            gState.ui_override.store(ui_override);
            gState.ui_master_dimmer.store(master_dim);
            // Auto-exit HW debug mode only on rising edge of ui_override
            if (ui_override && !prev_override) {
                galvo::clearDebugOutput();
                patterns::stopTestPattern();
            }
            JsonDocument resp(&jsonAllocator());
            resp["ui_override"]    = ui_override;
            resp["master_dimmer"]  = master_dim;
            String out;
            serializeJson(resp, out);
            req->send(200, "application/json", out);
        }
    );

    // ---- POST /api/temp/offset ---- calibration offset per sensor ----
    s_server.on("/api/temp/offset", HTTP_POST, [](AsyncWebServerRequest* req){},
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            if (!isAuthorised(req)) { denyUnauth(req); return; }
            JsonDocument doc(&jsonAllocator());
            if (deserializeJson(doc, data, len) != DeserializationError::Ok)
                return req->send(400);
            for (uint8_t i = 0; i < temp::NUM_SENSORS; i++) {
                char key[8]; snprintf(key, sizeof(key), "s%u", i);
                if (doc[key].is<float>() || doc[key].is<int>())
                    temp::setSensorOffset(i, doc[key].as<float>());
            }
            req->send(200, "application/json", "{\"ok\":true}");
        });

    s_server.on("/api/temp/name", HTTP_POST, [](AsyncWebServerRequest* req){},
    nullptr,
    [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
        JsonDocument doc(&jsonAllocator());
        if (deserializeJson(doc, data, len) != DeserializationError::Ok) {
            req->send(400, "application/json", "{\"error\":\"bad json\"}");
            return;
        }
        uint8_t idx   = doc["idx"]  | 255;
        const char* n = doc["name"] | "";
        if (idx < temp::NUM_SENSORS && strlen(n) > 0 && strlen(n) < temp::SENSOR_NAME_LEN) {
            temp::setSensorName(idx, n);
            req->send(200, "application/json", "{\"ok\":true}");
        } else {
            req->send(400, "application/json", "{\"error\":\"invalid\"}");
        }
    });

    // ── /api/projection GET ── galvo rate + projection geometry ─────────────
    s_server.on("/api/projection", HTTP_GET, [](AsyncWebServerRequest* req) {
        char buf[512];
        // Calculate derived values
        float kpps       = (float)gProjection.galvo_kpps;
        float angle_rad  = gProjection.exit_angle_deg * (float)M_PI / 180.0f;
        float dist_m     = gProjection.distance_m;

        // Image size at projection distance
        float img_w_m    = 2.0f * tanf(angle_rad) * dist_m;
        float img_h_m    = img_w_m;

        // Max safe kpps for current exit angle (ILDA rated at ilda_test_angle)
        float rated_angle = gProjection.ilda_test_angle_deg;
        float cur_angle   = gProjection.exit_angle_deg;
        float rated_kpps  = 15.0f;
        float max_safe_kpps = (cur_angle <= rated_angle) ? 60.0f
                            : rated_kpps * (rated_angle / cur_angle);
        if (max_safe_kpps > 60.0f) max_safe_kpps = 60.0f;

        bool ne555_ok = (kpps >= 5.0f);

        // Per-channel and combined power
        float total_mw   = gProjection.totalPowerMw();
        float vis_mw     = gProjection.visPowerMw();
        float blhaz_mw   = gProjection.blueLightHazardMw();

        // Auto white balance gains
        uint8_t awb_r, awb_g, awb_b;
        gProjection.autoWhiteBalance(awb_r, awb_g, awb_b);

        // Safety: irradiance based on total (worst-case) power over scan area
        float area_cm2   = (img_w_m * 100.0f) * (img_h_m * 100.0f);
        float irr_mw_cm2 = (area_cm2 > 0.1f) ? (total_mw / area_cm2) : 999.f;
        float min_dist_m = (irr_mw_cm2 > 1.0f)
            ? dist_m * sqrtf(irr_mw_cm2 / 1.0f) : 0.0f;
        // Blue-light weighted irradiance (B(λ) factor, photochemical)
        float blhaz_irr  = (area_cm2 > 0.1f) ? (blhaz_mw / area_cm2) : 999.f;

        snprintf(buf, sizeof(buf),
            "{"
            "\"kpps\":%u,"
            "\"scan_angle_mech\":%.1f,"
            "\"exit_angle\":%.1f,"
            "\"ilda_test_angle\":%.1f,"
            "\"power_r_mw\":%.0f,"
            "\"power_g_mw\":%.0f,"
            "\"power_b_mw\":%.0f,"
            "\"total_mw\":%.0f,"
            "\"vis_mw\":%.0f,"
            "\"blhaz_mw\":%.1f,"
            "\"awb_r\":%u,\"awb_g\":%u,\"awb_b\":%u,"
            "\"distance_m\":%.2f,"
            "\"img_w_m\":%.2f,"
            "\"img_h_m\":%.2f,"
            "\"max_safe_kpps\":%.1f,"
            "\"ne555_ok\":%s,"
            "\"irr_mw_cm2\":%.3f,"
            "\"blhaz_irr\":%.3f,"
            "\"min_dist_m\":%.1f"
            "}",
            (unsigned)gProjection.galvo_kpps,
            gProjection.scan_angle_mech_deg,
            gProjection.exit_angle_deg,
            gProjection.ilda_test_angle_deg,
            gProjection.power_r_mw,
            gProjection.power_g_mw,
            gProjection.power_b_mw,
            total_mw, vis_mw, blhaz_mw,
            (unsigned)awb_r, (unsigned)awb_g, (unsigned)awb_b,
            gProjection.distance_m,
            img_w_m, img_h_m,
            max_safe_kpps,
            ne555_ok ? "true" : "false",
            irr_mw_cm2, blhaz_irr,
            min_dist_m
        );
        req->send(200, "application/json", buf);
    });

    // ── /api/projection POST ── update galvo rate + geometry ────────────────
    s_server.on("/api/projection", HTTP_POST, [](AsyncWebServerRequest* req){},
    nullptr,
    [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
        JsonDocument doc(&jsonAllocator());
        if (deserializeJson(doc, data, len) != DeserializationError::Ok) {
            req->send(400, "application/json", "{\"error\":\"bad json\"}");
            return;
        }
        bool changed = false;
        if (!doc["kpps"].isNull()) {
            uint16_t v = doc["kpps"];
            if (v >= 12 && v <= 60) { gProjection.galvo_kpps = v; changed = true; }
        }
        if (!doc["scan_angle_mech"].isNull()) {
            float v = doc["scan_angle_mech"];
            if (v > 0 && v <= 45) { gProjection.scan_angle_mech_deg = v; changed = true; }
        }
        if (!doc["exit_angle"].isNull()) {
            float v = doc["exit_angle"];
            if (v > 0 && v <= 45) { gProjection.exit_angle_deg = v; changed = true; }
        }
        if (!doc["ilda_test_angle"].isNull()) {
            float v = doc["ilda_test_angle"];
            if (v > 0 && v <= 20) { gProjection.ilda_test_angle_deg = v; changed = true; }
        }
        if (!doc["power_r_mw"].isNull()) {
            float v = doc["power_r_mw"];
            if (v >= 0 && v <= 10000) { gProjection.power_r_mw = v; changed = true; }
        }
        if (!doc["power_g_mw"].isNull()) {
            float v = doc["power_g_mw"];
            if (v >= 0 && v <= 10000) { gProjection.power_g_mw = v; changed = true; }
        }
        if (!doc["power_b_mw"].isNull()) {
            float v = doc["power_b_mw"];
            if (v >= 0 && v <= 10000) { gProjection.power_b_mw = v; changed = true; }
        }
        if (!doc["distance_m"].isNull()) {
            float v = doc["distance_m"];
            if (v > 0.1f && v <= 100) { gProjection.distance_m = v; changed = true; }
        }
        if (changed) {
            // Persist to NVS
            Preferences prefs;
            prefs.begin("projection", false);
            prefs.putUShort("kpps",    gProjection.galvo_kpps);
            prefs.putFloat("scan_ang", gProjection.scan_angle_mech_deg);
            prefs.putFloat("exit_ang", gProjection.exit_angle_deg);
            prefs.putFloat("ilda_ang", gProjection.ilda_test_angle_deg);
            prefs.putFloat("pwr_r",    gProjection.power_r_mw);
            prefs.putFloat("pwr_g",    gProjection.power_g_mw);
            prefs.putFloat("pwr_b",    gProjection.power_b_mw);
            prefs.putFloat("dist_m",   gProjection.distance_m);
            prefs.end();
            // Log power summary
            float vis = gProjection.visPowerMw();
            float blh = gProjection.blueLightHazardMw();
            ESP_LOGI("webui", "Projection: R=%.0fmW G=%.0fmW B=%.0fmW -> vis=%.0fmW_vis BLH=%.1fmW",
                     gProjection.power_r_mw, gProjection.power_g_mw, gProjection.power_b_mw,
                     vis, blh);
            ESP_LOGI("webui", "Projection config updated: %u kpps, exit=%.1f deg, dist=%.1f m",
                     (unsigned)gProjection.galvo_kpps,
                     gProjection.exit_angle_deg, gProjection.distance_m);
        }
        req->send(200, "application/json", "{\"ok\":true}");
    });


    // ── /api/projection/awb — apply auto white balance from laser power specs ─
    s_server.on("/api/projection/awb", HTTP_POST, [](AsyncWebServerRequest* req) {
        uint8_t gr, gg, gb;
        gProjection.autoWhiteBalance(gr, gg, gb);
        if (xSemaphoreTake(mtx::config, pdMS_TO_TICKS(10)) == pdTRUE) {
            gConfig.gain_r = gr;
            gConfig.gain_g = gg;
            gConfig.gain_b = gb;
            xSemaphoreGive(mtx::config);
        }
        // Persist to NVS (re-use config save)
        Preferences prefs;
        prefs.begin("config", false);
        prefs.putUChar("gain_r", gr);
        prefs.putUChar("gain_g", gg);
        prefs.putUChar("gain_b", gb);
        prefs.end();
        char buf[80];
        snprintf(buf, sizeof(buf),
            "{\"ok\":true,\"gain_r\":%u,\"gain_g\":%u,\"gain_b\":%u}",
            (unsigned)gr, (unsigned)gg, (unsigned)gb);
        ESP_LOGI("webui", "Auto white balance applied: R=%u G=%u B=%u", gr, gg, gb);
        req->send(200, "application/json", buf);
    });

    // Cache-Control lets the browser reuse the gzipped index.html from its own
    // cache instead of re-fetching ~106 KB on every load/refresh. The re-fetch
    // was the last remaining internal-DRAM spike: the LittleFS .gz read plus
    // the concurrent lwIP TX buffers drove `largest` down near the failsafe
    // limit on cold loads. With a one-hour max-age, only the very first load
    // pays that cost; refreshes and revisits are served from browser cache and
    // never touch the device heap.
    s_server.serveStatic("/", LittleFS, "/")
        .setDefaultFile("index.html")
        .setCacheControl("max-age=3600");
    s_server.begin();
    ESP_LOGI(TAG, "WebUI at http://%s/", WiFi.localIP().toString().c_str());
}

/* ============================================================
 * Task: WS client housekeeping + CPU monitor
 * ============================================================ */
void task(void*) {
    // State updates run via HTTP /api/status (browser polls every 1s).
    // This greatly reduces core 0 load: no JSON serialization in the task loop.
    for (;;) {
        // (WebSocket removed — state served via /api/state poll)
        cpu_mon::update();             // has internal 500ms rate-limit
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
}  // namespace web_ui
