#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>

#define DEFAULT_CHANNEL memory
#include "debug.h"
#include "kernel/errno.h"
#include "kernel/memory64.h"
#include "emu/a64/cache.h"
#include "fs/fd.h"

// The 64-bit counterpart of kernel/memory.c. The model is the same:
//
//   * a mapped guest page is backed by a host mmap() at whatever address the
//     host picks, so a guest address is never a host address and every access
//     goes through the page table;
//   * struct data is refcounted so that fork can share pages (copy-on-write);
//   * pages can grow downwards (P_GROWSDOWN) and be written copy-on-write
//     (P_COW), which is what makes the guest's stack and its fork() work;
//   * guest 4 KiB pages are carved out of host pages, whose size is whatever
//     the host uses (16 KiB on Apple silicon), so protection changes have to be
//     rounded down to real_page_size exactly as the 32-bit engine does.
//
// The differences are the 64-bit types, the four-level radix page table, and
// that there is no upper bound to check against: A64_MAX_PAGES is where
// iteration stops, not a limit on what can be mapped.

static void mem64_changed(struct mem64 *mem) {
    mem64_changed_(&mem->mmu);
}

// Page-table plumbing ********************************************************

static struct a64_pt_node *a64_pt_node_new(void) {
    return calloc(1, sizeof(struct a64_pt_node));
}

// Walk to the node that holds the leaves for this page, creating nodes on the
// way when asked to. Returns NULL if out of memory.
static struct a64_pt_node *a64_pt_leaf_node(struct mem64 *mem, page64_t page, bool create) {
    if (mem->root == NULL) {
        if (!create)
            return NULL;
        mem->root = a64_pt_node_new();
        if (mem->root == NULL)
            return NULL;
        mem->nodes_used++;
    }
    struct a64_pt_node *node = mem->root;
    for (int level = 0; level < A64_PT_LEVELS - 1; level++) {
        unsigned idx = a64_pt_index(page, level);
        struct a64_pt_node *next = node->entries[idx];
        if (next == NULL) {
            if (!create)
                return NULL;
            next = a64_pt_node_new();
            if (next == NULL)
                return NULL;
            node->entries[idx] = next;
            mem->nodes_used++;
        }
        node = next;
    }
    return node;
}

// The entry for a page, allocating it if asked to. An entry with data == NULL
// means "not mapped", which is how kernel/memory.c reads a zeroed pgdir slot.
static struct pt_entry *a64_pt_entry(struct mem64 *mem, page64_t page, bool create) {
    struct a64_pt_node *node = a64_pt_leaf_node(mem, page, create);
    if (node == NULL)
        return NULL;
    unsigned idx = a64_pt_index(page, A64_PT_LEVELS - 1);
    struct pt_entry *entry = node->entries[idx];
    if (entry == NULL) {
        if (!create)
            return NULL;
        entry = calloc(1, sizeof(struct pt_entry));
        if (entry == NULL)
            return NULL;
        node->entries[idx] = entry;
    }
    return entry;
}

struct pt_entry *mem64_pt(struct mem64 *mem, page64_t page) {
    if (page >= A64_MAX_PAGES)
        return NULL;
    struct pt_entry *entry = a64_pt_entry(mem, page, false);
    if (entry == NULL || entry->data == NULL)
        return NULL;
    return entry;
}

// Freeing the leaf entry as well as clearing the slot keeps the tree from
// growing when the guest maps and unmaps pages over and over.
static void a64_pt_del(struct mem64 *mem, page64_t page) {
    struct a64_pt_node *node = a64_pt_leaf_node(mem, page, false);
    if (node == NULL)
        return;
    unsigned idx = a64_pt_index(page, A64_PT_LEVELS - 1);
    free(node->entries[idx]);
    node->entries[idx] = NULL;
}

// Leaf entries are freed here as well as by a64_pt_del; that is harmless
// because a64_pt_del sets the slot back to NULL.
static void a64_pt_free_subtree(void *node, int level) {
    if (node == NULL)
        return;
    struct a64_pt_node *n = node;
    if (level == A64_PT_LEVELS - 1) {
        for (int i = 0; i < A64_PT_ENTRIES; i++)
            free(n->entries[i]);
    } else {
        for (int i = 0; i < A64_PT_ENTRIES; i++)
            a64_pt_free_subtree(n->entries[i], level + 1);
    }
    free(n);
}

