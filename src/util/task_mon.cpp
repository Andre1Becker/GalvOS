#include "task_mon.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "stack_mon.h"

// Arduino core's setup()/loop() task handle (cores/esp32/main.cpp) -- not
// declared in a public header, so declared extern here.
extern TaskHandle_t loopTaskHandle;

namespace taskMon {
namespace {

const char* stateStr(eTaskState s) {
    switch (s) {
        case eRunning:   return "running";
        case eReady:     return "ready";
        case eBlocked:   return "blocked";
        case eSuspended: return "suspended";
        case eDeleted:   return "deleted";
        default:         return "invalid";
    }
}

void addTask(JsonArray& tasks, TaskHandle_t h, const char* name) {
    if (!h) return;
    JsonObject o = tasks.add<JsonObject>();
    o["name"]             = name;
    o["core"]             = xTaskGetAffinity(h); // 0 / 1, tskNO_AFFINITY (-1) if unpinned
    o["priority"]         = (int)uxTaskPriorityGet(h);
    o["state"]            = stateStr(eTaskGetState(h));
    o["stack_free_bytes"] = (uint32_t)uxTaskGetStackHighWaterMark(h) * sizeof(StackType_t);
}

} // namespace

void buildJson(JsonDocument& doc) {
    JsonArray tasks = doc["tasks"].to<JsonArray>();

    // Tier 1: every task this firmware creates -- exact, via stack_mon's
    // existing registry (see main.cpp's startTask() helper).
    for (size_t i = 0; i < stackMon::count(); i++) {
        addTask(tasks, stackMon::handleAt(i), stackMon::nameAt(i));
    }

    // Tier 2: well-known framework tasks -- best-effort, silently omitted
    // if not found (name mismatch, or not created e.g. AsyncTCP idle).
    addTask(tasks, loopTaskHandle, "loopTask");
    addTask(tasks, xTaskGetIdleTaskHandleForCPU(0), "IDLE0");
    addTask(tasks, xTaskGetIdleTaskHandleForCPU(1), "IDLE1");
    addTask(tasks, xTaskGetHandle("async_tcp"), "async_tcp"); // AsyncTCP.cpp's own literal task name
}

} // namespace taskMon
