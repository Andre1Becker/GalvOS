#include "web_ui.h"
#include "config.h"
#include "../warpGrid.h"
#include "../brightnessField.h"
#include "../inverseFilter.h"
#include "safety/safety.h"
#include "output/galvo_out.h"
#include "patterns/pattern_engine.h"
#include "patterns/curve_patterns.h"
#include "patterns/preset_patterns.h"
#include "patterns/countdown_timer.h"
#include "patterns/text_renderer.h"
#include "ilda/ilda_player.h"
#include "patterns/calib_patterns.h"
#include "patterns/point_optimizer.h"
#include "patterns/weld_patterns.h"
#include "mutex.h"
#include "storage/playlist.h"
#include "net/ota_update.h"
#include "net/etherdream.h"
#include "net/helios_net.h"
#include "net/osc_in.h"
#include "net/sacn_in.h"
#include "storage/sd_card.h"
#include "storage/svg_store.h"
#include "pinmap.h"
#include "sensors/temp_monitor.h"
#include "util/log_buffer.h"
#include "util/cpu_monitor.h"
#include "util/stack_mon.h"
#include "util/mem_registry.h"
#include "net/ntp_client.h"
#include "net/backup_manager.h"
#include "bpm_clock.h"
#include "../sequencer.h"
#include "../modulator_engine.h"
#include "../patterns/camera.h"
#include "../patterns/duplicator.h"
#include "../patterns/spatial_noise.h"
#include "../patterns/dotter.h"
#include "net/community_presets.h"
#include "patterns/preset_patterns.h"
#include "patterns/countdown_timer.h"
#include <Arduino.h>
#include <math.h>
#include <string.h>
#include <ctype.h>
#include <memory>
#include <atomic>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <ArduinoJson.h>
#include "json_alloc.h"
#include <LittleFS.h>
#include <SD.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_heap_caps.h>

AsyncWebServer s_server(80);  // file-scope: ota_update sieht es

