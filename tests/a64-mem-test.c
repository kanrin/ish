// Self-test for the AArch64 engine's address space (kernel/memory64.c).
//
// The i386 engine's page table is two levels of 10 bits, i.e. exactly 4 GiB, so
// it cannot even express the addresses an AArch64 guest uses. This test
// exercises the 64-bit replacement on the host, with no guest involved:
//
//   ninja -C build && ./build/a64_mem_test     (or: meson test a64_mem)
//
// The wrappers below honour the locking contract documented in
// kernel/memory64.h (maps under the write lock, accesses under the read lock),
// which is the same one kernel/memory.c has.

#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include "kernel/memory64.h"

static int failures;

#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        printf("FAIL %s:%d: ", __FILE__, __LINE__); \
        printf(__VA_ARGS__); \
        printf("\n"); \
        failures++; \
    } \
} while (0)

static int map(struct mem64 *m, page64_t start, pages64_t pages, unsigned flags) {
    write_wrlock(&m->lock);
    int err = mem64_map_nothing(m, start, pages, flags);
    write_wrunlock(&m->lock);
    return err;
}

static int unmap(struct mem64 *m, page64_t start, pages64_t pages) {
    write_wrlock(&m->lock);
    int err = mem64_unmap(m, start, pages);
    write_wrunlock(&m->lock);
    return err;
}

static int set_flags(struct mem64 *m, page64_t start, pages64_t pages, int flags) {
    write_wrlock(&m->lock);
    int err = mem64_set_flags(m, start, pages, flags);
    write_wrunlock(&m->lock);
    return err;
}

static int share(struct mem64 *src, struct mem64 *dst, page64_t start, pages64_t pages) {
    write_wrlock(&src->lock);
    write_wrlock(&dst->lock);
    int err = mem64_copy_on_write(src, dst, start, pages);
    write_wrunlock(&dst->lock);
    write_wrunlock(&src->lock);
    return err;
}

static void *ptr(struct mem64 *m, uaddr_t addr, int type) {
    read_wrlock(&m->lock);
    void *p = mem64_ptr(m, addr, type);
    read_wrunlock(&m->lock);
    return p;
}

static bool mapped(struct mem64 *m, page64_t page) {
    read_wrlock(&m->lock);
    bool is_mapped = mem64_pt(m, page) != NULL;
    read_wrunlock(&m->lock);
    return is_mapped;
}

static bool is_hole(struct mem64 *m, page64_t start, pages64_t pages) {
    read_wrlock(&m->lock);
    bool hole = mem64_is_hole(m, start, pages);
    read_wrunlock(&m->lock);
    return hole;
}

