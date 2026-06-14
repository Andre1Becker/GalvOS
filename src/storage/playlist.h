#pragma once
#include <Arduino.h>
/** playlist.h */
/** playlist.h — SD-Show-Playlist Autoplay
 *  Loads playlist.json from SD: [{file,loop_count,pause_ms}, ...]
 */
namespace playlist {
bool loadFromSD();      // /playlist.json parsen
void start();
void stop();
void task(void*);       // Autoplay-Task
bool isActive();
uint8_t currentEntry();
}
