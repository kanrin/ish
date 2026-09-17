#ifndef KERNEL_NATIVE_H
#define KERNEL_NATIVE_H

#include "misc.h"
#include "util/list.h"
#include "util/sync.h"

struct task;

// The address space of a native (in-process) task.
//
// A native task's memory is ordinary host memory, not the guest shadow address
// space of kernel/memory.c, so a "user address" in a native task is a real
// host pointer. Because that memory is shared with the rest of the app, every
// dereference of a native pointer has to be validated against the list of
// regions the task actually mapped: a bogus pointer must turn into the same
// EFAULT a guest would get, not into a crash of the whole app.
//
// This is the skeleton: the region bookkeeping lands with native-mm (M5).
struct native_mm {
    struct list regions; // struct native_region
    lock_t lock;
};

// True iff [addr, addr+count) lies entirely inside a mapped, readable region
// of the task's native address space.
bool native_mm_contains(struct task *task, uaddr_t addr, size_t count);

#endif
