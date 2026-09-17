#ifndef CALLS_H
#define CALLS_H

#include "kernel/task.h"
#include "kernel/errno.h"
#include "fs/fd.h"
#include "fs/dev.h"
#include "kernel/fs.h"
#include "misc.h"

#include "kernel/signal.h"
#include "fs/sock.h"
#include "kernel/time.h"
#include "kernel/resource.h"
#include "kernel/ptrace.h"

void handle_interrupt(int interrupt);

// Entry point for system calls that does not depend on struct cpu_state.
// See the definition in kernel/calls.c for the rationale.
//
// Returns the raw syscall result. Interpretation depends on the caller: a
// 32-bit guest only uses the low 32 bits (that is what handle_interrupt()
// stores into eax), while a native caller casts it to whatever type its own
// ABI gives that syscall. Syscalls that hand out user addresses return
// uaddr_t so that native callers see all 64 bits; everything else is
// effectively 32 bits wide and may have unspecified high bits.
uint64_t do_syscall(unsigned syscall_num, uint64_t arg1, uint64_t arg2, uint64_t arg3,
        uint64_t arg4, uint64_t arg5, uint64_t arg6);

int must_check user_read(uaddr_t addr, void *buf, size_t count);
int must_check user_write(uaddr_t addr, const void *buf, size_t count);
int must_check user_read_task(struct task *task, uaddr_t addr, void *buf, size_t count);
int must_check user_write_task(struct task *task, uaddr_t addr, const void *buf, size_t count);
int must_check user_write_task_ptrace(struct task *task, uaddr_t addr, const void *buf, size_t count);
int must_check user_read_string(uaddr_t addr, char *buf, size_t max);
int must_check user_write_string(uaddr_t addr, const char *buf);
#define user_get(addr, var) user_read(addr, &(var), sizeof(var))
#define user_put(addr, var) user_write(addr, &(var), sizeof(var))
#define user_get_task(task, addr, var) user_read_task(task, addr, &(var), sizeof(var))
#define user_put_task(task, addr, var) user_write_task(task, addr, &(var), sizeof(var))

// process lifecycle
dword_t sys_clone(dword_t flags, uaddr_t stack, uaddr_t ptid, uaddr_t tls, uaddr_t ctid);
dword_t sys_fork(void);
dword_t sys_vfork(void);
dword_t sys_execve(uaddr_t file, uaddr_t argv, uaddr_t envp);
int do_execve(const char *file, size_t argc, const char *argv, const char *envp);
dword_t sys_exit(dword_t status);
noreturn void do_exit(int status);
noreturn void do_exit_group(int status);
dword_t sys_exit_group(dword_t status);
dword_t sys_wait4(pid_t_ pid, uaddr_t status_addr, dword_t options, uaddr_t rusage_addr);
dword_t sys_waitid(int_t idtype, pid_t_ id, uaddr_t info_addr, int_t options);
dword_t sys_waitpid(pid_t_ pid, uaddr_t status_addr, dword_t options);

// memory management
uaddr_t sys_brk(uaddr_t new_brk);

#define MMAP_SHARED 0x1
#define MMAP_PRIVATE 0x2
#define MMAP_FIXED 0x10
#define MMAP_ANONYMOUS 0x20
uaddr_t sys_mmap(uaddr_t args_addr);
uaddr_t sys_mmap2(uaddr_t addr, dword_t len, dword_t prot, dword_t flags, fd_t fd_no, dword_t offset);
int_t sys_munmap(uaddr_t addr, uint_t len);
int_t sys_mprotect(uaddr_t addr, uint_t len, int_t prot);
int_t sys_mremap(uaddr_t addr, dword_t old_len, dword_t new_len, dword_t flags);
dword_t sys_madvise(uaddr_t addr, dword_t len, dword_t advice);
dword_t sys_mbind(uaddr_t addr, dword_t len, int_t mode, uaddr_t nodemask, dword_t maxnode, uint_t flags);
int_t sys_mlock(uaddr_t addr, dword_t len);
int_t sys_msync(uaddr_t addr, dword_t len, int_t flags);