// Iteration and allocation ***************************************************

// Advance to the next mapped page, or to A64_MAX_PAGES if there is none.
//
// The 32-bit engine's mem_next_page only skips absent subtrees and can
// therefore land on unmapped pages, which is tolerable when the whole address
// space is 2^20 pages. Here it would mean walking 2^36 page numbers (about
// half a minute of CPU per call), so this skips absent subtrees *and* the
// unmapped remainder of a leaf node, and only ever returns a page that is
// actually mapped. Every caller only cares about mapped pages, so this is a
// superset of the old behaviour.
void mem64_next_page(struct mem64 *mem, page64_t *page) {
    (*page)++;
    if (mem->root == NULL) {
        *page = A64_MAX_PAGES;
        return;
    }
    while (*page < A64_MAX_PAGES) {
        struct a64_pt_node *node = mem->root;
        bool skipped = false;
        for (int level = 0; level < A64_PT_LEVELS - 1; level++) {
            void *child = node->entries[a64_pt_index(*page, level)];
            if (child == NULL) {
                // Nothing at all below this subtree: jump past all of it.
                pages64_t span = a64_pt_span(level);
                *page = (*page & ~(span - 1)) + span;
                skipped = true;
                break;
            }
            node = child;
        }
        if (skipped)
            continue;
        // The path down to a leaf node exists. Look for a mapped page from the
        // current slot onwards; at most A64_PT_ENTRIES slots, because the
        // node's other slots cannot be skipped without missing a mapping.
        unsigned idx = a64_pt_index(*page, A64_PT_LEVELS - 1);
        for (; idx < A64_PT_ENTRIES; idx++) {
            if (node->entries[idx] != NULL) {
                *page = (*page & ~(page64_t) (A64_PT_ENTRIES - 1)) + idx;
                return;
            }
        }
        // This leaf node holds no mappings, so skip the rest of it.
        *page = (*page & ~(page64_t) (A64_PT_ENTRIES - 1)) + A64_PT_ENTRIES;
    }
}

// The first mapped page strictly above the given one, or A64_MAX_PAGES.
static page64_t mem64_next_mapped_page(struct mem64 *mem, page64_t page) {
    mem64_next_page(mem, &page);
    return page;
}

bool mem64_is_hole(struct mem64 *mem, page64_t start, pages64_t pages) {
    for (page64_t page = start; page < start + pages; mem64_next_page(mem, &page)) {
        if (mem64_pt(mem, page) != NULL)
            return false;
    }
    return true;
}

// Search downwards from the top, like the 32-bit engine, but step over absent
// subtrees instead of over every page number.
page64_t mem64_find_hole(struct mem64 *mem, pages64_t size) {
    if (size == 0)
        return A64_BAD_PAGE;
    const page64_t bottom = A64_PAGE(A64_VA_BOTTOM);
    page64_t hole_end = 0; // exclusive top of the hole being scanned, 0 for none
    page64_t page = A64_MAX_PAGES;
    while (page > bottom) {
        page--;
        struct a64_pt_node *l0 = mem->root;
        struct a64_pt_node *l1 = l0 == NULL ? NULL : l0->entries[a64_pt_index(page, 0)];
        if (l1 == NULL) {
            // The whole subtree is missing, so this entire span is free.
            page64_t floor = page & ~(a64_pt_span(1) - 1);
            if (floor < bottom)
                floor = bottom;
            if (hole_end == 0)
                hole_end = page + 1;
            if (hole_end - floor >= size)
                return hole_end - size;
            page = floor;
            continue;
        }
        if (mem64_pt(mem, page) != NULL) {
            hole_end = 0;
            continue;
        }
        if (hole_end == 0)
            hole_end = page + 1;
        if (hole_end - page >= size)
            return hole_end - size;
    }
    return A64_BAD_PAGE;
}

// Mapping ********************************************************************

