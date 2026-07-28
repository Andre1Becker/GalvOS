#include <Arduino.h>
#include "dmx_in.h"
#include "pinmap.h"
#include "config.h"
#include "util/log_buffer.h"
#include <esp_dmx.h>
#include <esp_log.h>

namespace dmx_in {

static const char* TAG = "dmx";
static const dmx_port_t DMX_PORT = DMX_NUM_1;

static uint8_t s_channels[DMX_CHANNELS_USED] = {0};
static SemaphoreHandle_t s_mux;
static volatile uint32_t s_last_packet_ms = 0;

// Full last-received frame (raw, absolute channel indices -- buf[N] is DMX
// channel N, index 0 is the start code and unused). Lets getChannel() serve
// any absolute address (e.g. a BPM channel far outside the 16/25-channel
// pattern-control window) without widening that window's semantics.
static uint8_t  s_raw[DMX_PACKET_SIZE] = {0};
static volatile size_t s_raw_size = 0;

void init() {
    s_mux = xSemaphoreCreateMutex();

    dmx_config_t cfg = DMX_CONFIG_DEFAULT;
    dmx_personality_t pers[1] = {
        {DMX_CHANNELS_USED, "Laser 16ch"}
    };

    dmx_driver_install(DMX_PORT, &cfg, pers, 1);
    // MAX485 module: receive only, DE+RE fixed to GND -- TX and DE_RE unused
    dmx_set_pin(DMX_PORT, DMX_PIN_NO_CHANGE, PIN_DMX_RX, DMX_PIN_NO_CHANGE);

    ESP_LOGI(TAG, "DMX driver installed @ port %d", DMX_PORT);
}

void task(void*) {
    uint8_t buf[DMX_PACKET_SIZE];
    dmx_packet_t pkt;

    for (;;) {
        size_t size = dmx_receive(DMX_PORT, &pkt, pdMS_TO_TICKS(50));
        if (size <= 0) continue;
        if (pkt.err != DMX_OK) {
            ESP_LOGD(TAG, "DMX rx err %d", pkt.err);
            continue;
        }

        dmx_read(DMX_PORT, buf, size);
        // Raw frame copy for getChannel() -- always stored regardless of
        // dmx_address, one memcpy, no per-channel loop.
        size_t copy_size = size > DMX_PACKET_SIZE ? DMX_PACKET_SIZE : size;
        memcpy(s_raw, buf, copy_size);
        s_raw_size = copy_size;

        uint16_t addr = gConfig.dmx_address;   // 1-basiert
        if (addr == 0 || addr + DMX_CHANNELS_USED - 1 > 512) continue;

        if (xSemaphoreTake(s_mux, pdMS_TO_TICKS(5)) == pdTRUE) {
            for (int i = 0; i < DMX_CHANNELS_USED; i++) {
                s_channels[i] = buf[addr + i];
            }
            xSemaphoreGive(s_mux);
        }
        s_last_packet_ms = millis();
        gState.last_dmx_ms = s_last_packet_ms;
        gState.dmx_frame_count++;

        if (gConfig.debug_log_dmx) {
            ESP_LOGI(TAG, "RX size=%u addr=%u ch0..3=%u,%u,%u,%u",
                     (unsigned)size, addr, buf[addr], buf[addr+1], buf[addr+2], buf[addr+3]);
            LOG_I(logbuf::CAT_DMX, "DMX RX size=%u addr=%u", (unsigned)size, addr);
        }
    }
}

void getChannels(uint8_t out[DMX_CHANNELS_USED]) {
    if (xSemaphoreTake(s_mux, pdMS_TO_TICKS(5)) == pdTRUE) {
        memcpy(out, s_channels, DMX_CHANNELS_USED);
        xSemaphoreGive(s_mux);
    } else {
        memset(out, 0, DMX_CHANNELS_USED);
    }
}

bool isReceiving() {
    return (millis() - s_last_packet_ms) < 1000;
}

uint8_t getChannel(uint16_t address) {
    if (address == 0 || address >= s_raw_size) return 0;
    return s_raw[address];
}

}  // namespace dmx_in
