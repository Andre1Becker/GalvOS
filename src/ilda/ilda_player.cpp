/**
 * ilda_player.cpp -- ILDA .ild file player
 *
 * ILDA binary format (big-endian):
 *   Each section header: 32 bytes
 *     bytes  0- 3: "ILDA"  magic
 *     Bytes  4- 6: reserved (0)
 *     Byte   7:    Format-ID
 *     Bytes  8-15: Frame/Section-Name (ASCII, null-padded)
 *     Bytes 16-23: Company-Name
 *     Bytes 24-25: Point-Count (uint16 big-endian)
 *     Bytes 26-27: Frame-Number
 *     Bytes 28-29: Total-Frames
 *     Byte  30:    Scanner-Head
 *     Byte  31:    reserved
 *
 * Point formats:
 *   Fmt 0 (3D indexed):   X(i16), Y(i16), Z(i16), Status(u8), ColorIdx(u8) = 8B
 *   Fmt 1 (2D indexed):   X(i16), Y(i16),          Status(u8), ColorIdx(u8) = 6B
 *   Fmt 4 (3D true-color):X(i16), Y(i16), Z(i16), Status(u8), B(u8), G(u8), R(u8) = 10B (1 Pad)
 *   Fmt 5 (2D true-color): X(i16), Y(i16),         Status(u8), B(u8), G(u8), R(u8) = 8B (1 Pad)
 *
 * Status byte: Bit 6 = Blanking (1=laser off)
 */
#include "ilda_player.h"
#include "storage/sd_card.h"
#include "mutex.h"
#include "output/galvo_out.h"
#include "util/mem_registry.h"
#include <Arduino.h>
#include <SD.h>
#include <string.h>
#include <esp_log.h>

static const char* TAG = "ilda";

