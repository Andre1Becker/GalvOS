#include "playlist.h"
#include "sd_card.h"
#include "ilda/ilda_player.h"
#include "config.h"
#include "mutex.h"
#include <Arduino.h>
#include <SD.h>
#include <ArduinoJson.h>
#include <esp_log.h>

namespace playlist {

static const char* TAG = "playlist";
static volatile bool s_active = false;
static TaskHandle_t  s_task_h = nullptr;

bool loadFromSD() {
    gPlaylist.count = 0;
    gPlaylist.current = 0;

    if (!sd_card::isReady()) return false;
    LOCK_SD();

    File f = SD.open("/playlist.json");
    if (!f) {
        ESP_LOGW(TAG, "/playlist.json not found");
        return false;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) {
        ESP_LOGE(TAG, "JSON Error: %s", err.c_str());
        return false;
    }

    JsonArray arr = doc.as<JsonArray>();
    uint8_t n = 0;
    for (JsonObject entry : arr) {
        if (n >= PLAYLIST_MAX_ENTRIES) break;
        gPlaylist.entries[n].file_idx   = entry["file"]     | 0;
        gPlaylist.entries[n].loop_count = entry["loop"]     | 1;
        gPlaylist.entries[n].pause_ms   = entry["pause_ms"] | 0;
        n++;
    }
    gPlaylist.count    = n;
    gPlaylist.loop_all = doc["loop_all"] | true;

    ESP_LOGI(TAG, "Playlist: %u entries, loop_all=%d", n, gPlaylist.loop_all);
    return n > 0;
}

void start() {
    if (gPlaylist.count == 0) {
        ESP_LOGW(TAG, "Playlist empty"); return;
    }
    gPlaylist.active  = true;
    gPlaylist.current = 0;
    s_active = true;
    ESP_LOGI(TAG, "Playlist started");
}

void stop() {
    s_active = false;
    gPlaylist.active = false;
    ilda::stop();
}

bool isActive()       { return s_active && gPlaylist.active; }
uint8_t currentEntry(){ return gPlaylist.current; }

void task(void*) {
    for (;;) {
        if (!s_active || gPlaylist.count == 0) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        uint8_t idx = gPlaylist.current;
        const PlaylistEntry& e = gPlaylist.entries[idx];

        // file load and abspielen
        bool ok = ilda::loadFile(e.file_idx);
        if (!ok) {
            ESP_LOGW(TAG, "entry %u: file %u not loadable", idx, e.file_idx);
            goto next_entry;
        }

        ilda::gILDA.loop = (e.loop_count == 0);
        ilda::gILDA.speed = 128;

        // wait until done (loop_count times)
        if (e.loop_count > 0) {
            for (uint8_t lp = 0; lp < e.loop_count; lp++) {
                // Wait for end of frame
                uint32_t timeout = millis() + 30000; // max 30s per loop
                while (s_active && millis() < timeout) {
                    if (ilda::gILDA.current_frame >= ilda::gILDA.total_frames - 1)
                        break;
                    vTaskDelay(pdMS_TO_TICKS(100));
                }
                if (!s_active) goto done;
                // next loop iteration: reset frame
                ilda::stop();
                if (lp < e.loop_count - 1) ilda::loadFile(e.file_idx);
            }
        } else {
            // Infinite -> continue after 60 seconds
            vTaskDelay(pdMS_TO_TICKS(60000));
        }

        // inter-entry pause
        if (e.pause_ms > 0 && s_active) {
            ilda::stop();
            vTaskDelay(pdMS_TO_TICKS(e.pause_ms));
        }

        next_entry:
        gPlaylist.current = (idx + 1) % gPlaylist.count;
        if (gPlaylist.current == 0 && !gPlaylist.loop_all) {
            ESP_LOGI(TAG, "Playlist end");
            s_active = false;
            gPlaylist.active = false;
        }
        continue;

        done:
        s_active = false;
        gPlaylist.active = false;
    }
}

} // namespace playlist