int mem64_map(struct mem64 *mem, page64_t start, pages64_t pages, void *memory, size_t offset, unsigned flags) {
    if (memory == MAP_FAILED)
        return errno_map();
    if (pages == 0)
        return 0;
    if (start >= A64_MAX_PAGES || start + pages > A64_MAX_PAGES)
        return _EINVAL;
    // A mapping has to start on a host page boundary or the translation cannot
    // be expressed with one pointer per page (host pages are 16 KiB on Apple
    // silicon while guest pages are 4 KiB).
    assert((uintptr_t) memory % real_page_size == 0);

    struct data *data = malloc(sizeof(struct data));
    if (data == NULL)
        return _ENOMEM;
    *data = (struct data) {
        .data = memory,
        .size = pages * A64_PAGE_SIZE + offset,
    };

    for (page64_t page = start; page < start + pages; page++) {
        if (mem64_pt(mem, page) != NULL)
            mem64_unmap(mem, page, 1);
        data->refcount++;
        struct pt_entry *pt = a64_pt_entry(mem, page, true);
        if (pt == NULL) {
            if (--data->refcount == 0)
                free(data);
            return _ENOMEM;
        }
        pt->data = data;
        pt->offset = ((page - start) << A64_PAGE_BITS) + offset;
        pt->flags = flags;
    }
    mem64_changed(mem);
    return 0;
}

int mem64_map_nothing(struct mem64 *mem, page64_t start, pages64_t pages, unsigned flags) {
    if (pages == 0)
        return 0;
    void *memory = mmap(NULL, pages * A64_PAGE_SIZE,
            PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);
    return mem64_map(mem, start, pages, memory, 0, flags | P_ANONYMOUS);
}

int mem64_unmap(struct mem64 *mem, page64_t start, pages64_t pages) {
    for (page64_t page = start; page < start + pages; page++)
        if (mem64_pt(mem, page) == NULL)
            return -1;
    return mem64_unmap_always(mem, start, pages);
}

int mem64_unmap_always(struct mem64 *mem, page64_t start, pages64_t pages) {
    for (page64_t page = start; page < start + pages && page < A64_MAX_PAGES; mem64_next_page(mem, &page)) {
        struct pt_entry *pt = mem64_pt(mem, page);
        if (pt == NULL)
            continue;
        a64_code_cache_invalidate_page(mem->mmu.cache, page);
        struct data *data = pt->data;
        a64_pt_del(mem, page);
        if (--data->refcount == 0) {
            int err = munmap(data->data, data->size);
            if (err != 0)
                die("munmap(%p, %lu) failed: %s", data->data, data->size, strerror(errno));
            if (data->fd != NULL)
                fd_close(data->fd);
            free(data);
        }
    }
    mem64_changed(mem);
    return 0;
}

int mem64_set_flags(struct mem64 *mem, page64_t start, pages64_t pages, int flags) {
    for (page64_t page = start; page < start + pages; page++)
        if (mem64_pt(mem, page) == NULL)
            return _ENOMEM;
    for (page64_t page = start; page < start + pages; page++) {
        struct pt_entry *entry = mem64_pt(mem, page);
        int old_flags = entry->flags;
        entry->flags = flags;
        // Only bother the host when the mapping becomes more permissive.
        if ((flags & ~old_flags) & (P_READ | P_WRITE)) {
            void *data = (char *) entry->data->data + entry->offset;
            // Protection is per host page and several guest pages can share
            // one, so round down.
            data = (void *) ((uintptr_t) data & ~(real_page_size - 1));
            int prot = PROT_READ;
            if (flags & P_WRITE)
                prot |= PROT_WRITE;
            if (mprotect(data, real_page_size, prot) < 0)
                return errno_map();
        }
    }
    mem64_changed(mem);
    return 0;
}

int mem64_copy_on_write(struct mem64 *src, struct mem64 *dst, page64_t start, pages64_t pages) {
    for (page64_t page = start; page < start + pages && page < A64_MAX_PAGES; mem64_next_page(src, &page)) {
        struct pt_entry *entry = mem64_pt(src, page);
        if (entry == NULL)
            continue;
        if (mem64_unmap_always(dst, page, 1) < 0)
            return -1;
        if (!(entry->flags & P_SHARED))
            entry->flags |= P_COW;
        entry->data->refcount++;
        struct pt_entry *dst_entry = a64_pt_entry(dst, page, true);
        if (dst_entry == NULL)
            return _ENOMEM;
        dst_entry->data = entry->data;
        dst_entry->offset = entry->offset;
        dst_entry->flags = entry->flags;
    }
    mem64_changed(src);
    mem64_changed(dst);
    return 0;
}