namespace web_ui {

static const char* TAG = "web";

// ── Concurrent-request counter ────────────────────────────────────────────────
// Diagnostic only: a browser hard-reload opens several parallel connections
// (static assets) plus the SPA immediately firing off its full set of initial
// /api/* GETs -- all sharing the single system-wide CONFIG_LWIP_MAX_SOCKETS=16
// netconn pool (also used by AsyncTCP, etherdream's UDP+TCP sockets, artnet_in,
// ntp_client). If that pool runs dry, unrelated lwIP calls elsewhere (e.g.
// etherdream's UDP beacon sendto()) fail with ENOMEM despite a healthy general
// heap. Logged by etherdream.cpp at the moment of a beacon send failure to
// test that theory against hardware.
static std::atomic<int> s_active_requests{0};

int activeRequests() { return s_active_requests.load(); }

// ── Camera-in-the-loop calibration session (/api/calib-cam/*) ────────────────
// Lets a host-side optimizer (OpenCV + Optuna) select a calib_patterns::
// pattern, override optimizer parameters live, and measure the projection.
// Overrides are RAM-only (never persisted) and scoped to the one optimizer
// profile the active calib-cam pattern runs under; the pre-session values of
// that profile are snapshotted on the first override and restored on stop
// (or force-stopped from pattern_engine::task() the instant E-Stop trips --
// see calibCamForceStop()), so an aborted tuning run can never leave a normal
// preset's optimizer profile altered.
static bool                s_calibcam_active   = false;
static uint8_t             s_calibcam_pat_idx  = 0;      // calib_patterns:: index (11..16)
static uint8_t             s_calibcam_profile  = 0;      // profile owning the session's snapshot
static bool                s_calibcam_has_snap = false;
static OptimizerLiveConfig s_calibcam_snapshot;

// Restores the snapshotted profile (if any) and clears session state. Shared
// by the /stop handler and calibCamForceStop() (E-Stop path) so both go
// through the exact same cleanup.
static void stopCalibCamSession() {
    if (s_calibcam_has_snap) {
        gOptimizerProfiles[s_calibcam_profile] = s_calibcam_snapshot;
        if (s_calibcam_profile == gActiveOptimizerProfile) syncOptimizerConfig();
        gPatternCacheGen++;
        s_calibcam_has_snap = false;
    }
    s_calibcam_active = false;
    gState.calib_active    = false;
    gState.calib_no_thresh = false;
    gState.calib_raw_duty  = false;
    if (gState.master_dimmer.load() == 0) gState.ui_master_dimmer.store(0);
}

bool calibCamActive() { return s_calibcam_active; }
void calibCamForceStop() { stopCalibCamSession(); }

// Applies recognized OptimizerLiveConfig overrides from `src` onto `cfg`,
// clamped to the exact same bounds as /api/optimizer-live (kept in sync by
// hand -- both lists are short and reviewed together). Every applied key is
// echoed into `applied` as its effective (post-clamp) value; every key in
// `src` that isn't a known field name (or is the "profile" selector, handled
// by the caller) is appended to `ignored`. Finally runs
// normalizeOptimizerConfig() (config.h) so a min/max pair left inverted by
// independent per-field clamping -- min_blank_samples > blank_samples,
// min_corner_pts > max_corner_pts -- is corrected and re-echoed into
// `applied` with its effective value.
static void applyOptimizerOverrides(JsonObjectConst src, OptimizerLiveConfig& cfg,
                                     JsonObject applied, JsonArray ignored) {
    for (JsonPairConst kv : src) {
        const char* key = kv.key().c_str();
        if (!strcmp(key, "profile")) continue;  // selector, not a field
        JsonVariantConst val = kv.value();
        if (!strcmp(key, "corner_angle_deg") && val.is<float>()) {
            cfg.corner_angle_deg = constrain((float)val, 0.0f, 180.0f);
            applied["corner_angle_deg"] = cfg.corner_angle_deg;
        } else if (!strcmp(key, "min_corner_pts") && val.is<int>()) {
            cfg.min_corner_pts = constrain((int)val, 1, 20);
            applied["min_corner_pts"] = cfg.min_corner_pts;
        } else if (!strcmp(key, "max_corner_pts") && val.is<int>()) {
            cfg.max_corner_pts = constrain((int)val, 1, 20);
            applied["max_corner_pts"] = cfg.max_corner_pts;
        } else if (!strcmp(key, "pts_per_1000_units") && val.is<float>()) {
            cfg.pts_per_1000_units = constrain((float)val, 0.1f, 50.0f);
            applied["pts_per_1000_units"] = cfg.pts_per_1000_units;
        } else if (!strcmp(key, "blank_samples") && val.is<int>()) {
            cfg.blank_samples = constrain((int)val, 1, 100);
            applied["blank_samples"] = cfg.blank_samples;
        } else if (!strcmp(key, "max_pts_per_frame") && val.is<int>()) {
            cfg.max_pts_per_frame = constrain((int)val, 50, (int)PATTERN_POINTS_MAX);
            applied["max_pts_per_frame"] = cfg.max_pts_per_frame;
        } else if (!strcmp(key, "min_blank_samples") && val.is<int>()) {
            cfg.min_blank_samples = constrain((int)val, 1, 100);
            applied["min_blank_samples"] = cfg.min_blank_samples;
        } else if (!strcmp(key, "blank_pts_per_1000_units") && val.is<float>()) {
            cfg.blank_pts_per_1000_units = constrain((float)val, 0.1f, 50.0f);
            applied["blank_pts_per_1000_units"] = cfg.blank_pts_per_1000_units;
        } else if (!strcmp(key, "min_interior_pts_per_segment") && val.is<int>()) {
            cfg.min_interior_pts_per_segment = constrain((int)val, 0, 50);
            applied["min_interior_pts_per_segment"] = cfg.min_interior_pts_per_segment;
        } else if (!strcmp(key, "stage1_blank_target") && val.is<int>()) {
            cfg.stage1_blank_target = constrain((int)val, 1, 100);
            applied["stage1_blank_target"] = cfg.stage1_blank_target;
        } else if (!strcmp(key, "resample_enabled") && val.is<bool>()) {
            cfg.resample_enabled = (bool)val;
            applied["resample_enabled"] = cfg.resample_enabled;
        } else if (!strcmp(key, "resample_spacing_units") && val.is<float>()) {
            cfg.resample_spacing_units = constrain((float)val, 10.0f, 2000.0f);
            applied["resample_spacing_units"] = cfg.resample_spacing_units;
        } else if (!strcmp(key, "curvature_resample_enabled") && val.is<bool>()) {
            cfg.curvature_resample_enabled = (bool)val;
            applied["curvature_resample_enabled"] = cfg.curvature_resample_enabled;
        } else if (!strcmp(key, "curvature_gain") && val.is<float>()) {
            cfg.curvature_gain = constrain((float)val, 0.0f, 20.0f);
            applied["curvature_gain"] = cfg.curvature_gain;
        } else if (!strcmp(key, "min_spacing_units") && val.is<float>()) {
            cfg.min_spacing_units = constrain((float)val, 1.0f, 2000.0f);
            applied["min_spacing_units"] = cfg.min_spacing_units;
        } else if (!strcmp(key, "max_spacing_units") && val.is<float>()) {
            cfg.max_spacing_units = constrain((float)val, 1.0f, 4000.0f);
            applied["max_spacing_units"] = cfg.max_spacing_units;
        } else if (!strcmp(key, "ringing_comp_enabled") && val.is<bool>()) {
            cfg.ringing_comp_enabled = (bool)val;
            applied["ringing_comp_enabled"] = cfg.ringing_comp_enabled;
        } else if (!strcmp(key, "ring_freq_hz") && val.is<float>()) {
            cfg.ring_freq_hz = constrain((float)val, 1.0f, 2000.0f);
            applied["ring_freq_hz"] = cfg.ring_freq_hz;
        } else if (!strcmp(key, "ring_damping_ratio") && val.is<float>()) {
            cfg.ring_damping_ratio = constrain((float)val, 0.0f, 0.9f);
            applied["ring_damping_ratio"] = cfg.ring_damping_ratio;
        } else if (!strcmp(key, "jitter_enabled") && val.is<bool>()) {
            cfg.jitter_enabled = (bool)val;
            applied["jitter_enabled"] = cfg.jitter_enabled;
        } else if (!strcmp(key, "jitter_amount_units") && val.is<float>()) {
            cfg.jitter_amount_units = constrain((float)val, 0.0f, 2000.0f);
            applied["jitter_amount_units"] = cfg.jitter_amount_units;
        } else if (!strcmp(key, "vel_clamp_enabled") && val.is<bool>()) {
            cfg.vel_clamp_enabled = (bool)val;
            applied["vel_clamp_enabled"] = cfg.vel_clamp_enabled;
        } else if (!strcmp(key, "max_step_units") && val.is<float>()) {
            cfg.max_step_units = constrain((float)val, 50.0f, 32767.0f);
            applied["max_step_units"] = cfg.max_step_units;
        } else if (!strcmp(key, "accel_clamp_enabled") && val.is<bool>()) {
            cfg.accel_clamp_enabled = (bool)val;
            applied["accel_clamp_enabled"] = cfg.accel_clamp_enabled;
        } else if (!strcmp(key, "max_accel_units") && val.is<float>()) {
            cfg.max_accel_units = constrain((float)val, 10.0f, 32767.0f);
            applied["max_accel_units"] = cfg.max_accel_units;
        } else if (!strcmp(key, "reorder_segments") && val.is<bool>()) {
            cfg.reorder_segments = (bool)val;
            applied["reorder_segments"] = cfg.reorder_segments;
        } else if (!strcmp(key, "reorder_2opt") && val.is<bool>()) {
            cfg.reorder_2opt = (bool)val;
            applied["reorder_2opt"] = cfg.reorder_2opt;
        } else {
            ignored.add(key);
        }
    }
    OptimizerNormalizeResult norm = normalizeOptimizerConfig(cfg);
    if (norm.min_blank_samples_corrected) applied["min_blank_samples"] = cfg.min_blank_samples;
    if (norm.min_corner_pts_corrected)    applied["min_corner_pts"]    = cfg.min_corner_pts;
}

// Lists, into `out`, every OptimizerLiveConfig field where `cur` differs from
// `snap` -- i.e. the overrides a calib-cam session has actually applied so
// far. Used by GET /api/calib-cam/status.
static void diffOptimizerOverrides(const OptimizerLiveConfig& cur,
                                    const OptimizerLiveConfig& snap, JsonObject out) {
    if (cur.corner_angle_deg != snap.corner_angle_deg) out["corner_angle_deg"] = cur.corner_angle_deg;
    if (cur.min_corner_pts != snap.min_corner_pts) out["min_corner_pts"] = cur.min_corner_pts;
    if (cur.max_corner_pts != snap.max_corner_pts) out["max_corner_pts"] = cur.max_corner_pts;
    if (cur.pts_per_1000_units != snap.pts_per_1000_units) out["pts_per_1000_units"] = cur.pts_per_1000_units;
    if (cur.blank_samples != snap.blank_samples) out["blank_samples"] = cur.blank_samples;
    if (cur.max_pts_per_frame != snap.max_pts_per_frame) out["max_pts_per_frame"] = cur.max_pts_per_frame;
    if (cur.min_blank_samples != snap.min_blank_samples) out["min_blank_samples"] = cur.min_blank_samples;
    if (cur.blank_pts_per_1000_units != snap.blank_pts_per_1000_units) out["blank_pts_per_1000_units"] = cur.blank_pts_per_1000_units;
    if (cur.min_interior_pts_per_segment != snap.min_interior_pts_per_segment) out["min_interior_pts_per_segment"] = cur.min_interior_pts_per_segment;
    if (cur.stage1_blank_target != snap.stage1_blank_target) out["stage1_blank_target"] = cur.stage1_blank_target;
    if (cur.resample_enabled != snap.resample_enabled) out["resample_enabled"] = cur.resample_enabled;
    if (cur.resample_spacing_units != snap.resample_spacing_units) out["resample_spacing_units"] = cur.resample_spacing_units;
    if (cur.curvature_resample_enabled != snap.curvature_resample_enabled) out["curvature_resample_enabled"] = cur.curvature_resample_enabled;
    if (cur.curvature_gain != snap.curvature_gain) out["curvature_gain"] = cur.curvature_gain;
    if (cur.min_spacing_units != snap.min_spacing_units) out["min_spacing_units"] = cur.min_spacing_units;
    if (cur.max_spacing_units != snap.max_spacing_units) out["max_spacing_units"] = cur.max_spacing_units;
    if (cur.ringing_comp_enabled != snap.ringing_comp_enabled) out["ringing_comp_enabled"] = cur.ringing_comp_enabled;
    if (cur.ring_freq_hz != snap.ring_freq_hz) out["ring_freq_hz"] = cur.ring_freq_hz;
    if (cur.ring_damping_ratio != snap.ring_damping_ratio) out["ring_damping_ratio"] = cur.ring_damping_ratio;
    if (cur.jitter_enabled != snap.jitter_enabled) out["jitter_enabled"] = cur.jitter_enabled;
    if (cur.jitter_amount_units != snap.jitter_amount_units) out["jitter_amount_units"] = cur.jitter_amount_units;
    if (cur.vel_clamp_enabled != snap.vel_clamp_enabled) out["vel_clamp_enabled"] = cur.vel_clamp_enabled;
    if (cur.max_step_units != snap.max_step_units) out["max_step_units"] = cur.max_step_units;
    if (cur.accel_clamp_enabled != snap.accel_clamp_enabled) out["accel_clamp_enabled"] = cur.accel_clamp_enabled;
    if (cur.max_accel_units != snap.max_accel_units) out["max_accel_units"] = cur.max_accel_units;
    if (cur.reorder_segments != snap.reorder_segments) out["reorder_segments"] = cur.reorder_segments;
    if (cur.reorder_2opt != snap.reorder_2opt) out["reorder_2opt"] = cur.reorder_2opt;
}

// ── PSRAM-backed JSON response ────────────────────────────────────────────────
// serializeJson() into an Arduino String allocates on the internal DRAM heap,
// which is the scarce resource shared with lwIP/WiFi. On the polled /api/state
// path this repeatedly pressured internal heap (root cause of the low-heap /
// WiFiUdp ENOMEM spiral). This serializes straight into a PSRAM buffer and
// streams it chunked; the shared_ptr deleter frees the buffer whether the
// response completes or the client aborts mid-stream.
static void sendJsonPsram(AsyncWebServerRequest* req, const JsonDocument& doc, int status = 200) {
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
    resp->setCode(status);
    req->send(resp);
}

// Serializes one optimizer telemetry record. Field names mirror
// optimizer::Stats exactly, so the WebUI reads what the firmware calls it.
static void fillOptimizerStats(JsonObject dst, const optimizer::Stats& s) {
    dst["emitted_lit"]         = s.emittedLit;
    dst["emitted_blank"]       = s.emittedBlank;
    dst["truncated"]           = s.truncated;
    dst["planned_total"]       = s.plannedTotal;
    dst["jump_count"]          = s.jumpCount;
    dst["jump_distance_total"] = s.jumpDistanceTotal;
    dst["calls"]               = s.calls;
    dst["stage2_scale"]        = s.stage2Scale;
    dst["stage1_triggered"]    = s.stage1Triggered;
    dst["stage15_triggered"]   = s.stage15Triggered;
    dst["ringing_active"]      = s.ringingActive;
}

// ── PSRAM-cached index.html.gz ────────────────────────────────────────────────
// serveStatic's LittleFS-backed AsyncFileResponse re-reads the ~103 KB gzipped
// bundle from flash into internal-heap chunks on every request. Under normal
// browsing this only happens once (Cache-Control: max-age=3600 keeps repeat
// loads out of the device entirely), but a browser hard-reload sends
// Cache-Control: no-cache and skips its own cache validation, forcing a fresh
// fetch every time -- that cold-load DRAM spike is what pushed `largest` to
// 1524 B in a HEAP_CRITICAL failsafe reboot. Loading the file into PSRAM once
// at boot and streaming straight from there removes the repeated flash-read +
// internal-heap churn; only the small per-connection TCP framing buffer
// (ESPAsyncWebServer's own ASYNC_RESPONCE_BUFF_SIZE) still touches internal RAM.
static std::shared_ptr<uint8_t> s_index_gz_buf;
static size_t                   s_index_gz_len = 0;

static void loadIndexGzToPsram() {
    File f = LittleFS.open("/index.html.gz", "r");
    if (!f) { ESP_LOGE(TAG, "index.html.gz not found on LittleFS"); return; }
    size_t len = f.size();
    uint8_t* buf = (uint8_t*)heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
    if (!buf) {
        ESP_LOGE(TAG, "PSRAM alloc failed for index.html.gz (%u B)", (unsigned)len);
        f.close();
        return;
    }
    size_t got = f.read(buf, len);
    f.close();
    if (got != len) {
        ESP_LOGE(TAG, "index.html.gz short read %u/%u", (unsigned)got, (unsigned)len);
        heap_caps_free(buf);
        return;
    }
    s_index_gz_buf.reset(buf, [](uint8_t* p) { heap_caps_free(p); });
    s_index_gz_len = len;
    memreg::track("WebUI Bundle Cache", len, true);
    ESP_LOGI(TAG, "index.html.gz cached in PSRAM (%u B)", (unsigned)len);
}

static void serveIndexGz(AsyncWebServerRequest* req) {
    if (!s_index_gz_buf) { req->send(500, "text/plain", "index unavailable"); return; }
    std::shared_ptr<uint8_t> buf = s_index_gz_buf;  // keep alive for the response's lifetime
    size_t len = s_index_gz_len;
    AsyncWebServerResponse* resp = req->beginChunkedResponse(
        "text/html",
        [buf, len](uint8_t* out, size_t maxLen, size_t index) -> size_t {
            if (index >= len) return 0;
            // Heavy-IO window: lets etherdream's beacon skip sends while this
            // transfer is actively pressuring the shared internal heap.
            gState.heavy_io_until_ms.store(millis() + 3000);
            size_t n = std::min(maxLen, len - index);
            memcpy(out, buf.get() + index, n);
            return n;
        });
    resp->addHeader("Content-Encoding", "gzip");
    resp->addHeader("Cache-Control", "max-age=3600");
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
static volatile bool   s_scan_error   = false;
static volatile bool   s_scan_done    = false;  // true once a completed result is waiting to be picked up

// Scan results are fetched straight from the IDF driver (esp_wifi_scan_get_ap_records)
// instead of via WiFi.SSID(i)/RSSI(i) -- those read WiFiScanClass's own cache, which is
// only populated by its SCAN_DONE event handler, an async hop through the arduino
// event-queue task whose timing turned out to be unreliable under load. Own PSRAM
// storage sidesteps that dependency entirely.
struct ScanNetInfo { char ssid[33]; int8_t rssi; uint8_t channel; bool secure; };
static const int    SCAN_MAX_NETS = 32;
static ScanNetInfo* s_scan_nets   = nullptr;

// ── /api/paint/set chunked-body buffer (fixed PSRAM, no per-request heap alloc) ──
// Previous impl used `new String()` per request, freed only on the success
// path. An aborted/dropped upload (never reaching index+len==total) leaked
// it permanently -- root cause of the post-5.21.0 heap exhaustion.
static const size_t PAINT_BODY_CAP  = 32768;
static char*        s_paint_body    = nullptr;
static size_t       s_paint_body_len = 0;
// Owning request for the in-progress upload -- guards the single shared
// buffer above against a second, overlapping /api/paint/set (e.g. Live
// mode's 120ms debounce re-firing while a prior POST is still in flight on
// a slow link). Without this, the second request's index==0 chunk resets
// s_paint_body_len out from under the first, which then never reaches
// index+len==total and never calls req->send() -- the client hangs until
// its own AbortController fires ("Request timed out [POST /api/paint/set]"),
// and that one stuck request can back up every other request behind it on
// the async_tcp task (explains an "offline" conn-pill at the same time).
static AsyncWebServerRequest* s_paint_body_owner = nullptr;

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
    s_prefs.putFloat("out_scale",   gConfig.outputScale);
    s_prefs.putBool("swap",         gConfig.swap_xy);
    s_prefs.putBool("invx",         gConfig.invert_x);
    s_prefs.putBool("invy",         gConfig.invert_y);
    s_prefs.putUChar("gain_r",      gConfig.gain_r);
    s_prefs.putUChar("gain_g",      gConfig.gain_g);
    s_prefs.putUChar("gain_b",      gConfig.gain_b);
    s_prefs.putUChar("thresh_r",    gConfig.thresh_r);
    s_prefs.putUChar("thresh_g",    gConfig.thresh_g);
    s_prefs.putUChar("thresh_b",    gConfig.thresh_b);
    s_prefs.putBool ("gamma_en",    gConfig.gamma_enable);
    s_prefs.putBool("osc_en",       gConfig.osc_enabled);
    s_prefs.putBool("sacn_en",      gConfig.sacn_enabled);
    s_prefs.putBool("helios_en",    gConfig.helios_net_enabled);
    s_prefs.putBool("artnet_en",    gConfig.artnet_enabled);
    s_prefs.putBool("edream_en",    gConfig.etherdream_enabled);
    s_prefs.putBool("dbg_dmx",      gConfig.debug_log_dmx);
    s_prefs.putBool("dbg_artnet",   gConfig.debug_log_artnet);
    s_prefs.putBool("dbg_edream",   gConfig.debug_log_etherdream);
    s_prefs.putBool("dbg_helios",   gConfig.debug_log_helios_net);
    s_prefs.putBool("dbg_osc",      gConfig.debug_log_osc);
    s_prefs.putBool("dbg_sacn",     gConfig.debug_log_sacn);
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
    // Suffixes must match PROF_MAP in main.cpp::loadConfig().
    static const struct { const char* sfx; uint8_t idx; } PMAP[] = {
        {"_s",  OPT_PROFILE_VECTOR},      {"_c",   OPT_PROFILE_SMOOTH},
        {"_w",  OPT_PROFILE_WAVES},       {"_3",   OPT_PROFILE_WIREFRAME},
        {"_sol",OPT_PROFILE_MULTIOBJECT}, {"_sc",  OPT_PROFILE_PARTICLES},
        {"_tr", OPT_PROFILE_TRAILS},      {"_txt", OPT_PROFILE_TEXT},
    };
    for (auto& pm : PMAP) {
        const OptimizerLiveConfig& p = gOptimizerProfiles[pm.idx];
        char k[16];
        #define SAVE_F(b,f)  snprintf(k,sizeof(k),"%s%s",b,pm.sfx); s_prefs.putFloat(k,p.f)
        #define SAVE_U(b,f)  snprintf(k,sizeof(k),"%s%s",b,pm.sfx); s_prefs.putUChar(k,p.f)
        #define SAVE_S(b,f)  snprintf(k,sizeof(k),"%s%s",b,pm.sfx); s_prefs.putUShort(k,p.f)
        #define SAVE_B(b,f)  snprintf(k,sizeof(k),"%s%s",b,pm.sfx); s_prefs.putBool(k,p.f)
        SAVE_F("opt_cad",   corner_angle_deg);
        SAVE_U("opt_mincp", min_corner_pts);
        SAVE_U("opt_maxcp", max_corner_pts);
        SAVE_F("opt_ppu",   pts_per_1000_units);
        SAVE_U("opt_blank", blank_samples);
        SAVE_S("opt_maxppf",max_pts_per_frame);
        SAVE_U("opt_minbl", min_blank_samples);
        SAVE_F("opt_blppu", blank_pts_per_1000_units);
        SAVE_U("opt_minip", min_interior_pts_per_segment);
        SAVE_U("opt_s1tgt", stage1_blank_target);
        SAVE_B("opt_rsen",  resample_enabled);
        SAVE_F("opt_rssp",  resample_spacing_units);
        SAVE_B("opt_cven",  curvature_resample_enabled);
        SAVE_F("opt_cvgn",  curvature_gain);
        SAVE_F("opt_cvmn",  min_spacing_units);
        SAVE_F("opt_cvmx",  max_spacing_units);
        SAVE_B("opt_rngen", ringing_comp_enabled);
        SAVE_B("opt_jten",  jitter_enabled);
        SAVE_F("opt_jtam",  jitter_amount_units);
        SAVE_F("opt_rngfq", ring_freq_hz);
        SAVE_F("opt_rngdr", ring_damping_ratio);
        SAVE_B("opt_vcen",  vel_clamp_enabled);
        SAVE_F("opt_vcstp", max_step_units);
        SAVE_B("opt_acen",  accel_clamp_enabled);
        SAVE_F("opt_acmax", max_accel_units);
        SAVE_B("opt_reord", reorder_segments);
        SAVE_B("opt_ro2op", reorder_2opt);
        #undef SAVE_F
        #undef SAVE_U
        #undef SAVE_S
        #undef SAVE_B
    }
    s_prefs.putBool ("zone_en",   gZone.enabled);
    s_prefs.putUChar("zone_cnt",  gZone.count);
    s_prefs.putBytes("zone_x",    (const void*)gZone.x, sizeof(gZone.x));
    s_prefs.putBytes("zone_y",    (const void*)gZone.y, sizeof(gZone.y));
    s_prefs.putBool ("warp_en",   gWarp.enabled);
    s_prefs.putUChar("warp_grid", gWarp.gridSize);
    s_prefs.putBytes("warp_pts",  (const void*)gWarp.points, sizeof(gWarp.points));
    s_prefs.putBool ("brt_en",    gBrightness.enabled);
    s_prefs.putUChar("brt_grid",  gBrightness.gridSize);
    s_prefs.putBytes("brt_gain",  (const void*)gBrightness.gain, sizeof(gBrightness.gain));
    s_prefs.putBool ("if_en",     gInverseFilter.enabled);
    s_prefs.putFloat("if_alpha",  gInverseFilter.regAlpha);
    s_prefs.putFloat("if_wn_x",   gInverseFilter.x.wnHz);
    s_prefs.putFloat("if_zt_x",   gInverseFilter.x.zeta);
    s_prefs.putFloat("if_wn_y",   gInverseFilter.y.wnHz);
    s_prefs.putFloat("if_zt_y",   gInverseFilter.y.zeta);
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

// Missing keys leave gWarp untouched -- it was already default-constructed
// to the identity grid before this runs, so a fresh/pre-7a NVS falls back to
// identity exactly as required (backward compatible).
static void loadWarp() {
    s_prefs.begin("laser", true);
    gWarp.enabled  = s_prefs.getBool ("warp_en",   gWarp.enabled);
    uint8_t n      = s_prefs.getUChar("warp_grid", gWarp.gridSize);
    if (n < 2)              n = 2;
    if (n > WARP_GRID_MAX)  n = WARP_GRID_MAX;
    gWarp.gridSize = n;
    s_prefs.getBytes("warp_pts", (void*)gWarp.points, sizeof(gWarp.points));
    s_prefs.end();
    warp::init();
}

// Missing keys leave gBrightness untouched -- already default-constructed to
// the identity grid (gain 255 everywhere) before this runs, same fallback
// reasoning as loadWarp() above.
static void loadBrightness() {
    s_prefs.begin("laser", true);
    gBrightness.enabled = s_prefs.getBool ("brt_en",   gBrightness.enabled);
    uint8_t n           = s_prefs.getUChar("brt_grid", gBrightness.gridSize);
    if (n < 2)              n = 2;
    if (n > WARP_GRID_MAX)  n = WARP_GRID_MAX;
    gBrightness.gridSize = n;
    s_prefs.getBytes("brt_gain", (void*)gBrightness.gain, sizeof(gBrightness.gain));
    s_prefs.end();
    brightness::init();
}

// Missing keys leave gInverseFilter untouched -- already default-constructed
// to "disabled, both axes unmeasured" before this runs, same fallback
// reasoning as loadWarp()/loadBrightness() above. gProjection.galvo_kpps
// must already be loaded (main.cpp does this before web_ui::init()) since
// invfilter::init() needs the live sample rate to design the biquads.
static void loadInverseFilter() {
    s_prefs.begin("laser", true);
    gInverseFilter.enabled    = s_prefs.getBool ("if_en",    gInverseFilter.enabled);
    gInverseFilter.regAlpha   = s_prefs.getFloat("if_alpha", gInverseFilter.regAlpha);
    gInverseFilter.x.wnHz     = s_prefs.getFloat("if_wn_x",  gInverseFilter.x.wnHz);
    gInverseFilter.x.zeta     = s_prefs.getFloat("if_zt_x",  gInverseFilter.x.zeta);
    gInverseFilter.y.wnHz     = s_prefs.getFloat("if_wn_y",  gInverseFilter.y.wnHz);
    gInverseFilter.y.zeta     = s_prefs.getFloat("if_zt_y",  gInverseFilter.y.zeta);
    s_prefs.end();
    invfilter::init((uint32_t)gProjection.galvo_kpps * 1000);
}

/* ============================================================
 * JSON Builders
 * ============================================================ */
// Core status fields shared by /api/state (buildStateJson(), the WebUI's
// full state) and /api/status (external camera-autotuning script, see
// docs/06-camera-autotuning.md and scripts/optimizeGalvo/optimizeGalvo.py's
// getStatus()) -- one place computes these so the two responses can no
// longer drift out of field-name/value sync (State_fix.md architecture #15).
static void buildCoreStatusJson(JsonDocument& doc) {
    doc["estop_ok"]        = gState.estop_ok.load();
    doc["scanfail_ok"]     = gState.scanfail_ok.load();
    doc["laser_armed"]     = gState.laser_armed.load();
    doc["source"]          = (int)gState.source;
    doc["master_dimmer"]   = gState.master_dimmer.load();
    doc["ui_override"]     = gState.ui_override.load();
    doc["ui_master_dimmer"]= gState.ui_master_dimmer.load();
    doc["points_per_sec"]  = galvo::pointsPerSec();
    doc["buffer_fill"]     = galvo::bufferFillLevel();
    uint32_t core_age = millis() - gState.last_dmx_ms.load();
    doc["last_dmx_age_ms"] = (gState.last_dmx_ms.load() == 0) ? -1 : (int32_t)core_age;
    doc["fw_version"]      = LASER_FW_VERSION;
    { char pw[12]; snprintf(pw, sizeof(pw), "%08X", (uint32_t)(ESP.getEfuseMac() >> 16));
      doc["ota_pass"] = pw; }
    doc["hostname"]        = gConfig.hostname;
    doc["ip"]              = WiFi.localIP().toString();
    doc["rssi"]            = WiFi.RSSI();
    doc["uptime_s"]        = millis() / 1000;
    doc["free_heap"]       = ESP.getFreeHeap();
    doc["free_psram"]      = ESP.getFreePsram();
}

static void buildStateJson(JsonDocument& doc) {
    buildCoreStatusJson(doc);
    doc["watchdog_ok"]     = safety::watchdogOk();
    doc["subsystems_ok"]   = safety::subsystemsOk();
    doc["last_failsafe"]   = safety::lastFailsafeReason();
    doc["arm_requested"]   = safety::userArmRequest();
    doc["calib_active"]    = gState.calib_active;
    doc["ilda_active"]     = ilda::gILDA.active;
    doc["playlist_active"] = playlist::isActive();
    doc["safety_override"] = gConfig.safety_override;
    doc["etherdream_connected"] = etherdream::isConnected();
    doc["etherdream_playing"]   = etherdream::isPlaying();
    doc["helios_net_connected"] = helios_net::isConnected();
    doc["helios_net_playing"]   = helios_net::isPlaying();
    doc["osc_active"]           = osc_in::isActive();
    doc["sacn_active"]          = sacn_in::isReceiving();
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
    { // DEBUG (gain live-update issue)
      uint32_t su; uint8_t sgr, sgg, sgb, cgr, cgg, cgb;
      galvo::snapDebug(su, sgr, sgg, sgb, cgr, cgg, cgb);
      doc["dbg_snap_updates"] = su;
      doc["dbg_snap_gain_r"]  = sgr;  doc["dbg_snap_gain_g"] = sgg;  doc["dbg_snap_gain_b"] = sgb;
      doc["dbg_cfg_gain_r"]   = cgr;  doc["dbg_cfg_gain_g"]  = cgg;  doc["dbg_cfg_gain_b"]  = cgb;
    }
    doc["no_hw_mode"]      = galvo::noHwMode();
    doc["preset_idx"]      = static_cast<int8_t>(patterns::getPreset());
    doc["starfield_stars"] = presets::gStarfieldStarCount.load();
    doc["dmx_frame_count"] = gState.dmx_frame_count.load();
    doc["fps"]              = galvo::fps();
    doc["frame_n"]          = gState.frame_n.load();
    doc["frame_lit"]        = gState.frame_lit.load();
    doc["frame_blank"]      = gState.frame_blank.load();
    doc["text_truncated"]   = gState.text_truncated.load();
    // P9a: DAC output-limiting clip diagnostic (galvo_out.cpp galvoTask()) --
    // saturation from the last frame the optimizer/pattern layer can't see,
    // since clamping to dac_limit_min/max happens after they've already run.
    { uint32_t cx = gState.dacClipCountX.load(), cy = gState.dacClipCountY.load();
      uint32_t cAny = gState.dacClipCountAny.load(), total = gState.dacClipTotalPts.load();
      doc["dacClipX"]   = cx;
      doc["dacClipY"]   = cy;
      doc["dacClipPct"] = total ? (100.0f * (float)cAny / (float)total) : 0.0f;
    }
    doc["ntp_server"]      = gConfig.ntp_server;
    doc["ntp_tz"]          = gConfig.ntp_tz;
    doc["ntp_synced"]      = ntp_client::isSynced();
    doc["heap"]            = ESP.getFreeHeap();
    doc["cpu0"]            = cpu_mon::load0();
    doc["cpu1"]            = cpu_mon::load1();
    doc["psram"]           = ESP.getFreePsram();
    // Dashboard extras
    doc["ntp_synced"]      = ntp_client::isSynced();
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
    doc["temp_unit"]  = temp::getDisplayUnit();

    // BPM clock (0=Manual, 1=Tap, 2=DMX -- see bpm_clock::Source)
    doc["bpm"]        = bpm_clock::gBpm.bpm;
    doc["bpm_source"] = (int)bpm_clock::gBpm.source;
    doc["bpm_phase"]  = bpm_clock::gBpm.phase_ms;  // 0-999, for WebUI beat-flash indicators

    // Preset Sequencer -- lightweight transport status only; the full
    // playlist (steps[]) is fetched separately via GET /api/sequencer, the
    // same split /api/state vs /api/playlist already uses for Playlist.
    { LOCK_STATE();
      doc["seq_running"]    = gSequencer.running;
      doc["seq_loop"]       = gSequencer.loop;
      doc["seq_current"]    = gSequencer.currentStep;
      doc["seq_stepcount"]  = gSequencer.stepCount;
    }
}

static void buildConfigJson(JsonDocument& doc) {
    doc["dmx_address"]     = gConfig.dmx_address;
    doc["artnet_universe"] = gConfig.artnet_universe;
    doc["bpm_manual"]      = bpm_clock::manualBpm();
    doc["bpm_dmx_channel"] = bpm_clock::dmxChannel();
    doc["osc_enabled"]        = gConfig.osc_enabled;
    doc["sacn_enabled"]       = gConfig.sacn_enabled;
    doc["helios_net_enabled"] = gConfig.helios_net_enabled;
    doc["artnet_enabled"]     = gConfig.artnet_enabled;
    doc["etherdream_enabled"] = gConfig.etherdream_enabled;
    doc["debug_log_dmx"]        = gConfig.debug_log_dmx;
    doc["debug_log_artnet"]     = gConfig.debug_log_artnet;
    doc["debug_log_etherdream"] = gConfig.debug_log_etherdream;
    doc["debug_log_helios_net"] = gConfig.debug_log_helios_net;
    doc["debug_log_osc"]        = gConfig.debug_log_osc;
    doc["debug_log_sacn"]       = gConfig.debug_log_sacn;
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
    doc["thresh_r"]        = gConfig.thresh_r;
    doc["thresh_g"]        = gConfig.thresh_g;
    doc["thresh_b"]        = gConfig.thresh_b;
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
    doc["output_scale"]    = gConfig.outputScale;
    doc["gamma_enable"]    = gConfig.gamma_enable;
    doc["opt_active_profile"] = (uint8_t)gActiveOptimizerProfile;
    {
        // Member name list per profile -- drives the Optimizer tab's
        // right-hand column. Derived from presetClassOf(), so it stays
        // correct without a parallel table in the WebUI.
        JsonArray members = doc["opt_profile_members"].to<JsonArray>();
        for (uint8_t pi = 0; pi < OPT_PROFILE_COUNT; pi++) {
            JsonArray m = members.add<JsonArray>();
            const uint8_t n = presets::profileMemberCount(pi);
            for (uint8_t k = 0; k < n; k++) m.add(presets::profileMemberName(pi, k));
        }
    }
    {
        JsonArray profiles = doc["opt_profiles"].to<JsonArray>();
        for (uint8_t pi = 0; pi < OPT_PROFILE_COUNT; pi++) {
            const OptimizerLiveConfig& p = gOptimizerProfiles[pi];
            JsonObject o = profiles.add<JsonObject>();
            o["opt_corner_angle_deg"]             = p.corner_angle_deg;
            o["opt_min_corner_pts"]               = p.min_corner_pts;
            o["opt_max_corner_pts"]               = p.max_corner_pts;
            o["opt_pts_per_1000_units"]           = p.pts_per_1000_units;
            o["opt_blank_samples"]                = p.blank_samples;
            o["opt_max_pts_per_frame"]            = p.max_pts_per_frame;
            o["opt_min_blank_samples"]            = p.min_blank_samples;
            o["opt_blank_pts_per_1000_units"]     = p.blank_pts_per_1000_units;
            o["opt_min_interior_pts_per_segment"] = p.min_interior_pts_per_segment;
            o["opt_stage1_blank_target"]          = p.stage1_blank_target;
            o["opt_resample_enabled"]             = p.resample_enabled;
            o["opt_resample_spacing_units"]       = p.resample_spacing_units;
            o["opt_curvature_resample_enabled"]   = p.curvature_resample_enabled;
            o["opt_curvature_gain"]               = p.curvature_gain;
            o["opt_min_spacing_units"]            = p.min_spacing_units;
            o["opt_max_spacing_units"]            = p.max_spacing_units;
            o["opt_ringing_comp_enabled"]         = p.ringing_comp_enabled;
            o["opt_ring_freq_hz"]                 = p.ring_freq_hz;
            o["opt_ring_damping_ratio"]           = p.ring_damping_ratio;
            o["opt_jitter_enabled"]               = p.jitter_enabled;
            o["opt_jitter_amount_units"]          = p.jitter_amount_units;
            o["opt_vel_clamp_enabled"]            = p.vel_clamp_enabled;
            o["opt_max_step_units"]               = p.max_step_units;
            o["opt_accel_clamp_enabled"]          = p.accel_clamp_enabled;
            o["opt_max_accel_units"]              = p.max_accel_units;
            o["opt_reorder_segments"]             = p.reorder_segments;
            o["opt_reorder_2opt"]                 = p.reorder_2opt;
            optimizer::OptimizerConfig eff;
            eff.pts_per_1000_units      = p.pts_per_1000_units;
            eff.resample_spacing_units  = p.resample_spacing_units;
            eff.blank_pts_per_1000_units = p.blank_pts_per_1000_units;
            eff.max_step_units          = p.max_step_units;
            eff.max_accel_units         = p.max_accel_units;
            // Pillar 3 inputs, so ringingStatus() below sees the same numbers
            // the pattern paths build their OptimizerConfig from.
            eff.blank_samples         = p.blank_samples;
            eff.min_blank_samples     = p.min_blank_samples;
            eff.ringing_comp_enabled  = p.ringing_comp_enabled;
            eff.ring_freq_hz          = p.ring_freq_hz;
            eff.ring_damping_ratio    = p.ring_damping_ratio;
            eff.galvo_kpps            = gProjection.galvo_kpps;
            optimizer::applyPpsScaling(eff, gProjection.galvo_rated_kpps, gProjection.galvo_kpps);
            o["opt_eff_pts_per_1000_units"]      = eff.pts_per_1000_units;
            o["opt_eff_resample_spacing_units"]  = eff.resample_spacing_units;
            o["opt_eff_blank_pts_per_1000_units"] = eff.blank_pts_per_1000_units;
            o["opt_eff_max_step_units"]          = eff.max_step_units;
            o["opt_eff_max_accel_units"]         = eff.max_accel_units;
            // Whether ringing compensation actually takes effect at these
            // settings, and the impulse delay it needs -- see RingingStatus.
            optimizer::RingingStatus rs = optimizer::ringingStatus(eff);
            o["opt_eff_ringing_active"]  = rs.active;
            o["opt_eff_ring_shift_pts"]  = rs.shift_pts;
        }
    }
    // Top-level opt_* = active profile (backwards compat)
    {
        const OptimizerLiveConfig& p = gOptimizerConfig;
        doc["opt_corner_angle_deg"]             = p.corner_angle_deg;
        doc["opt_min_corner_pts"]               = p.min_corner_pts;
        doc["opt_max_corner_pts"]               = p.max_corner_pts;
        doc["opt_pts_per_1000_units"]           = p.pts_per_1000_units;
        doc["opt_blank_samples"]                = p.blank_samples;
        doc["opt_max_pts_per_frame"]            = p.max_pts_per_frame;
        doc["opt_min_blank_samples"]            = p.min_blank_samples;
        doc["opt_blank_pts_per_1000_units"]     = p.blank_pts_per_1000_units;
        doc["opt_min_interior_pts_per_segment"] = p.min_interior_pts_per_segment;
        doc["opt_stage1_blank_target"]          = p.stage1_blank_target;
        doc["opt_resample_enabled"]             = p.resample_enabled;
        doc["opt_resample_spacing_units"]       = p.resample_spacing_units;
        doc["opt_curvature_resample_enabled"]   = p.curvature_resample_enabled;
        doc["opt_curvature_gain"]               = p.curvature_gain;
        doc["opt_min_spacing_units"]            = p.min_spacing_units;
        doc["opt_max_spacing_units"]            = p.max_spacing_units;
        doc["opt_ringing_comp_enabled"]         = p.ringing_comp_enabled;
        doc["opt_ring_freq_hz"]                 = p.ring_freq_hz;
        doc["opt_ring_damping_ratio"]           = p.ring_damping_ratio;
        doc["opt_jitter_enabled"]               = p.jitter_enabled;
        doc["opt_jitter_amount_units"]          = p.jitter_amount_units;
        doc["opt_vel_clamp_enabled"]            = p.vel_clamp_enabled;
        doc["opt_max_step_units"]               = p.max_step_units;
        doc["opt_accel_clamp_enabled"]          = p.accel_clamp_enabled;
        doc["opt_max_accel_units"]              = p.max_accel_units;
        doc["opt_reorder_segments"]             = p.reorder_segments;
        doc["opt_reorder_2opt"]                 = p.reorder_2opt;
    }

    // opt_eff_* now included per-profile inside opt_profiles[] above.
}

/* ============================================================
 * WiFi scan task (background, blocking ~3s)
 * ============================================================ */
static void wifiScanTask(void*) {
    s_scan_running = true;
    s_scan_results = 0;
    s_scan_error   = false;

    // WiFi.scanNetworks() refuses to even try once WiFiScanClass's own
    // WIFI_SCANNING_BIT gets stuck (seen in the field as a permanent code -1):
    // it's set right before esp_wifi_scan_start() and only cleared by the
    // SCAN_DONE event, so a scan interrupted by a WiFi.mode() switch (e.g.
    // AP_STA->STA on reconnect) leaves it stuck forever with no way to recover
    // via the wrapper. Call esp_wifi_scan_start() directly instead -- block=true
    // blocks at the IDF level on the *driver's* own state, sidestepping the
    // wrapper's bit entirely, and gives the real esp_err_t on failure (WiFi.
    // scanNetworks() collapses every failure into the single code -2). Arduino's
    // own SCAN_DONE handler still fires afterward regardless of who started the
    // scan, so WiFi.scanComplete()/SSID()/RSSI() below still work as usual.
    WiFi.enableSTA(true);
    wifi_scan_config_t config = {};
    config.show_hidden          = false;
    config.scan_type            = WIFI_SCAN_TYPE_ACTIVE;
    config.scan_time.active.min = 100;
    config.scan_time.active.max = 300;

    esp_err_t err = ESP_FAIL;
    const int kMaxAttempts = 3;
    for (int attempt = 1; attempt <= kMaxAttempts; attempt++) {
        if (attempt > 1) {
            esp_wifi_scan_stop();
            vTaskDelay(pdMS_TO_TICKS(300 * attempt));
        }
        err = esp_wifi_scan_start(&config, true);
        if (err == ESP_OK) break;
        ESP_LOGW(TAG, "WiFi-Scan attempt %d/%d: esp_wifi_scan_start failed: %s, "
                      "wifi_status=%d int_free=%u int_largest=%u",
                 attempt, kMaxAttempts, esp_err_to_name(err), (int)WiFi.status(),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    }

    // block=true already guarantees the scan is physically done by the time
    // esp_wifi_scan_start() returns -- fetch results straight from the driver right
    // now rather than waiting on WiFiScanClass's own SCAN_DONE bit (see comment at
    // s_scan_nets: that async hop is what silently never resolved in the field).
    int n = 0;
    if (err == ESP_OK && s_scan_nets) {
        uint16_t ap_count = 0;
        esp_wifi_scan_get_ap_num(&ap_count);
        if (ap_count > SCAN_MAX_NETS) ap_count = SCAN_MAX_NETS;
        if (ap_count > 0) {
            wifi_ap_record_t* records = (wifi_ap_record_t*)heap_caps_malloc(
                sizeof(wifi_ap_record_t) * ap_count, MALLOC_CAP_SPIRAM);
            if (records) {
                esp_wifi_scan_get_ap_records(&ap_count, records);
                for (uint16_t i = 0; i < ap_count; i++) {
                    strlcpy(s_scan_nets[i].ssid, (const char*)records[i].ssid, sizeof(s_scan_nets[i].ssid));
                    s_scan_nets[i].rssi    = records[i].rssi;
                    s_scan_nets[i].channel = records[i].primary;
                    s_scan_nets[i].secure  = (records[i].authmode != WIFI_AUTH_OPEN);
                }
                heap_caps_free(records);
                n = ap_count;
            } else {
                ESP_LOGW(TAG, "WiFi-Scan: PSRAM alloc failed for %u AP records", ap_count);
            }
        }
    } else if (err != ESP_OK) {
        n = -1;
    }
    s_scan_results = (n < 0) ? 0 : n;
    s_scan_error   = (n < 0);
    s_scan_done    = true;
    s_scan_running = false;
    if (n < 0) {
        ESP_LOGW(TAG, "WiFi-Scan gave up after %d attempts (err=%s), AP_active=%d",
                 kMaxAttempts, esp_err_to_name(err), WiFi.getMode() == WIFI_AP_STA);
    } else {
        ESP_LOGI(TAG, "WiFi-Scan: %d networks found", s_scan_results);
    }
    vTaskDelete(nullptr);
}

// Sanitizes a client-supplied upload filename before it ever touches the SD
// card: drops any directory components (path traversal via "../" or "\"),
// replaces characters FAT LFN doesn't like, and caps the basename length so
// "/ilda/<name>" can never exceed ILDA_MAX_PATH. An oversized/unsanitized
// name used to get silently truncated by scanFiles()'s fixed-size buffers,
// pointing the file index at a path that no longer matched the real file on
// disk -- loadILDA() then failed with "file not found" and /api/ilda/play
// surfaced that as a bare HTTP 500.
static bool s_upload_ok = false;

static String sanitizeIldaFilename(const String& rawIn) {
    String raw = rawIn;
    int slash  = raw.lastIndexOf('/');
    int bslash = raw.lastIndexOf('\\');
    int cut    = slash > bslash ? slash : bslash;
    if (cut >= 0) raw = raw.substring(cut + 1);

    // Split off the extension so it survives truncation below (scanFiles()
    // only indexes names ending in .ild/.ilda).
    String base = raw;
    String ext  = "";
    int dot = raw.lastIndexOf('.');
    if (dot > 0 && raw.length() - dot <= 6) {
        base = raw.substring(0, dot);
        ext  = raw.substring(dot);
    }

    for (size_t i = 0; i < base.length(); i++) {
        char c = base[i];
        if (!(isalnum((unsigned char)c) || c == '_' || c == '-' || c == ' ' || c == '.'))
            base.setCharAt(i, '_');
    }
    for (size_t i = 0; i < ext.length(); i++) {
        char c = ext[i];
        if (!(isalnum((unsigned char)c) || c == '.'))
            ext.setCharAt(i, '_');
    }

    if (base.length() > ILDA_MAX_UPLOAD_NAME) base = base.substring(0, ILDA_MAX_UPLOAD_NAME);
    while (base.length() && (base[base.length() - 1] == ' ' || base[base.length() - 1] == '.'))
        base.remove(base.length() - 1);
    if (ext == ".") ext = "";

    if (base.length() == 0) base = "upload_" + String((uint32_t)millis());
    return base + ext;
}

// Same sanitization as sanitizeIldaFilename() (path-traversal strip, FAT-safe
// charset, length cap) but always forces a ".svg" extension -- SVG imports are
// stored flat under /svg/ regardless of what the source file was named.
static bool s_svg_upload_ok = false;

static String sanitizeSvgFilename(const String& rawIn) {
    String raw = rawIn;
    int slash  = raw.lastIndexOf('/');
    int bslash = raw.lastIndexOf('\\');
    int cut    = slash > bslash ? slash : bslash;
    if (cut >= 0) raw = raw.substring(cut + 1);

    String base = raw;
    int dot = raw.lastIndexOf('.');
    if (dot > 0) base = raw.substring(0, dot);

    for (size_t i = 0; i < base.length(); i++) {
        char c = base[i];
        if (!(isalnum((unsigned char)c) || c == '_' || c == '-' || c == ' ' || c == '.'))
            base.setCharAt(i, '_');
    }
    if (base.length() > SVG_MAX_UPLOAD_NAME) base = base.substring(0, SVG_MAX_UPLOAD_NAME);
    while (base.length() && (base[base.length() - 1] == ' ' || base[base.length() - 1] == '.'))
        base.remove(base.length() - 1);
    if (base.length() == 0) base = "upload_" + String((uint32_t)millis());
    return base + ".svg";
}

/* ============================================================
 * Init -- register all endpoints
 * ============================================================ */
void init() {
    generateAuthToken();
    loadZone();
    loadWarp();
    loadBrightness();
    loadInverseFilter();

    // Must run before any s_server.on(...) registration -- middleware wraps
    // every request the server handles, including static assets and API GETs.
    s_server.addMiddleware([](AsyncWebServerRequest* req, ArMiddlewareNext next) {
        s_active_requests++;
        req->onDisconnect([]() { s_active_requests--; });
        next();
    });

    s_paint_body = (char*)ps_malloc(PAINT_BODY_CAP);
    if (!s_paint_body) ESP_LOGE(TAG, "PSRAM alloc failed for paint body buffer");
    else memreg::track("Paint Body Buffer", PAINT_BODY_CAP, true);

    s_scan_nets = (ScanNetInfo*)ps_malloc(sizeof(ScanNetInfo) * SCAN_MAX_NETS);
    if (!s_scan_nets) ESP_LOGE(TAG, "PSRAM alloc failed for WiFi scan buffer");
    else memreg::track("WiFi Scan Buffer", sizeof(ScanNetInfo) * SCAN_MAX_NETS, true);

    if (!LittleFS.begin(true))
        ESP_LOGE(TAG, "LittleFS mount failed");
    else {
        loadIndexGzToPsram();
        community_presets::init();
        sequencer::init();
        modulator::init();
        camera::init();
        duplicator::init();
        spatial_noise::init();
        dotter::init();
    }

    // ---- Statische SPA ----
    // "/" and "/index.html" are served from the PSRAM cache (see
    // serveIndexGz above); serveStatic below only handles the small
    // remaining assets (favicon, logo). register serveStatic at end --
    // API routes take precedence
    s_server.on("/", HTTP_GET, serveIndexGz);
    s_server.on("/index.html", HTTP_GET, serveIndexGz);
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
            if (doc["osc_enabled"].is<bool>())        gConfig.osc_enabled        = doc["osc_enabled"];
            if (doc["sacn_enabled"].is<bool>())       gConfig.sacn_enabled       = doc["sacn_enabled"];
            if (doc["helios_net_enabled"].is<bool>()) gConfig.helios_net_enabled = doc["helios_net_enabled"];
            if (doc["artnet_enabled"].is<bool>())     gConfig.artnet_enabled     = doc["artnet_enabled"];
            if (doc["etherdream_enabled"].is<bool>()) gConfig.etherdream_enabled = doc["etherdream_enabled"];
            if (doc["debug_log_dmx"].is<bool>())        gConfig.debug_log_dmx        = doc["debug_log_dmx"];
            if (doc["debug_log_artnet"].is<bool>())     gConfig.debug_log_artnet     = doc["debug_log_artnet"];
            if (doc["debug_log_etherdream"].is<bool>()) gConfig.debug_log_etherdream = doc["debug_log_etherdream"];
            if (doc["debug_log_helios_net"].is<bool>()) gConfig.debug_log_helios_net = doc["debug_log_helios_net"];
            if (doc["debug_log_osc"].is<bool>())        gConfig.debug_log_osc        = doc["debug_log_osc"];
            if (doc["debug_log_sacn"].is<bool>())       gConfig.debug_log_sacn       = doc["debug_log_sacn"];
            if (doc["galvo_rated_kpps"].is<int>()) {
                int rk = constrain((int)doc["galvo_rated_kpps"], 1, 100);
                gProjection.galvo_rated_kpps = (uint16_t)rk;
                gPatternCacheGen++;   // PPS-derived optimizer params changed
                Preferences p; p.begin("projection", false);
                p.putUShort("rated_kpps", gProjection.galvo_rated_kpps); p.end();
            }
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
            if (doc["output_scale"].is<float>()) {
                gConfig.outputScale = constrain((float)doc["output_scale"], 0.5f, 1.0f);
            }
            persistConfig();
            req->send(200, "text/plain", "OK");
        });

    // ---- POST /api/bpm/tap ---- tap tempo, no body ----
    // Registered BEFORE the bare /api/bpm route below: ESPAsyncWebServer's
    // default URI matcher for a plain string is "BackwardCompatible"
    // (^{uri}(/.*)?$), so /api/bpm alone would otherwise also match
    // /api/bpm/tap and -- being first in registration order -- silently
    // swallow every tap request into its empty onRequest no-op (real logic
    // lives in that route's body callback, which never fires for a bodyless
    // POST). That left every tap unanswered, surfacing as a bare library
    // fallback 501 "Handler did not handle the request" with BPM frozen.
    s_server.on("/api/bpm/tap", HTTP_POST, [](AsyncWebServerRequest* req) {
        bpm_clock::tap();
        req->send(200, "text/plain", "OK");
    });

    // ---- POST /api/bpm ---- manual BPM + DMX channel selector ----
    // body: {"bpm": 128.0, "dmx_channel": 237} -- both fields optional
    s_server.on("/api/bpm", HTTP_POST,
        [](AsyncWebServerRequest* req) {},
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            JsonDocument doc(&jsonAllocator());
            if (deserializeJson(doc, data, len)) { req->send(400, "text/plain", "bad json"); return; }
            if (doc["bpm"].is<float>() || doc["bpm"].is<int>())
                bpm_clock::setManualBpm((float)doc["bpm"]);
            if (doc["dmx_channel"].is<int>())
                bpm_clock::setDmxChannel((uint16_t)constrain((int)doc["dmx_channel"], 1, 512));
            req->send(200, "text/plain", "OK");
        });

