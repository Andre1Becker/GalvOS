#pragma once
/** ota_update.h — OTA Firmware-Update via WiFi
 *  - ArduinoOTA: IDE/CLI Push via Port 3232
 *  - HTTP-Upload: POST /api/ota/upload (Binary)
 *  - URL: http://laser-XXXX.local/update (Browser-Upload)
 */
namespace ota_update {
void init();          // ArduinoOTA + HTTP-Endpoint register
void handle();        // Call in loop (ArduinoOTA.handle())
}
