#pragma once
/**
 * artnet_in.h -- Art-Net Receiver via WiFi/Ethernet
 *
 * Handles the same 16-channel layout as DMX. When active,
 * the DMX buffer is overwritten with the Art-Net data.
 */

namespace artnet_in {

void init();
void task(void*);

}  // namespace artnet_in
