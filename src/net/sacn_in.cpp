/**
 * sacn_in.cpp -- sACN / E1.31 receiver, universe 1
 *
 * Fixed byte offsets below follow ANSI E1.31-2016 (Root Layer + Framing
 * Layer + DMP Layer), assuming no Root Layer options and the standard
 * 1-property-per-slot DMP addressing every real-world sACN sender uses:
 *
 *   0- 1  Preamble Size
 *   2- 3  Post-amble Size
 *   4-15  ACN Packet Identifier ("ASC-E1.17\0\0\0")
 *  16-17  Root: Flags & Length
 *  18-21  Root: Vector (0x00000004 = VECTOR_ROOT_E131_DATA)
 *  22-37  Root: CID
 *  38-39  Framing: Flags & Length
 *  40-43  Framing: Vector (0x00000002 = VECTOR_E131_DATA_PACKET)
 *  44-107 Framing: Source Name
 * 108     Framing: Priority
 * 109-110 Framing: Synchronization Address
 * 111     Framing: Sequence Number
 * 112     Framing: Options
 * 113-114 Framing: Universe
 * 115-116 DMP: Flags & Length
 * 117     DMP: Vector (0x02)
 * 118     DMP: Address & Data Type (0xa1)
 * 119-120 DMP: First Property Address
 * 121-122 DMP: Address Increment
 * 123-124 DMP: Property value count
 * 125     DMX Start Code (0x00 = normal DMX)
 * 126-637 DMX data (512 slots)
 */
#include "sacn_in.h"
#include "config.h"
#include <Arduino.h>
#include <AsyncUDP.h>
#include <esp_log.h>
#include <string.h>

namespace sacn_in {

static const char* TAG = "sacn";
static const uint16_t SACN_PORT     = 5568;
static const uint16_t SACN_UNIVERSE = 1;
static const size_t   HEADER_BYTES  = 126;   // up to and including the start code
static const size_t   MIN_PACKET_BYTES = HEADER_BYTES + 512;

static const uint8_t ACN_PACKET_ID[12] = { 'A','S','C','-','E','1','.','1','7', 0, 0, 0 };

static AsyncUDP s_udp;
static uint8_t  s_slots[512] = {0};
static SemaphoreHandle_t s_mux;
static volatile uint32_t s_last_packet_ms = 0;

static inline uint32_t readU32BE(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static inline uint16_t readU16BE(const uint8_t* p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static void handlePacket(const uint8_t* d, size_t len) {
    if (!gConfig.sacn_enabled) return;   // WebUI Config tab toggle
    if (len < MIN_PACKET_BYTES) return;
    if (memcmp(d + 4, ACN_PACKET_ID, sizeof(ACN_PACKET_ID)) != 0) return;
    if (readU32BE(d + 18) != 0x00000004) return;   // VECTOR_ROOT_E131_DATA
    if (readU32BE(d + 40) != 0x00000002) return;   // VECTOR_E131_DATA_PACKET
    if (readU16BE(d + 113) != SACN_UNIVERSE) return;
    if (d[117] != 0x02) return;                    // DMP vector
    if (d[125] != 0x00) return;                    // only normal DMX start code

    if (xSemaphoreTake(s_mux, pdMS_TO_TICKS(5)) == pdTRUE) {
        memcpy(s_slots, d + HEADER_BYTES, sizeof(s_slots));
        xSemaphoreGive(s_mux);
    }
    s_last_packet_ms = millis();
}

void init() {
    s_mux = xSemaphoreCreateMutex();
    IPAddress group(239, 255, (uint8_t)(SACN_UNIVERSE >> 8), (uint8_t)(SACN_UNIVERSE & 0xFF));
    if (s_udp.listenMulticast(group, SACN_PORT)) {
        s_udp.onPacket([](AsyncUDPPacket packet) {
            handlePacket(packet.data(), packet.length());
        });
        ESP_LOGI(TAG, "sACN listening on %s:%u (universe %u)",
                 group.toString().c_str(), SACN_PORT, SACN_UNIVERSE);
    } else {
        ESP_LOGW(TAG, "sACN multicast join failed");
    }
}

void getChannels(uint8_t out[DMX_CHANNELS_USED]) {
    uint16_t addr = gConfig.dmx_address;   // 1-based, same convention as dmx_in/artnet_in
    if (xSemaphoreTake(s_mux, pdMS_TO_TICKS(5)) == pdTRUE) {
        if (addr == 0 || addr + DMX_CHANNELS_USED - 1 > 512) {
            memset(out, 0, DMX_CHANNELS_USED);
        } else {
            memcpy(out, s_slots + (addr - 1), DMX_CHANNELS_USED);
        }
        xSemaphoreGive(s_mux);
    } else {
        memset(out, 0, DMX_CHANNELS_USED);
    }
}

bool isReceiving() {
    return (millis() - s_last_packet_ms) < 1000;
}

}  // namespace sacn_in
