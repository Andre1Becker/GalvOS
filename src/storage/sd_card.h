#pragma once
/**
 * sd_card.h -- SD cardn-Verwaltung
 * SPI2 bus (shared with DAC8562, own CS pin)
 */
#include <stdint.h>
#include <stddef.h>

namespace sd_card {

bool    init();                          // SPI + Mount, gibt false if no Karte
bool    isReady();
bool    remount();                       // SD.end() + re-init (hot-swap support)
void    eject();                         // SD.end() cleanly — safe to remove card

// File list: build .ild files in the /ilda/ directory
// Returns count of found files (max ILDA_MAX_FILES)
uint8_t scanFiles();

// Path for index i (0-based) -> full path "/ilda/name.ild"
const char* filePath(uint8_t idx);
const char* fileName(uint8_t idx);      // only filename ohne Pfad
uint8_t     fileCount();

// Free space on card in kB
uint32_t    freeKB();
uint32_t    totalKB();
const char* errorMsg();   // "OK" or human-readable error string
const char* fsType();     // "MMC"/"SDSC"/"SDHC" or "-"

} // namespace sd_card
