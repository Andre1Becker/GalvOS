#include "alloc_probe.h"

#include <stdlib.h>

extern "C" {
void* __real_malloc(size_t size);
void  __real_free(void* ptr);
void* __wrap_malloc(size_t size);
void  __wrap_free(void* ptr);
}

namespace {
bool          sArmed   = false;
bool          sReached = false;
long          sLive    = 0;
unsigned long sTotal   = 0;
int           sFailNext = 0;
}  // namespace

extern "C" void* __wrap_malloc(size_t size) {
    sReached = true;
    if (sArmed && sFailNext > 0) {
        sFailNext--;
        return nullptr;
    }
    void* p = __real_malloc(size);
    if (sArmed && p) {
        sLive++;
        sTotal++;
    }
    return p;
}

extern "C" void __wrap_free(void* ptr) {
    if (sArmed && ptr) sLive--;
    __real_free(ptr);
}

namespace allocProbe {

void reset() {
    sArmed    = false;
    sLive     = 0;
    sTotal    = 0;
    sFailNext = 0;
}

void arm(bool on)        { sArmed = on; }
long live()              { return sLive; }
unsigned long total()    { return sTotal; }
void failNext(int n)     { sFailNext = n; }
bool active()            { return sReached; }

}  // namespace allocProbe
