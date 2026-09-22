#ifndef EMU_A64_CACHE_H
#define EMU_A64_CACHE_H

#include "emu/a64/mmu.h"

// The AArch64 engine's compiled-code cache: see emu/a64/cache.c.
struct a64_code_cache;

// Forget everything that was compiled from the given guest page. Safe to call
// with NULL (which is what the memory model does until the cache exists).
void a64_code_cache_invalidate_page(struct a64_code_cache *cache, page64_t page);
void a64_code_cache_invalidate_all(struct a64_code_cache *cache);

#endif
