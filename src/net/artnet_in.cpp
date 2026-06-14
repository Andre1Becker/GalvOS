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

extern "C" void artnetFrameCb(uint16_t universe, uint16_t length, uint8_t /*sequence*/, uint8_t* data) {
    if (universe != gConfig.artnet_universe) return;
    uint16_t addr = gConfig.dmx_address;
    if (addr == 0 || addr + DMX_CHANNELS_USED - 1 > length) return;
    for (int i = 0; i < DMX_CHANNELS_USED; i++) {
        s_artnet_data[i] = data[addr - 1 + i];
    }
    gState.last_dmx_ms = millis();
    gState.source = SRC_ARTNET;
}

void init() {
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

}  // namespace artnet_in
