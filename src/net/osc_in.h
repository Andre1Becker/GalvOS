#pragma once
/**
 * osc_in.h -- Open Sound Control (OSC 1.0) receiver over UDP
 *
 * Listens on UDP:9000 for individual OSC messages (no bundle support).
 * Supported addresses:
 *   /galvos/preset      i        select preset by index
 *   /galvos/color       f f f    r, g, b in 0.0-1.0
 *   /galvos/speed       f        0.0-1.0
 *   /galvos/brightness  f        0.0-1.0 -> master dimmer
 *   /galvos/enable      i        0|1 -> WebUI-priority override (see config.h
 *                                 gState.ui_override / pattern_engine readDmx())
 */

namespace osc_in {

void init();

bool isActive();   // true if a message arrived within the last second

}  // namespace osc_in
