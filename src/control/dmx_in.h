#pragma once
/**
 * dmx_in.h -- DMX512-Empfang via UART1 + MAX485
 */

#include "config.h"

namespace dmx_in {

void init();
void task(void*);

// Returns the current 16 DMX values from the configured start address
void getChannels(uint8_t out[DMX_CHANNELS_USED]);
bool isReceiving();

}  // namespace dmx_in
