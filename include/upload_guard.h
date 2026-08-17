#pragma once
/**
 * upload_guard.h -- pure "reject while armed" decision for uploads/restores.
 *
 * Any action that writes a new file/config to the device while the laser
 * could be live (ILDA upload, SVG upload, config restore, OTA firmware/FS)
 * must be refused while armed -- same rationale project-wide as the existing
 * "OTA blocked while armed" rule (see ota_update.cpp). Factored out of
 * web_ui.cpp so the decision + message text is shared by every call site
 * instead of re-typed at each one, and so it is host-testable without
 * pulling in ESPAsyncWebServer/Arduino (see test/test_optimizer/
 * test_upload_guard.cpp).
 *
 * Deliberately trivial: the actual arm-state source of truth is
 * gState.laser_armed (safety.cpp) -- this header only decides what to do
 * once that single bool is known. There is no "unknown" state on the
 * firmware side; an unknown/unreachable arm state is a client-side (WebUI)
 * concept handled by requireDisarmedForUpload() in data/index.html, which
 * refuses to proceed rather than guessing when /api/state can't be reached.
 */
#include <stdio.h>
#include <stddef.h>

namespace upload_guard {

// Returns true if the request must be rejected (laser armed), and formats a
// plain human-readable reason into msgOut for that case ("Laser armed --
// disarm before <action>", e.g. action="ILDA upload"). Plain text rather than
// a ready-made JSON envelope so each call site can embed it however its own
// response shape needs (bare {"error":...}, or a field inside a larger doc).
inline bool rejected(bool laserArmed, const char* action, char* msgOut, size_t msgOutLen) {
    if (!laserArmed) return false;
    snprintf(msgOut, msgOutLen, "Laser armed -- disarm before %s", action);
    return true;
}

} // namespace upload_guard
