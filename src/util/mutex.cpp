#include "mutex.h"
#include <esp_log.h>

namespace mtx {

SemaphoreHandle_t config  = nullptr;
SemaphoreHandle_t state   = nullptr;
SemaphoreHandle_t sd      = nullptr;
SemaphoreHandle_t preview = nullptr;
SemaphoreHandle_t zone    = nullptr;

void init() {
    config  = xSemaphoreCreateMutex();
    state   = xSemaphoreCreateMutex();
    sd      = xSemaphoreCreateMutex();
    preview = xSemaphoreCreateMutex();
    zone    = xSemaphoreCreateMutex();
    ESP_LOGI("mtx", "5 Mutexes created");
    configASSERT(config && state && sd && preview && zone);
}

} // namespace mtx
