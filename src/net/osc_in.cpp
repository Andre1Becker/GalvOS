/**
 * osc_in.cpp -- OSC 1.0 receiver (no bundle support)
 *
 * OSC message wire format:
 *   address pattern  : null-terminated ASCII starting with '/', padded to a
 *                      4-byte boundary with additional NULs
 *   type tag string  : null-terminated, starts with ',', one char per arg
 *                      ('i'=int32, 'f'=float32), padded to a 4-byte boundary
 *   arguments        : big-endian, 4 bytes each for i/f
 */
#include "osc_in.h"
#include "config.h"
#include "patterns/pattern_engine.h"
#include "patterns/preset_patterns.h"
#include "util/log_buffer.h"

#include <Arduino.h>
#include <AsyncUDP.h>
#include <esp_log.h>
#include <math.h>
#include <string.h>

namespace osc_in {

static const char* TAG = "osc";
static const uint16_t OSC_PORT = 9000;

static AsyncUDP  s_udp;
static volatile uint32_t s_last_packet_ms = 0;

static bool oscReadString(const uint8_t* buf, size_t len, size_t& off, String& out) {
    size_t start = off;
    while (off < len && buf[off] != 0) off++;
    if (off >= len) return false;
    out = String((const char*)(buf + start), off - start);
    off++;                        // skip terminating NUL
    off = (off + 3) & ~size_t(3); // pad to 4-byte boundary
    return off <= len;
}

static inline int32_t readInt32BE(const uint8_t* p) {
    return (int32_t)(((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                      ((uint32_t)p[2] << 8)  |  (uint32_t)p[3]);
}
static inline float readFloat32BE(const uint8_t* p) {
    uint32_t u = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                 ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
    float f;
    memcpy(&f, &u, sizeof(f));
    return f;
}

static inline uint8_t to255(float v) {
    return (uint8_t)constrain((int)lroundf(v * 255.0f), 0, 255);
}

static void handlePacket(const uint8_t* data, size_t len) {
    if (!gConfig.osc_enabled) return;   // WebUI Config tab toggle
    size_t off = 0;
    String addr, types;
    if (!oscReadString(data, len, off, addr)) return;
    if (addr.length() == 0 || addr[0] != '/') return;
    if (off >= len || data[off] != ',') return;   // bundles (#bundle) and no-arg messages both skipped
    if (!oscReadString(data, len, off, types)) return;

    s_last_packet_ms = millis();

    if (addr == "/galvos/preset") {
        if (types.length() < 2 || types[1] != 'i' || off + 4 > len) return;
        int idx = (int)readInt32BE(data + off);
        patterns::setPreset(presets::presetFromIndex(idx));

    } else if (addr == "/galvos/color") {
        if (types.length() < 4 || types[1] != 'f' || types[2] != 'f' || types[3] != 'f' || off + 12 > len) return;
        float r = readFloat32BE(data + off);
        float g = readFloat32BE(data + off + 4);
        float b = readFloat32BE(data + off + 8);
        gLivePreset.col_r        = to255(r);
        gLivePreset.col_g        = to255(g);
        gLivePreset.col_b        = to255(b);
        gLivePreset.col_override = true;

    } else if (addr == "/galvos/speed") {
        if (types.length() < 2 || types[1] != 'f' || off + 4 > len) return;
        gLivePreset.speed = to255(readFloat32BE(data + off));

    } else if (addr == "/galvos/brightness") {
        if (types.length() < 2 || types[1] != 'f' || off + 4 > len) return;
        gState.ui_master_dimmer.store(to255(readFloat32BE(data + off)));

    } else if (addr == "/galvos/enable") {
        if (types.length() < 2 || types[1] != 'i' || off + 4 > len) return;
        // Reuses the same arbitration WebUI uses (see pattern_engine.cpp
        // readDmx()): ui_override=true makes gState.source report SRC_WEBUI
        // and takes priority over DMX/Art-Net. Does NOT arm the laser --
        // arming stays a deliberate, separate safety action.
        gState.ui_override.store(readInt32BE(data + off) != 0);
    }
}

void init() {
    if (s_udp.listen(OSC_PORT)) {
        s_udp.onPacket([](AsyncUDPPacket packet) {
            handlePacket(packet.data(), packet.length());
        });
        ESP_LOGI(TAG, "OSC listener active | UDP:%d", OSC_PORT);
    } else {
        ESP_LOGW(TAG, "OSC listen failed on UDP:%d", OSC_PORT);
        LOG_W(logbuf::CAT_WIFI, "OSC: listen failed on UDP:%d", OSC_PORT);
    }
}

bool isActive() { return (millis() - s_last_packet_ms) < 1000; }

}  // namespace osc_in
