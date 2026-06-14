#pragma once
/**
 * ntp_client.h — NTP time sync for timestamp-aware logging
 * Configurable server via gConfig.ntp_server (default: pool.ntp.org)
 */
#include <stdint.h>
#include <time.h>

namespace ntp_client {

void init();                    // Call after WiFi is connected
void task(void*);               // Periodic re-sync task (runs on Core 0)
bool isSynced();                // True once first sync succeeded
time_t nowEpoch();              // Current UTC epoch (0 if not synced)
// Formatted: "HH:MM:SS" or "YYYY-MM-DD HH:MM:SS"
void formatTime(char* buf, size_t len, bool date = false);

} // namespace ntp_client
