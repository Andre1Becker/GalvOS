#pragma once
/**
 * wifi_watchdog.h — Gateway reachability watchdog.
 *
 * Periodically ICMP-pings the DHCP/static default gateway. If no reply
 * arrives for gConfig.wifi_watchdog_timeout_ms (default 5 min), forces a
 * failsafe reboot via safety::failsafeReboot() — covers the case where the
 * board still reports WL_CONNECTED and heap is healthy, but the router link
 * or the local network stack (AsyncTCP/lwIP) has silently wedged. Disabled
 * entirely via gConfig.wifi_watchdog_reboot_enabled (WebUI Config tab).
 */
#include <stdint.h>

namespace wifi_watchdog {

void task(void*);              // Periodic ping/reboot task (Core 0)
uint32_t msSinceLastReply();   // For WebUI diagnostics; UINT32_MAX if never replied

} // namespace wifi_watchdog
