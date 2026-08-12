#pragma once
/**
 * svg_store.h -- SVG file storage on the SD card, flat /svg/ directory.
 *
 * Shares the physical SD/SPI3 bus (and its mtx::sd lock) with sd_card.cpp,
 * but owns its own flat *.svg file table -- separate index space, own
 * extension filter, own playability check (an SVG needs XML well-formedness
 * + a rough element count, not sd_card's PSRAM-budget "too_large" logic).
 *
 * Lazy/self-contained: scanFiles() allocates its PSRAM table on first call
 * and re-checks sd_card::isReady() every time, so there is no separate
 * init() to wire into main.cpp's boot/remount sequencing -- calling
 * scanFiles() (already required after every upload/delete/rename) is
 * enough to pick up a card that was mounted after boot.
 */
#include <stdint.h>
#include <stddef.h>

namespace svg_store {

// Rescan /svg/ on the SD card. Returns 0 (and leaves the table empty) if the
// SD card isn't mounted. Creates /svg/ if missing.
uint8_t scanFiles();

bool deleteFile(uint8_t idx);
bool renameFile(uint8_t idx, const char* newName);

const char* filePath(uint8_t idx);   // "/svg/name.svg"
const char* fileName(uint8_t idx);   // display name (basename incl. ".svg")
uint32_t    fileSize(uint8_t idx);
uint32_t    fileMTime(uint8_t idx);
uint8_t     fileCount();

// Playability, computed once per scanFiles() call (see SVG_MAX_FILE_BYTES):
// oversized, malformed (no <svg> root), or no drawable elements found.
bool        playable(uint8_t idx);
const char* reason(uint8_t idx);     // human-readable reason when !playable(), "" otherwise
uint32_t    maxFileBytes();          // SVG_MAX_FILE_BYTES, exposed for the WebUI status line

} // namespace svg_store
