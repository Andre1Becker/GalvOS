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

}  // namespace web_ui

extern AsyncWebServer s_server;  // for ota_update
