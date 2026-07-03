#pragma once
#include "config.h"

namespace patterns {

void init();
void task(void*);
void setManualMode(bool enable, uint8_t pattern_id);
void triggerTestPattern(const char* name);
void stopTestPattern();           // cancel running hw test pattern immediately
void setPreset(int8_t idx);   // -1 = preset off, 0-39 = preset active
int8_t getPreset();
void   setCurve(int8_t idx);  // -1 = off, 0-8 = curve
int8_t getCurve();
void   setPaintActive(bool active);  // Paint-by-Finger mode on/off
bool   getPaintActive();

}  // namespace patterns
