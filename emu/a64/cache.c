#include "emu/a64/cache.h"

// The AArch64 engine's compiled-code cache.
//
// The i386 engine's counterpart is struct asbestos (asbestos/asbestos.h): a
// cache of fibers (arrays of gadget pointers) keyed by guest address, which
// must be thrown away whenever a mapped page changes. kernel/memory64.c calls
// a64_code_cache_invalidate_page() for exactly that reason.
//
// P0 only needs the hook to exist, so that the 64-bit memory model can be
// written and unit-tested on its own. The fiber cache itself arrives with the
// interpreter, at which point this file stops being a no-op.
//
// TODO(P1): replace with the real fiber cache and honour a64_code_cache.changes
// the way the TLB does (see emu/a64/mmu.h).

void a64_code_cache_invalidate_page(struct a64_code_cache *cache, page64_t page) {
    (void) cache;
    (void) page;
}

void a64_code_cache_invalidate_all(struct a64_code_cache *cache) {
    (void) cache;
}
