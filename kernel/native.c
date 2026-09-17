#include "kernel/native.h"
#include "kernel/task.h"

// See kernel/native.h for why native pointers must be validated rather than
// trusted. This fails closed until the region bookkeeping exists, which is
// also why no task is marked native yet (see native-mm, M5).
bool native_mm_contains(struct task *UNUSED(task), uaddr_t UNUSED(addr), size_t UNUSED(count)) {
    // TODO(native-mm): walk task->native_mm->regions and check that
    // [addr, addr+count) is covered by one of them.
    return false;
}