// file descriptor things
#define LOCK_SH_ 1
#define LOCK_EX_ 2
#define LOCK_NB_ 4
#define LOCK_UN_ 8
struct iovec_ {
    addr_t base;
    uint_t len;
};
dword_t sys_read(fd_t fd_no, uaddr_t buf_addr, dword_t size);
dword_t sys_readv(fd_t fd_no, uaddr_t iovec_addr, dword_t iovec_count);
dword_t sys_write(fd_t fd_no, uaddr_t buf_addr, dword_t size);
dword_t sys_writev(fd_t fd_no, uaddr_t iovec_addr, dword_t iovec_count);
dword_t sys__llseek(fd_t f, dword_t off_high, dword_t off_low, uaddr_t res_addr, dword_t whence);
dword_t sys_lseek(fd_t f, dword_t off, dword_t whence);
dword_t sys_pread(fd_t f, uaddr_t buf_addr, dword_t buf_size, off_t_ off);
dword_t sys_pwrite(fd_t f, uaddr_t buf_addr, dword_t size, off_t_ off);
dword_t sys_ioctl(fd_t f, dword_t cmd, dword_t arg);
dword_t sys_fcntl(fd_t f, dword_t cmd, dword_t arg);
dword_t sys_fcntl32(fd_t fd, dword_t cmd, dword_t arg);
dword_t sys_dup(fd_t fd);
dword_t sys_dup2(fd_t fd, fd_t new_fd);
dword_t sys_dup3(fd_t f, fd_t new_f, int_t flags);
dword_t sys_close(fd_t fd);
dword_t sys_fsync(fd_t f);
dword_t sys_flock(fd_t fd, dword_t operation);
int_t sys_pipe(uaddr_t pipe_addr);
int_t sys_pipe2(uaddr_t pipe_addr, int_t flags);
struct pollfd_ {
    fd_t fd;
    word_t events;
    word_t revents;
};
dword_t sys_poll(uaddr_t fds, dword_t nfds, int_t timeout);
dword_t sys_select(fd_t nfds, uaddr_t readfds_addr, uaddr_t writefds_addr, uaddr_t exceptfds_addr, uaddr_t timeout_addr);
dword_t sys_pselect(fd_t nfds, uaddr_t readfds_addr, uaddr_t writefds_addr, uaddr_t exceptfds_addr, uaddr_t timeout_addr, uaddr_t sigmask_addr);
dword_t sys_ppoll(uaddr_t fds, dword_t nfds, uaddr_t timeout_addr, uaddr_t sigmask_addr, dword_t sigsetsize);
fd_t sys_epoll_create(int_t flags);
fd_t sys_epoll_create0(void);
int_t sys_epoll_ctl(fd_t epoll, int_t op, fd_t fd, uaddr_t event_addr);
int_t sys_epoll_wait(fd_t epoll, uaddr_t events_addr, int_t max_events, int_t timeout);
int_t sys_epoll_pwait(fd_t epoll_f, uaddr_t events_addr, int_t max_events, int_t timeout, uaddr_t sigmask_addr, dword_t sigsetsize);

int_t sys_eventfd2(uint_t initval, int_t flags);
int_t sys_eventfd(uint_t initval);

