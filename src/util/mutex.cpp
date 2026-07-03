#include "mutex.h"
#include <esp_log.h>

namespace mtx {

SemaphoreHandle_t config  = nullptr;
SemaphoreHandle_t state   = nullptr;
SemaphoreHandle_t sd      = nullptr;
SemaphoreHandle_t zone    = nullptr;

void init() {
    config  = xSemaphoreCreateMutex();
    state   = xSemaphoreCreateMutex();
    sd      = xSemaphoreCreateMutex();
    zone    = xSemaphoreCreateMutex();
    ESP_LOGI("mtx", "4 Mutexes created");
    configASSERT(config && state && sd && zone);
}

} // namespace mtx
