#pragma once
/**
 * encoder.h -- Rotary Encoder (GPIO 15/40/41)
 * Drehen:        Pattern switch or Master-Dimmer
 * Short press:  switch mode (pattern / dimmer / ILDA)
 * Long press:  master dimmer toggle (laser ON/OFF)
 * 2× kurz:       next calibration pattern
 */
namespace encoder {
void init();
void task(void*);
}