// file management
fd_t sys_open(uaddr_t path_addr, dword_t flags, mode_t_ mode);
fd_t sys_openat(fd_t at, uaddr_t path_addr, dword_t flags, mode_t_ mode);
dword_t sys_close(fd_t fd);
dword_t sys_link(uaddr_t src_addr, uaddr_t dst_addr);
dword_t sys_linkat(fd_t src_at_f, uaddr_t src_addr, fd_t dst_at_f, uaddr_t dst_addr);
dword_t sys_unlink(uaddr_t path_addr);
dword_t sys_unlinkat(fd_t at_f, uaddr_t path_addr, int_t flags);
dword_t sys_rmdir(uaddr_t path_addr);
dword_t sys_rename(uaddr_t src_addr, uaddr_t dst_addr);
dword_t sys_renameat(fd_t src_at_f, uaddr_t src_addr, fd_t dst_at_f, uaddr_t dst_addr);
dword_t sys_renameat2(fd_t src_at_f, uaddr_t src_addr, fd_t dst_at_f, uaddr_t dst_addr, int_t flags);
dword_t sys_symlink(uaddr_t target_addr, uaddr_t link_addr);
dword_t sys_symlinkat(uaddr_t target_addr, fd_t at_f, uaddr_t link_addr);
dword_t sys_mknod(uaddr_t path_addr, mode_t_ mode, dev_t_ dev);
dword_t sys_mknodat(fd_t at_f, uaddr_t path_addr, mode_t_ mode, dev_t_ dev);
dword_t sys_access(uaddr_t path_addr, dword_t mode);
dword_t sys_faccessat(fd_t at_f, uaddr_t path, mode_t_ mode, dword_t flags);
dword_t sys_readlink(uaddr_t path, uaddr_t buf, dword_t bufsize);
dword_t sys_readlinkat(fd_t at_f, uaddr_t path, uaddr_t buf, dword_t bufsize);
int_t sys_getdents(fd_t f, uaddr_t dirents, dword_t count);
int_t sys_getdents64(fd_t f, uaddr_t dirents, dword_t count);
dword_t sys_stat64(uaddr_t path_addr, uaddr_t statbuf_addr);
dword_t sys_lstat64(uaddr_t path_addr, uaddr_t statbuf_addr);
dword_t sys_fstat64(fd_t fd_no, uaddr_t statbuf_addr);
dword_t sys_fstatat64(fd_t at, uaddr_t path_addr, uaddr_t statbuf_addr, dword_t flags);
dword_t sys_fchmod(fd_t f, dword_t mode);
dword_t sys_fchmodat(fd_t at_f, uaddr_t path_addr, dword_t mode);
dword_t sys_chmod(uaddr_t path_addr, dword_t mode);
dword_t sys_fchown32(fd_t f, dword_t owner, dword_t group);
dword_t sys_fchownat(fd_t at_f, uaddr_t path_addr, dword_t owner, dword_t group, int flags);
dword_t sys_chown32(uaddr_t path_addr, uid_t_ owner, uid_t_ group);
dword_t sys_lchown(uaddr_t path_addr, uid_t_ owner, uid_t_ group);
dword_t sys_truncate64(uaddr_t path_addr, dword_t size_low, dword_t size_high);
dword_t sys_ftruncate64(fd_t f, dword_t size_low, dword_t size_high);
dword_t sys_fallocate(fd_t f, dword_t mode, dword_t offset_low, dword_t offset_high, dword_t len_low, dword_t len_high);
dword_t sys_mkdir(uaddr_t path_addr, mode_t_ mode);
dword_t sys_mkdirat(fd_t at_f, uaddr_t path_addr, mode_t_ mode);
dword_t sys_utimensat(fd_t at_f, uaddr_t path_addr, uaddr_t times_addr, dword_t flags);
dword_t sys_utimes(uaddr_t path_addr, uaddr_t times_addr);
dword_t sys_utime(uaddr_t path_addr, uaddr_t times_addr);
dword_t sys_times( uaddr_t tbuf);
dword_t sys_umask(dword_t mask);

dword_t sys_sendfile(fd_t out_fd, fd_t in_fd, uaddr_t offset_addr, dword_t count);
dword_t sys_sendfile64(fd_t out_fd, fd_t in_fd, uaddr_t offset_addr, dword_t count);
dword_t sys_splice(fd_t in_fd, uaddr_t in_off_addr, fd_t out_fd, uaddr_t out_off_addr, dword_t count, dword_t flags);
dword_t sys_copy_file_range(fd_t in_fd, uaddr_t in_off, fd_t out_fd, uaddr_t out_off, dword_t len, uint_t flags);

dword_t sys_statfs(uaddr_t path_addr, uaddr_t buf_addr);
dword_t sys_statfs64(uaddr_t path_addr, dword_t buf_size, uaddr_t buf_addr);
dword_t sys_fstatfs(fd_t f, uaddr_t buf_addr);
dword_t sys_fstatfs64(fd_t f, uaddr_t buf_addr);
dword_t sys_statx(fd_t at_f, uaddr_t path_addr, int_t flags, uint_t mask, uaddr_t statx_addr);

#define MS_READONLY_ (1 << 0)
#define MS_NOSUID_ (1 << 1)
#define MS_NODEV_ (1 << 2)
#define MS_NOEXEC_ (1 << 3)
#define MS_SILENT_ (1 << 15)
dword_t sys_mount(uaddr_t source_addr, uaddr_t target_addr, uaddr_t type_addr, dword_t flags, uaddr_t data_addr);
dword_t sys_umount2(uaddr_t target_addr, dword_t flags);

dword_t sys_xattr_stub(uaddr_t path_addr, uaddr_t name_addr, uaddr_t value_addr, dword_t size, dword_t flags);

