/**
 * helios_net.cpp -- Helios DAC network emulation
 *
 * See helios_net.h for the frame layout. Architecture mirrors etherdream.cpp:
 * a single TCP data connection, points converted straight into LaserPoint and
 * handed to galvo::pushFrame(). No UDP discovery beacon -- Helios clients are
 * expected to be configured with the projector's IP directly.
 */
#include "helios_net.h"
#include "config.h"
#include "output/galvo_out.h"
#include "patterns/point_optimizer.h"
#include "safety/safety.h"
#include "util/log_buffer.h"
#include "util/mem_registry.h"
#include "util/ps_scratch.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_log.h>
#include <string.h>
#include <math.h>

namespace helios_net {

static const char* TAG = "helios_net";
static const uint16_t PORT_DATA = 7768;   // TCP data (7654/7765 taken by EtherDream)

// Points beyond this are refused wholesale (see handleClient() CMD_DATA-style
// guard in etherdream.cpp) -- a client asking for more than this either has a
// bug or is not speaking this protocol; there is no safe way to buffer an
// unbounded claimed count.
static const uint16_t MAX_RAW_PTS = 2000;

struct __attribute__((packed)) FrameHeader {
    uint16_t point_rate;    // informational, not currently enforced
    uint16_t point_count;
    uint8_t  flags;         // bit0: 1 = play, 0 = stop/idle
};

struct __attribute__((packed)) NetPoint {
    uint16_t x, y;   // centered on 0x8000, like the DAC8562 code space
    uint8_t  r, g, b;
};

static WiFiServer s_tcp(PORT_DATA);
static WiFiClient s_client;
static bool       s_playing  = false;
static uint32_t   s_total_pts = 0;

static size_t s_scratchBytes = 0;
static void trackScratch(size_t bytes) {
    s_scratchBytes += bytes;
    memreg::track("Helios Net Scratch", s_scratchBytes, true);
}

static void processFramePoints(uint8_t* buf, uint16_t count) {
    // Helios frames arrive pre-rendered, so this path never calls
    // optimizer::optimize() -- only max_pts_per_frame (frame budget clamp) and
    // transform (live Z-rot/move) are read below. It still goes through the
    // shared mapping rather than setting those two fields by hand: the two are
    // read from the same place every other render path reads them, and a
    // future field this path does start consuming cannot arrive unmapped.
    const optimizer::OptimizerConfig cfg = optimizer::configFromLive(
        gOptimizerConfig, gProjection.galvo_rated_kpps, gProjection.galvo_kpps);
    const size_t MAX_PTS = 1300;   // scratch ceiling, independent of the live budget
    static LaserPoint* pts = nullptr;
    if (!pts && psScratch(pts, MAX_PTS)) trackScratch(MAX_PTS * sizeof(LaserPoint));
    if (!pts) return;

    size_t cap = cfg.max_pts_per_frame;
    if (cap > MAX_PTS) cap = MAX_PTS;
    size_t n = 0;

    for (uint16_t i = 0; i < count && n < cap; i++) {
        NetPoint* np = (NetPoint*)(buf + i * sizeof(NetPoint));
        float xf, yf;
        cfg.transform.apply((float)np->x - 32768.0f, (float)np->y - 32768.0f, xf, yf);
        if (xf < -32767.f) xf = -32767.f; else if (xf > 32767.f) xf = 32767.f;
        if (yf < -32767.f) yf = -32767.f; else if (yf > 32767.f) yf = 32767.f;
        bool blank = (np->r == 0 && np->g == 0 && np->b == 0);
        pts[n++] = { (int16_t)lroundf(xf), (int16_t)lroundf(yf),
                     np->r, np->g, np->b, blank ? (uint8_t)1 : (uint8_t)0 };
    }

    if (n > 0 && s_playing && gConfig.helios_net_enabled) {
        gState.source.store(SRC_HELIOS);
        galvo::applyCalibration(pts, n);
        galvo::pushFrame(pts, n);
        s_total_pts += n;
    }
}

static void handleClient() {
    if (!s_client.connected()) return;

    while (s_client.available() >= (int)sizeof(FrameHeader)) {
        FrameHeader hdr;
        if (s_client.readBytes((uint8_t*)&hdr, sizeof(hdr)) != sizeof(hdr)) break;

        s_playing = (hdr.flags & 0x01) && hdr.point_count > 0;

        if (gConfig.debug_log_helios_net) {
            ESP_LOGI(TAG, "Frame rate=%u pts=%u flags=0x%02X playing=%d",
                     hdr.point_rate, hdr.point_count, hdr.flags, s_playing);
            LOG_I(logbuf::CAT_WIFI, "Helios net: pts=%u flags=0x%02X playing=%d",
                  hdr.point_count, hdr.flags, s_playing);
        }

        if (hdr.point_count == 0) continue;
        if (hdr.point_count > MAX_RAW_PTS) {
            ESP_LOGW(TAG, "Frame too large: %u points, dropping client", hdr.point_count);
            LOG_W(logbuf::CAT_WIFI, "Helios net: oversized frame (%u pts), client dropped",
                  hdr.point_count);
            s_client.stop();
            s_playing = false;
            return;
        }

        size_t pt_bytes = (size_t)hdr.point_count * sizeof(NetPoint);
        static uint8_t* pt_buf = nullptr;
        if (!pt_buf && psScratch(pt_buf, MAX_RAW_PTS * sizeof(NetPoint)))
            trackScratch(MAX_RAW_PTS * sizeof(NetPoint));
        if (!pt_buf) break;

        size_t got = 0;
        uint32_t t0 = millis();
        while (got < pt_bytes && millis() - t0 < 50) {
            int n = s_client.read(pt_buf + got, pt_bytes - got);
            if (n > 0) got += n;
            else vTaskDelay(1);
        }

        if (got == pt_bytes) {
            processFramePoints(pt_buf, hdr.point_count);
            if (gConfig.debug_log_helios_net) {
                ESP_LOGI(TAG, "Frame OK pts=%u bytes=%u took=%ums",
                         hdr.point_count, (unsigned)got, (unsigned)(millis() - t0));
            }
        } else {
            ESP_LOGW(TAG, "Frame timeout: %u/%u bytes", (unsigned)got, (unsigned)pt_bytes);
        }
    }
}

void init() {
    s_tcp.begin();
    s_tcp.setNoDelay(true);
    ESP_LOGI(TAG, "Helios net emulation active | TCP:%d", PORT_DATA);
}

void task(void*) {
    for (;;) {
        if (!s_client.connected()) {
            s_client = s_tcp.available();
            if (s_client) {
                s_playing = false;
                s_total_pts = 0;
                ESP_LOGI(TAG, "Client connected: %s", s_client.remoteIP().toString().c_str());
            }
        }

        handleClient();
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

bool isConnected() { return s_client.connected(); }
bool isPlaying()   { return s_playing; }

}  // namespace helios_net
