#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// FreeRTOS task stack-margin watchdog. Mirrors cpu_monitor.cpp's
// register-then-poll pattern. Logs once per task if free stack drops
// below a warning threshold, so shrinking headroom shows up in logs
// before it becomes a stack-canary crash.
namespace stackMon {

// Register a task for monitoring. Call once, right after task creation.
// stackBytes is the size passed to xTaskCreate(...) -- also fed into
// mem_registry as a running "Task Stacks (est.)" total, since FreeRTOS
// task stacks otherwise show up as unattributed internal-heap usage.
void watch(TaskHandle_t h, const char* name, size_t stackBytes);

// Call periodically (~1s) from any task.
void update();

} // namespace stackMon