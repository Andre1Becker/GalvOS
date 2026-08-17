/**
 * test_upload_guard.cpp -- host-side tests for upload_guard.h's
 * armed/disarmed admission decision (GalvOS Safety Check feature, see
 * upload_guard.h and web_ui.cpp's rejectIfArmed()/ILDA+SVG upload handlers).
 *
 * This is the one piece of the "disarm before upload" flow that can run on
 * the host: the actual HTTP handlers (web_ui.cpp, ota_update.cpp) pull in
 * ESPAsyncWebServer/Arduino, which this native test environment deliberately
 * excludes (see platformio.ini's [env:native] comment) -- so the accept/
 * reject *decision* was factored out into a tiny dependency-free header
 * specifically so it has something to be unit-tested against. The WebUI's
 * client-side half (requireDisarmedForUpload() in data/index.html: refuse on
 * an unreachable/unknown arm state, offer to disarm-and-continue) and the
 * end-to-end HTTP behavior remain manually verified against real hardware --
 * not claimed as covered here.
 *
 * Runs in the same native binary as test_contract.cpp (one main(), see that
 * file) -- `pio test -e native`.
 */

#include <unity.h>

#include <string.h>

#include "upload_guard.h"

void test_uploadGuard_allowsWhenDisarmed(void) {
    char msg[80] = "untouched";
    bool rejected = upload_guard::rejected(false, "ILDA upload", msg, sizeof(msg));
    TEST_ASSERT_FALSE(rejected);
    // Disarmed path must not touch the message buffer -- callers rely on it
    // staying whatever it was (e.g. cleared) when nothing was rejected.
    TEST_ASSERT_EQUAL_STRING("untouched", msg);
}

void test_uploadGuard_rejectsWhenArmed(void) {
    char msg[80] = "";
    bool rejected = upload_guard::rejected(true, "ILDA upload", msg, sizeof(msg));
    TEST_ASSERT_TRUE(rejected);
    TEST_ASSERT_TRUE(strlen(msg) > 0);
}

void test_uploadGuard_messageNamesTheAction(void) {
    char msg[80] = "";
    upload_guard::rejected(true, "SVG upload", msg, sizeof(msg));
    TEST_ASSERT_NOT_NULL(strstr(msg, "SVG upload"));
    TEST_ASSERT_NOT_NULL(strstr(msg, "armed"));
}
