#pragma once
/**
 * helios_net.h -- Helios DAC network emulation (custom TCP framing)
 *
 * Helios (https://github.com/Grix/helios_dac) is normally a USB device --
 * there is no official network protocol. This is a network transport for the
 * same point concept, framed the same way etherdream.cpp emulates Ether
 * Dream's TCP protocol (see that file for the reference architecture):
 *
 *   Header (5 bytes): point_rate(u16), point_count(u16), flags(u8, bit0=play)
 *   Point  (7 bytes) x point_count: x(u16), y(u16), r(u8), g(u8), b(u8)
 *
 * x/y arrive centered on 0x8000 (same convention as the DAC8562 code space,
 * see CLAUDE.md "DAC code: coordinate + 0x8000") and are re-centered to the
 * signed LaserPoint range before galvo::pushFrame().
 */

namespace helios_net {

void init();
void task(void*);

bool isConnected();
bool isPlaying();

}  // namespace helios_net
