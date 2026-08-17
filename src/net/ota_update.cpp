#include "ota_update.h"
#include "config.h"
#include "safety/safety.h"
#include <Arduino.h>
#include <ArduinoOTA.h>
#include <Update.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include "web_ui.h"
#include <esp_log.h>

namespace ota_update {

static const char* TAG = "ota";

static char        s_ota_pass[12];
static const char* OTA_USER = "admin";

// Per-endpoint upload progress/error state (firmware vs. filesystem image
// share the same body-handler logic, just a different Update.begin() command
// and target partition -- see otaUploadBody()).
struct OtaUploadState {
    bool   active = false;   // Update.begin() succeeded, chunks may be written
    bool   error  = false;
    char   errMsg[80] = "";
    size_t written = 0;
};
static OtaUploadState s_fw_state;
static OtaUploadState s_fs_state;

// Shared multipart body handler for both /api/ota/upload (U_FLASH) and
// /api/ota/upload-fs (U_SPIFFS) -- validates auth/arm-state once at index==0,
// streams chunks into Update.write(), and records the first error seen so
// the request-complete handler can report Update.errorString() back to the
// browser instead of a bare "Update failed".
static void otaUploadBody(AsyncWebServerRequest* req, size_t index,
                           uint8_t* data, size_t len, bool final,
                           int updateCommand, OtaUploadState& st,
                           const char* filename, const char* logTag) {
    if (index == 0) {
        st.active = false;
        st.error  = false;
        st.errMsg[0] = '\0';
        st.written = 0;
        if (!req->authenticate(OTA_USER, s_ota_pass)) {
            req->send(401, "text/plain", "Unauthorized");
            return;
        }
        if (gState.laser_armed.load()) {
            st.error = true;
            snprintf(st.errMsg, sizeof(st.errMsg), "Laser armed -- disarm before OTA");
            ESP_LOGE(TAG, "%s rejected: laser is armed!", logTag);
            return;
        }
        ESP_LOGI(TAG, "%s start: %s", logTag, filename);
        safety::emergencyStop();  // laser off during update
        if (updateCommand == U_SPIFFS) LittleFS.end();  // release partition before overwriting it
        // Always UPDATE_SIZE_UNKNOWN: req->contentLength() is the whole multipart
        // body (file + boundary/header overhead), not the file size, so it's always
        // a few hundred bytes larger than the actual image. mklittlefs pads the FS
        // image to exactly fill its partition, so that overhead alone was enough to
        // trip Update.begin()'s "size > partition size" check ("Bad Size Given") on
        // every filesystem upload. Update.begin() falls back to partition->size as
        // the write ceiling, and Update.end(true) trims to the actual bytes written.
        if (!Update.begin(UPDATE_SIZE_UNKNOWN, updateCommand)) {
            st.error = true;
            snprintf(st.errMsg, sizeof(st.errMsg), "%s", Update.errorString());
            ESP_LOGE(TAG, "%s Update.begin() failed: %s", logTag, Update.errorString());
            if (updateCommand == U_SPIFFS) LittleFS.begin(true);  // undo the unmount above -- begin() never took the partition
            return;
        }
        st.active = true;
    }
    if (!st.active) return;  // begin() failed or was rejected -- ignore remaining chunks

    if (len) {
        size_t written = Update.write(data, len);
        st.written += written;
        if (written != len) {
            st.error = true;
            snprintf(st.errMsg, sizeof(st.errMsg), "write mismatch: %s", Update.errorString());
            ESP_LOGE(TAG, "%s write mismatch: %u/%u (%s)", logTag,
                     (unsigned)written, (unsigned)len, Update.errorString());
        }
    }
    if (final) {
        if (Update.end(true)) {
            ESP_LOGI(TAG, "%s done: %u bytes", logTag, (unsigned)(index + len));
        } else {
            st.error = true;
            snprintf(st.errMsg, sizeof(st.errMsg), "%s", Update.errorString());
            ESP_LOGE(TAG, "%s end() failed: %s", logTag, Update.errorString());
        }
        st.active = false;
        if (updateCommand == U_SPIFFS) LittleFS.begin(true);  // remount for the WebUI to keep serving until reboot
    }
}

// Builds the {"ok":..,"bytes":..} / {"ok":false,"error":".."} response sent
// once an upload's onRequest handler fires (after the body handler above has
// run to completion).
static void sendOtaResult(AsyncWebServerRequest* req, const OtaUploadState& st) {
    bool ok = !st.error && !Update.hasError();
    char resp[160];
    if (ok) {
        snprintf(resp, sizeof(resp), "{\"ok\":true,\"bytes\":%u}", (unsigned)st.written);
    } else {
        const char* msg = st.errMsg[0] ? st.errMsg : Update.errorString();
        snprintf(resp, sizeof(resp), "{\"ok\":false,\"error\":\"%s\"}", msg);
    }
    req->send(ok ? 200 : 500, "application/json", resp);
}

// Self-contained update page -- styled to match the main WebUI's default
// (cyberpunk) theme tokens/card/button classes so it doesn't look like a
// bolted-on debug page. FW version is baked in server-side; UI_VERSION only
// exists client-side (data/index.html), so it's read by fetching "/" and
// pulling the constant out of the page text.
static const char* UPDATE_PAGE_TEMPLATE = R"HTML(<!DOCTYPE html>
<html lang="en"><head>
<meta charset="UTF-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<link href="https://fonts.googleapis.com/css2?family=Orbitron:wght@400;700;900&family=JetBrains+Mono:wght@400;500;700&display=swap" rel="stylesheet">
<title>GalvOS &mdash; Firmware Update</title>
<style>
:root {
  --bg:#0a0a0f; --card:#12121a; --border:#2a2a3a; --muted:#1c1c2e;
  --text:#e0e0e0; --text-dim:#8888a0; --accent:#00ff88; --accent-rgb:0,255,136;
  --bad:#ff3366; --font-head:'Orbitron',sans-serif; --font-mono:'JetBrains Mono',monospace;
}
* { box-sizing:border-box; }
body { background:var(--bg); color:var(--text); font-family:var(--font-mono); margin:0; padding:24px 16px 60px; }
.wrap { max-width:560px; margin:0 auto; }
h1 { font-family:var(--font-head); font-size:1.4rem; color:var(--accent); margin:0 0 4px; }
.ver { color:var(--text-dim); font-size:0.8rem; margin-bottom:24px; }
.card { background:var(--card); border:1px solid var(--border); border-radius:6px; padding:16px; margin-bottom:16px; }
.card h2 { font-family:var(--font-head); font-size:1rem; margin:0 0 12px; color:var(--text); }
.card p.hint { color:var(--text-dim); font-size:0.75rem; margin:4px 0 12px; }
.card p.hint code { background:var(--muted); border-radius:3px; padding:1px 4px; color:var(--text); }
input[type=file] { width:100%; color:var(--text); background:var(--bg); border:1px solid var(--border);
  border-radius:4px; padding:8px; font-family:var(--font-mono); margin-bottom:12px; }
button.btn, a.btn { font-family:var(--font-head); background:transparent; color:var(--text);
  border:2px solid var(--border); padding:8px 16px; min-height:40px; cursor:pointer; border-radius:4px;
  font-size:0.85rem; text-decoration:none; display:inline-block; }
button.btn:hover, a.btn:hover { border-color:var(--accent); color:var(--accent); }
button.btn:disabled { opacity:0.4; cursor:not-allowed; }
button.btn.primary { background:var(--accent); color:var(--bg); border-color:var(--accent); font-weight:700; }
.progress-wrap { display:none; height:8px; background:var(--muted); border-radius:4px; overflow:hidden; margin:12px 0; }
.progress-bar { height:100%; width:0%; background:var(--accent); transition:width 0.15s; }
.pct { font-size:0.75rem; color:var(--text-dim); }
.err { color:var(--bad); font-size:0.8rem; margin-top:8px; white-space:pre-wrap; }
.success { display:none; margin-top:12px; }
.success p { color:var(--accent); font-size:0.85rem; margin:0 0 10px; }
.guide ol { margin:0; padding-left:1.2em; font-size:0.8rem; color:var(--text-dim); line-height:1.6; }
.guide li b { color:var(--text); }
</style></head>
<body><div class="wrap">
<h1>GalvOS Laser Controller &mdash; Firmware Update</h1>
<div class="ver">Running: FW v{{FW_VER}} &middot; UI v<span id="ui-ver">...</span></div>

<div class="card guide">
  <h2>Quick Guide</h2>
  <ol>
    <li><b>Backup first</b> &mdash; download the config below, just in case.</li>
    <li><b>Firmware</b> changed &rarr; upload the firmware .bin.</li>
    <li><b>WebUI</b> changed too &rarr; also upload the filesystem .bin (skip it if only firmware changed).</li>
    <li>Wait for each upload to reach 100% with no error, then hit <b>Reboot Now</b>.</li>
  </ol>
</div>

<div class="card">
  <h2>Firmware</h2>
  <p class="hint">Upload a firmware .bin built for this board &mdash; conventionally
    <code>firmware_x.y.z.bin</code> in <code>.pio/build/esp32-s3-devkitc-1/</code>.</p>
  <input type="file" id="fw-file" accept=".bin">
  <div class="progress-wrap" id="fw-prog"><div class="progress-bar" id="fw-bar"></div></div>
  <span class="pct" id="fw-pct"></span>
  <div><button class="btn primary" id="fw-btn" onclick="doUpload('fw')">Upload Firmware</button></div>
  <div class="err" id="fw-err"></div>
  <div class="success" id="fw-success"><p>Firmware written. Reboot to run it.</p>
    <button class="btn primary" onclick="doReboot(this)">Reboot Now</button>
    <span class="pct reboot-status"></span></div>
</div>

<div class="card">
  <h2>WebUI / Filesystem</h2>
  <p class="hint">Upload a LittleFS filesystem image (WebUI assets) .bin &mdash; conventionally
    <code>littlefs_x.y.z.bin</code> in <code>.pio/build/esp32-s3-devkitc-1/</code>.</p>
  <input type="file" id="fs-file" accept=".bin">
  <div class="progress-wrap" id="fs-prog"><div class="progress-bar" id="fs-bar"></div></div>
  <span class="pct" id="fs-pct"></span>
  <div><button class="btn primary" id="fs-btn" onclick="doUpload('fs')">Upload Filesystem</button></div>
  <div class="err" id="fs-err"></div>
  <div class="success" id="fs-success"><p>Filesystem written. Reboot to use it.</p>
    <button class="btn primary" onclick="doReboot(this)">Reboot Now</button>
    <span class="pct reboot-status"></span></div>
</div>

<div class="card">
  <h2>Config Backup</h2>
  <p class="hint">Download the full device configuration (calibration, network, presets) before flashing.</p>
  <a class="btn" href="/api/backup" download="galvos_backup.json">Download Backup</a>
</div>

</div>
<script>
fetch('/').then(function(r){ return r.text(); }).then(function(t){
  var m = t.match(/UI_VERSION\s*=\s*'([\d.]+)'/);
  if (m) document.getElementById('ui-ver').textContent = m[1];
}).catch(function(){});

var ENDPOINTS = { fw: '/api/ota/upload', fs: '/api/ota/upload-fs' };
var FIELDS    = { fw: 'firmware',        fs: 'filesystem' };

// Pre-flight arm check -- the server rejects the upload outright while armed
// (see ota_update.cpp's otaUploadBody()), but that rejection only happens
// after the whole file has already been sent. This asks first and offers to
// disarm, so a large firmware/filesystem image isn't uploaded just to be
// thrown away. Unreachable/unknown status refuses rather than guessing.
async function requireDisarmedForUpload() {
  var r = await fetch('/api/state').then(function(x) { return x.ok ? x.json() : null; }).catch(function() { return null; });
  if (!r || typeof r.laser_armed !== 'boolean') return { ok: false, msg: 'Laser status unknown -- cannot upload.' };
  if (!r.laser_armed) return { ok: true };
  if (!confirm('Laser is armed. Disarm laser before upload?\n\nOK = Disarm & Continue\nCancel = Cancel')) return { ok: false, msg: '' };
  var dr = await fetch('/api/arm', { method: 'POST', body: '0' }).catch(function() { return null; });
  if (!dr || !dr.ok) return { ok: false, msg: 'Disarm failed.' };
  var chk = await fetch('/api/state').then(function(x) { return x.ok ? x.json() : null; }).catch(function() { return null; });
  if (!chk || chk.laser_armed) return { ok: false, msg: 'Disarm did not take effect -- upload cancelled.' };
  return { ok: true };
}

async function doUpload(kind) {
  var file = document.getElementById(kind + '-file').files[0];
  var errEl = document.getElementById(kind + '-err');
  var btn   = document.getElementById(kind + '-btn');
  errEl.textContent = '';
  document.getElementById(kind + '-success').style.display = 'none';
  if (!file) { errEl.textContent = 'Select a .bin file first.'; return; }

  var guard = await requireDisarmedForUpload();
  if (!guard.ok) { errEl.textContent = guard.msg; return; }

  var progWrap = document.getElementById(kind + '-prog');
  var bar      = document.getElementById(kind + '-bar');
  var pct      = document.getElementById(kind + '-pct');
  progWrap.style.display = 'block';
  bar.style.width = '0%';
  pct.textContent = '0%';
  btn.disabled = true;

  var fd = new FormData();
  fd.append(FIELDS[kind], file);

  var xhr = new XMLHttpRequest();
  xhr.open('POST', ENDPOINTS[kind]);
  xhr.upload.onprogress = function(e) {
    if (!e.lengthComputable) return;
    var p = Math.round(e.loaded / e.total * 100);
    bar.style.width = p + '%';
    pct.textContent = p + '%';
  };
  xhr.onload = function() {
    btn.disabled = false;
    var res = null;
    try { res = JSON.parse(xhr.responseText); } catch (e) {}
    if (xhr.status === 200 && res && res.ok) {
      bar.style.width = '100%';
      pct.textContent = '100%';
      document.getElementById(kind + '-success').style.display = 'block';
    } else {
      errEl.textContent = (res && res.error) ? res.error : ('Upload failed (HTTP ' + xhr.status + ')');
    }
  };
  xhr.onerror = function() {
    btn.disabled = false;
    errEl.textContent = 'Network error during upload.';
  };
  xhr.send(fd);
}

function doReboot(btn) {
  btn.disabled = true;
  var status = btn.nextElementSibling;
  status.textContent = ' Rebooting...';
  fetch('/api/reboot', { method: 'POST' })
    .then(function(r) {
      if (!r.ok) throw new Error('HTTP ' + r.status);
      status.textContent = ' Reboot sent -- device restarting, reconnect in a few seconds.';
    })
    .catch(function() {
      // A network error here is expected too: the device drops the connection
      // as part of restarting, often before the response is fully read.
      status.textContent = ' Reboot sent -- device restarting, reconnect in a few seconds.';
    });
}
</script>
</body></html>)HTML";

