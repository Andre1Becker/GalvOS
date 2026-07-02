#include <Arduino.h>
#include "artnet_in.h"
#include "config.h"
#include <ArtnetWifi.h>
#include <WiFi.h>
#include <esp_log.h>

namespace artnet_in {

static const char* TAG = "artnet";
static ArtnetWifi s_artnet;
static uint8_t    s_artnet_data[DMX_CHANNELS_USED] = {0};
static SemaphoreHandle_t s_mux;
static volatile uint32_t s_last_packet_ms = 0;

extern "C" void artnetFrameCb(uint16_t universe, uint16_t length, uint8_t /*sequence*/, uint8_t* data) {
    if (universe != gConfig.artnet_universe) return;
    uint16_t addr = gConfig.dmx_address;
    if (addr == 0 || addr + DMX_CHANNELS_USED - 1 > length) return;
    if (xSemaphoreTake(s_mux, pdMS_TO_TICKS(5)) == pdTRUE) {
        for (int i = 0; i < DMX_CHANNELS_USED; i++) {
            s_artnet_data[i] = data[addr - 1 + i];
        }
        xSemaphoreGive(s_mux);
    }
    s_last_packet_ms = millis();
    gState.last_dmx_ms = s_last_packet_ms;
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
