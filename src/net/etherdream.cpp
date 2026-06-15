/**
 * etherdream.cpp — EtherDream DAC network-Protokoll Emulation
 *
 * Protokoll-reference: https://ether-dream.com/protocol.html
 * Compatible with: QLC+, Pangolin BEYOND, Mamba Black, LaserBoy, Shownet
 *
 * Ablauf:
 *   1. UDP broadcast on port 7654 (beacon every 1s)
 *      → software discovers device automatically
 *   2. TCP port 7765 (control connection)
 *      → PREPARE / BEGIN_PLAYBACK / DATA / STOP Commands
 *   3. ESP32 → applyCalibration() → galvo::pushFrame()
 */
#include "etherdream.h"
#include "config.h"
#include "output/galvo_out.h"
#include "safety/safety.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <esp_log.h>
#include <string.h>

namespace etherdream {

static const char* TAG = "edream";
static const uint16_t PORT_DISC = 7654;   // UDP Broadcast Discovery
static const uint16_t PORT_DATA = 7765;   // TCP Data/Control

// ── EtherDream protocol structures ──────────────────────────────
struct __attribute__((packed)) DACStatus {
    uint8_t  protocol          = 0;
    uint8_t  light_engine_state= 1;  // ready
    uint8_t  playback_state    = 0;  // idle
    uint8_t  source            = 0;  // network
    uint16_t light_engine_flags= 0;
    uint16_t playback_flags    = 0;
    uint16_t source_flags      = 0;
    uint16_t fullness          = 0;
    uint32_t point_rate        = 0;
    uint32_t point_count       = 0;
};

struct __attribute__((packed)) DACBroadcast {
    uint8_t  mac[6];
    uint8_t  hw_rev = 0x12;
    uint8_t  sw_rev = 0x10;
    uint32_t buffer_capacity = 1800;
    uint32_t max_point_rate  = 100000;
    DACStatus status;
};

struct __attribute__((packed)) CommandHeader {
    uint8_t command;
};

// EtherDream commands
enum ECmd : uint8_t {
    CMD_PREPARE     = 0x70,  // 'p'
    CMD_BEGIN       = 0x62,  // 'b'
    CMD_DATA        = 0x64,  // 'd'
    CMD_STOP        = 0x73,  // 's'
    CMD_ESTOP       = 0x00,
    CMD_ESTOP2      = 0xFF,
    CMD_CLEAR_ESTOP = 0x63,  // 'c'
    CMD_PING        = 0x3F,  // '?'
    CMD_VERSION     = 0x56,  // 'V'
};

struct __attribute__((packed)) DataPoint {
    uint16_t control;         // Bit 15 = shutter
    int16_t  x;
    int16_t  y;
    uint16_t r, g, b;         // 16-bit (only the high byte is used)
    uint16_t i, u1, u2;
};

struct __attribute__((packed)) DataHeader {
    uint32_t flags;
    uint16_t point_count;
};

// ── internal state ────────────────────────────────────────────
static WiFiUDP    s_udp;
static WiFiServer s_tcp(PORT_DATA);
static WiFiClient s_client;

static bool     s_running     = false;
static bool     s_prepared    = false;
static uint32_t s_total_pts   = 0;
static uint32_t s_last_beacon = 0;

// response buffer
static uint8_t  s_resp[64];

// ── send response ──────────────────────────────────────────────
static void sendResponse(uint8_t cmd, uint8_t response, const DACStatus* st) {
    DACStatus cur_st{};
    cur_st.playback_state = s_running ? 1 : 0;
    cur_st.fullness = (uint16_t)(s_total_pts % 1800);
    cur_st.point_count = s_total_pts;

    const DACStatus& use_st = st ? *st : cur_st;

    s_resp[0] = 0x21;  // '!'  response marker
    s_resp[1] = cmd;
    s_resp[2] = response;  // 0x00 = ACK
    memcpy(&s_resp[3], &use_st, sizeof(DACStatus));
    s_client.write(s_resp, 3 + sizeof(DACStatus));
}

// ── send beacon ───────────────────────────────────────────────
static void sendBeacon() {
    // Only send when WiFi is actually up — avoids filling lwIP socket
    // buffers with failed broadcasts (ENOMEM / error 118), which would
    // exhaust the shared lwIP pool and starve AsyncTCP / WebSocket.
    if (WiFi.status() != WL_CONNECTED) return;

    DACBroadcast bc{};
    uint64_t mac = ESP.getEfuseMac();
    memcpy(bc.mac, &mac, 6);
    bc.status.playback_state = s_running ? 1 : 0;
    bc.status.fullness = (uint16_t)(s_total_pts % 1800);

    if (!s_udp.beginPacket("255.255.255.255", PORT_DISC)) {
        ESP_LOGD(TAG, "Beacon: beginPacket failed (no route), skip");
        return;
    }
    s_udp.write((uint8_t*)&bc, sizeof(bc));
    if (s_udp.endPacket() == 0) {
        // Send failed: close and reopen socket to flush any stuck state
        ESP_LOGD(TAG, "Beacon: endPacket failed, resetting UDP socket");
        s_udp.stop();
        vTaskDelay(pdMS_TO_TICKS(10));
        s_udp.begin(PORT_DISC);
    }
}

// ── process point data ───────────────────────────────────────
static void processDataPoints(uint8_t* buf, uint16_t count) {
    // Temporary frame buffer (stack -- count limited by TCP-MTU)
    const size_t MAX_PTS = 256;
    static LaserPoint pts[MAX_PTS];  // static: not on stack
    size_t n = 0;

    for (uint16_t i = 0; i < count && n < MAX_PTS; i++) {
        DataPoint* dp = (DataPoint*)(buf + i * sizeof(DataPoint));
        bool blank = (dp->control & 0x8000) || (dp->r == 0 && dp->g == 0 && dp->b == 0);
        pts[n++] = {
            dp->x, dp->y,
            (uint8_t)(dp->r >> 8),
            (uint8_t)(dp->g >> 8),
            (uint8_t)(dp->b >> 8),
            blank ? (uint8_t)1 : (uint8_t)0
        };
    }

    if (n > 0 && s_running) {
        galvo::applyCalibration(pts, n);
        galvo::pushFrame(pts, n);
        s_total_pts += n;
    }
}

// ── handle TCP client ─────────────────────────────────────────
static void handleClient() {
    if (!s_client.connected()) return;

    while (s_client.available() >= 1) {
        uint8_t cmd = s_client.read();
        switch (cmd) {

            case CMD_PING:
                sendResponse(CMD_PING, 0x00, nullptr);
                break;

            case CMD_PREPARE:
                s_prepared = true;
                s_running  = false;
                s_total_pts = 0;
                sendResponse(CMD_PREPARE, 0x00, nullptr);
                ESP_LOGI(TAG, "PREPARE");
                break;

            case CMD_BEGIN: {
                // BEGIN payload: low_water_mark(u32) + point_rate(u32)
                uint8_t args[8];
                if (s_client.readBytes(args, 8) == 8) {
                    uint32_t rate;
                    memcpy(&rate, &args[4], 4);
                    ESP_LOGI(TAG, "BEGIN @ %u pps", rate);
                }
                s_running = true;
                sendResponse(CMD_BEGIN, 0x00, nullptr);
                break;
            }

            case CMD_DATA: {
                // read DATA header
                DataHeader hdr;
                if (s_client.readBytes((uint8_t*)&hdr, sizeof(hdr)) != sizeof(hdr))
                    break;

                size_t pt_bytes = hdr.point_count * sizeof(DataPoint);
                if (pt_bytes > 8192) {
                    ESP_LOGW(TAG, "DATA too large: %u points", hdr.point_count);
                    break;
                }

                // Read point data (with timeout)
                static uint8_t pt_buf[8192];  // static: not on task stack
                size_t got = 0;
                uint32_t t0 = millis();
                while (got < pt_bytes && millis()-t0 < 50) {
                    int n = s_client.read(pt_buf + got, pt_bytes - got);
                    if (n > 0) got += n;
                    else vTaskDelay(1);
                }

                if (got == pt_bytes) {
                    processDataPoints(pt_buf, hdr.point_count);
                    sendResponse(CMD_DATA, 0x00, nullptr);
                } else {
                    ESP_LOGW(TAG, "DATA Timeout: %u/%u Bytes", got, pt_bytes);
                    sendResponse(CMD_DATA, 0x01, nullptr);
                }
                break;
            }

            case CMD_STOP:
                s_running = false;
                sendResponse(CMD_STOP, 0x00, nullptr);
                ESP_LOGI(TAG, "STOP");
                break;

            case CMD_ESTOP:
            case CMD_ESTOP2:
                s_running = false;
                safety::emergencyStop();
                sendResponse(cmd, 0x00, nullptr);
                ESP_LOGW(TAG, "E-STOP from network");
                break;

            case CMD_CLEAR_ESTOP:
                sendResponse(CMD_CLEAR_ESTOP, 0x00, nullptr);
                break;

            case CMD_VERSION:
                s_client.print("ESP32-Laser-v1.4\n");
                break;

            default:
                ESP_LOGW(TAG, "Unknown command: 0x%02X", cmd);
                break;
        }
    }
}

// ── public API ────────────────────────────────────────────────
void init() {
    s_udp.begin(PORT_DISC);
    s_tcp.begin();
    s_tcp.setNoDelay(true);
    ESP_LOGI(TAG, "EtherDream Emulation active | UDP:%d TCP:%d",
             PORT_DISC, PORT_DATA);
}

void task(void*) {
    for (;;) {
        // Beacon every 1000ms (back off to 5s if WiFi is not connected)
        uint32_t beacon_interval = (WiFi.status() == WL_CONNECTED) ? 1000 : 5000;
        if (millis() - s_last_beacon > beacon_interval) {
            sendBeacon();
            s_last_beacon = millis();
        }

        // accept new client
        if (!s_client.connected()) {
            s_client = s_tcp.available();
            if (s_client) {
                s_running = false;
                ESP_LOGI(TAG, "Client connected: %s",
                         s_client.remoteIP().toString().c_str());
                // Send initial status response (protocol requires it)
                sendResponse(0x00, 0x00, nullptr);
            }
        }

        handleClient();
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

bool isConnected() { return s_client.connected(); }
bool isPlaying()   { return s_running; }

} // namespace etherdream
