#pragma once
/**
 * artnet_in.h -- Art-Net Receiver via WiFi/Ethernet
 *
 * Handles the same 16-channel layout as DMX. Runs independently of
 * dmx_in; readDmx() in pattern_engine.cpp arbitrates between sources.
 */

#include "config.h"

namespace artnet_in {

void init();
void task(void*);

// Returns the current 16 Art-Net values from the configured start address
void getChannels(uint8_t out[DMX_CHANNELS_USED]);
bool isReceiving();

}  // namespace artnet_in
