#pragma once

#include <ArduinoJson.h>
#include <esp_heap_caps.h>

// Backs JsonDocument allocations with PSRAM instead of internal DRAM.
// Internal heap is shared with lwIP/WiFi and is the scarce resource on this
// board; PSRAM has ~7.7 MB free and is the right place for request-scoped
// JSON buffers (web_ui API responses, playlist load/save).
class SpiRamAllocator : public Allocator {
 public:
  void* allocate(size_t size) override {
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
  }
  void deallocate(void* ptr) override {
    heap_caps_free(ptr);
  }
  void* reallocate(void* ptr, size_t newSize) override {
    return heap_caps_realloc(ptr, newSize, MALLOC_CAP_SPIRAM);
  }
};

inline SpiRamAllocator& jsonAllocator() {
  static SpiRamAllocator instance;
  return instance;
}