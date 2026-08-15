#pragma once
/**
 * wifi_watchdog.h — Gateway reachability watchdog, two escalating tiers.
 *
 * Periodically ICMP-pings the DHCP/static default gateway — covers the case
 * where the board still reports WL_CONNECTED and heap is healthy, but the
 * WiFi driver missed a silent AP-side drop and the link is actually dead.
 *   1. wifi_watchdog_soft_timeout_ms: WiFi.disconnect()+reconnect() --
 *      re-associates the STA link only, nothing else on the board is
 *      touched (laser stays armed, pattern engine/DMX/Art-Net keep running).
 *   2. wifi_watchdog_timeout_ms: still unreachable after that -- true last
 *      resort, safety::failsafeReboot(). An unplanned reboot mid-show is
 *      worse than a temporarily unreachable WebUI, so this tier is
 *      deliberately far out and only fires once the gentle recovery has
 *      clearly failed.
 * Disabled entirely via gConfig.wifi_watchdog_reboot_enabled (WebUI Config
 * tab) -- disables both tiers.
 */
#include <stdint.h>

namespace wifi_watchdog {

void task(void*);              // Periodic ping/reboot task (Core 0)
uint32_t msSinceLastReply();   // For WebUI diagnostics; UINT32_MAX if never replied
uint32_t softRecoveryCount();  // Cumulative tier-1 (WiFi.disconnect/reconnect) attempts

} // namespace wifi_watchdog
