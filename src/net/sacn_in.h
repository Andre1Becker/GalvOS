#pragma once
/**
 * sacn_in.h -- sACN / E1.31 receiver (ANSI E1.31-2016), universe 1 only
 *
 * Joins the E1.31 multicast group 239.255.0.1:5568 (239.255.<uni_hi>.<uni_lo>
 * for universe 1) and parses Root/Framing/DMP layers down to the 512 DMX
 * slots, using the same start-address slot mapping as DMX512/Art-Net
 * (gConfig.dmx_address). Lowest priority of the three DMX-shaped sources --
 * see pattern_engine.cpp readDmx(), where Art-Net and DMX512 are checked
 * first.
 */

#include "config.h"

namespace sacn_in {

void init();

// Returns the current 16(+) DMX values from the configured start address
void getChannels(uint8_t out[DMX_CHANNELS_USED]);
bool isReceiving();

}  // namespace sacn_in
