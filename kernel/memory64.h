#ifndef KERNEL_MEMORY64_H
#define KERNEL_MEMORY64_H

#include "kernel/memory.h" // struct pt_entry, struct data, the P_* flags, real_page_size
#include "emu/a64/mmu.h"

// The address space of an AArch64 guest: the 64-bit counterpart of struct mem
// in kernel/memory.h.
//
// Same model as the i386 engine -- guest pages are host mmap()s at arbitrary
// addresses, and the page table translates guest addresses into them -- but
// with a 48-bit address space, so the page table is a four-level radix (see
// emu/a64/mmu.h) instead of two levels of 10 bits.
//
// The struct pt_entry, struct data and P_* definitions are shared with the
// 32-bit engine on purpose: they describe a mapping, not an address width.

// An intermediate page-table node. Level 0 is the root; levels 1..A64_PT_LEVELS-2
// hold further nodes; the last level holds struct pt_entry pointers. Nodes are
// allocated on demand, so a mostly-empty 48-bit address space costs almost
// nothing (which is why the whole space can be described without a limit like
// the 32-bit engine's MEM_PAGES).
struct a64_pt_node {
    void *entries[A64_PT_ENTRIES];
};

struct mem64 {
    struct a64_pt_node *root; // NULL until something is mapped
    int nodes_used;
    struct mmu64 mmu;
    wrlock_t lock;
};

// ------- locking, same contract as kernel/memory.h -------
//
//   * mem64_map*, mem64_unmap*, mem64_set_flags and mem64_copy_on_write must be
//     called with the write lock held (write_wrlock(&mem->lock));
//   * mem64_ptr must be called with the read lock held
//     (read_wrlock(&mem->lock)). It releases and reacquires it internally when
//     it has to change the page table (stack growth, copy-on-write), exactly
//     like kernel/memory.c's mem_ptr does;
//   * the pure lookups (mem64_pt, mem64_is_hole, mem64_find_hole,
//     mem64_next_page) only read the page table; call them with either lock
//     held, as their callers in the kernel do.

void mem64_init(struct mem64 *mem);
void mem64_destroy(struct mem64 *mem);

// The page table entry for a page, or NULL if it is not mapped.
struct pt_entry *mem64_pt(struct mem64 *mem, page64_t page);
// Advance *page to the next mapped page, or to A64_MAX_PAGES if there is none.
// Used to iterate over the mappings of an address space that has 2^36 pages in
// it, most of which are empty.
void mem64_next_page(struct mem64 *mem, page64_t *page);
// Is [start, start+pages) entirely unmapped?
bool mem64_is_hole(struct mem64 *mem, page64_t start, pages64_t pages);
// Find a hole of the given size, searching downwards from the top of the
// address space like the 32-bit engine does.
page64_t mem64_find_hole(struct mem64 *mem, pages64_t size);

// Map memory + offset into the address space, unmapping anything in the way.
// Takes ownership of memory, which is released with munmap(memory,
// pages * A64_PAGE_SIZE) once the last reference goes away.
int mem64_map(struct mem64 *mem, page64_t start, pages64_t pages, void *memory, size_t offset, unsigned flags);
// Map anonymous zeroed memory.
int mem64_map_nothing(struct mem64 *mem, page64_t start, pages64_t pages, unsigned flags);
int mem64_unmap(struct mem64 *mem, page64_t start, pages64_t pages);
int mem64_unmap_always(struct mem64 *mem, page64_t start, pages64_t pages);
int mem64_set_flags(struct mem64 *mem, page64_t start, pages64_t pages, int flags);
// Give dst the same pages as src, shared copy-on-write (this is how fork gives
// a child its own view of the parent's memory).
int mem64_copy_on_write(struct mem64 *src, struct mem64 *dst, page64_t start, pages64_t pages);

// The host address behind a guest address, or NULL if it cannot be accessed
// (which is what the caller turns into a SIGSEGV for the guest). As in
// kernel/memory.c this may map a page on demand: a write to an unmapped page
// just below a P_GROWSDOWN region grows that region, and a write to a
// P_COW page makes a private copy of it.
void *mem64_ptr(struct mem64 *mem, uaddr_t addr, int type);

#endif
