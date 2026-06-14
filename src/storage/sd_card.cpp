/**
 * sd_card.cpp -- SD card on SPI2_HOST (shared with DAC8562)
 *
 * Architecture:
 *   - DAC8562 and SD card share SPI2_HOST (initialized by galvo::init())
 *   - galvo::init() configures SPI2 with MISO=GPIO2, DMA=AUTO
 *   - SD card added as second SPI2 device via spi_bus_add_device(CS=GPIO9)
 *   - galvo DAC uses CS=GPIO10, SD uses CS=GPIO9 — fully independent
 *   - Arduino SD library wraps the IDF device handle via SdSpiDriver
 *
 * Pins (all on SPI2 bus):
 *   SCK  = GPIO12 (PIN_GALVO_SCK,  shared)
 *   MOSI = GPIO11 (PIN_GALVO_MOSI, shared)
 *   MISO = GPIO2  (PIN_SD_MISO,    SD only — DAC has no MISO)
 *   CS   = GPIO9  (PIN_SD_CS,      SD only)
 *   CS   = GPIO10 (PIN_GALVO_CS,   DAC only)
 *
 * Note: GPIO39/41 are no longer used for SD. They remain freed from
 *       NE555/Encoder assignments but are currently unconnected.
 */
#include "sd_card.h"
#include "mutex.h"
#include "pinmap.h"
#include <Arduino.h>
#include <SD.h>
#include <SPI.h>
#include <esp_log.h>
#include <string.h>
#include "util/log_buffer.h"
#include <driver/gpio.h>
#include <driver/spi_master.h>
#include "output/galvo_out.h"

static const char* TAG = "sd_card";

// Arduino SPIClass wrapping SPI2_HOST — same bus as DAC, different CS
static SPIClass s_sd_spi(HSPI);  // HSPI = SPI2_HOST on ESP32-S3

static bool     s_ready      = false;
static uint8_t  s_file_count = 0;
static char     s_error_msg[64] = "Not initialized";
static char     s_fs_type[8]    = "-";

static char     s_paths[ILDA_MAX_FILES][ILDA_MAX_PATH];
static char     s_names[ILDA_MAX_FILES][64];

