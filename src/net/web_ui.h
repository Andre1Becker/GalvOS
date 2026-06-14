#pragma once
/**
 * web_ui.h
 */

#include "config.h"
#include <ESPAsyncWebServer.h>

namespace web_ui {

void init();
void task(void*);

// Called by the pattern task to update the preview snapshot
void publishPreviewFrame(const LaserPoint* pts, size_t count);

}  // namespace web_ui

extern AsyncWebServer s_server;  // for ota_update
