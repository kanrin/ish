#include <string.h>
#include "kernel/calls.h"
#include "kernel/native.h"

// The two kinds of memory a task can have.
//
// A native task's memory is ordinary host memory (kernel/native.h), so a user
// address is a host pointer and an access is a bounds-checked memcpy -- the
// bounds check matters because native code shares the process with the rest of
// the app, so an unchecked bogus pointer would take the whole app down.
//
// A guest task's memory lives in the shadow address space of kernel/memory.c,
// so its addresses are 32 bits wide and have to be translated page by page.
static int __user_read_task(struct task *task, uaddr_t addr, void *buf, size_t count) {
    if (task->native) {
        if (!native_mm_contains(task, addr, count))
            return 1;
        memcpy(buf, (const void *) (uintptr_t) addr, count);
        return 0;
    }
    // A guest address is 32 bits wide, so a wider value cannot have come out
    // of a guest register: it is a bad address.
    if (addr > UINT32_MAX)
        return 1;
    addr_t gaddr = addr;
    char *cbuf = (char *) buf;
    addr_t p = gaddr;
    while (p < gaddr + count) {
        addr_t chunk_end = (PAGE(p) + 1) << PAGE_BITS;
        if (chunk_end > gaddr + count)
            chunk_end = gaddr + count;
        const char *ptr = mem_ptr(task->mem, p, MEM_READ);
        if (ptr == NULL)
            return 1;
        memcpy(&cbuf[p - gaddr], ptr, chunk_end - p);
        p = chunk_end;
    }
    return 0;
}

static int __user_write_task(struct task *task, uaddr_t addr, const void *buf, size_t count, bool ptrace) {
    if (task->native) {
        if (!native_mm_contains(task, addr, count))
            return 1;
        memcpy((void *) (uintptr_t) addr, buf, count);
        return 0;
    }
    if (addr > UINT32_MAX)
        return 1;
    addr_t gaddr = addr;
    const char *cbuf = (const char *) buf;
    addr_t p = gaddr;
    while (p < gaddr + count) {
        addr_t chunk_end = (PAGE(p) + 1) << PAGE_BITS;
        if (chunk_end > gaddr + count)
            chunk_end = gaddr + count;
        char *ptr = mem_ptr(task->mem, p, ptrace ? MEM_WRITE_PTRACE : MEM_WRITE);
        if (ptr == NULL)
            return 1;
        memcpy(ptr, &cbuf[p - gaddr], chunk_end - p);
        p = chunk_end;
    }
    return 0;
}

int user_read_task(struct task *task, uaddr_t addr, void *buf, size_t count) {
    read_wrlock(&task->mem->lock);
    int res = __user_read_task(task, addr, buf, count);
    read_wrunlock(&task->mem->lock);
    return res;
}

int user_read(uaddr_t addr, void *buf, size_t count) {
    return user_read_task(current, addr, buf, count);
}

int user_write_task(struct task *task, uaddr_t addr, const void *buf, size_t count) {
    read_wrlock(&task->mem->lock);
    int res = __user_write_task(task, addr, buf, count, false);
    read_wrunlock(&task->mem->lock);
    return res;
}

int user_write_task_ptrace(struct task *task, uaddr_t addr, const void *buf, size_t count) {
    read_wrlock(&task->mem->lock);
    int res = __user_write_task(task, addr, buf, count, true);
    read_wrunlock(&task->mem->lock);
    return res;
}

int user_write(uaddr_t addr, const void *buf, size_t count) {
    return user_write_task(current, addr, buf, count);
}

int user_read_string(uaddr_t addr, char *buf, size_t max) {
    if (addr == 0)
        return 1;
    read_wrlock(&current->mem->lock);
    size_t i = 0;
    while (i < max) {
        if (__user_read_task(current, addr + i, &buf[i], sizeof(buf[i]))) {
            read_wrunlock(&current->mem->lock);
            return 1;
        }
        if (buf[i] == '\0')
            break;
        i++;
    }
    read_wrunlock(&current->mem->lock);
    return 0;
}

int user_write_string(uaddr_t addr, const char *buf) {
    if (addr == 0)
        return 1;
    read_wrlock(&current->mem->lock);
    size_t i = 0;
    do {
        if (__user_write_task(current, addr + i, &buf[i], sizeof(buf[i]), false)) {
            read_wrunlock(&current->mem->lock);
            return 1;
        }
        i++;
    } while (buf[i - 1] != '\0');
    read_wrunlock(&current->mem->lock);
    return 0;
}