void init() {
    // ── ArduinoOTA (IDE/CLI) ──────────────────────────────────
    ArduinoOTA.setHostname(gConfig.hostname);
    ArduinoOTA.setPort(3232);
    // Password from chip ID (same as WiFi PW)
    char ota_pass[12];
    snprintf(ota_pass, sizeof(ota_pass), "%08X",
             (uint32_t)(ESP.getEfuseMac() >> 16));
    ArduinoOTA.setPassword(ota_pass);

    ArduinoOTA.onStart([]() {
        ESP_LOGW(TAG, "OTA Start — Laser disarmed");
        safety::emergencyStop();  // laser off during update
    });
    ArduinoOTA.onEnd([]() {
        ESP_LOGI(TAG, "OTA done — restarting");
    });
    ArduinoOTA.onError([](ota_error_t e) {
        ESP_LOGE(TAG, "OTA Error: %u", e);
    });
    ArduinoOTA.onProgress([](uint32_t done, uint32_t total) {
        static uint32_t last_pct = 0;
        uint32_t pct = done * 100 / total;
        if (pct != last_pct && pct % 10 == 0) {
            ESP_LOGI(TAG, "OTA: %u%%", pct);
            last_pct = pct;
        }
    });
    ArduinoOTA.begin();

    // ── HTTP-Upload-Endpoints (/update page + /api/ota/upload*) ────────────
    // OTA password: same as ArduinoOTA (chip-ID based)
    snprintf(s_ota_pass, sizeof(s_ota_pass), "%08X",
             (uint32_t)(ESP.getEfuseMac() >> 16));

    s_server.on("/update", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (!req->authenticate(OTA_USER, s_ota_pass))
            return req->requestAuthentication("GalvOS OTA");
        String html(UPDATE_PAGE_TEMPLATE);
        html.replace("{{FW_VER}}", LASER_FW_VERSION);
        req->send(200, "text/html", html);
    });

    // ---- POST /api/ota/upload ---- firmware image (U_FLASH) ----
    s_server.on("/api/ota/upload", HTTP_POST,
        [](AsyncWebServerRequest* req) { sendOtaResult(req, s_fw_state); },
        [](AsyncWebServerRequest* req, String filename, size_t index,
           uint8_t* data, size_t len, bool final) {
            otaUploadBody(req, index, data, len, final, U_FLASH, s_fw_state,
                          filename.c_str(), "HTTP-OTA(fw)");
        });

    // ---- POST /api/ota/upload-fs ---- LittleFS image (U_SPIFFS) ----
    s_server.on("/api/ota/upload-fs", HTTP_POST,
        [](AsyncWebServerRequest* req) { sendOtaResult(req, s_fs_state); },
        [](AsyncWebServerRequest* req, String filename, size_t index,
           uint8_t* data, size_t len, bool final) {
            otaUploadBody(req, index, data, len, final, U_SPIFFS, s_fs_state,
                          filename.c_str(), "HTTP-OTA(fs)");
        });

    // OTA password in log (visible on serial -- for setup only)
    ESP_LOGW(TAG, "HTTP-OTA Auth: user='admin' pass='%s'", s_ota_pass);

    ESP_LOGI(TAG, "OTA ready | Hostname: %s | ArduinoOTA-PW: %s",
             gConfig.hostname, ota_pass);
}

void handle() {
    ArduinoOTA.handle();
}

bool uploadInProgress() {
    return s_fw_state.active || s_fs_state.active;
}

} // namespace ota_update