// process information
pid_t_ sys_getpid(void);
pid_t_ sys_gettid(void);
pid_t_ sys_getppid(void);
pid_t_ sys_getpgid(pid_t_ pid);
dword_t sys_setpgid(pid_t_ pid, pid_t_ pgid);
pid_t_ sys_getpgrp(void);
dword_t sys_setpgrp(void);
uid_t_ sys_getuid32(void);
uid_t_ sys_getuid(void);
int_t sys_setuid(uid_t uid);
uid_t_ sys_geteuid32(void);
uid_t_ sys_geteuid(void);
int_t sys_setgid(uid_t gid);
uid_t_ sys_getgid32(void);
uid_t_ sys_getgid(void);
uid_t_ sys_getegid32(void);
uid_t_ sys_getegid(void);
dword_t sys_setresuid(uid_t_ ruid, uid_t_ euid, uid_t_ suid);
dword_t sys_setresgid(uid_t_ rgid, uid_t_ egid, uid_t_ sgid);
int_t sys_setreuid(uid_t_ ruid, uid_t_ euid);
int_t sys_setregid(uid_t_ rgid, uid_t_ egid);
int_t sys_getresuid(uaddr_t ruid_addr, uaddr_t euid_addr, uaddr_t suid_addr);
int_t sys_getresgid(uaddr_t rgid_addr, uaddr_t egid_addr, uaddr_t sgid_addr);
int_t sys_getgroups(dword_t size, uaddr_t list);
int_t sys_setgroups(dword_t size, uaddr_t list);
int_t sys_capget(uaddr_t header_addr, uaddr_t data_addr);
int_t sys_capset(uaddr_t header_addr, uaddr_t data_addr);
dword_t sys_getcwd(uaddr_t buf_addr, dword_t size);
dword_t sys_chdir(uaddr_t path_addr);
dword_t sys_chroot(uaddr_t path_addr);
dword_t sys_fchdir(fd_t f);
int_t sys_personality(dword_t pers);
int task_set_thread_area(struct task *task, uaddr_t u_info);
int sys_set_thread_area(uaddr_t u_info);
int sys_set_tid_address(uaddr_t blahblahblah);
dword_t sys_setsid(void);
dword_t sys_getsid(void);

int_t sys_sched_yield(void);
int_t sys_prctl(dword_t option, uint_t arg2, uint_t arg3, uint_t arg4, uint_t arg5);
int_t sys_arch_prctl(int_t code, uaddr_t addr);
int_t sys_reboot(int_t magic, int_t magic2, int_t cmd);

// system information
#define UNAME_LENGTH 65
struct uname {
    char system[UNAME_LENGTH];   // Linux
    char hostname[UNAME_LENGTH]; // my-compotar
    char release[UNAME_LENGTH];  // 1.2.3-ish
    char version[UNAME_LENGTH];  // SUPER AWESOME
    char arch[UNAME_LENGTH];     // i686
    char domain[UNAME_LENGTH];   // lol
};
void do_uname(struct uname *uts);
dword_t sys_uname(uaddr_t uts_addr);
dword_t sys_sethostname(uaddr_t hostname_addr, dword_t hostname_len);

struct sys_info {
    dword_t uptime;
    dword_t loads[3];
    dword_t totalram;
    dword_t freeram;
    dword_t sharedram;
    dword_t bufferram;
    dword_t totalswap;
    dword_t freeswap;
    word_t procs;
    dword_t totalhigh;
    dword_t freehigh;
    dword_t mem_unit;
    char pad;
};
dword_t sys_sysinfo(uaddr_t info_addr);

// futexes
dword_t sys_futex(uaddr_t uaddr, dword_t op, dword_t val, uaddr_t timeout_or_val2, uaddr_t uaddr2, dword_t val3);
int_t sys_set_robust_list(uaddr_t robust_list, dword_t len);
int_t sys_get_robust_list(pid_t_ pid, uaddr_t robust_list_ptr, uaddr_t len_ptr);

// misc
dword_t sys_getrandom(uaddr_t buf_addr, dword_t len, dword_t flags);
int_t sys_syslog(int_t type, uaddr_t buf_addr, int_t len);
int_t sys_ipc(uint_t call, int_t first, int_t second, int_t third, uaddr_t ptr, int_t fifth);

// The 400-odd implementations above have their own, per-ABI signatures and are
// called through this generic one. Every platform iSH targets passes integer
// arguments in registers that are at least 64 bits wide, so a callee that
// declares a 32-bit parameter simply ignores the upper bits, and a callee that
// declares a uaddr_t parameter receives the whole host pointer. Widening here
// is what lets a native caller and a 32-bit guest share one table.
typedef uint64_t (*syscall_t)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

#endif
