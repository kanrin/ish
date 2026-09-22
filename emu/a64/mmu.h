#ifndef EMU_A64_MMU_H
#define EMU_A64_MMU_H

#include "misc.h"

// The guest address space of the AArch64 engine.
//
// The i386 engine's counterpart is emu/mmu.h, where addr_t/page_t are 32 bits
// and the page table is two levels of 10 bits (exactly 4 GiB). An AArch64
// guest cannot be described that way, so this is a separate, 64-bit model:
//
//   * 48-bit virtual addresses, split into 4 KiB pages (like Linux on arm64
//     with a 4 KiB granule and 48-bit VA: 36 bits of page number)
//   * a 4-level radix page table, 9 bits per level, which is the same shape as
//     the real aarch64 MMU and is sparse: only the paths that are mapped cost
//     memory, so the huge address space is not a problem
//
// Everything here is expressed in uaddr_t, the 64-bit user pointer type that
// syscalls already use (see misc.h), so there is no conversion at the syscall
// boundary.
//
// Note that the *host* still reads and writes guest memory through pointers
// into the shadow mappings, exactly like the i386 engine: a guest address is
// not a host address, and every access goes through the page table. See
// kernel/memory64.c.

typedef uint64_t page64_t;  // a page number: address >> A64_PAGE_BITS
typedef uint64_t pages64_t; // a count of pages

#define A64_PAGE_BITS 12
#define A64_PAGE_SIZE (1ULL << A64_PAGE_BITS)
#define A64_PAGE(addr) ((uaddr_t) (addr) >> A64_PAGE_BITS)
#define A64_PGOFFSET(addr) ((uaddr_t) (addr) & (A64_PAGE_SIZE - 1))
#define A64_PAGE_ROUND_UP(bytes) (((bytes) + A64_PAGE_SIZE - 1) >> A64_PAGE_BITS)

#define A64_VA_BITS 48
#define A64_VA_TOP (1ULL << A64_VA_BITS)
// Nothing is mapped below this, like the i386 engine's implicit 0x40000 floor.
#define A64_VA_BOTTOM 0x10000ULL

// 4 levels of 9 bits covers 36 bits of page number, i.e. 48 bits of address.
#define A64_PT_LEVELS 4
#define A64_PT_BITS 9
#define A64_PT_ENTRIES (1 << A64_PT_BITS)
#define A64_MAX_PAGES (A64_VA_TOP >> A64_PAGE_BITS)
#define A64_BAD_PAGE 0 // page 0 is never mappable, so it makes a good sentinel

// Which entry of the page table (or of an intermediate node) a page lives in.
// Level 0 is the root, level A64_PT_LEVELS - 1 holds the leaves.
static inline unsigned a64_pt_index(page64_t page, int level) {
    return (page >> (A64_PT_BITS * (A64_PT_LEVELS - 1 - level))) & (A64_PT_ENTRIES - 1);
}

// How many pages one node at the given level spans.
static inline pages64_t a64_pt_span(int level) {
    return 1ULL << (A64_PT_BITS * (A64_PT_LEVELS - 1 - level));
}

// A TLB entry for the AArch64 engine.
//
// This is 32 bytes rather than 24 on purpose: the gadgets are assembly and
// index entries by shifting, so the size has to stay a power of two (the i386
// gadgets get away with `lsl x9, x9, 4` because their entry is 16 bytes).
struct tlb64_entry {
    page64_t page;            // 0 is not a valid page number, so 0 means empty
    page64_t page_if_writable;
    uintptr_t data_minus_addr; // host address of the page minus the page address
    uint64_t reserved;
};
static_assert(sizeof(struct tlb64_entry) == 32, "tlb64 entry size");

struct mem64;
struct a64_code_cache;

// The MMU side of the engine: what the TLB needs to know about, plus the
// version counter that tells it when its entries went stale.
struct mmu64 {
    struct a64_code_cache *cache; // NULL until the engine's code cache exists
    uint64_t changes;
};

static inline void mem64_changed_(struct mmu64 *mmu) {
    mmu->changes++;
}

#endif
