/**
 * svg_store.cpp -- flat *.svg file table on the SD card's /svg/ directory
 *
 * See svg_store.h for the lazy-init rationale. All SD access goes through
 * mtx::sd (LOCK_SD()) -- the same lock sd_card.cpp uses for the physical
 * SPI3 bus, since this module shares that card, not a separate one.
 */
#include "svg_store.h"
#include "sd_card.h"
#include "mutex.h"
#include "pinmap.h"
#include <Arduino.h>
#include <SD.h>
#include <string.h>
#include <ctype.h>
#include "util/mem_registry.h"
#include "util/ps_scratch.h"

static const char* TAG = "svg_store";

typedef char PathRow[SVG_MAX_PATH];
typedef char NameRow[80];
typedef char ReasonRow[48];

static uint8_t    s_file_count = 0;
static PathRow*   s_paths    = nullptr;
static NameRow*   s_names    = nullptr;
static uint32_t*  s_sizes    = nullptr;
static uint32_t*  s_mtimes   = nullptr;
static bool*      s_playable = nullptr;
static ReasonRow* s_reasons  = nullptr;
static char*      s_scanBuf  = nullptr;   // PSRAM scratch, SVG_MAX_FILE_BYTES+1, reused per file during scan

namespace svg_store {

static bool ensureAlloc() {
    if (s_paths) return true;
    if (!psScratch(s_paths, SVG_MAX_FILES) || !psScratch(s_names, SVG_MAX_FILES) ||
        !psScratch(s_sizes, SVG_MAX_FILES) || !psScratch(s_mtimes, SVG_MAX_FILES) ||
        !psScratch(s_playable, SVG_MAX_FILES) || !psScratch(s_reasons, SVG_MAX_FILES) ||
        !psScratch(s_scanBuf, SVG_MAX_FILE_BYTES + 1)) {
        ESP_LOGE(TAG, "PSRAM alloc failed for SVG file table");
        return false;
    }
    memreg::track("SVG File Table", SVG_MAX_FILES *
        (sizeof(PathRow) + sizeof(NameRow) + 2 * sizeof(uint32_t) + sizeof(bool) + sizeof(ReasonRow))
        + (SVG_MAX_FILE_BYTES + 1), true);
    return true;
}

// Reads the already-opened file f (positioned at 0, size == s_sizes[idx])
// and fills in s_playable[idx]/s_reasons[idx]. Kept out of scanFiles() for
// readability -- the three criteria from the Part 2 spec: size cap,
// well-formedness (an <svg> root tag), and a rough drawable-element count.
static void checkPlayability(File& f, uint8_t idx) {
    if (s_sizes[idx] == 0) {
        s_playable[idx] = false;
        strlcpy(s_reasons[idx], "empty file", sizeof(ReasonRow));
        return;
    }
    if (s_sizes[idx] > SVG_MAX_FILE_BYTES) {
        s_playable[idx] = false;
        snprintf(s_reasons[idx], sizeof(ReasonRow), "too large (max %lu KB)",
                 (unsigned long)(SVG_MAX_FILE_BYTES / 1024));
        return;
    }

    size_t n = f.read((uint8_t*)s_scanBuf, s_sizes[idx]);
    s_scanBuf[n] = 0;

    if (strstr(s_scanBuf, "<svg") == nullptr) {
        s_playable[idx] = false;
        strlcpy(s_reasons[idx], "malformed: no <svg> root element", sizeof(ReasonRow));
        return;
    }

    static const char* kShapeTags[] = {
        "<path", "<rect", "<circle", "<ellipse", "<line", "<polyline", "<polygon"
    };
    int elemCount = 0;
    for (size_t t = 0; t < sizeof(kShapeTags) / sizeof(kShapeTags[0]); t++) {
        const char* p = s_scanBuf;
        while ((p = strstr(p, kShapeTags[t])) != nullptr) { elemCount++; p += strlen(kShapeTags[t]); }
    }
    if (elemCount == 0) {
        s_playable[idx] = false;
        strlcpy(s_reasons[idx], "no drawable elements found", sizeof(ReasonRow));
        return;
    }

    s_playable[idx] = true;
    s_reasons[idx][0] = 0;
}

uint8_t scanFiles() {
    if (!ensureAlloc()) return 0;
    LOCK_SD();
    s_file_count = 0;
    if (!sd_card::isReady()) return 0;

    if (!SD.exists("/svg")) {
        SD.mkdir("/svg");
        ESP_LOGI(TAG, "directory /svg created");
    }

    memset(s_paths,    0, SVG_MAX_FILES * sizeof(PathRow));
    memset(s_names,    0, SVG_MAX_FILES * sizeof(NameRow));
    memset(s_sizes,    0, SVG_MAX_FILES * sizeof(uint32_t));
    memset(s_mtimes,   0, SVG_MAX_FILES * sizeof(uint32_t));
    memset(s_playable, 0, SVG_MAX_FILES * sizeof(bool));
    memset(s_reasons,  0, SVG_MAX_FILES * sizeof(ReasonRow));

    File dir = SD.open("/svg");
    if (!dir || !dir.isDirectory()) { if (dir) dir.close(); return 0; }

    File f = dir.openNextFile();
    while (f && s_file_count < SVG_MAX_FILES) {
        if (!f.isDirectory()) {
            const char* name = f.name();
            size_t len = strlen(name);
            if (len > 4 && strcasecmp(name + len - 4, ".svg") == 0) {
                uint8_t i = s_file_count;
                snprintf(s_paths[i], SVG_MAX_PATH, "/svg/%s", name);
                strlcpy(s_names[i], name, sizeof(NameRow));
                s_sizes[i]  = (uint32_t)f.size();
                s_mtimes[i] = (uint32_t)f.getLastWrite();
                checkPlayability(f, i);
                ESP_LOGI(TAG, "  [%u] %s (%u bytes)%s", i, s_names[i], s_sizes[i],
                          s_playable[i] ? "" : " [blocked]");
                s_file_count++;
            }
        }
        f.close();
        f = dir.openNextFile();
    }
    dir.close();

    ESP_LOGI(TAG, "SVG scan: %u files found", s_file_count);
    return s_file_count;
}

bool deleteFile(uint8_t idx) {
    if (idx >= s_file_count) return false;
    LOCK_SD();
    bool ok = SD.remove(s_paths[idx]);
    if (!ok) ESP_LOGE(TAG, "delete failed: %s", s_paths[idx]);
    return ok;
}

bool renameFile(uint8_t idx, const char* newName) {
    if (idx >= s_file_count || !newName || !newName[0]) return false;
    LOCK_SD();
    char newPath[SVG_MAX_PATH];
    snprintf(newPath, sizeof(newPath), "/svg/%s", newName);
    if (strcmp(newPath, s_paths[idx]) == 0) return true;
    if (SD.exists(newPath)) { ESP_LOGW(TAG, "rename target exists: %s", newPath); return false; }
    bool ok = SD.rename(s_paths[idx], newPath);
    if (!ok) ESP_LOGE(TAG, "rename failed: %s -> %s", s_paths[idx], newPath);
    return ok;
}

const char* filePath(uint8_t idx)  { return idx < s_file_count ? s_paths[idx]   : nullptr; }
const char* fileName(uint8_t idx)  { return idx < s_file_count ? s_names[idx]   : nullptr; }
uint32_t    fileSize(uint8_t idx)  { return idx < s_file_count ? s_sizes[idx]   : 0; }
uint32_t    fileMTime(uint8_t idx) { return idx < s_file_count ? s_mtimes[idx]  : 0; }
uint8_t     fileCount()            { return s_file_count; }
bool        playable(uint8_t idx)  { return idx < s_file_count ? s_playable[idx] : false; }
const char* reason(uint8_t idx)    { return idx < s_file_count ? s_reasons[idx]  : "invalid index"; }
uint32_t    maxFileBytes()         { return SVG_MAX_FILE_BYTES; }

} // namespace svg_store
