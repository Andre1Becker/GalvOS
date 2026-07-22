/**
 * sd_card.cpp -- SD card on its own independent SPI3 bus
 *
 * FIX vX.Y.Z: previously wired onto GPIO12/11/2/9, reusing the DAC8562's
 * SPI2 pins under the assumption that Arduino's SPIClass(HSPI) on
 * ESP32-S3 attaches to SPI2_HOST. It does not -- HSPI is bound to the
 * independent SPI3 peripheral (register base 0x60025000). Wiring it onto
 * SPI2's GPIOs meant two different peripherals both tried to drive the
 * same pins through the GPIO matrix, which only lets one peripheral own
 * a pin's output at a time: SPIClass::begin() silently stole GPIO12/11
 * away from the DAC every time it ran, and real SD traffic then showed
 * up on the DAC's SCK/MOSI lines -- the root cause of "galvo goes
 * erratic when a card is inserted".
 *
 * Now SD uses fully dedicated GPIOs on SPI3 -- no pins, no registers,
 * no bus in common with the DAC's SPI2 at all. Arduino's SD library
 * drives the SPIClass object directly (esp32-hal-spi.c raw HAL), not
 * the ESP-IDF spi_master driver -- unrelated to how galvo_out.cpp
 * talks to SPI2.
 *
 * Pins (all on SPI3, independent from galvo's SPI2):
 *   SCK  = GPIO5  (PIN_SD_SCK)
 *   MOSI = GPIO6  (PIN_SD_MOSI)
 *   MISO = GPIO1  (PIN_SD_MISO)
 *   CS   = GPIO42 (PIN_SD_CS)
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
#include "util/mem_registry.h"
#include "util/ps_scratch.h"
#include <driver/gpio.h>

static const char* TAG = "sd_card";

// Arduino SPIClass on SPI3_HOST — independent peripheral from the DAC's SPI2
static SPIClass s_sd_spi(HSPI);  // HSPI = SPI3_HOST on ESP32-S3

static bool     s_ready      = false;
static uint8_t  s_file_count = 0;
static char     s_error_msg[64] = "Not initialized";
static char     s_fs_type[8]    = "-";

// File tables (~7.7 KB) in PSRAM -- lazily allocated in init(), was DRAM .bss
typedef char PathRow[ILDA_MAX_PATH];
typedef char NameRow[64];
static PathRow* s_paths = nullptr;
static NameRow* s_names = nullptr;

namespace sd_card {

bool init() {
    if (!psScratch(s_paths, ILDA_MAX_FILES) || !psScratch(s_names, ILDA_MAX_FILES)) {
        strlcpy(s_error_msg, "File table alloc failed", sizeof(s_error_msg));
        ESP_LOGE(TAG, "SD: %s", s_error_msg);
        return false;
    }
    memreg::track("SD File Table", ILDA_MAX_FILES * (sizeof(PathRow) + sizeof(NameRow)), true);

    // SD runs on SPI3 via Arduino's SPIClass(HSPI) -- a completely
    // independent hardware peripheral from the DAC's SPI2, on its own
    // dedicated GPIOs (see pinmap.h for why the two must never share
    // pins). SPIClass drives it through esp32-hal-spi.c's raw register
    // HAL, not the ESP-IDF spi_master driver -- fine, since it no longer
    // touches anything galvo_out.cpp uses.

    // MISO idle pull-up: some cards tri-state DO while deselected: without
    // a pull-up the line floats and can be misread as garbage during the
    // idle-poll (0xFF) sequences below.
    gpio_reset_pin((gpio_num_t)PIN_SD_MISO);
    gpio_set_direction((gpio_num_t)PIN_SD_MISO, GPIO_MODE_INPUT);
    gpio_set_pull_mode((gpio_num_t)PIN_SD_MISO, GPIO_PULLUP_ONLY);
    vTaskDelay(pdMS_TO_TICKS(20));

    // Attach SPIClass to SD's dedicated SPI3 pins
    s_sd_spi.begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);

    ESP_LOGI(TAG, "SD on SPI3 (independent): SCK=%d MOSI=%d MISO=%d CS=%d",
             PIN_SD_SCK, PIN_SD_MOSI, PIN_SD_MISO, PIN_SD_CS);

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
    memset(s_paths, 0, ILDA_MAX_FILES * sizeof(PathRow));
    memset(s_names, 0, ILDA_MAX_FILES * sizeof(NameRow));

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
                    // full path
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