// Access *********************************************************************

// Like kernel/memory.c's mem_ptr_nofault: never maps anything, so the
// interpreter can call it without risking a fault inside a fault.
static void *mem64_ptr_nofault(struct mem64 *mem, uaddr_t addr, int type) {
    struct pt_entry *entry = mem64_pt(mem, A64_PAGE(addr));
    if (entry == NULL)
        return NULL;
    if (type == MEM_WRITE && !P_WRITABLE(entry->flags))
        return NULL;
    return (char *) entry->data->data + entry->offset + A64_PGOFFSET(addr);
}

void *mem64_ptr(struct mem64 *mem, uaddr_t addr, int type) {
    if (addr >= A64_VA_TOP)
        return NULL;
    void *old_ptr = mem64_ptr_nofault(mem, addr, type); // only for the assert

    page64_t page = A64_PAGE(addr);
    struct pt_entry *entry = mem64_pt(mem, page);

    if (entry == NULL) {
        // Nothing mapped here. If the next mapped page above us belongs to a
        // region that is willing to grow downwards, this is the guest's stack
        // growing.
        //
        // Finding that page has to go through the page table: stepping through
        // page numbers would cost as much as walking the entire 48-bit address
        // space (see mem64_next_page).
        page64_t p = mem64_next_mapped_page(mem, page);
        if (p >= A64_MAX_PAGES)
            return NULL;
        if (!(mem64_pt(mem, p)->flags & P_GROWSDOWN))
            return NULL;

        // Changing the page table needs the write lock, but this is called
        // with the read lock held (same dance as kernel/memory.c; TODO: factor
        // this into a helper in both engines).
        read_wrunlock(&mem->lock);
        write_wrlock(&mem->lock);
        mem64_map_nothing(mem, page, 1, P_WRITE | P_GROWSDOWN);
        write_wrunlock(&mem->lock);
        read_wrlock(&mem->lock);

        entry = mem64_pt(mem, page);
    }

    if (entry != NULL && (type == MEM_WRITE || type == MEM_WRITE_PTRACE)) {
        if (type != MEM_WRITE_PTRACE && !(entry->flags & P_WRITE))
            return NULL;
        if (type == MEM_WRITE_PTRACE) {
            // Pretend the page is writable: writing it through ptrace must not
            // be observable by the guest.
            entry->flags |= P_WRITE | P_COW;
        }
        // Whatever was compiled from this page is stale now.
        a64_code_cache_invalidate_page(mem->mmu.cache, page);
        if (entry->flags & P_COW) {
            void *data = (char *) entry->data->data + entry->offset;
            void *copy = mmap(NULL, A64_PAGE_SIZE, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);

            read_wrunlock(&mem->lock);
            write_wrlock(&mem->lock);
            memcpy(copy, data, A64_PAGE_SIZE);
            mem64_map(mem, page, 1, copy, 0, entry->flags & ~P_COW);
            write_wrunlock(&mem->lock);
            read_wrlock(&mem->lock);
        }
    }

    void *ptr = mem64_ptr_nofault(mem, addr, type);
    assert(old_ptr == NULL || old_ptr == ptr || type == MEM_WRITE_PTRACE);
    return ptr;
}

// Lifetime *******************************************************************

void mem64_init(struct mem64 *mem) {
    mem->root = NULL;
    mem->nodes_used = 0;
    mem->mmu.cache = NULL;
    mem->mmu.changes = 0;
    wrlock_init(&mem->lock);
}

void mem64_destroy(struct mem64 *mem) {
    write_wrlock(&mem->lock);
    mem64_unmap_always(mem, 0, A64_MAX_PAGES);
    a64_pt_free_subtree(mem->root, 0);
    mem->root = NULL;
    write_wrunlock(&mem->lock);
    wrlock_destroy(&mem->lock);
}


