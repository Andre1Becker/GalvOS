#pragma once
/**
 * web_ui.h
 */

#include "config.h"
#include <ESPAsyncWebServer.h>

namespace web_ui {

void init();
void task(void*);
int  activeRequests();  // diagnostic: requests currently in flight (see web_ui.cpp)

// Camera-in-the-loop calibration session (/api/calib-cam/*, see web_ui.cpp).
// calibCamForceStop() restores the session's optimizer snapshot and clears
// gState.calib_active -- called from pattern_engine::task() the instant
// E-Stop trips, so a tuning run can never leave a profile's live values
// altered after an emergency stop.
bool calibCamActive();
void calibCamForceStop();

}  // namespace web_ui

extern AsyncWebServer s_server;  // for ota_update
