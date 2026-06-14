#pragma once
/**
 * helios_usb.h -- Helios DAC USB Vendor-Class Emulation
 *
 * Helios protocol reference: https://github.com/Grix/helios_dac
 * Vendor ID 0x1209, Product ID 0xE500 (Generic Helios)
 *
 * We use the ESP32-S3 USB-OTG controller in vendor-class mode.
 * Helios uses a simple bulk-transfer protocol:
 *   - Frame: 5 Bytes Header (rate_l, rate_h, count_l, count_h, flags)
 *            followed by points of 7 bytes each (x_h, x_l|y_h, y_l, r, g, b, i)
 *   - Status-Request: 0x03 0x00 -> Response 2 Byte
 *   - Stop: 0x01 0x00
 *
 * Stub implementation. Fully implement when the first part runs.
 */

namespace helios_usb {

void init();
void task(void*);

}