int main(void) {
    struct mem64 mem;
    mem64_init(&mem);

    // A page at 4 GiB + 4 KiB: the smallest address the 32-bit page table
    // cannot describe at all.
    uaddr_t high = (1ULL << 32) + A64_PAGE_SIZE;
    CHECK(map(&mem, A64_PAGE(high), 1, P_READ | P_WRITE) == 0, "map a page above 4 GiB");
    void *p = ptr(&mem, high, MEM_WRITE);
    CHECK(p != NULL, "translate an address above 4 GiB");
    if (p != NULL) {
        memset(p, 0xab, 16);
        void *r = ptr(&mem, high, MEM_READ);
        CHECK(r != NULL && *(unsigned char *) r == 0xab, "read back what was written");
        CHECK(*(unsigned char *) ptr(&mem, high + 15, MEM_READ) == 0xab,
                "the last byte written is still inside the page");
    }
    CHECK(mapped(&mem, A64_PAGE(high)), "the page table entry exists");
    CHECK(!mapped(&mem, A64_PAGE(high) + 1), "the next page is not mapped");

    // An address near the top of the 48-bit space, to walk every level of the
    // radix rather than just the low ones.
    uaddr_t higher = 0x7f0000000000ULL;
    CHECK(higher < A64_VA_TOP, "0x7f0000000000 fits in 48 bits");
    CHECK(map(&mem, A64_PAGE(higher), 2, P_READ | P_WRITE) == 0,
            "map two pages at the top of the address space");
    CHECK(ptr(&mem, higher + A64_PAGE_SIZE, MEM_WRITE) != NULL,
            "translate the second page of that mapping");

    // Permissions are per page, and a page boundary is a page boundary.
    uaddr_t ro = 0x200000000ULL;
    CHECK(map(&mem, A64_PAGE(ro), 1, P_READ) == 0, "map a read-only page");
    CHECK(ptr(&mem, ro, MEM_READ) != NULL, "read a read-only page");
    CHECK(ptr(&mem, ro, MEM_WRITE) == NULL, "writing a read-only page must fail");
    CHECK(ptr(&mem, ro + A64_PAGE_SIZE - 1, MEM_READ) != NULL, "last byte of the page");
    CHECK(ptr(&mem, ro + A64_PAGE_SIZE, MEM_READ) == NULL,
            "the first byte after the page is unmapped");
    CHECK(set_flags(&mem, A64_PAGE(ro), 1, P_READ | P_WRITE) == 0, "make it writable");
    CHECK(ptr(&mem, ro, MEM_WRITE) != NULL, "now the write goes through");

    CHECK(ptr(&mem, 0x1000000000ULL, MEM_READ) == NULL, "an unmapped address");

    // P_GROWSDOWN: writing below the region maps a page, which is how the
    // guest's stack grows.
    uaddr_t stack_top = 0x300000000ULL + 8 * A64_PAGE_SIZE;
    CHECK(map(&mem, A64_PAGE(stack_top), 1, P_READ | P_WRITE | P_GROWSDOWN) == 0,
            "map the top of a stack");
    CHECK(ptr(&mem, stack_top - A64_PAGE_SIZE, MEM_WRITE) != NULL,
            "a write below a P_GROWSDOWN region maps a page");
    CHECK(mapped(&mem, A64_PAGE(stack_top) - 1), "and that page is really mapped");
    CHECK(ptr(&mem, stack_top - 2 * A64_PAGE_SIZE, MEM_WRITE) != NULL,
            "the grown page can grow again, as a real stack does");

    // Copy-on-write: this is what fork does. The child shares the parent's page
    // until one of them writes to it.
    struct mem64 child;
    mem64_init(&child);
    uaddr_t cow = 0x400000000ULL;
    CHECK(map(&mem, A64_PAGE(cow), 1, P_READ | P_WRITE) == 0, "map the parent's page");
    void *parent_page = ptr(&mem, cow, MEM_WRITE);
    CHECK(parent_page != NULL, "translate the parent's page");
    if (parent_page != NULL)
        memset(parent_page, 0x11, A64_PAGE_SIZE);
    CHECK(share(&mem, &child, A64_PAGE(cow), 1) == 0, "share it with a child");
    void *child_page = ptr(&child, cow, MEM_READ);
    CHECK(child_page != NULL && child_page == parent_page,
            "before any write the child shares the same host page");
    CHECK(child_page != NULL && *(unsigned char *) child_page == 0x11,
            "and sees the parent's data");
    void *child_copy = ptr(&child, cow, MEM_WRITE);
    CHECK(child_copy != NULL && child_copy != parent_page,
            "a write in the child copies the page");
    if (child_copy != NULL)
        memset(child_copy, 0x22, 1);
    CHECK(*(unsigned char *) ptr(&mem, cow, MEM_READ) == 0x11,
            "the parent's copy is unaffected");
    CHECK(*(unsigned char *) ptr(&child, cow, MEM_READ) == 0x22,
            "the child keeps its own value");
    mem64_destroy(&child);

    // The hole finder hands out free space, and mapped space stops being free.
    page64_t hole;
    read_wrlock(&mem.lock);
    hole = mem64_find_hole(&mem, 4);
    read_wrunlock(&mem.lock);
    CHECK(hole != A64_BAD_PAGE, "found a hole");
    CHECK(hole > A64_PAGE(A64_VA_BOTTOM), "the hole is inside the address space");
    CHECK(is_hole(&mem, hole, 4), "what it returned really is a hole");
    CHECK(map(&mem, hole, 4, P_READ | P_WRITE) == 0, "fill the hole");
    CHECK(!is_hole(&mem, hole, 4), "now it is not a hole any more");

    // Unmapping gives the memory back and forgets the page.
    CHECK(unmap(&mem, hole, 4) == 0, "unmap it");
    CHECK(!mapped(&mem, hole), "the page is gone");
    CHECK(unmap(&mem, hole, 4) == -1, "unmapping it twice reports a missing page");

    // Iterating mappings visits the mapped pages and skips the 2^36 pages that
    // are not mapped (a naive loop could never finish).
    int mapped_seen = 0;
    read_wrlock(&mem.lock);
    for (page64_t page = 0; page < A64_MAX_PAGES; mem64_next_page(&mem, &page)) {
        if (page >= A64_MAX_PAGES)
            break;
        if (mem64_pt(&mem, page) != NULL)
            mapped_seen++;
    }
    read_wrunlock(&mem.lock);
    CHECK(mapped_seen >= 6, "iteration found the mapped pages (found %d)", mapped_seen);

    mem64_destroy(&mem);

    if (failures == 0) {
        printf("a64 memory: all checks passed\n");
        return 0;
    }
    printf("a64 memory: %d checks FAILED\n", failures);
    return 1;
}