    // ---- POST /api/arm ----
    s_server.on("/api/arm", HTTP_POST,
        [](AsyncWebServerRequest* req) {},
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            bool arm = (len > 0 && data[0] == '1');
            if (arm && ota_update::uploadInProgress()) {
                req->send(409, "text/plain", "OTA in progress -- cannot arm");
                return;
            }
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
            if (doc["thresh_r"].is<int>()) gConfig.thresh_r = doc["thresh_r"];
            if (doc["thresh_g"].is<int>()) gConfig.thresh_g = doc["thresh_g"];
            if (doc["thresh_b"].is<int>()) gConfig.thresh_b = doc["thresh_b"];
            if (doc["gamma_enable"].is<bool>()) gConfig.gamma_enable = doc["gamma_enable"];
            if (doc["dac_limit_min"].is<int>() && doc["dac_limit_max"].is<int>()) {
                int lo = constrain((int)doc["dac_limit_min"], 0, 65535);
                int hi = constrain((int)doc["dac_limit_max"], 0, 65535);
                if (lo < hi) {
                    gConfig.dac_limit_min = (uint16_t)lo;
                    gConfig.dac_limit_max = (uint16_t)hi;
                }
            }
            if (doc["output_scale"].is<float>()) {
                gConfig.outputScale = constrain((float)doc["output_scale"], 0.5f, 1.0f);
            }
            } // LOCK_CONFIG
            req->send(200, "text/plain", "OK");
        });

    // ---- GET /api/optimizer-stats ---- (read-only telemetry)
    // Registered ahead of the other /api/optimizer* routes so a future
    // prefix-matching handler on that stem cannot swallow it.
    s_server.on("/api/optimizer-stats", HTTP_GET, [](AsyncWebServerRequest* req) {
        // Snapshot both records before serializing: the pattern task on core 1
        // keeps writing them while this handler runs on core 0, and a struct
        // copy at least keeps the numbers of one response self-consistent.
        optimizer::Stats last  = optimizer::gLastStats;
        optimizer::Stats frame = optimizer::gFrameStats;
        JsonDocument doc(&jsonAllocator());
        JsonObject l = doc["last"].to<JsonObject>();
        fillOptimizerStats(l, last);
        JsonObject f = doc["frame"].to<JsonObject>();
        fillOptimizerStats(f, frame);
        sendJsonPsram(req, doc);
    });

    // ---- POST /api/optimizer-profile-switch ---- (switch active profile)
    s_server.on("/api/optimizer-profile-switch", HTTP_POST,
        [](AsyncWebServerRequest* req) {},
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            JsonDocument doc(&jsonAllocator());
            if (deserializeJson(doc, data, len)) { req->send(400, "text/plain", "bad json"); return; }
            int prof = doc["profile"] | -1;
            if (prof < 0 || prof >= OPT_PROFILE_COUNT) { req->send(400, "text/plain", "bad profile"); return; }
            gActiveOptimizerProfile = (uint8_t)prof;
            syncOptimizerConfig();
            gPatternCacheGen++;
            req->send(200, "text/plain", "OK");
        });

    // ---- POST /api/optimizer-live ---- (apply immediately, no persist)
    s_server.on("/api/optimizer-live", HTTP_POST,
        [](AsyncWebServerRequest* req) {},
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            JsonDocument doc(&jsonAllocator());
            if (deserializeJson(doc, data, len)) { req->send(400, "text/plain", "bad json"); return; }
            uint8_t targetProf = gActiveOptimizerProfile;
            if (doc["profile"].is<int>()) { int pr=(int)doc["profile"]; if(pr>=0&&pr<OPT_PROFILE_COUNT) targetProf=(uint8_t)pr; }
            OptimizerLiveConfig& P = gOptimizerProfiles[targetProf];
            if (doc["corner_angle_deg"].is<float>())
                P.corner_angle_deg = constrain((float)doc["corner_angle_deg"], 0.0f, 180.0f);
            if (doc["min_corner_pts"].is<int>())
                P.min_corner_pts = constrain((int)doc["min_corner_pts"], 1, 20);
            if (doc["max_corner_pts"].is<int>())
                P.max_corner_pts = constrain((int)doc["max_corner_pts"], 1, 20);
            if (doc["pts_per_1000_units"].is<float>())
                P.pts_per_1000_units = constrain((float)doc["pts_per_1000_units"], 0.1f, 50.0f);
            if (doc["blank_samples"].is<int>())
                P.blank_samples = constrain((int)doc["blank_samples"], 1, 100);
            if (doc["max_pts_per_frame"].is<int>())
                P.max_pts_per_frame = constrain((int)doc["max_pts_per_frame"], 50, (int)PATTERN_POINTS_MAX);
            if (doc["min_blank_samples"].is<int>())
                P.min_blank_samples = constrain((int)doc["min_blank_samples"], 1, 100);
            if (doc["blank_pts_per_1000_units"].is<float>())
                P.blank_pts_per_1000_units = constrain((float)doc["blank_pts_per_1000_units"], 0.1f, 50.0f);
            if (doc["min_interior_pts_per_segment"].is<int>())
                P.min_interior_pts_per_segment = constrain((int)doc["min_interior_pts_per_segment"], 0, 50);
            if (doc["stage1_blank_target"].is<int>())
                P.stage1_blank_target = constrain((int)doc["stage1_blank_target"], 1, 100);
            if (doc["resample_enabled"].is<bool>())
                P.resample_enabled = (bool)doc["resample_enabled"];
            if (doc["resample_spacing_units"].is<float>())
                P.resample_spacing_units = constrain((float)doc["resample_spacing_units"], 10.0f, 2000.0f);
            if (doc["curvature_resample_enabled"].is<bool>())
                P.curvature_resample_enabled = (bool)doc["curvature_resample_enabled"];
            if (doc["curvature_gain"].is<float>())
                P.curvature_gain = constrain((float)doc["curvature_gain"], 0.0f, 20.0f);
            if (doc["min_spacing_units"].is<float>())
                P.min_spacing_units = constrain((float)doc["min_spacing_units"], 1.0f, 2000.0f);
            if (doc["max_spacing_units"].is<float>())
                P.max_spacing_units = constrain((float)doc["max_spacing_units"], 1.0f, 4000.0f);
            if (doc["ringing_comp_enabled"].is<bool>())
                P.ringing_comp_enabled = (bool)doc["ringing_comp_enabled"];
            if (doc["ring_freq_hz"].is<float>())
                P.ring_freq_hz = constrain((float)doc["ring_freq_hz"], 1.0f, 2000.0f);
            if (doc["ring_damping_ratio"].is<float>())
                P.ring_damping_ratio = constrain((float)doc["ring_damping_ratio"], 0.0f, 0.9f);
            if (doc["jitter_enabled"].is<bool>())
                P.jitter_enabled = (bool)doc["jitter_enabled"];
            if (doc["jitter_amount_units"].is<float>())
                P.jitter_amount_units = constrain((float)doc["jitter_amount_units"], 0.0f, 2000.0f);
            if (doc["vel_clamp_enabled"].is<bool>())
                P.vel_clamp_enabled = (bool)doc["vel_clamp_enabled"];
            if (doc["max_step_units"].is<float>())
                P.max_step_units = constrain((float)doc["max_step_units"], 50.0f, 32767.0f);
            if (doc["accel_clamp_enabled"].is<bool>())
                P.accel_clamp_enabled = (bool)doc["accel_clamp_enabled"];
            if (doc["max_accel_units"].is<float>())
                P.max_accel_units = constrain((float)doc["max_accel_units"], 10.0f, 32767.0f);
            if (doc["reorder_segments"].is<bool>())
                P.reorder_segments = (bool)doc["reorder_segments"];
            if (doc["reorder_2opt"].is<bool>())
                P.reorder_2opt = (bool)doc["reorder_2opt"];
            // Every field above clamps its own range independently -- catch an
            // inverted min/max pair (see normalizeOptimizerConfig()'s doc comment
            // in config.h) before it reaches the optimizer.
            OptimizerNormalizeResult norm = normalizeOptimizerConfig(P);
            if (targetProf == gActiveOptimizerProfile) syncOptimizerConfig();
            gPatternCacheGen++;
            if (norm.any()) {
                JsonDocument resp(&jsonAllocator());
                resp["ok"] = true;
                JsonObject corrected = resp["corrected"].to<JsonObject>();
                if (norm.min_blank_samples_corrected) corrected["min_blank_samples"] = P.min_blank_samples;
                if (norm.min_corner_pts_corrected)    corrected["min_corner_pts"]    = P.min_corner_pts;
                sendJsonPsram(req, resp);
            } else {
                req->send(200, "text/plain", "OK");
            }
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
                // zone_outline is DISPATCH index 4 (see calib_patterns.h's
                // pattern list) -- was previously (and incorrectly)
                // `CALIB_PATTERN_COUNT - 1`, which drifts to whatever pattern
                // happens to be appended last (most recently: cam_spiral,
                // and now warp_test_grid) instead of staying pinned to
                // zone_outline. Fixed as a direct consequence of Prompt 7a
                // appending a new pattern (idx 17) to this same table.
                gState.calib_idx     = 4;  // zone_outline
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

    // ── Camera Closed-Loop Keystone (Prompt 7a) ──────────────────────
    // NOTE: specific routes registered before the /api/warp prefix would
    // matter for -- there are none here, all four routes have distinct full
    // paths, so ordering is not load-bearing the way /calib-pattern/stop vs
    // /calib-pattern is.

    // ---- GET /api/warp/get ----
    s_server.on("/api/warp/get", HTTP_GET, [](AsyncWebServerRequest* req) {
        JsonDocument doc(&jsonAllocator());
        doc["enabled"]  = gWarp.enabled;
        doc["gridSize"] = gWarp.gridSize;
        JsonArray rows = doc["points"].to<JsonArray>();
        for (uint8_t r = 0; r < gWarp.gridSize; r++) {
            JsonArray row = rows.add<JsonArray>();
            for (uint8_t c = 0; c < gWarp.gridSize; c++) {
                JsonArray pt = row.add<JsonArray>();
                pt.add(gWarp.points[r][c][0]);
                pt.add(gWarp.points[r][c][1]);
            }
        }
        sendJsonPsram(req, doc);
    });

    // ---- POST /api/warp/set ----
    // Body: { "gridSize": 2..5, "points": [[[x,y],...],...], "enabled": bool }
    // All fields optional; "points", if present, must be a gridSize x
    // gridSize array of [x,y] pairs (using the request's own "gridSize" if
    // given, else the currently stored one) with x,y in [-1.5..1.5]. Whole
    // payload is validated before anything is applied -- no partial writes
    // on a bad request. Changing gridSize WITHOUT sending a matching
    // "points" array resets the grid to identity for the new size, rather
    // than leaving stale control points computed for the old size.
    s_server.on("/api/warp/set", HTTP_POST,
        [](AsyncWebServerRequest* req) {},
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            JsonDocument doc(&jsonAllocator());
            if (deserializeJson(doc, data, len)) { req->send(400, "text/plain", "bad json"); return; }

            uint8_t gridSize = gWarp.gridSize;
            if (!doc["gridSize"].isNull()) {
                int gs = doc["gridSize"].is<int>() ? (int)doc["gridSize"] : -1;
                if (gs < 2 || gs > WARP_GRID_MAX) {
                    req->send(400, "application/json",
                        "{\"error\":\"gridSize must be an integer 2..5\"}");
                    return;
                }
                gridSize = (uint8_t)gs;
            }

            float newPts[WARP_GRID_MAX][WARP_GRID_MAX][2];
            bool havePts = !doc["points"].isNull();
            if (havePts) {
                JsonArrayConst rows = doc["points"];
                if (rows.size() != gridSize) {
                    req->send(400, "application/json",
                        "{\"error\":\"points must have gridSize rows\"}");
                    return;
                }
                uint8_t r = 0;
                for (JsonArrayConst row : rows) {
                    if (row.size() != gridSize) {
                        req->send(400, "application/json",
                            "{\"error\":\"each points row must have gridSize entries\"}");
                        return;
                    }
                    uint8_t c = 0;
                    for (JsonArrayConst pt : row) {
                        float px = pt[0].is<float>() ? pt[0].as<float>() : 999.0f;
                        float py = pt[1].is<float>() ? pt[1].as<float>() : 999.0f;
                        if (pt.size() != 2 ||
                            px < -1.5f || px > 1.5f || py < -1.5f || py > 1.5f) {
                            req->send(400, "application/json",
                                "{\"error\":\"each point must be [x,y] with x,y in -1.5..1.5\"}");
                            return;
                        }
                        newPts[r][c][0] = px;
                        newPts[r][c][1] = py;
                        c++;
                    }
                    r++;
                }
            }

            gWarp.gridSize = gridSize;
            if (havePts) {
                for (uint8_t r = 0; r < gridSize; r++)
                    for (uint8_t c = 0; c < gridSize; c++) {
                        gWarp.points[r][c][0] = newPts[r][c][0];
                        gWarp.points[r][c][1] = newPts[r][c][1];
                    }
            } else {
                gWarp.resetIdentity();
            }
            if (doc["enabled"].is<bool>()) gWarp.enabled = doc["enabled"];
            warp::refresh();
            persistConfig();
            req->send(200, "text/plain", "OK");
        });

    // ---- POST /api/warp/reset ---- (grid back to identity, enabled untouched)
    s_server.on("/api/warp/reset", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            warp::reset();
            persistConfig();
            req->send(200, "text/plain", "OK");
        });

    // ---- POST /api/warp/test ---- { "active": bool }
    // Toggles the WARP_GRID_TEST calibration pattern (border + gWarp.gridSize
    // interior lines, calib_patterns.cpp) -- same gState.calib_* mechanism as
    // /api/calib-pattern.
    s_server.on("/api/warp/test", HTTP_POST,
        [](AsyncWebServerRequest* req) {},
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            JsonDocument doc(&jsonAllocator());
            if (deserializeJson(doc, data, len)) { req->send(400, "text/plain", "bad json"); return; }
            bool active = doc["active"] | false;
            if (active) {
                ilda::stop();
                gTextConfig.active   = false;
                gState.calib_idx     = calib_patterns::CALIB_WARP_GRID_IDX;
                gState.calib_bright  = 200;
                gState.calib_channel = 0;
                gState.calib_no_thresh = false;
                gState.calib_raw_duty  = false;
                const uint8_t prof = calib_patterns::profileOf(gState.calib_idx);
                if (prof != gActiveOptimizerProfile) {
                    gActiveOptimizerProfile = prof;
                    syncOptimizerConfig();
                    gPatternCacheGen++;
                }
                if (gState.ui_master_dimmer.load() < 200) gState.ui_master_dimmer.store(200);
                gState.calib_active = true;
            } else {
                gState.calib_active = false;
            }
            req->send(200, "text/plain", active ? "TEST" : "STOP");
        });

    // ── Per-Segment Brightness Compensation (Prompt 7c) ──────────────────
    // Same grid shape/validation conventions as /api/warp/* above -- no
    // "test" pattern for this one (the prompt doesn't ask for one; the
    // existing calib patterns already double as a live-brightness preview
    // since the gain field applies to every pattern, not just a dedicated
    // test shape).

    // ---- GET /api/brightness/get ----
    s_server.on("/api/brightness/get", HTTP_GET, [](AsyncWebServerRequest* req) {
        JsonDocument doc(&jsonAllocator());
        doc["enabled"]  = gBrightness.enabled;
        doc["gridSize"] = gBrightness.gridSize;
        JsonArray rows = doc["gain"].to<JsonArray>();
        for (uint8_t r = 0; r < gBrightness.gridSize; r++) {
            JsonArray row = rows.add<JsonArray>();
            for (uint8_t c = 0; c < gBrightness.gridSize; c++)
                row.add(gBrightness.gain[r][c]);
        }
        sendJsonPsram(req, doc);
    });

    // ---- POST /api/brightness/set ----
    // Body: { "gridSize": 2..5, "gain": [[0..255,...],...], "enabled": bool }
    // All fields optional; "gain", if present, must be a gridSize x gridSize
    // array of 0..255 integers (using the request's own "gridSize" if given,
    // else the currently stored one). Whole payload validated before
    // anything is applied. Changing gridSize WITHOUT sending a matching
    // "gain" array resets the grid to identity (255) for the new size.
    s_server.on("/api/brightness/set", HTTP_POST,
        [](AsyncWebServerRequest* req) {},
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            JsonDocument doc(&jsonAllocator());
            if (deserializeJson(doc, data, len)) { req->send(400, "text/plain", "bad json"); return; }

            uint8_t gridSize = gBrightness.gridSize;
            if (!doc["gridSize"].isNull()) {
                int gs = doc["gridSize"].is<int>() ? (int)doc["gridSize"] : -1;
                if (gs < 2 || gs > WARP_GRID_MAX) {
                    req->send(400, "application/json",
                        "{\"error\":\"gridSize must be an integer 2..5\"}");
                    return;
                }
                gridSize = (uint8_t)gs;
            }

            uint8_t newGain[WARP_GRID_MAX][WARP_GRID_MAX];
            bool haveGain = !doc["gain"].isNull();
            if (haveGain) {
                JsonArrayConst rows = doc["gain"];
                if (rows.size() != gridSize) {
                    req->send(400, "application/json",
                        "{\"error\":\"gain must have gridSize rows\"}");
                    return;
                }
                uint8_t r = 0;
                for (JsonArrayConst row : rows) {
                    if (row.size() != gridSize) {
                        req->send(400, "application/json",
                            "{\"error\":\"each gain row must have gridSize entries\"}");
                        return;
                    }
                    uint8_t c = 0;
                    for (JsonVariantConst cell : row) {
                        int v = cell.is<int>() ? (int)cell : -1;
                        if (v < 0 || v > 255) {
                            req->send(400, "application/json",
                                "{\"error\":\"each gain value must be an integer 0..255\"}");
                            return;
                        }
                        newGain[r][c] = (uint8_t)v;
                        c++;
                    }
                    r++;
                }
            }

            gBrightness.gridSize = gridSize;
            if (haveGain) {
                for (uint8_t r = 0; r < gridSize; r++)
                    for (uint8_t c = 0; c < gridSize; c++)
                        gBrightness.gain[r][c] = newGain[r][c];
            } else {
                gBrightness.resetIdentity();
            }
            if (doc["enabled"].is<bool>()) gBrightness.enabled = doc["enabled"];
            brightness::refresh();
            persistConfig();
            req->send(200, "text/plain", "OK");
        });

    // ---- POST /api/brightness/reset ---- (grid back to identity, enabled untouched)
    s_server.on("/api/brightness/reset", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            brightness::reset();
            persistConfig();
            req->send(200, "text/plain", "OK");
        });

    // ---- GET /api/inverse-filter/get ----
    s_server.on("/api/inverse-filter/get", HTTP_GET, [](AsyncWebServerRequest* req) {
        JsonDocument doc(&jsonAllocator());
        doc["enabled"]  = gInverseFilter.enabled;
        doc["regAlpha"] = gInverseFilter.regAlpha;
        doc["active"]   = invfilter::isActive();
        JsonObject x = doc["x"].to<JsonObject>();
        x["wnHz"] = gInverseFilter.x.wnHz;
        x["zeta"] = gInverseFilter.x.zeta;
        JsonObject y = doc["y"].to<JsonObject>();
        y["wnHz"] = gInverseFilter.y.wnHz;
        y["zeta"] = gInverseFilter.y.zeta;
        sendJsonPsram(req, doc);
    });

    // ---- POST /api/inverse-filter/set ----
    // Body: { "enabled": bool, "regAlpha": float, "x": {"wnHz","zeta"},
    //         "y": {"wnHz","zeta"} } -- all fields optional. Whole payload
    // validated before anything is applied. wnHz=0 means "unmeasured" for
    // that axis (passed through unfiltered even when enabled). Never
    // auto-applied by the calibration tooling -- see docs/feature-prompts/
    // DECISIONS.md, Prompt 12b.
    s_server.on("/api/inverse-filter/set", HTTP_POST,
        [](AsyncWebServerRequest* req) {},
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            JsonDocument doc(&jsonAllocator());
            if (deserializeJson(doc, data, len)) { req->send(400, "text/plain", "bad json"); return; }

            float regAlpha = gInverseFilter.regAlpha;
            if (!doc["regAlpha"].isNull()) {
                if (!doc["regAlpha"].is<float>() || (float)doc["regAlpha"] <= 0.0f) {
                    req->send(400, "application/json",
                        "{\"error\":\"regAlpha must be a positive number\"}");
                    return;
                }
                regAlpha = doc["regAlpha"];
            }

            InverseFilterAxisModel newX = gInverseFilter.x, newY = gInverseFilter.y;
            auto parseAxis = [&](JsonVariantConst obj, InverseFilterAxisModel& out) -> const char* {
                if (obj.isNull()) return nullptr;
                if (!obj["wnHz"].isNull()) {
                    if (!obj["wnHz"].is<float>() || (float)obj["wnHz"] < 0.0f)
                        return "wnHz must be a non-negative number";
                    out.wnHz = obj["wnHz"];
                }
                if (!obj["zeta"].isNull()) {
                    if (!obj["zeta"].is<float>()) return "zeta must be a number";
                    out.zeta = constrain((float)obj["zeta"], 0.0f, 0.9f);
                }
                return nullptr;
            };
            if (const char* err = parseAxis(doc["x"], newX)) {
                char buf[64]; snprintf(buf, sizeof(buf), "{\"error\":\"x.%s\"}", err);
                req->send(400, "application/json", buf);
                return;
            }
            if (const char* err = parseAxis(doc["y"], newY)) {
                char buf[64]; snprintf(buf, sizeof(buf), "{\"error\":\"y.%s\"}", err);
                req->send(400, "application/json", buf);
                return;
            }

            gInverseFilter.regAlpha = regAlpha;
            gInverseFilter.x = newX;
            gInverseFilter.y = newY;
            if (doc["enabled"].is<bool>()) gInverseFilter.enabled = doc["enabled"];
            invfilter::refresh((uint32_t)gProjection.galvo_kpps * 1000);
            gPatternCacheGen++;
            persistConfig();
            req->send(200, "text/plain", "OK");
        });

    // ---- POST /api/inverse-filter/reset ---- (both axes back to
    // "unmeasured", enabled/regAlpha untouched)
    s_server.on("/api/inverse-filter/reset", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            invfilter::reset();
            gPatternCacheGen++;
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

    // ---- POST /api/calib-thresh-test {"active":true,"channel":0-3} ----
    // Basiswert test beam: static, minimal-level beam with gain/gamma/
    // dimmer bypassed (see galvo_out.cpp galvoTask()), so the Base R/G/B
    // sliders in the Parameter card have a direct, unmasked effect.
    s_server.on("/api/calib-thresh-test", HTTP_POST,
        [](AsyncWebServerRequest* req) {},
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            JsonDocument doc(&jsonAllocator());
            if (deserializeJson(doc, data, len)) { req->send(400, "text/plain", "bad json"); return; }
            if (doc["channel"].is<int>())
                gState.calib_thresh_ch = constrain((int)doc["channel"], 0, 3);
            gState.calib_thresh_test = doc["active"] | false;
            req->send(200, "text/plain", "OK");
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
            int idx = doc["idx"] | -1;
            patterns::setPreset(presets::presetFromIndex(idx));
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

    // ---- POST /api/temp-unit ---- WebUI display unit (0=C, 1=F, 2=K)
    s_server.on("/api/temp-unit", HTTP_POST,
        [](AsyncWebServerRequest* req) {},
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            JsonDocument doc(&jsonAllocator());
            if (deserializeJson(doc, data, len)) { req->send(400, "text/plain", "bad json"); return; }
            if (doc["unit"].is<int>()) temp::setDisplayUnit((uint8_t)constrain((int)doc["unit"], 0, 2));
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
            if (doc["col_anim_bpm_sync"].is<bool>()) gLivePreset.col_anim_bpm_sync = doc["col_anim_bpm_sync"];
            if (doc["col_seg_count"].is<int>())  gLivePreset.col_seg_count  = (uint8_t)(int)doc["col_seg_count"];
            if (doc["col_seg_dir"].is<int>())    gLivePreset.col_seg_dir    = (int8_t)(int)doc["col_seg_dir"];
            if (doc["rotation"].is<int>())     gLivePreset.rotation    = (int16_t)(int)doc["rotation"];
            if (doc["kaleido_enabled"].is<bool>())  gLivePreset.kaleido_enabled  = doc["kaleido_enabled"];
            if (doc["kaleido_mode"].is<int>())      gLivePreset.kaleido_mode     = (uint8_t)constrain((int)doc["kaleido_mode"], 0, 1);
            if (doc["kaleido_segments"].is<int>()) {
                uint8_t v = (uint8_t)constrain((int)doc["kaleido_segments"], 2, KALEIDO_SEGMENTS_MAX);
                v &= ~(uint8_t)1;             // even only -- true kaleidoscope fold requires it
                if (v < 2) v = 2;
                gLivePreset.kaleido_segments = v;
            }
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
            // Points-Only mode renders dwelling dots, not lines -- run it
            // through the Particles profile (no corner dwell, tuned for
            // fast/accurate blank jumps) regardless of the active preset's
            // own class, mirroring how Text mode forces OPT_PROFILE_TEXT.
            if (gLivePreset.points_mode_enabled && gActiveOptimizerProfile != OPT_PROFILE_PARTICLES) {
                gActiveOptimizerProfile = OPT_PROFILE_PARTICLES;
                syncOptimizerConfig();
                gPatternCacheGen++;
            }
            if (doc["points_count"].is<int>())  gLivePreset.points_count  = (uint8_t)constrain((int)doc["points_count"], 2, POINTS_MODE_MAX_DOTS);
            if (doc["points_fade_in_on"].is<bool>())  gLivePreset.points_fade_in_on  = doc["points_fade_in_on"];
            if (doc["points_fade_out_on"].is<bool>()) gLivePreset.points_fade_out_on = doc["points_fade_out_on"];
            if (doc["points_fade_in_ms"].is<int>())   gLivePreset.points_fade_in_ms  = (uint16_t)constrain((int)doc["points_fade_in_ms"], 0, 10000);
            if (doc["points_fade_out_ms"].is<int>())  gLivePreset.points_fade_out_ms = (uint16_t)constrain((int)doc["points_fade_out_ms"], 0, 10000);
            if (doc["points_fade_dir"].is<int>())     gLivePreset.points_fade_dir    = (uint8_t)constrain((int)doc["points_fade_dir"], 0, 5);
            if (doc["points_static_on"].is<bool>())   gLivePreset.points_static_on   = doc["points_static_on"];
            if (doc["points_bpm_sync"].is<bool>())    gLivePreset.points_bpm_sync    = doc["points_bpm_sync"];
            if (doc["random_pts_hold_ms"].is<int>())  gLivePreset.random_pts_hold_ms = (uint16_t)constrain((int)doc["random_pts_hold_ms"], 50, 5000);
            if (doc["bp_trail_len"].is<int>())    gLivePreset.bp_trail_len    = (uint8_t)constrain((int)doc["bp_trail_len"], 0, 12);
            if (doc["bp_endless"].is<bool>())     gLivePreset.bp_endless      = doc["bp_endless"];
            if (doc["bp_duration_sec"].is<int>()) gLivePreset.bp_duration_sec = (uint16_t)constrain((int)doc["bp_duration_sec"], 1, 90);
            if (doc["spiral_arms"].is<int>())     gLivePreset.spiral_arms     = (uint8_t)constrain((int)doc["spiral_arms"], 1, 6);
            if (doc["tunnel_rings"].is<int>())    gLivePreset.tunnel_rings    = (uint8_t)constrain((int)doc["tunnel_rings"], 3, 12);
            if (doc["tunnel_sides"].is<int>())    gLivePreset.tunnel_sides    = (uint8_t)constrain((int)doc["tunnel_sides"], 3, 10);
            if (doc["explosion_rays"].is<int>())  gLivePreset.explosion_rays  = (uint8_t)constrain((int)doc["explosion_rays"], 4, 40);
            if (doc["fw_max_shells"].is<int>())   gLivePreset.fw_max_shells   = (uint8_t)constrain((int)doc["fw_max_shells"], 1, 3);
            if (doc["fw_glitter"].is<bool>())     gLivePreset.fw_glitter      = doc["fw_glitter"];
            if (doc["mw_dots"].is<int>())         gLivePreset.mw_dots         = (uint8_t)constrain((int)doc["mw_dots"], 10, 60);
            if (doc["mw_tilt"].is<int>())         gLivePreset.mw_tilt         = (uint8_t)constrain((int)doc["mw_tilt"], 20, 80);
            if (doc["hline_bounce"].is<bool>())   gLivePreset.hline_bounce    = doc["hline_bounce"];
            req->send(200, "text/plain", "OK");
        });

    // ---- GET/POST /api/seg_colors ---- per-segment colors for line-based presets
    s_server.on("/api/seg_colors", HTTP_GET, [](AsyncWebServerRequest* req) {
        JsonDocument doc(&jsonAllocator());
        doc["enabled"] = (bool)gLivePreset.seg_colors_enabled;
        JsonArray r = doc["r"].to<JsonArray>();
        JsonArray g = doc["g"].to<JsonArray>();
        JsonArray b = doc["b"].to<JsonArray>();
        for (int i = 0; i < 10; i++) {
            r.add(gLivePreset.seg_col_r[i]);
            g.add(gLivePreset.seg_col_g[i]);
            b.add(gLivePreset.seg_col_b[i]);
        }
        sendJsonPsram(req, doc);
    });
    s_server.on("/api/seg_colors", HTTP_POST,
        [](AsyncWebServerRequest* req) {},
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            JsonDocument doc(&jsonAllocator());
            if (deserializeJson(doc, data, len)) { req->send(400, "text/plain", "bad json"); return; }
            if (doc["enabled"].is<bool>())
                gLivePreset.seg_colors_enabled = doc["enabled"];
            if (doc["r"].is<JsonArray>() && doc["g"].is<JsonArray>() && doc["b"].is<JsonArray>()) {
                JsonArray ra = doc["r"].as<JsonArray>();
                JsonArray ga = doc["g"].as<JsonArray>();
                JsonArray ba = doc["b"].as<JsonArray>();
                for (size_t i = 0; i < 10 && i < ra.size(); i++) {
                    gLivePreset.seg_col_r[i] = (uint8_t)constrain((int)ra[i], 0, 255);
                    gLivePreset.seg_col_g[i] = (uint8_t)constrain((int)ga[i], 0, 255);
                    gLivePreset.seg_col_b[i] = (uint8_t)constrain((int)ba[i], 0, 255);
                }
            }
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
                if (doc["orbit_reverse"].is<bool>()) gTextConfig.orbit_reverse = doc["orbit_reverse"];
                if (doc["active"].is<bool>())        gTextConfig.active    = doc["active"];
                // Text has its own optimizer profile (blank-jump-dominated,
                // many short glyph strokes -- see OPT_PROFILE_TEXT in
                // config.h), mirroring how setPreset() switches profile to
                // match presetClassOf(). Text mode sits outside the preset
                // system, so it has to switch here instead.
                if (gTextConfig.active && gActiveOptimizerProfile != OPT_PROFILE_TEXT) {
                    gActiveOptimizerProfile = OPT_PROFILE_TEXT;
                    syncOptimizerConfig();
                    gPatternCacheGen++;
                }
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
        doc["orbit_reverse"] = gTextConfig.orbit_reverse;
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
            if (index == 0) {
                if (s_paint_body_owner && s_paint_body_owner != req) {
                    // A different upload is already mid-flight -- refuse
                    // instead of resetting its buffer out from under it.
                    req->send(503, "text/plain", "paint upload busy, retry");
                    return;
                }
                s_paint_body_owner = req;
                s_paint_body_len   = 0;
                req->onDisconnect([]() { s_paint_body_owner = nullptr; });
            } else if (s_paint_body_owner != req) {
                return; // stale chunk for a request we already rejected/finished
            }
            if (total >= PAINT_BODY_CAP) {
                req->send(400, "text/plain", "body too large");
                s_paint_body_owner = nullptr;
                return;
            }
            if (s_paint_body_len + len < PAINT_BODY_CAP) {
                memcpy(s_paint_body + s_paint_body_len, data, len);
                s_paint_body_len += len;
            }
            if (index + len != total) return;
            s_paint_body[s_paint_body_len] = 0;
            JsonDocument doc(&jsonAllocator());
            DeserializationError jerr = deserializeJson(doc, s_paint_body, s_paint_body_len);
            // Buffer is fully consumed by this point (in-place parse or not) --
            // free it for the next request before taking any early return.
            s_paint_body_owner = nullptr;
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

    // ---- POST /api/weld/set ---- Laser Welding effect, partial patch ----
    // Registered BEFORE the prefix-matching GET /api/weld below (route-ordering
    // rule). Body: {enabled?, direction?, speed?, glow?, sparks?, spark_life?}.
    s_server.on("/api/weld/set", HTTP_POST,
        [](AsyncWebServerRequest* req) {},
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t index, size_t total) {
            if (index != 0 || len != total) { req->send(400, "text/plain", "chunked body unsupported"); return; }
            JsonDocument doc(&jsonAllocator());
            if (deserializeJson(doc, data, len)) { req->send(400, "text/plain", "bad json"); return; }
            if (doc["enabled"].is<bool>()) {
                bool en = doc["enabled"].as<bool>();
                if (en && !gWeld.enabled) weld::reset();   // false -> true: fresh run
                gWeld.enabled = en;
            }
            if (doc["direction"].is<int>()) {
                uint8_t dir = (uint8_t)constrain(doc["direction"].as<int>(), 0, 2);
                gWeld.direction = dir;
                if (dir == WELD_DIR_REVERSE) weld::seekEnd();   // start at the far end
            }
            if (doc["speed"].is<int>())      gWeld.speed_units   = (uint16_t)constrain(doc["speed"].as<int>(), 200, 40000);
            if (doc["glow"].is<int>())       gWeld.glow_units    = (uint16_t)constrain(doc["glow"].as<int>(), 200, 20000);
            if (doc["sparks"].is<int>())     gWeld.spark_count   = (uint8_t)constrain(doc["sparks"].as<int>(), WELD_SPARK_COUNT_MIN, WELD_SPARK_COUNT_MAX);
            if (doc["spark_life"].is<int>()) gWeld.spark_life_ms = (uint16_t)constrain(doc["spark_life"].as<int>(), 40, 1200);
            req->send(200, "text/plain", "OK");
        });

    // ---- GET /api/weld ---- current welding settings (reload / multi-client) ----
    s_server.on("/api/weld", HTTP_GET, [](AsyncWebServerRequest* req) {
        JsonDocument doc(&jsonAllocator());
        doc["enabled"]    = (bool)gWeld.enabled;
        doc["direction"]  = (int)gWeld.direction;
        doc["speed"]      = (int)gWeld.speed_units;
        doc["glow"]       = (int)gWeld.glow_units;
        doc["sparks"]     = (int)gWeld.spark_count;
        doc["spark_life"] = (int)gWeld.spark_life_ms;
        sendJsonPsram(req, doc);
    });

    // ---- GET /api/sd/info ---- detailed SD card info ----
    // Registered *before* the plainer "/api/sd" route below: ESPAsyncWebServer
    // matches plain-string routes by prefix, not exact-match, so "/api/sd"
    // would otherwise swallow every request to "/api/sd/info" (its handler
    // never even runs) -- this silently fed the WebUI the file-listing JSON
    // (no "fs_type" key) whenever it asked for the info JSON, showing
    // "undefined" in the SD status line. See CLAUDE.md's route-ordering rule.
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

    // ---- GET /api/sd/download?idx=N ---- download an ILDA file by index ----
    // Registered before the plain "/api/sd" route below for the same reason
    // as "/api/sd/info" above -- it's a longer sibling of that prefix-matching
    // route and would otherwise never be reached.
    s_server.on("/api/sd/download", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (!req->hasParam("idx")) { req->send(400, "text/plain", "idx required"); return; }
        uint8_t idx = (uint8_t)req->getParam("idx")->value().toInt();
        if (idx >= sd_card::fileCount()) { req->send(404, "text/plain", "not found"); return; }
        // AsyncFileResponse extracts the download filename from the path's own
        // basename, so it always names the file correctly even for entries in
        // subfolders (sd_card::fileName() would include the subfolder prefix).
        AsyncWebServerResponse* resp = req->beginResponse(SD, sd_card::filePath(idx), "application/octet-stream", true);
        if (!resp) { req->send(404, "text/plain", "file missing on SD"); return; }
        req->send(resp);
    });

    // ---- GET /api/sd ---- SD cardn-Status and file list
    s_server.on("/api/sd", HTTP_GET, [](AsyncWebServerRequest* req) {
        JsonDocument doc(&jsonAllocator());
        doc["ready"]     = sd_card::isReady();
        doc["file_count"]= sd_card::fileCount();
        doc["free_kb"]   = sd_card::freeKB();
        doc["total_kb"]  = sd_card::totalKB();

        // loadILDA() needs sizeof(LaserPoint)==8 bytes of PSRAM per point,
        // plus whatever else is already resident (WebUI buffers, WiFi scan
        // cache, ...). We don't know a file's point format without parsing
        // it, so assume the worst case (format 1, 2D indexed = 6 bytes/pt,
        // the smallest on-disk point size -> the most points per byte) to
        // get an upper bound on the PSRAM a file could require:
        //   required ~= file_size * (sizeof(LaserPoint) / 6)
        // Reserve 1MB of headroom below the actual free PSRAM for other
        // allocations that happen concurrently with a load.
        constexpr size_t kPsramReserve   = 1024 * 1024;
        constexpr size_t kWorstPtSize    = 6;
        size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        size_t budget      = (free_psram > kPsramReserve) ? (free_psram - kPsramReserve) : 0;
        size_t max_ilda_bytes = budget * kWorstPtSize / sizeof(LaserPoint);
        doc["ilda_max_kb"] = max_ilda_bytes / 1024;

        JsonArray files  = doc["files"].to<JsonArray>();
        for (uint8_t i = 0; i < sd_card::fileCount(); i++) {
            JsonObject fo = files.add<JsonObject>();
            fo["idx"]   = i;
            fo["name"]  = sd_card::fileName(i);
            fo["path"]  = sd_card::filePath(i);
            fo["size"]  = sd_card::fileSize(i);
            fo["mtime"] = sd_card::fileMTime(i);
            fo["too_large"] = sd_card::fileSize(i) > max_ilda_bytes;
        }
        doc["ilda_active"]  = ilda::gILDA.active;
        doc["ilda_file"]    = ilda::gILDA.file_idx;
        doc["ilda_frame"]   = ilda::gILDA.current_frame;
        doc["ilda_total"]   = ilda::gILDA.total_frames;
        doc["ilda_points"]  = ilda::gILDA.total_points;
        sendJsonPsram(req, doc);
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

    // ---- POST /api/sd/delete ---- delete an ILDA file by index ----
    s_server.on("/api/sd/delete", HTTP_POST,
        [](AsyncWebServerRequest* req) {},
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            JsonDocument doc(&jsonAllocator());
            if (deserializeJson(doc, data, len)) { req->send(400, "text/plain", "bad json"); return; }
            uint8_t idx = doc["idx"] | 255;
            if (idx == 255) { req->send(400, "text/plain", "idx required"); return; }
            if (ilda::gILDA.active && ilda::gILDA.file_idx == (int8_t)idx) ilda::stop();
            bool ok = sd_card::deleteFile(idx);
            if (ok) sd_card::scanFiles();
            req->send(ok ? 200 : 500, "application/json",
                ok ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"delete failed\"}");
        });

    // ---- POST /api/sd/rename ---- rename an ILDA file by index ----
    s_server.on("/api/sd/rename", HTTP_POST,
        [](AsyncWebServerRequest* req) {},
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            JsonDocument doc(&jsonAllocator());
            if (deserializeJson(doc, data, len)) { req->send(400, "text/plain", "bad json"); return; }
            uint8_t idx = doc["idx"] | 255;
            const char* rawName = doc["name"] | "";
            if (idx == 255 || !rawName[0]) { req->send(400, "text/plain", "idx and name required"); return; }
            String newName = sanitizeIldaFilename(String(rawName));
            if (ilda::gILDA.active && ilda::gILDA.file_idx == (int8_t)idx) ilda::stop();
            bool ok = sd_card::renameFile(idx, newName.c_str());
            if (ok) sd_card::scanFiles();
            req->send(ok ? 200 : 500, "application/json",
                ok ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"rename failed (name taken or SD error)\"}");
        });

    // ---- GET /api/svg/list ---- SVG file list on SD card (/svg/ directory) ----
    // Separate index space from /api/sd's ILDA list -- own endpoints, own
    // playability check (well-formedness + rough element count, not a
    // PSRAM-budget estimate like ILDA's too_large).
    s_server.on("/api/svg/list", HTTP_GET, [](AsyncWebServerRequest* req) {
        JsonDocument doc(&jsonAllocator());
        doc["ready"]      = sd_card::isReady();
        doc["file_count"] = svg_store::fileCount();
        doc["max_bytes"]  = svg_store::maxFileBytes();
        JsonArray files = doc["files"].to<JsonArray>();
        for (uint8_t i = 0; i < svg_store::fileCount(); i++) {
            JsonObject fo = files.add<JsonObject>();
            fo["idx"]      = i;
            fo["name"]     = svg_store::fileName(i);
            fo["size"]     = svg_store::fileSize(i);
            fo["mtime"]    = svg_store::fileMTime(i);
            fo["playable"] = svg_store::playable(i);
            fo["reason"]   = svg_store::reason(i);
        }
        sendJsonPsram(req, doc);
    });

    // ---- GET /api/svg/get?idx=N ---- raw SVG text of a stored file ----
    // Streamed straight from SD (like /api/sd/download) instead of buffered
    // server-side -- no fixed-size body cap needed on this path, the client
    // pipeline is what caps what it will usefully parse (see svg_store's
    // SVG_MAX_FILE_BYTES playability check for the upload/list side).
    s_server.on("/api/svg/get", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (!req->hasParam("idx")) { req->send(400, "text/plain", "idx required"); return; }
        uint8_t idx = (uint8_t)req->getParam("idx")->value().toInt();
        if (idx >= svg_store::fileCount()) { req->send(404, "text/plain", "not found"); return; }
        AsyncWebServerResponse* resp = req->beginResponse(SD, svg_store::filePath(idx), "image/svg+xml", false);
        if (!resp) { req->send(404, "text/plain", "file missing on SD"); return; }
        req->send(resp);
    });

    // ---- POST /api/svg/delete ---- delete an SVG file by index ----
    s_server.on("/api/svg/delete", HTTP_POST,
        [](AsyncWebServerRequest* req) {},
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            JsonDocument doc(&jsonAllocator());
            if (deserializeJson(doc, data, len)) { req->send(400, "text/plain", "bad json"); return; }
            uint8_t idx = doc["idx"] | 255;
            if (idx == 255) { req->send(400, "text/plain", "idx required"); return; }
            bool ok = svg_store::deleteFile(idx);
            if (ok) svg_store::scanFiles();
            req->send(ok ? 200 : 500, "application/json",
                ok ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"delete failed\"}");
        });

    // ---- POST /api/svg/rename ---- rename an SVG file by index ----
    s_server.on("/api/svg/rename", HTTP_POST,
        [](AsyncWebServerRequest* req) {},
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            JsonDocument doc(&jsonAllocator());
            if (deserializeJson(doc, data, len)) { req->send(400, "text/plain", "bad json"); return; }
            uint8_t idx = doc["idx"] | 255;
            const char* rawName = doc["name"] | "";
            if (idx == 255 || !rawName[0]) { req->send(400, "text/plain", "idx and name required"); return; }
            String newName = sanitizeSvgFilename(String(rawName));
            bool ok = svg_store::renameFile(idx, newName.c_str());
            if (ok) svg_store::scanFiles();
            req->send(ok ? 200 : 500, "application/json",
                ok ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"rename failed (name taken or SD error)\"}");
        });

    // ---- POST /api/svg/upload ---- upload an SVG file to the SD card ----
    // Mirrors "Feature 11" ILDA upload below (same chunked-multipart shape).
    s_server.on("/api/svg/upload", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            req->send(s_svg_upload_ok ? 200 : 400, "application/json",
                s_svg_upload_ok ? "{\"status\":\"ok\",\"rescan\":true}"
                                : "{\"error\":\"upload failed (could not create file on SD)\"}");
            if (s_svg_upload_ok) svg_store::scanFiles();
        },
        [](AsyncWebServerRequest* req, String filename, size_t index,
           uint8_t* data, size_t len, bool final) {
            static File s_svg_upload_file;
            if (index == 0) {
                String path = "/svg/" + sanitizeSvgFilename(filename);
                ESP_LOGI("upload", "Start: %s (orig: %s)", path.c_str(), filename.c_str());
                { LOCK_SD();
                  if (!SD.exists("/svg")) SD.mkdir("/svg");   // first upload may race scanFiles()'s own mkdir
                  s_svg_upload_file = SD.open(path, FILE_WRITE);
                }
                s_svg_upload_ok = (bool)s_svg_upload_file;
                if (!s_svg_upload_file) ESP_LOGE("upload", "could not create file: %s", path.c_str());
            }
            if (s_svg_upload_file && len)
                s_svg_upload_file.write(data, len);
            if (final && s_svg_upload_file) {
                s_svg_upload_file.close();
                ESP_LOGI("upload", "Done: %s (%u bytes)", filename.c_str(), index+len);
            }
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
            // speed/size/loop are plain fields on gILDA, untouched by loadFile()/loadILDA(),
            // so applying them up front (rather than after the load finishes) is equivalent.
            if (doc["speed"].is<int>())    ilda::gILDA.speed    = doc["speed"];
            if (doc["size"].is<int>())     ilda::gILDA.size_val = doc["size"];
            if (doc["loop"].is<bool>())    ilda::gILDA.loop     = doc["loop"];
            // Runs the actual SD read/parse on a background task -- see startLoad()'s
            // comment. This handler must return quickly; the frontend polls
            // /api/ilda/status (gILDA.loading / errorMsg()) for the real outcome.
            if (!ilda::startLoad(idx)) {
                JsonDocument err(&jsonAllocator());
                err["ok"]    = false;
                err["error"] = ilda::errorMsg();
                sendJsonPsram(req, err, 409);
                return;
            }
            req->send(202, "text/plain", "loading");
        });

    // ---- POST /api/ilda/param ---- live-update speed/size/loop/color/invert
    // without touching current playback (no reload, no stop) -- applied by
    // getFrame()/ildaTask on the very next frame since they read gILDA directly
    s_server.on("/api/ilda/param", HTTP_POST,
        [](AsyncWebServerRequest* req) {},
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            JsonDocument doc(&jsonAllocator());
            if (deserializeJson(doc, data, len)) { req->send(400, "text/plain", "bad json"); return; }
            if (doc["speed"].is<int>())         ilda::gILDA.speed        = doc["speed"];
            if (doc["size"].is<int>())          ilda::gILDA.size_val     = doc["size"];
            if (doc["loop"].is<bool>())         ilda::gILDA.loop         = doc["loop"];
            if (doc["invert_x"].is<bool>())     ilda::gILDA.invert_x     = doc["invert_x"];
            if (doc["invert_y"].is<bool>())     ilda::gILDA.invert_y     = doc["invert_y"];
            if (doc["col_override"].is<bool>()) ilda::gILDA.col_override = doc["col_override"];
            if (doc["col_r"].is<int>())         ilda::gILDA.col_r        = doc["col_r"];
            if (doc["col_g"].is<int>())         ilda::gILDA.col_g        = doc["col_g"];
            if (doc["col_b"].is<int>())         ilda::gILDA.col_b        = doc["col_b"];
            if (doc["blank_reshape_enabled"].is<bool>())
                ilda::gILDA.blank_reshape_enabled = doc["blank_reshape_enabled"];
            req->send(200, "text/plain", "OK");
        });

    // ---- POST /api/ilda/stop ---- stop ILDA
    s_server.on("/api/ilda/stop", HTTP_POST,
        [](AsyncWebServerRequest* req) { ilda::stop(); req->send(200,"text/plain","OK"); });

    // ---- POST /api/ilda/enable ---- master on/off, disabling also force-stops
    s_server.on("/api/ilda/enable", HTTP_POST,
        [](AsyncWebServerRequest* req) {},
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            JsonDocument doc(&jsonAllocator());
            if (deserializeJson(doc, data, len)) { req->send(400, "text/plain", "bad json"); return; }
            ilda::setEnabled(doc["enabled"] | false);
            req->send(200, "text/plain", "OK");
        });

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
        doc["loading"] = ilda::gILDA.loading;
        doc["error"]   = ilda::errorMsg();  // "OK" when the last load succeeded (or none ran yet)
        doc["enabled"] = ilda::gILDA.enabled;
        doc["paused"]  = ilda::isPaused();
        doc["file_idx"]= ilda::gILDA.file_idx;
        doc["frame"]   = ilda::gILDA.current_frame;
        doc["total"]   = ilda::gILDA.total_frames;
        doc["speed"]   = ilda::gILDA.speed;
        doc["size"]    = ilda::gILDA.size_val;
        doc["loop"]    = ilda::gILDA.loop;
        doc["invert_x"]     = ilda::gILDA.invert_x;
        doc["invert_y"]     = ilda::gILDA.invert_y;
        doc["col_override"] = ilda::gILDA.col_override;
        doc["col_r"]   = ilda::gILDA.col_r;
        doc["col_g"]   = ilda::gILDA.col_g;
        doc["col_b"]   = ilda::gILDA.col_b;
        doc["blank_reshape_enabled"] = ilda::gILDA.blank_reshape_enabled;
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

    // ── Camera-in-the-loop calibration API (/api/calib-cam/*) ────────
    // Registered before /api/calib-pattern/... and well before the
    // serveStatic catch-all (see the ordering-bug note further down) --
    // same ESPAsyncWebServer prefix-matching pitfall as calib-pattern's
    // /stop + /list vs. its own bare route.

    // POST /api/calib-cam/start  {"pattern": "square", "channel": 3}
    // "channel" is optional: 0=white 1=R 2=G 3=B, defaults to 3 (blue) if omitted.
    s_server.on("/api/calib-cam/start", HTTP_POST,
        [](AsyncWebServerRequest* req) {},
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            JsonDocument doc(&jsonAllocator());
            if (deserializeJson(doc, data, len)) { req->send(400, "text/plain", "bad json"); return; }
            const char* name = doc["pattern"] | "";
            int8_t idx = calib_patterns::camPatternIndex(name);
            if (idx < 0) { req->send(400, "text/plain", "unknown pattern"); return; }

            // A previous session that was never cleanly /stop-ped (client
            // crash, page reload mid-run) could still be holding a snapshot --
            // restore it before starting the new one so overrides never leak
            // across sessions.
            stopCalibCamSession();

            // This pattern's own last-drawn position (calib_patterns::generate()'s
            // cross-frame seam bridge) could be stale by a whole previous session -
            // without this, the first frame bridges from that stale spot via a
            // real (blanked, but large/fast) jump, visible as a spurious extra
            // shape in exactly the kind of accumulated capture optimizeGalvo.py's
            // camera-in-the-loop measurement relies on.
            calib_patterns::resetSeamState((uint8_t)idx);

            const uint8_t prof = calib_patterns::profileOf((uint8_t)idx);
            if (prof != gActiveOptimizerProfile) {
                gActiveOptimizerProfile = prof;
                syncOptimizerConfig();
                gPatternCacheGen++;
            }

            ilda::stop();
            gTextConfig.active   = false;
            gState.calib_idx     = (uint8_t)idx;
            gState.calib_bright  = 200;
            // 0=white, 1=R, 2=G, 3=B (see calib_patterns.cpp's cam_* pattern
            // comment) -- default blue: keeps a single laser diode's optics
            // in the loop instead of a combined white dot that can smear/
            // offset on a mono camera if R/G/B aren't perfectly co-boresighted.
            gState.calib_channel = doc["channel"].is<int>()
                ? (uint8_t)constrain((int)doc["channel"], 0, 3) : 3;
            gState.calib_no_thresh = false;
            gState.calib_raw_duty  = false;
            if (gState.ui_master_dimmer.load() < 200) gState.ui_master_dimmer.store(200);
            gState.calib_active  = true;

            s_calibcam_active  = true;
            s_calibcam_pat_idx = (uint8_t)idx;
            s_calibcam_profile = prof;

            JsonDocument resp(&jsonAllocator());
            resp["ok"]      = true;
            resp["pattern"] = name;
            resp["channel"] = gState.calib_channel;
            sendJsonPsram(req, resp);
        });

    // POST /api/calib-cam/params  {optimizer overrides..., "profile": N}
    s_server.on("/api/calib-cam/params", HTTP_POST,
        [](AsyncWebServerRequest* req) {},
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            if (!s_calibcam_active) { req->send(400, "text/plain", "no active calib-cam session"); return; }
            JsonDocument doc(&jsonAllocator());
            if (deserializeJson(doc, data, len)) { req->send(400, "text/plain", "bad json"); return; }

            if (doc["profile"].is<int>()) {
                int pr = (int)doc["profile"];
                if (pr < 0 || pr >= OPT_PROFILE_COUNT) { req->send(400, "text/plain", "bad profile"); return; }
                if ((uint8_t)pr != s_calibcam_profile) {
                    req->send(400, "text/plain", "profile must match the active calib-cam pattern's profile");
                    return;
                }
            }

            if (!s_calibcam_has_snap) {
                s_calibcam_snapshot = gOptimizerProfiles[s_calibcam_profile];
                s_calibcam_has_snap = true;
            }

            JsonDocument resp(&jsonAllocator());
            JsonObject applied = resp["applied"].to<JsonObject>();
            JsonArray  ignored = resp["ignored"].to<JsonArray>();
            applyOptimizerOverrides(doc.as<JsonObjectConst>(),
                                     gOptimizerProfiles[s_calibcam_profile], applied, ignored);

            if (s_calibcam_profile == gActiveOptimizerProfile) syncOptimizerConfig();
            gPatternCacheGen++;

            resp["ok"] = true;
            sendJsonPsram(req, resp);
        });

    // POST /api/calib-cam/stop
    s_server.on("/api/calib-cam/stop", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            stopCalibCamSession();
            req->send(200, "application/json", "{\"ok\":true}");
        });

    // GET /api/calib-cam/status
    s_server.on("/api/calib-cam/status", HTTP_GET,
        [](AsyncWebServerRequest* req) {
            JsonDocument doc(&jsonAllocator());
            doc["active"]  = s_calibcam_active;
            doc["pattern"] = s_calibcam_active ? calib_patterns::camPatternName(s_calibcam_pat_idx) : "";
            doc["channel"] = gState.calib_channel;
            JsonObject overrides = doc["overrides"].to<JsonObject>();
            if (s_calibcam_has_snap)
                diffOptimizerOverrides(gOptimizerProfiles[s_calibcam_profile], s_calibcam_snapshot, overrides);
            sendJsonPsram(req, doc);
        });

    // ── calibration-Pattern API ──────────────────────────────────
    // NOTE: specific routes (/stop, /list) must be registered BEFORE
    // the bare /api/calib-pattern route — ESPAsyncWebServer matches
    // the first registered handler whose prefix matches the URL.

    // POST /api/calib-pattern/stop  (registered first — avoids prefix match)
    s_server.on("/api/calib-pattern/stop", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            gState.calib_active    = false;
            gState.calib_no_thresh = false;
            gState.calib_raw_duty  = false;
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
            // Three Circles (idx 6): skip mapVisibleRange() threshold floor so
            // gain slider changes are not masked. All other patterns use the
            // normal threshold-remapped output path.
            gState.calib_no_thresh = (gState.calib_active && gState.calib_idx == 6);
            // Color-ramp linearity patterns (idx 18-20): bypass gain/dimmer/
            // gamma/threshold entirely -- see galvo_out.cpp's calib_raw_duty
            // branch and calib_patterns.cpp's calibRampImpl() comment.
            gState.calib_raw_duty = (gState.calib_active &&
                                      calib_patterns::isRampIdx(gState.calib_idx));
            // Calib mode enabled -> disable ILDA and text
            if (gState.calib_active) {
                ilda::stop();
                gTextConfig.active = false;
                // Run the pattern under the profile it was designed to
                // exercise, so its sliders are the ones on screen.
                const uint8_t prof = calib_patterns::profileOf(gState.calib_idx);
                if (prof != gActiveOptimizerProfile) {
                    gActiveOptimizerProfile = prof;
                    syncOptimizerConfig();
                    gPatternCacheGen++;
                }
                // Three Circles needs full dimmer so gain spans its full visible
                // range; other patterns use 200 to leave headroom.
                const uint8_t dimTarget = (gState.calib_idx == 6) ? 255 : 200;
                if (gState.ui_master_dimmer.load() < dimTarget)
                    gState.ui_master_dimmer.store(dimTarget);
            }
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
        doc["helios_net_connected"] = helios_net::isConnected();
        doc["helios_net_playing"]   = helios_net::isPlaying();
        doc["osc_active"]           = osc_in::isActive();
        doc["sacn_active"]          = sacn_in::isReceiving();
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

    // ── Preset Sequencer (BPM-synced preset playlist) ────────
    // NOTE: the /api/sequencer/* action routes are registered before the
    // bare /api/sequencer route, matching this file's existing convention
    // (see /api/text vs /api/text/vertices above) even though ESPAsyncWebServer
    // matches these particular URLs by exact string, not prefix.
    s_server.on("/api/sequencer/start", HTTP_POST, [](AsyncWebServerRequest* req) {
        sequencer::start();
        req->send(200, "text/plain", "OK");
    });
    s_server.on("/api/sequencer/stop", HTTP_POST, [](AsyncWebServerRequest* req) {
        sequencer::stop();
        req->send(200, "text/plain", "OK");
    });
    s_server.on("/api/sequencer/next", HTTP_POST, [](AsyncWebServerRequest* req) {
        sequencer::next();
        req->send(200, "text/plain", "OK");
    });
    s_server.on("/api/sequencer/prev", HTTP_POST, [](AsyncWebServerRequest* req) {
        sequencer::prev();
        req->send(200, "text/plain", "OK");
    });
    s_server.on("/api/sequencer/step", HTTP_POST,
        [](AsyncWebServerRequest* req) {}, nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            JsonDocument doc(&jsonAllocator());
            if (deserializeJson(doc, data, len)) { req->send(400, "text/plain", "bad json"); return; }
            sequencer::jumpTo((uint8_t)constrain((int)(doc["step"] | 0), 0, 255));
            req->send(200, "text/plain", "OK");
        });
    s_server.on("/api/sequencer", HTTP_GET, [](AsyncWebServerRequest* req) {
        JsonDocument doc(&jsonAllocator());
        JsonObject root = doc.to<JsonObject>();
        sequencer::fillStateJson(root);
        sendJsonPsram(req, doc);
    });
    s_server.on("/api/sequencer", HTTP_POST,
        [](AsyncWebServerRequest* req) {}, nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            JsonDocument doc(&jsonAllocator());
            if (deserializeJson(doc, data, len)) { req->send(400, "text/plain", "bad json"); return; }
            sequencer::setPlaylist(doc["steps"].as<JsonArrayConst>(), doc["loop"] | true);
            req->send(200, "text/plain", "OK");
        });
    s_server.on("/api/sequencer", HTTP_DELETE, [](AsyncWebServerRequest* req) {
        sequencer::clear();
        req->send(200, "text/plain", "OK");
    });

    // ── Modulator Engine (LFO/Noise/Envelope/Sequencer parameter matrix) ──
    // Slot index addressed via ?idx=N (query param) rather than a /:id path
    // segment -- ESPAsyncWebServer routes in this file are always matched
    // as exact strings (see the /api/sequencer/step precedent above), so a
    // path-param style route would need a regex pattern nothing else here
    // uses; req->hasParam()/getParam() (already used for ?token=/?after=)
    // is the established way this codebase reads request-scoped indices.
    //
    // NOTE: the /api/modulators/* action routes are registered before the
    // bare /api/modulators route, matching this file's existing convention.
    s_server.on("/api/modulators/trigger", HTTP_POST, [](AsyncWebServerRequest* req) {
        if (!req->hasParam("idx")) { req->send(400, "text/plain", "missing idx"); return; }
        modulator::trigger((uint8_t)atoi(req->getParam("idx")->value().c_str()));
        req->send(200, "text/plain", "OK");
    });
    s_server.on("/api/modulators/reset", HTTP_POST, [](AsyncWebServerRequest* req) {
        modulator::resetAll();
        req->send(200, "text/plain", "OK");
    });
    s_server.on("/api/modulators/bindings", HTTP_GET, [](AsyncWebServerRequest* req) {
        JsonDocument doc(&jsonAllocator());
        JsonArray arr = doc.to<JsonArray>();
        modulator::fillBindingsJson(arr);
        sendJsonPsram(req, doc);
    });
    s_server.on("/api/modulators/bindings", HTTP_POST,
        [](AsyncWebServerRequest* req) {}, nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            JsonDocument doc(&jsonAllocator());
            if (deserializeJson(doc, data, len)) { req->send(400, "text/plain", "bad json"); return; }
            modulator::setBindings(doc["bindings"].as<JsonArrayConst>());
            req->send(200, "text/plain", "OK");
        });
    // Registry metadata (types/shapes/targets/curveTypes/loopModes) --
    // boot-once fetch, cached client-side. Doesn't change at runtime in
    // Phase 1, but is served fresh (not statically embedded) so a future
    // module that calls modulator::registerModTarget() etc. from its own
    // init() shows up in the WebUI without any WebUI code change.
    s_server.on("/api/modulators/meta", HTTP_GET, [](AsyncWebServerRequest* req) {
        JsonDocument doc(&jsonAllocator());
        JsonObject root = doc.to<JsonObject>();
        modulator::fillMetaJson(root);
        sendJsonPsram(req, doc);
    });
    s_server.on("/api/modulators", HTTP_GET, [](AsyncWebServerRequest* req) {
        JsonDocument doc(&jsonAllocator());
        JsonObject root = doc.to<JsonObject>();
        modulator::fillStateJson(root);
        JsonArray binds = root["bindings"].to<JsonArray>();
        modulator::fillBindingsJson(binds);
        sendJsonPsram(req, doc);
    });
    s_server.on("/api/modulators", HTTP_POST,
        [](AsyncWebServerRequest* req) {}, nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            JsonDocument doc(&jsonAllocator());
            if (deserializeJson(doc, data, len)) { req->send(400, "text/plain", "bad json"); return; }
            JsonArrayConst mods = doc["modulators"].as<JsonArrayConst>();
            uint8_t i = 0;
            for (JsonObjectConst o : mods) {
                if (i >= modulator::MOD_SLOTS) break;
                modulator::setModulator(i, o);
                i++;
            }
            for (; i < modulator::MOD_SLOTS; i++) modulator::clearModulator(i);
            req->send(200, "text/plain", "OK");
        });
    s_server.on("/api/modulators", HTTP_PATCH,
        [](AsyncWebServerRequest* req) {}, nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            if (!req->hasParam("idx")) { req->send(400, "text/plain", "missing idx"); return; }
            uint8_t idx = (uint8_t)atoi(req->getParam("idx")->value().c_str());
            JsonDocument doc(&jsonAllocator());
            if (deserializeJson(doc, data, len)) { req->send(400, "text/plain", "bad json"); return; }
            if (!modulator::setModulator(idx, doc.as<JsonObjectConst>())) {
                req->send(400, "text/plain", "bad idx"); return;
            }
            req->send(200, "text/plain", "OK");
        });
    s_server.on("/api/modulators", HTTP_DELETE, [](AsyncWebServerRequest* req) {
        if (!req->hasParam("idx")) { req->send(400, "text/plain", "missing idx"); return; }
        modulator::clearModulator((uint8_t)atoi(req->getParam("idx")->value().c_str()));
        req->send(200, "text/plain", "OK");
    });

    // ── Feature 11: ILDA file upload via HTTP ────────────────
    s_server.on("/api/ilda/upload", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            req->send(s_upload_ok ? 200 : 400, "application/json",
                s_upload_ok ? "{\"status\":\"ok\",\"rescan\":true}"
                            : "{\"error\":\"upload failed (could not create file on SD)\"}");
            if (s_upload_ok) sd_card::scanFiles();  // SD neu scannen
        },
        [](AsyncWebServerRequest* req, String filename, size_t index,
           uint8_t* data, size_t len, bool final) {
            static File s_upload_file;
            if (index == 0) {
                String path = "/ilda/" + sanitizeIldaFilename(filename);
                ESP_LOGI("upload", "Start: %s (orig: %s)", path.c_str(), filename.c_str());
                { LOCK_SD(); s_upload_file = SD.open(path, FILE_WRITE); }
                s_upload_ok = (bool)s_upload_file;
                if (!s_upload_file) ESP_LOGE("upload", "could not create file: %s", path.c_str());
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
    // Gibt {status:"scanning"}, {status:"error", message}, or {status:"done", networks:[...]} zurueck
    s_server.on("/api/wifi-scan", HTTP_GET, [](AsyncWebServerRequest* req) {
        JsonDocument doc(&jsonAllocator());
        if (s_scan_running) {
            doc["status"] = "scanning";
            sendJsonPsram(req, doc);
            return;
        }
        if (!s_scan_done) {
            // No scan has ever completed yet -- kick one off. (Not "0 found" -- that
            // used to be indistinguishable from this and caused an endless auto-retry
            // loop that silently re-scanned forever without ever reporting a result.)
            WiFi.scanDelete();
            xTaskCreatePinnedToCore(wifiScanTask, "wifi_scan", 4096, nullptr, 2, nullptr, 0);
            doc["status"] = "scanning";
            sendJsonPsram(req, doc);
            return;
        }
        if (s_scan_error) {
            doc["status"]  = "error";
            doc["message"] = "WiFi scan failed -- radio busy or stuck, please retry";
            s_scan_done = false;
            sendJsonPsram(req, doc);
            return;
        }
        // Ergebnisse liefern -- from our own PSRAM buffer, not WiFi.SSID(i)/RSSI(i)
        // (see s_scan_nets comment: those read a cache the scan task no longer fills)
        int n = s_scan_results;
        doc["status"] = "done";
        JsonArray arr = doc["networks"].to<JsonArray>();
        if (s_scan_nets) {
            for (int i = 0; i < n && i < SCAN_MAX_NETS; i++) {
                JsonObject net = arr.add<JsonObject>();
                net["ssid"]    = s_scan_nets[i].ssid;
                net["rssi"]    = s_scan_nets[i].rssi;
                net["secure"]  = s_scan_nets[i].secure;
                net["channel"] = s_scan_nets[i].channel;
            }
        }
        WiFi.scanDelete();
        s_scan_results = 0;
        s_scan_done    = false;
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
                s_scan_done    = false;
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
        doc["ap_active"] = (WiFi.getMode() == WIFI_AP_STA);
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
    // ESP.restart() is scheduled via esp_timer instead of a blocking delay()
    // here: this handler runs on the async_tcp task (CONFIG_ASYNC_TCP_RUNNING_CORE),
    // and blocking that task stalls it from actually flushing this response
    // over the socket before the restart tears the connection down. esp_timer's
    // callback runs on its own dedicated task, so the response gets a clean
    // window to go out first.
    s_server.on("/api/reboot", HTTP_POST, [](AsyncWebServerRequest* req) {
        req->send(200, "text/plain", "reboot");
        static esp_timer_handle_t s_reboot_timer = nullptr;
        esp_timer_create_args_t args = {};
        args.callback = [](void*) { ESP.restart(); };
        args.name = "reboot";
        esp_timer_create(&args, &s_reboot_timer);
        esp_timer_start_once(s_reboot_timer, 500000);  // 500ms
    });

    // ---- GET /api/log ----
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

    // ---- POST /api/log/client ----
    // Mirrors WebUI error toasts into the ESP32 log so they survive after the
    // browser tab is closed (toasts themselves are ephemeral, see UI JS `toast()`).
    s_server.on("/api/log/client", HTTP_POST,
        [](AsyncWebServerRequest* req) {},
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            JsonDocument doc(&jsonAllocator());
            if (deserializeJson(doc, data, len)) { req->send(400, "text/plain", "bad json"); return; }
            const char* msg = doc["msg"] | "";
            if (msg[0]) LOG_W(logbuf::CAT_USER, "UI error: %s", msg);
            req->send(200, "text/plain", "OK");
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

    // ---- GET /api/meminfo (Log tab memory viewer) ----
    s_server.on("/api/meminfo", HTTP_GET, [](AsyncWebServerRequest* req) {
        JsonDocument doc(&jsonAllocator());
        JsonObject heap = doc["heap"].to<JsonObject>();
        heap["total"]    = ESP.getHeapSize();
        heap["free"]     = ESP.getFreeHeap();
        heap["largest"]  = ESP.getMaxAllocHeap();
        heap["min_ever"] = ESP.getMinFreeHeap();
        heap["critical"] = gConfig.heap_critical_bytes;
        JsonObject psram = doc["psram"].to<JsonObject>();
        psram["total"]    = ESP.getPsramSize();
        psram["free"]     = ESP.getFreePsram();
        psram["largest"]  = ESP.getMaxAllocPsram();
        psram["min_ever"] = ESP.getMinFreePsram();
        JsonArray owners = doc["owners"].to<JsonArray>();
        for (size_t i = 0; i < memreg::count(); i++) {
            const memreg::Owner& o = memreg::get(i);
            JsonObject e = owners.add<JsonObject>();
            e["name"]  = o.name;
            e["bytes"] = o.bytes;
            e["psram"] = o.psram;
        }
        sendJsonPsram(req, doc);
    });

    // ---- POST /api/factory-reset ----
    s_server.on("/api/factory-reset", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            s_prefs.begin("laser", false); s_prefs.clear(); s_prefs.end();
            req->send(200, "text/plain", "reset"); delay(500); ESP.restart();
        });

    // ---- GET /api/backup ---- full-config JSON download ----
    // Calib + all 8 optimizer profiles + network + system/thermal settings,
    // straight from the live in-RAM structs (kept in sync with NVS by
    // persistConfig()/loadConfig() -- see BackupManager for the field list).
    s_server.on("/api/backup", HTTP_GET, [](AsyncWebServerRequest* req) {
        JsonDocument doc(&jsonAllocator());
        BackupManager::serializeToJson(doc);
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
        resp->addHeader("Content-Disposition", "attachment; filename=\"galvos_backup.json\"");
        req->send(resp);
    });

    // ---- GET /api/backup/info ---- metadata only (no download) ----
    s_server.on("/api/backup/info", HTTP_GET, [](AsyncWebServerRequest* req) {
        JsonDocument doc(&jsonAllocator());
        doc["fw"]        = LASER_FW_VERSION;
        doc["schema"]    = BackupManager::SCHEMA_VERSION;
        doc["timestamp"] = (uint32_t)ntp_client::nowEpoch();
        sendJsonPsram(req, doc);
    });

    // ---- POST /api/restore ---- validate + apply a backup JSON, then reboot ----
    // Every value is validated before anything is written (see
    // BackupManager::deserializeFromJson) -- a single rejected key aborts the
    // whole restore untouched. On success the live structs are already
    // updated by BackupManager; this handler only has to persist them (reusing
    // the same NVS writers as the live WebUI endpoints) and restart, same as
    // OTA never leaves the device running on half-applied config.
    s_server.on("/api/restore", HTTP_POST,
        [](AsyncWebServerRequest* req) {},
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            if (gState.laser_armed.load()) {
                req->send(403, "application/json",
                    "{\"error\":\"Laser armed — disarm before restoring\"}");
                return;
            }
            JsonDocument doc(&jsonAllocator());
            if (deserializeJson(doc, data, len) != DeserializationError::Ok) {
                req->send(400, "application/json", "{\"error\":\"bad json\"}");
                return;
            }
            JsonDocument result(&jsonAllocator());
            JsonArray rejected = result["rejected"].to<JsonArray>();
            bool ok = BackupManager::deserializeFromJson(doc, rejected);
            result["ok"] = ok;
            if (!ok) {
                sendJsonPsram(req, result, 400);
                return;
            }
            persistConfig();  // calib + optimizer profiles + net, "laser" namespace
            { Preferences p; p.begin("projection", false);
              p.putUShort("kpps",       gProjection.galvo_kpps);
              p.putUShort("rated_kpps", gProjection.galvo_rated_kpps);
              p.putFloat("scan_ang",    gProjection.scan_angle_mech_deg);
              p.putFloat("exit_ang",    gProjection.exit_angle_deg);
              p.putFloat("ilda_ang",    gProjection.ilda_test_angle_deg);
              p.putFloat("pwr_r",       gProjection.power_r_mw);
              p.putFloat("pwr_g",       gProjection.power_g_mw);
              p.putFloat("pwr_b",       gProjection.power_b_mw);
              p.putFloat("dist_m",      gProjection.distance_m);
              p.end(); }
            { Preferences p; p.begin("laser", false);
              p.putUChar("t_warn",   gSafety.temp_warn_c);
              p.putUChar("t_red",    gSafety.temp_reduce_c);
              p.putUChar("t_shut",   gSafety.temp_shutdown_c);
              p.putUChar("fan_min",  gSafety.fan_min_pct);
              p.putBool ("fan_auto", gSafety.fan_auto);
              p.end(); }
            ESP_LOGW(TAG, "Config restored from backup -- rebooting");
            sendJsonPsram(req, result);
            vTaskDelay(pdMS_TO_TICKS(800));
            ESP.restart();
        });

    // ── Community Presets (/api/community/*) ─────────────────────────────────
    // GitHub-hosted preset JSONs (optimizer tuning + preset playback params),
    // downloaded by the browser and POSTed here for validation + LittleFS
    // storage. See community_presets.h for the on-disk schema/bounds; the
    // firmware never talks to GitHub itself, only the browser does.

    // ---- GET /api/community/fs-info ----
    s_server.on("/api/community/fs-info", HTTP_GET, [](AsyncWebServerRequest* req) {
        JsonDocument doc(&jsonAllocator());
        community_presets::fsInfo(doc.to<JsonObject>());
        sendJsonPsram(req, doc);
    });

    // ---- GET /api/community/list ----
    s_server.on("/api/community/list", HTTP_GET, [](AsyncWebServerRequest* req) {
        JsonDocument doc(&jsonAllocator());
        JsonObject root = doc.to<JsonObject>();
        community_presets::list(root["presets"].to<JsonArray>());
        sendJsonPsram(req, doc);
    });

    // ---- POST /api/community/save ---- validate + write to LittleFS
    s_server.on("/api/community/save", HTTP_POST,
        [](AsyncWebServerRequest* req) {},
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            if (len > community_presets::MAX_FILE_BYTES) {
                req->send(413, "application/json", "{\"error\":\"preset too large\"}");
                return;
            }
            JsonDocument doc(&jsonAllocator());
            if (deserializeJson(doc, data, len) != DeserializationError::Ok) {
                req->send(400, "application/json", "{\"error\":\"bad json\"}");
                return;
            }
            String reason;
            if (!community_presets::validate(doc, reason)) {
                JsonDocument err(&jsonAllocator());
                err["error"] = reason;
                sendJsonPsram(req, err, 400);
                return;
            }
            if (!community_presets::save(doc, reason)) {
                JsonDocument err(&jsonAllocator());
                err["error"] = reason;
                sendJsonPsram(req, err, 500);
                return;
            }
            req->send(200, "text/plain", "OK");
        });

    // ---- DELETE /api/community/delete ---- body: {id}
    s_server.on("/api/community/delete", HTTP_DELETE,
        [](AsyncWebServerRequest* req) {},
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            JsonDocument doc(&jsonAllocator());
            if (deserializeJson(doc, data, len) != DeserializationError::Ok) {
                req->send(400, "application/json", "{\"error\":\"bad json\"}");
                return;
            }
            const char* id = doc["id"] | "";
            if (!community_presets::remove(String(id))) {
                req->send(404, "application/json", "{\"error\":\"not found\"}");
                return;
            }
            req->send(200, "text/plain", "OK");
        });

    // ---- POST /api/community/rename ---- body: {id, name}
    s_server.on("/api/community/rename", HTTP_POST,
        [](AsyncWebServerRequest* req) {},
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            JsonDocument doc(&jsonAllocator());
            if (deserializeJson(doc, data, len) != DeserializationError::Ok) {
                req->send(400, "application/json", "{\"error\":\"bad json\"}");
                return;
            }
            const char* id   = doc["id"]   | "";
            const char* name = doc["name"] | "";
            if (!community_presets::rename(String(id), String(name))) {
                req->send(404, "application/json", "{\"error\":\"not found\"}");
                return;
            }
            req->send(200, "text/plain", "OK");
        });

    // ---- POST /api/community/activate ---- body: {id}
    // Applies preset_params (which built-in preset + color/speed/size) then
    // layers optimizer_profile on top of gOptimizerConfig as a RAM-only
    // override -- same non-persisted pattern as the calib-cam session
    // overrides above. Order matters: setPreset() re-syncs gOptimizerConfig
    // from the preset's class profile, so the override must apply after it
    // or it would be immediately clobbered.
    s_server.on("/api/community/activate", HTTP_POST,
        [](AsyncWebServerRequest* req) {},
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            JsonDocument reqDoc(&jsonAllocator());
            if (deserializeJson(reqDoc, data, len) != DeserializationError::Ok) {
                req->send(400, "application/json", "{\"error\":\"bad json\"}");
                return;
            }
            const char* id = reqDoc["id"] | "";
            JsonDocument doc(&jsonAllocator());
            if (!community_presets::load(String(id), doc)) {
                req->send(404, "application/json", "{\"error\":\"not found\"}");
                return;
            }

            JsonObjectConst pp = doc["preset_params"];
            int idx = pp["preset_idx"] | -1;
            presets::Preset p = presets::presetFromIndex(idx);
            if (p == presets::Preset::None) {
                req->send(400, "application/json", "{\"error\":\"invalid preset_idx\"}");
                return;
            }
            if (pp["col_r"].is<int>())    gLivePreset.col_r    = constrain((int)pp["col_r"], 0, 255);
            if (pp["col_g"].is<int>())    gLivePreset.col_g    = constrain((int)pp["col_g"], 0, 255);
            if (pp["col_b"].is<int>())    gLivePreset.col_b    = constrain((int)pp["col_b"], 0, 255);
            if (pp["speed"].is<int>())    gLivePreset.speed    = constrain((int)pp["speed"], 0, 255);
            if (pp["size_val"].is<int>()) gLivePreset.size_val = constrain((int)pp["size_val"], 0, 255);

            patterns::setPreset(p);

            JsonObjectConst op = doc["optimizer_profile"];
            if (!op.isNull()) {
                JsonDocument scratch(&jsonAllocator());
                JsonObject applied = scratch["applied"].to<JsonObject>();
                JsonArray  ignored = scratch["ignored"].to<JsonArray>();
                applyOptimizerOverrides(op, gOptimizerConfig, applied, ignored);
                gPatternCacheGen++;
            }

            JsonDocument result(&jsonAllocator());
            result["ok"]   = true;
            result["idx"]  = idx;
            result["name"] = doc["meta"]["name"] | id;
            sendJsonPsram(req, result);
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
                    "{\"ok\":true,\"info\":\"Sweep via pattern_engine — use /api/preset\"}");
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

    // ═══ POST /api/debug/resonance — Resonance test: single-axis sine drive ═
    // Prompt 13 (galvo resonance measurement). Requires laser_armed (or
    // gDebugNoHW), same guard as /api/debug/hw. See docs/feature-prompts/
    // DECISIONS.md, Prompt 13 for why this exists as a firmware primitive
    // instead of a Python-side HTTP loop (HTTP round trips are far too slow
    // to synthesize a sweep anywhere near 2000Hz).
    // Body JSON: {axis: 0|1, freq_hz: float, amp: int, r, g, b}
    //   axis: 0=X, 1=Y. amp: DAC-space peak (-32767..32767), clamped
    //   server-side against dac_limit_min/max before being armed -- unlike
    //   /api/debug/hw's deliberately-unclamped single point, this endpoint
    //   drives a SUSTAINED signal for an automated sweep, so a caller bug
    //   can't park the beam outside the safe range for the whole sweep.
    // Special command: {cmd:"off"}
    s_server.on("/api/debug/resonance", HTTP_POST,
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
            if (doc["cmd"].is<const char*>() && strcmp(doc["cmd"], "off") == 0) {
                galvo::clearResonanceTest();
                req->send(200, "application/json", "{\"ok\":true,\"cmd\":\"off\"}");
                return;
            }

            uint8_t axis   = (doc["axis"] | 0) ? 1 : 0;
            float   freqHz = doc["freq_hz"] | 0.0f;
            int32_t amp    = doc["amp"] | 0;
            uint8_t r = doc["r"] | 0, g = doc["g"] | 0, b = doc["b"] | 0;

            if (!(freqHz > 0.0f) || freqHz > 5000.0f) {
                req->send(400, "application/json",
                    "{\"error\":\"freq_hz out of range (0-5000)\"}");
                return;
            }

            uint16_t limMin, limMax;
            if (xSemaphoreTake(mtx::config, pdMS_TO_TICKS(10)) == pdTRUE) {
                limMin = gConfig.dac_limit_min;
                limMax = gConfig.dac_limit_max;
                xSemaphoreGive(mtx::config);
            } else {
                limMin = 0x0666; limMax = 0xF999;   // fallback: factory-safe default
            }
            int32_t safeMax = std::min((int32_t)0x8000 - (int32_t)limMin,
                                        (int32_t)limMax - (int32_t)0x8000);
            bool clamped = false;
            if (amp > safeMax)  { amp = safeMax;  clamped = true; }
            if (amp < -safeMax) { amp = -safeMax; clamped = true; }

            galvo::setResonanceTest(axis, freqHz, (int16_t)amp, r, g, b);

            char buf[160];
            snprintf(buf, sizeof(buf),
                "{\"ok\":true,\"axis\":%u,\"freq_hz\":%.2f,\"amp\":%d,\"clamped\":%s}",
                axis, (double)freqHz, (int)amp, clamped ? "true" : "false");
            req->send(200, "application/json", buf);
        });

    // ═══ GET /api/debug/resonance — current resonance-test status ═══════
    s_server.on("/api/debug/resonance", HTTP_GET, [](AsyncWebServerRequest* req) {
        char buf[96];
        snprintf(buf, sizeof(buf), "{\"active\":%d,\"armed\":%d}",
            (int)galvo::isResonanceTestActive(), (int)gState.laser_armed.load());
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

    // ---- GET /api/status ---- lightweight status for external tooling
    // (browser dashboard uses /api/state instead) ----
    // Thin subset-projection of buildCoreStatusJson() -- same field names/
    // values as /api/state's overlapping fields, so this can no longer drift
    // out of sync with the WebUI's real state the way the old hand-rolled
    // sprintf() did (State_fix.md architecture #15). Sole consumer is the
    // external camera-autotuning script (scripts/optimizeGalvo/optimizeGalvo.py's
    // getStatus(), docs/06-camera-autotuning.md).
    s_server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest* req) {
        JsonDocument doc(&jsonAllocator());
        buildCoreStatusJson(doc);
        doc["debug_mode"] = galvo::noHwMode();  // same flag as /api/state's "no_hw_mode"
        sendJsonPsram(req, doc);
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
            // Auto-exit HW debug / resonance-test mode only on rising edge
            // of ui_override -- switching back to normal WebUI operation
            // shouldn't leave the galvo stuck driving a debug point or a
            // resonance-test sine.
            if (ui_override && !prev_override) {
                galvo::clearDebugOutput();
                galvo::clearResonanceTest();
                patterns::stopTestPattern();
            }
            JsonDocument resp(&jsonAllocator());
            resp["ui_override"]    = ui_override;
            resp["master_dimmer"]  = master_dim;
            sendJsonPsram(req, resp);
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
        float rated_kpps  = (float)gProjection.galvo_rated_kpps;
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
            "\"rated_kpps\":%u,"
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
            (unsigned)gProjection.galvo_rated_kpps,
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

    // ── /api/projection/awb — apply auto white balance from laser power specs ─
    // NOTE: must be registered BEFORE the bare /api/projection POST route
    // below -- ESPAsyncWebServer matches the first registered handler whose
    // prefix matches the URL (same reason calib-pattern/stop+list are
    // registered before the bare calib-pattern route). Registered here
    // shadowed by /api/projection POST, this handler was unreachable and
    // every request fell through to that handler's empty onRequest lambda,
    // which never calls send() -- hence the 501.
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
            if (v >= 12 && v <= 60) {
                gProjection.galvo_kpps = v; changed = true; gPatternCacheGen++;
                // Inverse-filter biquads are designed for a specific sample
                // rate (Prompt 12b) -- redesign them whenever it changes.
                invfilter::refresh((uint32_t)gProjection.galvo_kpps * 1000);
            }
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


    // ── /api/galvo/autotune GET ── poll sample-rate autotune progress/result ──
    s_server.on("/api/galvo/autotune", HTTP_GET, [](AsyncWebServerRequest* req) {
        galvo::AutotuneStatus st = galvo::autotuneStatus();
        char buf[192];
        snprintf(buf, sizeof(buf),
            "{\"running\":%s,\"done\":%s,\"floor_unstable\":%s,\"candidate_kpps\":%u,"
            "\"result_kpps\":%u,\"step\":%u,\"step_total\":%u}",
            st.running ? "true" : "false",
            st.done    ? "true" : "false",
            st.floor_unstable ? "true" : "false",
            (unsigned)st.candidate_kpps,
            (unsigned)st.result_kpps,
            (unsigned)st.step,
            (unsigned)st.step_total);
        req->send(200, "application/json", buf);
    });

    // ── /api/galvo/autotune POST ── start/abort the sample-rate sweep ─────────
    // Body: {"action":"start"} (default) | {"action":"abort"}
    s_server.on("/api/galvo/autotune", HTTP_POST, [](AsyncWebServerRequest* req){},
    nullptr,
    [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
        if (!isAuthorised(req)) { denyUnauth(req); return; }
        JsonDocument doc(&jsonAllocator());
        if (deserializeJson(doc, data, len) != DeserializationError::Ok) {
            req->send(400, "application/json", "{\"error\":\"bad json\"}");
            return;
        }
        const char* action = doc["action"] | "start";
        if (strcmp(action, "abort") == 0) galvo::autotuneAbort();
        else                              galvo::autotuneStart();
        req->send(200, "application/json", "{\"ok\":true}");
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
