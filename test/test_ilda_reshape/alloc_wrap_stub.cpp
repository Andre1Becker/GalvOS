/**
 * alloc_wrap_stub.cpp -- pass-through malloc/free wrap for this test binary.
 *
 * [env:native]'s build_flags apply `-Wl,--wrap=malloc,--wrap=free` to every
 * test program in the suite (needed by test_optimizer's allocFreeSymmetric
 * probe, see test/test_optimizer/alloc_probe.cpp), which means the linker
 * requires a `__wrap_malloc`/`__wrap_free` symbol in EVERY test binary, not
 * just the one that actually counts allocations. This suite does not test
 * allocation behavior, so it just forwards straight through.
 */
#include <stdlib.h>

extern "C" {
void* __real_malloc(size_t size);
void  __real_free(void* ptr);

void* __wrap_malloc(size_t size) { return __real_malloc(size); }
void  __wrap_free(void* ptr)     { __real_free(ptr); }
}
