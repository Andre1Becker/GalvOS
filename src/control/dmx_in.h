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

// Returns a single DMX channel value by absolute 1-based address (1-512),
// independent of gConfig.dmx_address / the pattern-control window above.
// Lock-free (single byte read from the last received frame -- worst case a
// one-frame-stale value, same tradeoff as galvo_out.cpp's gain/thresh
// snapshot). Returns 0 if that address was never received.
uint8_t getChannel(uint16_t address);

}  // namespace dmx_in
