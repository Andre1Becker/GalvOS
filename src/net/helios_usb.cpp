#include "helios_usb.h"
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>

namespace helios_usb {

static const char* TAG = "helios";

void init() {
    // TODO: TinyUSB Vendor-Class register
    //   tusb_desc_device.idVendor  = 0x1209;
    //   tusb_desc_device.idProduct = 0xE500;
    //   tud_vendor_init();
    ESP_LOGW(TAG, "Helios USB stub -- TinyUSB vendor-class to be implemented");
}

void task(void*) {
    for (;;) {
        // tud_task() handle here or via FreeRTOS-Hook
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

}
