#include <Arduino.h>
#include "artnet_in.h"
#include "config.h"
#include "util/log_buffer.h"
#include <ArtnetWifi.h>
#include <WiFi.h>
#include <esp_log.h>

namespace artnet_in {

static const char* TAG = "artnet";
static ArtnetWifi s_artnet;
static uint8_t    s_artnet_data[DMX_CHANNELS_USED] = {0};
static SemaphoreHandle_t s_mux;
static volatile uint32_t s_last_packet_ms = 0;

extern "C" void artnetFrameCb(uint16_t universe, uint16_t length, uint8_t sequence, uint8_t* data) {
    if (!gConfig.artnet_enabled) return;   // WebUI Config tab toggle
    if (universe != gConfig.artnet_universe) {
        if (gConfig.debug_log_artnet) {
            ESP_LOGI(TAG, "RX universe=%u (expected %u) len=%u -- ignored", universe, gConfig.artnet_universe, length);
        }
        return;
    }
    uint16_t addr = gConfig.dmx_address;
    if (addr == 0 || addr + DMX_CHANNELS_USED - 1 > length) {
        if (gConfig.debug_log_artnet) {
            ESP_LOGW(TAG, "RX universe=%u len=%u addr=%u -- out of range", universe, length, addr);
            LOG_W(logbuf::CAT_WIFI, "Art-Net: addr %u out of range (len=%u)", addr, length);
        }
        return;
    }
    if (xSemaphoreTake(s_mux, pdMS_TO_TICKS(5)) == pdTRUE) {
        for (int i = 0; i < DMX_CHANNELS_USED; i++) {
            s_artnet_data[i] = data[addr - 1 + i];
        }
        xSemaphoreGive(s_mux);
    }
    s_last_packet_ms = millis();
    gState.last_dmx_ms = s_last_packet_ms;

    if (gConfig.debug_log_artnet) {
        ESP_LOGI(TAG, "RX universe=%u seq=%u len=%u accepted", universe, sequence, length);
        LOG_I(logbuf::CAT_WIFI, "Art-Net RX universe=%u len=%u", universe, length);
    }
}

void init() {
    s_mux = xSemaphoreCreateMutex();
    s_artnet.setArtDmxCallback(artnetFrameCb);
    s_artnet.begin();
    ESP_LOGI(TAG, "Art-Net started, listening universe %u", gConfig.artnet_universe);
}

void task(void*) {
    for (;;) {
        s_artnet.read();
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

void getChannels(uint8_t out[DMX_CHANNELS_USED]) {
    if (xSemaphoreTake(s_mux, pdMS_TO_TICKS(5)) == pdTRUE) {
        memcpy(out, s_artnet_data, DMX_CHANNELS_USED);
        xSemaphoreGive(s_mux);
    } else {
        memset(out, 0, DMX_CHANNELS_USED);
    }
}

bool isReceiving() {
    return (millis() - s_last_packet_ms) < 1000;
}

}  // namespace artnet_in
