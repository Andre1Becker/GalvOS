#include "ota_update.h"
#include "config.h"
#include "safety/safety.h"
#include <Arduino.h>
#include <ArduinoOTA.h>
#include <Update.h>
#include <ESPAsyncWebServer.h>
#include "web_ui.h"
#include <esp_log.h>

namespace ota_update {

static const char* TAG = "ota";

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
        ESP_LOGI(TAG, "OTA Ende — Neustart");
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

    // ── HTTP-Upload-Endpoint /api/ota/upload ──────────────────
    // Liefert Upload-Seite
    // OTA password: same as ArduinoOTA (chip-ID based)
    static char s_ota_pass[12];
    snprintf(s_ota_pass, sizeof(s_ota_pass), "%08X",
             (uint32_t)(ESP.getEfuseMac() >> 16));
    static const char* OTA_USER = "admin";

    s_server.on("/update", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (!req->authenticate(OTA_USER, s_ota_pass))
            return req->requestAuthentication("Mikoy Laser OTA");
        req->send(200, "text/html",
            "<html><head><meta charset='UTF-8'>"
            "<title>OTA Update</title></head><body>"
            "<h2>Mikoy Laser — Firmware Update</h2>"
            "<p style='font-family:monospace;color:#666'>Passwort: Chip-ID (see Serial / Dashboard)</p>"
            "<form method='POST' action='/api/ota/upload' "
            "enctype='multipart/form-data'>"
            "<input type='file' name='firmware' accept='.bin'><br><br>"
            "<input type='submit' value='Firmware hochload'>"
            "</form></body></html>");
    });

    s_server.on("/api/ota/upload", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            if (!req->authenticate(OTA_USER, s_ota_pass))
                return req->requestAuthentication("Mikoy Laser OTA");
            bool ok = !Update.hasError();
            AsyncWebServerResponse* r = req->beginResponse(
                ok ? 200 : 500, "text/plain",
                ok ? "Update OK — ESP32 startet neu..." : "Update failed");
            r->addHeader("Connection", "close");
            req->send(r);
            if (ok) {
                vTaskDelay(pdMS_TO_TICKS(500));
                ESP.restart();
            }
        },
        [](AsyncWebServerRequest* req, String filename, size_t index,
           uint8_t* data, size_t len, bool final) {
            // safetyspruefungen vor OTA-Start
            if (index == 0) {
                if (!req->authenticate(OTA_USER, s_ota_pass)) {
                    req->send(401, "text/plain", "Unauthorized");
                    return;
                }
                if (gState.laser_armed.load()) {
                    ESP_LOGE(TAG, "OTA rejected: laser is armed!");
                    req->send(403, "text/plain", "Laser armed — OTA not erlaubt");
                    return;
                }
                ESP_LOGI(TAG, "HTTP-OTA Start: %s", filename.c_str());
                safety::emergencyStop();
                if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
                    ESP_LOGE(TAG, "Update.begin() failed: %s",
                             Update.errorString());
                    return;
                }
            }
            // write() Rueckgabe check
            if (len) {
                size_t written = Update.write(data, len);
                if (written != len)
                    ESP_LOGE(TAG, "OTA write mismatch: %u/%u", written, len);
            }
            if (final) {
                if (Update.end(true)) {
                    ESP_LOGI(TAG, "HTTP-OTA fertig: %u Bytes", index+len);
                } else {
                    ESP_LOGE(TAG, "HTTP-OTA end() failed: %s",
                             Update.errorString());
                }
            }
        });

    // OTA password in log (visible on serial -- for setup only)
    ESP_LOGW(TAG, "HTTP-OTA Auth: user='admin' pass='%s'", s_ota_pass);

    ESP_LOGI(TAG, "OTA bereit | Hostname: %s | ArduinoOTA-PW: %s",
             gConfig.hostname, ota_pass);
}

void handle() {
    ArduinoOTA.handle();
}

} // namespace ota_update