namespace ilda {

// ── global state ────────────────────────────────────────────────────
ILDAConfig gILDA;

// ── ILDA default color palette (64 colors, indexed format) ──────
static const uint8_t ILDA_PALETTE[64][3] = {
    {255,0,0},{255,16,0},{255,32,0},{255,48,0},{255,64,0},{255,80,0},
    {255,96,0},{255,112,0},{255,128,0},{255,144,0},{255,160,0},{255,176,0},
    {255,192,0},{255,208,0},{255,224,0},{255,240,0},{255,255,0},{224,255,0},
    {192,255,0},{160,255,0},{128,255,0},{96,255,0},{64,255,0},{32,255,0},
    {0,255,0},{0,255,32},{0,255,64},{0,255,96},{0,255,128},{0,255,160},
    {0,255,192},{0,255,224},{0,255,255},{0,224,255},{0,192,255},{0,160,255},
    {0,128,255},{0,96,255},{0,64,255},{0,32,255},{0,0,255},{32,0,255},
    {64,0,255},{96,0,255},{128,0,255},{160,0,255},{192,0,255},{224,0,255},
    {255,0,255},{255,0,224},{255,0,192},{255,0,160},{255,0,128},{255,0,96},
    {255,0,64},{255,0,32},{128,128,128},{160,160,160},{192,192,192},
    {224,224,224},{255,255,255},{255,255,255},{255,255,255},{0,0,0}
};

// ── Frame structure in PSRAM ──────────────────────────────────────
struct ILDAFrame {
    uint16_t    point_count;
    LaserPoint* points;      // points into PSRAM pool
};

static ILDAFrame*   s_frames     = nullptr;  // array in PSRAM
static LaserPoint*  s_point_pool = nullptr;  // large PSRAM block
static uint16_t     s_frame_count = 0;
static volatile uint16_t s_play_frame = 0;
static volatile bool     s_has_new   = false;
static volatile bool     s_playing   = false;
static volatile bool     s_paused    = false;
static TaskHandle_t s_task = nullptr;

// ── Read ILDA header ────────────────────────────────────────────
struct ILDAHeader {
    char     magic[4];
    uint8_t  reserved[3];
    uint8_t  format;
    char     frame_name[8];
    char     company[8];
    uint16_t point_count;   // big-endian!
    uint16_t frame_num;
    uint16_t total_frames;
    uint8_t  scanner_head;
    uint8_t  reserved2;
};

static inline uint16_t be16(uint8_t* p) { return ((uint16_t)p[0]<<8)|p[1]; }
static inline int16_t  bei16(uint8_t* p) { return (int16_t)be16(p); }

// ── load file and all frames into PSRAM buffer ─────────────────
static bool loadILDA(const char* path) {
    // free previous allocation
    if (s_frames)    { heap_caps_free(s_frames); s_frames = nullptr; memreg::untrack("ILDA Frames"); }
    if (s_point_pool){ heap_caps_free(s_point_pool); s_point_pool = nullptr; memreg::untrack("ILDA Point Pool"); }
    s_frame_count = 0;

    LOCK_SD();
    File f = SD.open(path);
    if (!f) {
        ESP_LOGE(TAG, "file not found: %s", path);
        return false;
    }

    // ── Pass 1: count (total frames + points) ────────────────────
    uint16_t total_frames = 0;
    uint32_t total_points = 0;
    uint8_t  hdr_buf[32];

    while (f.available() >= 32) {
        if (f.read(hdr_buf, 32) != 32) break;
        if (memcmp(hdr_buf, "ILDA", 4) != 0) break;

        uint8_t  fmt = hdr_buf[7];
        uint16_t npts = be16(&hdr_buf[24]);

        if (npts == 0) break;  // End-of-File-Marker

        uint8_t pt_size = 0;
        switch (fmt) {
            case 0: pt_size = 8;  break;  // 3D indexed
            case 1: pt_size = 6;  break;  // 2D indexed
            case 4: pt_size = 10; break;  // 3D true-color (with 1 pad byte)
            case 5: pt_size = 8;  break;  // 2D true-color (with 1 pad byte)
            default:
                ESP_LOGW(TAG, "Unknown ILDA format %u, skipping frame", fmt);
                f.seek(f.position() + npts * 8);
                continue;
        }
        total_frames++;
        total_points += npts;
        f.seek(f.position() + npts * pt_size);
    }

    if (total_frames == 0) {
        ESP_LOGW(TAG, "No valid frames in %s", path);
        f.close();
        return false;
    }

    // ── allocate PSRAM ───────────────────────────────────────────
    s_frames = (ILDAFrame*)ps_malloc(total_frames * sizeof(ILDAFrame));
    s_point_pool = (LaserPoint*)ps_malloc(total_points * sizeof(LaserPoint));

    if (!s_frames || !s_point_pool) {
        ESP_LOGE(TAG, "PSRAM allocation failed (frames=%u, points=%u)",
                 total_frames, total_points);
        if (s_frames) { heap_caps_free(s_frames); s_frames = nullptr; }
        if (s_point_pool) { heap_caps_free(s_point_pool); s_point_pool = nullptr; }
        f.close();
        return false;
    }
    memreg::track("ILDA Frames", total_frames * sizeof(ILDAFrame), true);
    memreg::track("ILDA Point Pool", total_points * sizeof(LaserPoint), true);

    // ── Pass 2: read data ─────────────────────────────────────────
    f.seek(0);
    uint32_t pool_offset = 0;
    uint16_t fi = 0;

    while (f.available() >= 32 && fi < total_frames) {
        if (f.read(hdr_buf, 32) != 32) break;
        if (memcmp(hdr_buf, "ILDA", 4) != 0) break;

        uint8_t  fmt  = hdr_buf[7];
        uint16_t npts = be16(&hdr_buf[24]);
        if (npts == 0) break;

        s_frames[fi].point_count = npts;
        s_frames[fi].points = &s_point_pool[pool_offset];

        uint8_t raw[10];  // max. 10 bytes per point
        uint8_t pt_size = (fmt==0)?8 : (fmt==1)?6 : (fmt==4)?10 : 8;

        for (uint16_t pi = 0; pi < npts; pi++) {
            if (f.read(raw, pt_size) != pt_size) break;
            LaserPoint& lp = s_point_pool[pool_offset + pi];

            int16_t x, y;
            uint8_t status;

            switch (fmt) {
                case 0:  // 3D indexed
                    x = bei16(raw);   y = bei16(raw+2);
                    status = raw[6];
                    if (status & 0x40) {
                        lp = LaserPoint(x, y, 0, 0, 0, 1);
                    } else {
                        uint8_t ci = raw[7] & 0x3F;
                        lp = LaserPoint(x, y, ILDA_PALETTE[ci][0],
                                         ILDA_PALETTE[ci][1], ILDA_PALETTE[ci][2], 0);
                    }
                    break;
                case 1:  // 2D indexed
                    x = bei16(raw);   y = bei16(raw+2);
                    status = raw[4];
                    if (status & 0x40) {
                        lp = LaserPoint(x, y, 0, 0, 0, 1);
                    } else {
                        uint8_t ci = raw[5] & 0x3F;
                        lp = LaserPoint(x, y, ILDA_PALETTE[ci][0],
                                         ILDA_PALETTE[ci][1], ILDA_PALETTE[ci][2], 0);
                    }
                    break;
                case 4:  // 3D true-color (10 Byte: X,Y,Z,status,pad,B,G,R)
                    x = bei16(raw);   y = bei16(raw+2);
                    status = raw[6];
                    lp = LaserPoint(x, y, raw[9], raw[8], raw[7],
                                    (status & 0x40) ? 1 : 0);
                    break;
                case 5:  // 2D true-color (8 Byte: X,Y,status,pad,B,G,R)
                    x = bei16(raw);   y = bei16(raw+2);
                    status = raw[4];
                    lp = LaserPoint(x, y, raw[7], raw[6], raw[5],
                                    (status & 0x40) ? 1 : 0);
                    break;
            }
        }

        pool_offset += npts;
        fi++;
    }

    f.close();
    s_frame_count = fi;
    gILDA.total_frames = fi;
    gILDA.total_points = pool_offset;

    ESP_LOGI(TAG, "ILDA loaded: %u frames, %u points, PSRAM: %u kB",
             fi, pool_offset, (uint32_t)((fi*sizeof(ILDAFrame) + pool_offset*8) / 1024));
    return true;
}

// ── Playback-Task ────────────────────────────────────────────────────
static void ildaTask(void*) {
    while (s_playing) {
        if (!s_paused && s_frame_count > 0) {
            // Frame-Rate: speed=128 → ~30fps, speed=255 → ~60fps, speed=1 → ~5fps
            uint32_t delay_ms = (uint32_t)(1000.0f / (5.0f + gILDA.speed / 4.5f));
            gILDA.current_frame = s_play_frame;
            s_has_new = true;

            vTaskDelay(pdMS_TO_TICKS(delay_ms));

            s_play_frame++;
            if (s_play_frame >= s_frame_count) {
                if (gILDA.loop) {
                    s_play_frame = 0;
                } else {
                    s_play_frame = s_frame_count - 1;
                    s_paused = true;
                }
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
    vTaskDelete(nullptr);
}

// ── Oeffentliche API ─────────────────────────────────────────────────
void init() {
    gILDA = ILDAConfig{};
}

bool loadFile(uint8_t idx) {
    const char* path = sd_card::filePath(idx);
    if (!path) {
        ESP_LOGW(TAG, "file index %u invalid (max %u)", idx, sd_card::fileCount()-1);
        return false;
    }
    stop();
    if (!loadILDA(path)) return false;

    gILDA.file_idx = (int8_t)idx;
    gILDA.active   = true;
    s_play_frame   = 0;
    s_playing      = true;
    s_paused       = false;

    xTaskCreatePinnedToCore(ildaTask, "ilda_play", 4096, nullptr, 4, &s_task, 0);
    ESP_LOGI(TAG, "ILDA started: %s", sd_card::fileName(idx));
    return true;
}

void stop() {
    s_playing = false;
    if (s_task) { vTaskDelay(pdMS_TO_TICKS(50)); s_task = nullptr; }
    gILDA.active = false;
    gILDA.file_idx = -1;
    s_has_new = false;
}

void pause(bool p) { s_paused = p; }

size_t getFrame(LaserPoint* out, size_t max_pts) {
    if (!gILDA.active || !s_frames || s_frame_count == 0) return 0;
    uint16_t fi = s_play_frame;
    if (fi >= s_frame_count) fi = s_frame_count - 1;

    const ILDAFrame& fr = s_frames[fi];
    size_t n = (fr.point_count < max_pts) ? fr.point_count : max_pts;

    // apply scaling (ILDA coordinates +-32767 -> galvo +-32767)
    float scale = 0.25f + (gILDA.size_val / 255.f) * 1.5f;
    for (size_t i = 0; i < n; i++) {
        out[i] = fr.points[i];
        if (scale != 1.0f && !out[i].blank) {
            out[i].x = (int16_t)constrain(out[i].x * scale, -32767.f, 32767.f);
            out[i].y = (int16_t)constrain(out[i].y * scale, -32767.f, 32767.f);
        }
    }
    s_has_new = false;
    return n;
}

bool hasNewFrame() { return s_has_new; }

void setFromDMX(uint8_t dmx_val) {
    if (dmx_val == 0) {
        if (gILDA.active) stop();
        return;
    }
    uint8_t fc = sd_card::fileCount();
    if (fc == 0) return;

    uint8_t idx;
    if (dmx_val == 255) {
        idx = fc - 1;
    } else {
        idx = (uint8_t)(dmx_val - 1);  // 1-40 → 0-39
        if (idx >= fc) idx = fc - 1;
    }

    // Only reload if different index
    if (!gILDA.active || gILDA.file_idx != (int8_t)idx) {
        loadFile(idx);
    }
}

} // namespace ilda
