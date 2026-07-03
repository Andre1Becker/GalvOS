#include "mutex.h"
#include <esp_log.h>

namespace mtx {

SemaphoreHandle_t config  = nullptr;
SemaphoreHandle_t state   = nullptr;
SemaphoreHandle_t sd      = nullptr;
SemaphoreHandle_t zone    = nullptr;
SemaphoreHandle_t paint   = nullptr;

void init() {
    config  = xSemaphoreCreateMutex();
    state   = xSemaphoreCreateMutex();
    sd      = xSemaphoreCreateMutex();
    zone    = xSemaphoreCreateMutex();
    paint   = xSemaphoreCreateMutex();
    ESP_LOGI("mtx", "5 Mutexes created");
    configASSERT(config && state && sd && zone && paint);
}

} // namespace mtx
