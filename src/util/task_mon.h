#pragma once
#include <ArduinoJson.h>

// Read-only FreeRTOS task snapshot for the WebUI's Log tab Task Viewer.
// Answers "what's actually running on Core 0" without a serial monitor.
//
// No per-task CPU% here -- unlike an ESP-IDF-component build, this
// framework's prebuilt libfreertos.a has configUSE_TRACE_FACILITY compiled
// OUT (verified: uxTaskGetSystemState()/vTaskGetRunTimeStats() are absent
// from the archive's symbol table, undefined-reference at link time). That
// can only be fixed by rebuilding the ESP-IDF FreeRTOS component from
// source -- a build-system change, not something to take on quietly in a
// safety-critical firmware for a viewer feature. See task_mon.cpp for
// exactly which introspection functions ARE present and used instead.
//
// Two-tier task list, since there's no enumeration API without trace
// facility either: (1) every task this firmware itself creates, via
// stack_mon.cpp's existing registry (name/handle, already populated at
// every startTask() call site) -- exact, complete for our own code; (2)
// a small set of well-known framework tasks (Arduino's loopTask, both
// FreeRTOS idle tasks, AsyncTCP's service task) resolved by handle/name
// at call time -- best-effort, silently omitted if not found. WiFi/lwIP's
// own internal tasks are NOT enumerable this way and do not appear.
//
// On-demand only: call from an HTTP handler (see web_ui.cpp's /api/tasks),
// never a hot loop -- cheap, but there's no reason to run it unpolled.
namespace taskMon {

// Appends a "tasks" array to doc: one object per task with name, core
// (0/1, or -1 if unpinned), priority, state, and free stack bytes.
void buildJson(JsonDocument& doc);

} // namespace taskMon
