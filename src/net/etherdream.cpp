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
#include "util/log_buffer.h"
#include "util/mem_registry.h"
#include "util/ps_scratch.h"
#include "net/web_ui.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <string.h>
#include <errno.h>

namespace etherdream {

static const char* TAG = "edream";
static const uint16_t PORT_DISC = 7654;   // UDP Broadcast Discovery
static const uint16_t PORT_DATA = 7765;   // TCP Data/Control

// ── EtherDream protocol structures ──────────────────────────────
// light_engine_state: 0=ready, 1=warmup, 2=cooldown, 3=e-stop
// playback_state:     0=idle,  1=prepared, 2=playing
struct __attribute__((packed)) DACStatus {
    uint8_t  protocol          = 0;
    uint8_t  light_engine_state= 0;  // ready
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
    uint16_t hw_rev = 0x12;
    uint16_t sw_rev = 0x10;
    uint16_t buffer_capacity = 1800;
    uint32_t max_point_rate  = 100000;
    DACStatus status;
};

// Must match DACBroadcast::buffer_capacity above -- clients pace DATA writes
// off the advertised buffer capacity, so accepting fewer points per DATA
// command than we advertise causes every oversized frame to hit the "too
// large" guard in the CMD_DATA handler below.
static const uint16_t MAX_DATA_PTS = 1800;

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

// EtherDream response codes (byte 0 of the response packet)
enum EResp : uint8_t {
    RESP_ACK     = 0x61,  // 'a' - command executed
    RESP_FULL    = 0x46,  // 'F' - NAK, buffer full
    RESP_INVALID = 0x49,  // 'I' - NAK, invalid command
    RESP_ESTOP   = 0x21,  // '!' - NAK, DAC in emergency-stop condition
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
static uint32_t s_beacon_fail_count = 0;
static uint32_t s_beacon_fail_since_ms = 0;  // 0 = currently healthy

// If the beacon UDP send keeps failing (lwIP ENOMEM etc.) for this long,
// the socket-rebuild-after-3 below isn't recovering it -- the shared lwIP
// pool is starved and the WebUI/WS goes unreachable with it. Reboot rather
// than let it dangle indefinitely.
static const uint32_t BEACON_FAIL_REBOOT_MS = 30000;

// response buffer
static uint8_t  s_resp[64];

// Running total of PSRAM RX/frame scratch (was DRAM .bss before v6.04.0)
static size_t s_scratchBytes = 0;
static void trackScratch(size_t bytes) {
    s_scratchBytes += bytes;
    memreg::track("EtherDream Scratch", s_scratchBytes, true);
}

// light_engine_state was previously defaulting to 1 ("warmup") forever --
// a real client (e.g. laser show software) waits for it to report 0
// ("ready") before ever sending PREPARE/BEGIN, and gives up with a read
// timeout when it never does. There is no warmup/cooldown delay on this
// hardware, so the only non-ready state that actually applies is e-stop.
static inline uint8_t currentLightEngineState() {
    return gState.estop_ok.load() ? 0 : 3;
}

// ── send response ──────────────────────────────────────────────
// Wire format is exactly: response(1) + command(1) + dac_status(20) = 22
// bytes. There is no separate framing/marker byte -- a stray extra byte
// here desyncs every subsequent response on the stream by one byte, which
// is what a real Ether Dream client (e.g. laser show software) reports as
// "received response to unexpected command".
static void sendResponse(uint8_t cmd, uint8_t response, const DACStatus* st) {
    DACStatus cur_st{};
    cur_st.light_engine_state = currentLightEngineState();
    cur_st.playback_state = s_running ? 2 : (s_prepared ? 1 : 0);
    cur_st.fullness = (uint16_t)(s_total_pts % 1800);
    cur_st.point_count = s_total_pts;

    const DACStatus& use_st = st ? *st : cur_st;

    s_resp[0] = response;  // RESP_ACK / RESP_FULL / RESP_INVALID / RESP_ESTOP
    s_resp[1] = cmd;
    memcpy(&s_resp[2], &use_st, sizeof(DACStatus));
    size_t written = s_client.write(s_resp, 2 + sizeof(DACStatus));

    if (gConfig.debug_log_etherdream) {
        ESP_LOGI(TAG, "TX resp=0x%02X cmd=0x%02X wrote=%u/%u state=%u playback=%u",
                 response, cmd, (unsigned)written, (unsigned)(2 + sizeof(DACStatus)),
                 use_st.light_engine_state, use_st.playback_state);
        LOG_I(logbuf::CAT_WIFI, "EtherDream TX resp=0x%02X cmd=0x%02X wrote=%u/%u",
              response, cmd, (unsigned)written, (unsigned)(2 + sizeof(DACStatus)));
    }
}

// ── send beacon ───────────────────────────────────────────────
static void sendBeacon() {
    // Only send when WiFi is actually up — avoids filling lwIP socket
    // buffers with failed broadcasts (errno ENOMEM/ENOBUFS, or a wedged
    // stack reporting EHOSTUNREACH/ENETUNREACH), which would starve
    // AsyncTCP / WebSocket the same way regardless of which errno it is.
    if (WiFi.status() != WL_CONNECTED) {
        s_beacon_fail_count = 0;
        s_beacon_fail_since_ms = 0;
        return;
    }

    // A large static HTTP response (e.g. index.html.gz on a browser hard
    // reload) is actively streaming and pressuring the shared internal
    // heap -- skip this cycle rather than add another allocation to it.
    // Fail-streak state is left untouched: this only pauses attempts, it
    // doesn't mask a starvation that was already in progress beforehand.
    if (millis() < gState.heavy_io_until_ms.load()) {
        return;
    }

    DACBroadcast bc{};
    uint64_t mac = ESP.getEfuseMac();
    memcpy(bc.mac, &mac, 6);
    bc.status.light_engine_state = currentLightEngineState();
    bc.status.playback_state = s_running ? 2 : (s_prepared ? 1 : 0);
    bc.status.fullness = (uint16_t)(s_total_pts % 1800);

    if (!s_udp.beginPacket("255.255.255.255", PORT_DISC)) {
        ESP_LOGD(TAG, "Beacon: beginPacket failed (no route), skip");
        return;
    }
    s_udp.write((uint8_t*)&bc, sizeof(bc));
    if (s_udp.endPacket() == 0) {
        // WiFiUDP::endPacket() already logged errno via log_e() -- sendto()
        // hasn't been called again since, so errno still reflects its result.
        int send_errno = errno;

        // Single miss is usually transient lwIP pressure (e.g. a WS client
        // mid-handshake) that clears on its own. Resetting the socket on
        // every miss is itself an lwIP allocation -- exactly the wrong
        // move while the pool is tight. Only rebuild after repeated misses.
        s_beacon_fail_count++;
        if (s_beacon_fail_since_ms == 0) s_beacon_fail_since_ms = millis();

        // Ground truth at the exact moment of failure -- the safety task's
        // periodic heap sample (every ~2.5s) can miss a dip that already
        // recovered by the time it samples. Also log RSSI: if these
        // failures line up with a weak/dropping signal, that confirms a
        // WiFi-layer wedge (matching EHOSTUNREACH) rather than a heap issue.
        size_t free_int    = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        size_t largest_int = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
        int8_t rssi = WiFi.RSSI();
        // Requests currently in flight through web_ui's AsyncWebServer --
        // shares the system-wide CONFIG_LWIP_MAX_SOCKETS=16 netconn pool with
        // this very socket. A spike here at fail time (e.g. during a browser
        // hard-reload's burst of parallel asset + /api/* requests) would
        // explain ENOMEM on a perfectly healthy general heap.
        int http_inflight = web_ui::activeRequests();
        ESP_LOGW(TAG, "Beacon: endPacket failed (%u consecutive, errno=%d) heap=%u/%u rssi=%d http_inflight=%d",
                 s_beacon_fail_count, send_errno,
                 (unsigned)free_int, (unsigned)largest_int, (int)rssi, http_inflight);
        LOG_W(logbuf::CAT_WIFI, "Beacon fail #%u errno=%d heap=%u/%u rssi=%d http=%d",
              s_beacon_fail_count, send_errno,
              (unsigned)free_int, (unsigned)largest_int, (int)rssi, http_inflight);
        bool mem_pressure = (send_errno == ENOMEM || send_errno == ENOBUFS);

        // send_errno is often EHOSTUNREACH/ENETUNREACH (118/114), not
        // ENOMEM/ENOBUFS -- the lwIP/WiFi stack is wedged, not the heap.
        // Empirically this state does NOT self-recover and leaves the
        // WebUI permanently unreachable, so the reboot stays unconditional
        // regardless of which errno caused it. Only the failsafe reason
        // label is corrected to not falsely claim OOM.
        if (millis() - s_beacon_fail_since_ms >= BEACON_FAIL_REBOOT_MS) {
            // Rebuild attempts below haven't helped for 30s straight --
            // this is the WiFiUdp.cpp "could not send data" spiral that
            // leaves the WebUI unreachable. Reboot regardless of errno.
            safety::failsafeReboot(mem_pressure ? "UDP_ENOMEM" : "UDP_SEND_FAIL");
        }

        if (s_beacon_fail_count >= 3) {
            s_udp.stop();
            vTaskDelay(pdMS_TO_TICKS(10));
            s_udp.begin(PORT_DISC);
            s_beacon_fail_count = 0;
        }
    } else {
        s_beacon_fail_count = 0;
        s_beacon_fail_since_ms = 0;
    }
}

// ── process point data ───────────────────────────────────────
static void processDataPoints(uint8_t* buf, uint16_t count) {
    // Frame scratch in PSRAM -- was DRAM .bss ("static: not on stack"). Sized
    // to MAX_DATA_PTS (matches the read buffer above) so a full-size DATA
    // frame doesn't get silently truncated on its way to galvo::pushFrame().
    const size_t MAX_PTS = MAX_DATA_PTS;
    static LaserPoint* pts = nullptr;
    if (!pts && psScratch(pts, MAX_PTS)) trackScratch(MAX_PTS * sizeof(LaserPoint));
    if (!pts) return;
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

    if (n > 0 && s_running && gConfig.etherdream_enabled) {
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

        if (gConfig.debug_log_etherdream) {
            ESP_LOGI(TAG, "RX cmd=0x%02X ('%c') avail=%d", cmd,
                     (cmd >= 0x20 && cmd < 0x7F) ? (char)cmd : '.', s_client.available());
            LOG_I(logbuf::CAT_WIFI, "EtherDream RX cmd=0x%02X avail=%d", cmd, s_client.available());
        }

        switch (cmd) {

            case CMD_PING:
                sendResponse(CMD_PING, RESP_ACK, nullptr);
                break;

            case CMD_PREPARE:
                s_prepared = true;
                s_running  = false;
                s_total_pts = 0;
                sendResponse(CMD_PREPARE, RESP_ACK, nullptr);
                ESP_LOGI(TAG, "PREPARE");
                break;

            case CMD_BEGIN: {
                // BEGIN payload: low_water_mark(u32) + point_rate(u32)
                uint8_t args[8];
                uint32_t bt0 = millis();
                size_t got_args = s_client.readBytes(args, 8);
                if (gConfig.debug_log_etherdream) {
                    ESP_LOGI(TAG, "BEGIN args read=%u/8 took=%ums", (unsigned)got_args, (unsigned)(millis() - bt0));
                    LOG_I(logbuf::CAT_WIFI, "EtherDream BEGIN args=%u/8 took=%ums", (unsigned)got_args, (unsigned)(millis() - bt0));
                }
                if (got_args == 8) {
                    uint32_t rate;
                    memcpy(&rate, &args[4], 4);
                    ESP_LOGI(TAG, "BEGIN @ %u pps", rate);
                }
                s_running = true;
                sendResponse(CMD_BEGIN, RESP_ACK, nullptr);
                break;
            }

            case CMD_DATA: {
                uint32_t dt0 = millis();
                // read DATA header
                DataHeader hdr;
                size_t got_hdr = s_client.readBytes((uint8_t*)&hdr, sizeof(hdr));
                if (got_hdr != sizeof(hdr)) {
                    if (gConfig.debug_log_etherdream) {
                        ESP_LOGW(TAG, "DATA header read=%u/%u took=%ums",
                                 (unsigned)got_hdr, (unsigned)sizeof(hdr), (unsigned)(millis() - dt0));
                    }
                    break;
                }

                size_t pt_bytes = hdr.point_count * sizeof(DataPoint);
                if (hdr.point_count > MAX_DATA_PTS) {
                    // Client claims more points than our advertised buffer_capacity
                    // allows -- either a bug or a non-conforming implementation.
                    // Must still send a response (client is blocked waiting for
                    // one) and must not try to resync by draining an unbounded
                    // byte count -- dropping the connection is the only way to
                    // guarantee the stream doesn't desync for every command after.
                    ESP_LOGW(TAG, "DATA too large: %u points, dropping client", hdr.point_count);
                    sendResponse(CMD_DATA, RESP_INVALID, nullptr);
                    s_client.stop();
                    break;
                }

                // Read point data (with timeout). RX buffer in PSRAM sized to
                // MAX_DATA_PTS (matches the advertised buffer_capacity) -- was
                // DRAM .bss ("static: not on task stack").
                static uint8_t* pt_buf = nullptr;
                if (!pt_buf && psScratch(pt_buf, MAX_DATA_PTS * sizeof(DataPoint)))
                    trackScratch(MAX_DATA_PTS * sizeof(DataPoint));
                if (!pt_buf) break;
                size_t got = 0;
                uint32_t t0 = millis();
                while (got < pt_bytes && millis()-t0 < 50) {
                    int n = s_client.read(pt_buf + got, pt_bytes - got);
                    if (n > 0) got += n;
                    else vTaskDelay(1);
                }

                if (gConfig.debug_log_etherdream) {
                    ESP_LOGI(TAG, "DATA pts=%u bytes=%u/%u total_took=%ums",
                             hdr.point_count, (unsigned)got, (unsigned)pt_bytes, (unsigned)(millis() - dt0));
                    LOG_I(logbuf::CAT_WIFI, "EtherDream DATA pts=%u bytes=%u/%u took=%ums",
                          hdr.point_count, (unsigned)got, (unsigned)pt_bytes, (unsigned)(millis() - dt0));
                }

                if (got == pt_bytes) {
                    processDataPoints(pt_buf, hdr.point_count);
                    sendResponse(CMD_DATA, RESP_ACK, nullptr);
                } else {
                    ESP_LOGW(TAG, "DATA Timeout: %u/%u Bytes", got, pt_bytes);
                    sendResponse(CMD_DATA, RESP_INVALID, nullptr);
                }
                break;
            }

            case CMD_STOP:
                s_running = false;
                sendResponse(CMD_STOP, RESP_ACK, nullptr);
                ESP_LOGI(TAG, "STOP");
                break;

            case CMD_ESTOP:
            case CMD_ESTOP2:
                s_running = false;
                safety::emergencyStop();
                sendResponse(cmd, RESP_ACK, nullptr);
                ESP_LOGW(TAG, "E-STOP from network");
                break;

            case CMD_CLEAR_ESTOP:
                sendResponse(CMD_CLEAR_ESTOP, RESP_ACK, nullptr);
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
        uint32_t beacon_interval = (WiFi.status() != WL_CONNECTED) ? 5000
                                  : (s_beacon_fail_count > 0)       ? 3000 : 1000;
        if (millis() - s_last_beacon > beacon_interval) {
            sendBeacon();
            s_last_beacon = millis();
        }

        // accept new client
        if (!s_client.connected()) {
            s_client = s_tcp.available();
            if (s_client) {
                s_running  = false;
                s_prepared = false;
                ESP_LOGI(TAG, "Client connected: %s",
                         s_client.remoteIP().toString().c_str());
                // Send initial status response (protocol requires it)
                sendResponse(0x00, RESP_ACK, nullptr);
            }
        }

        handleClient();
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

bool isConnected() { return s_client.connected(); }
bool isPlaying()   { return s_running; }

} // namespace etherdream
