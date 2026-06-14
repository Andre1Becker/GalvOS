#pragma once
/**
 * safety.h -- Hardware-Safety-Aggregation for Klasse-4-Betrieb
 *
 * Konzept:
 *   The laser power rail is switched via an external SSR/relay.
 *   The relay is only ACTIVE (laser-capable) if ALL of the following conditions
 *   are simultaneously met:
 *     1. Interlock switch closed (enclosure lid)
 *     2. Schluesselschalter ON
 *     3. Emergency stop not gedrueckt
 *     4. Scan-Fail-Hardware meldet OK
 *     5. Firmware watchdog: heartbeat every <100 ms
 *     6. Software sets "arm = true" (user action in UI)
 *
 *   If ONE condition is missing -> PIN_LASER_ENABLE is LOW -> relay drops out.
 *
 *   Important: PIN_LASER_ENABLE switches NOT a logic signal, but a
 *   real power path. Even faulty software can keep the laser
 *   not "einfach so" einschalten, weil dazu at least Interlock+Key
 *   must be physically closed.
 */

#include "pinmap.h"
#include "config.h"

namespace safety {

void init();           // GPIOs konfigurieren
void task(void*);      // FreeRTOS task on core 0
bool allOk();          // Aggregations-Check
void requestArm(bool); // User-Wunsch ARM/DISARM
void heartbeat();
void subsystemHeartbeat(int sys);  // call each loop iteration
void emergencyStop(); // immediate shutdown (E-stop from network/OTA)

// ── ARM diagnostics ────────────────────────────────────────────────────
// Individual condition checks for /api/state — lets the UI show *why*
// the laser stays DISARMED.
bool watchdogOk();
bool subsystemsOk();
bool userArmRequest();

}  // namespace safety