namespace sd_card {

bool init() {
    // SPI2 bus is already initialized by galvo::init() with:
    //   MOSI=GPIO11, MISO=GPIO2, SCK=GPIO12, DMA=AUTO
    // We add the SD card as a second device on the same bus (CS=GPIO9).
    // The Arduino SD library uses SPIClass — attach it to SPI2 (HSPI)
    // with the correct pins. SPIClass::begin() on an already-initialized
    // IDF bus is safe: it calls spi_bus_add_device() internally when
    // the bus was set up by the same Arduino SPI infrastructure.
    //
    // IMPORTANT: galvo uses the IDF low-level API directly (spi_device_polling_transmit).
    // We must ensure SD transactions use SPI transactions (not polling) to avoid
    // collisions. The Arduino SD library uses spi_device_transmit() which is
    // properly queued.

    // GPIO2 strapping pin: ensure pull-up is active
    gpio_reset_pin((gpio_num_t)PIN_SD_MISO);
    gpio_set_direction((gpio_num_t)PIN_SD_MISO, GPIO_MODE_INPUT);
    gpio_set_pull_mode((gpio_num_t)PIN_SD_MISO, GPIO_PULLUP_ONLY);
    vTaskDelay(pdMS_TO_TICKS(20));

    // Attach SPIClass to SPI2 with the shared pins + SD CS
    s_sd_spi.begin(PIN_GALVO_SCK, PIN_SD_MISO, PIN_GALVO_MOSI, PIN_SD_CS);

    ESP_LOGI(TAG, "SD on SPI2: SCK=%d MOSI=%d MISO=%d CS=%d",
             PIN_GALVO_SCK, PIN_GALVO_MOSI, PIN_SD_MISO, PIN_SD_CS);

    // SD spec: >=74 clock pulses with CS=HIGH before CMD0
    pinMode(PIN_SD_CS, OUTPUT);
    digitalWrite(PIN_SD_CS, HIGH);
    s_sd_spi.beginTransaction(SPISettings(400000, MSBFIRST, SPI_MODE0));
    for (int i = 0; i < 10; i++) s_sd_spi.transfer(0xFF);
    s_sd_spi.endTransaction();
    vTaskDelay(pdMS_TO_TICKS(20));

    LOCK_SD();
    bool mounted = false;
    for (int attempt = 1; attempt <= 3 && !mounted; attempt++) {
        mounted = SD.begin(PIN_SD_CS, s_sd_spi, 4000000);
        if (!mounted) {
            ESP_LOGW(TAG, "SD: mount attempt %d/3 failed — retrying...", attempt);
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }
    if (!mounted) {
        strlcpy(s_error_msg,
            "Mount failed - card in slot? Check SPI2 wiring.",
            sizeof(s_error_msg));
        ESP_LOGE(TAG, "SD: %s", s_error_msg);
        LOG_W(logbuf::CAT_SYSTEM, "SD: Mount failed");
        s_ready = false; return false;
    }

    uint8_t cardType = SD.cardType();
    if (cardType == CARD_NONE) {
        strlcpy(s_error_msg, "No card detected in slot", sizeof(s_error_msg));
        ESP_LOGE(TAG, "SD: %s", s_error_msg);
        s_ready = false; return false;
    }

    const char* types[] = {"NONE","MMC","SDSC","SDHC","UNKNOWN"};
    const char* typeStr  = types[cardType < 5 ? cardType : 4];
    strlcpy(s_fs_type,   typeStr, sizeof(s_fs_type));
    strlcpy(s_error_msg, "OK",    sizeof(s_error_msg));
    uint64_t totalMB = SD.totalBytes() / (1024*1024);
    uint64_t freeMB  = (SD.totalBytes() - SD.usedBytes()) / (1024*1024);
    ESP_LOGI(TAG, "SD card OK: type=%s  %llu MB total  %llu MB free", typeStr, totalMB, freeMB);
    LOG_I(logbuf::CAT_SYSTEM, "SD: OK type=%s %lluMB free", typeStr, freeMB);
    s_ready = true;

    if (!SD.exists("/ilda")) {
        SD.mkdir("/ilda");
        ESP_LOGI(TAG, "directory /ilda created");
    }

    scanFiles();
    return true;
}

bool isReady() { return s_ready; }

void eject() {
    LOCK_SD();
    if (s_ready) {
        SD.end();
        s_ready = false;
        strlcpy(s_error_msg, "Ejected - safe to remove", sizeof(s_error_msg));
        strlcpy(s_fs_type, "-", sizeof(s_fs_type));
        s_file_count = 0;
        ESP_LOGI(TAG, "SD card ejected safely");
        LOG_I(logbuf::CAT_SYSTEM, "SD: ejected - safe to remove card");
    }
}

bool remount() {
    if (s_ready) {
        LOCK_SD();
        SD.end();
        s_ready = false;
    }
    strlcpy(s_error_msg, "Remounting...", sizeof(s_error_msg));
    ESP_LOGI(TAG, "SD: remounting...");
    LOG_I(logbuf::CAT_SYSTEM, "SD: remounting...");
    bool ok = init();
    if (ok) {
        LOG_I(logbuf::CAT_SYSTEM, "SD: remount OK");
    } else {
        LOG_W(logbuf::CAT_SYSTEM, "SD: remount failed");
    }
    return ok;
}


uint8_t scanFiles() {
    LOCK_SD();
    if (!s_ready) return 0;
    s_file_count = 0;
    memset(s_paths, 0, sizeof(s_paths));
    memset(s_names, 0, sizeof(s_names));

    File root = SD.open("/ilda");
    if (!root || !root.isDirectory()) {
        ESP_LOGW(TAG, "/ilda directory unreadable");
        return 0;
    }

    File f = root.openNextFile();
    while (f && s_file_count < ILDA_MAX_FILES) {
        if (!f.isDirectory()) {
            const char* name = f.name();
            size_t len = strlen(name);
            // Only .ild files (case-insensitive)
            if (len > 4) {
                const char* ext = name + len - 4;
                if (strcasecmp(ext, ".ild") == 0) {
                    // Full Pfad
                    snprintf(s_paths[s_file_count], ILDA_MAX_PATH, "/ilda/%s", name);
                    strncpy(s_names[s_file_count], name, 63);
                    s_names[s_file_count][63] = '\0';
                    ESP_LOGI(TAG, "  [%u] %s", s_file_count, s_names[s_file_count]);
                    s_file_count++;
                }
            }
        }
        f.close();
        f = root.openNextFile();
    }
    root.close();
    ESP_LOGI(TAG, "SD scan: %u ILDA files found", s_file_count);
    return s_file_count;
}

const char* filePath(uint8_t idx) {
    if (idx >= s_file_count) return nullptr;
    return s_paths[idx];
}
const char* fileName(uint8_t idx) {
    if (idx >= s_file_count) return nullptr;
    return s_names[idx];
}
uint8_t  fileCount()    { return s_file_count; }
uint32_t freeKB()       { return s_ready ? (uint32_t)((SD.totalBytes()-SD.usedBytes())/1024) : 0; }
uint32_t totalKB()      { return s_ready ? (uint32_t)(SD.totalBytes()/1024) : 0; }
const char* errorMsg()  { return s_error_msg; }
const char* fsType()    { return s_fs_type;   }

} // namespace sd_card
