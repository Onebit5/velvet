// SPDX-License-Identifier: GPL-2.0-only
/*
 * arch/x86_64/syscall.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the syscall dispatcher, and the calls that are the machine's own.
 */

#include <asm/syscall.h>
#include <asm/smp.h>
#include <asm/msr.h>
#include <asm/fpu.h>
#include <asm/gdt.h>
#include <asm/uaccess.h>
#include <velvet/syscalls.h>
#include <velvet/errno.h>
#include <velvet/addrspace.h>
#include <velvet/file.h>
#include <velvet/kmalloc.h>
#include <velvet/kprintf.h>
#include <velvet/namei.h>
#include <velvet/pit.h>
#include <velvet/process.h>
#include <velvet/sched.h>
#include <velvet/signal.h>
#include <velvet/string.h>
#include <velvet/thread.h>
#include <velvet/usermode.h>
#include <velvet/vmm.h>
#include <uapi/velvet/fcntl.h>
#include <uapi/velvet/prctl.h>
#include <uapi/velvet/signal.h>
#include <uapi/velvet/unistd.h>
#include <uapi/asm/sigcontext.h>

#define MSR_STAR   0xc0000081
#define MSR_LSTAR  0xc0000082
#define MSR_SFMASK 0xc0000084

#define EFER_SCE (1ull << 0)    /* without this, `syscall` is #UD */

#define RFLAGS_IF (1ull << 9)
#define RFLAGS_DF (1ull << 10)
#define RFLAGS_TF (1ull << 8)
#define RFLAGS_RF (1ull << 16)

/*
 * the flags a program may hand back through sigreturn: the arithmetic
 * ones, direction, trap, resume and alignment check. iopl and the rest
 * stay the kernel's
 */
#define RFLAGS_USER 0x50dd5ull

/* how long the `syscall` instruction is, which a restart steps back over */
#define SYSCALL_INSN_LEN 2

/* implemented in syscall.asm */
extern void syscall_entry(void);

/*
 * the table. each entry unpacks the registers into the portable call's
 * own arguments, in the order the abi puts them: rdi, rsi, rdx, r10, r8
 * and r9
 */
typedef int64_t (*syscall_fn)(struct pt_regs *p_regs);

#define A0 p_regs->rdi
#define A1 p_regs->rsi
#define A2 p_regs->rdx
#define A3 p_regs->r10
#define A4 p_regs->r8
#define A5 p_regs->r9

#define CALL0(name)                                                     \
    static int64_t do_##name(struct pt_regs *p_regs)                    \
    {                                                                   \
        (void)p_regs;                                                   \
        return sys_##name();                                            \
    }
#define CALL1(name, t0)                                                 \
    static int64_t do_##name(struct pt_regs *p_regs)                    \
    {                                                                   \
        return sys_##name((t0)A0);                                      \
    }
#define CALL2(name, t0, t1)                                             \
    static int64_t do_##name(struct pt_regs *p_regs)                    \
    {                                                                   \
        return sys_##name((t0)A0, (t1)A1);                              \
    }
#define CALL3(name, t0, t1, t2)                                         \
    static int64_t do_##name(struct pt_regs *p_regs)                    \
    {                                                                   \
        return sys_##name((t0)A0, (t1)A1, (t2)A2);                      \
    }
#define CALL4(name, t0, t1, t2, t3)                                     \
    static int64_t do_##name(struct pt_regs *p_regs)                    \
    {                                                                   \
        return sys_##name((t0)A0, (t1)A1, (t2)A2, (t3)A3);              \
    }
#define CALL5(name, t0, t1, t2, t3, t4)                                 \
    static int64_t do_##name(struct pt_regs *p_regs)                    \
    {                                                                   \
        return sys_##name((t0)A0, (t1)A1, (t2)A2, (t3)A3, (t4)A4);      \
    }
#define CALL6(name, t0, t1, t2, t3, t4, t5)                             \
    static int64_t do_##name(struct pt_regs *p_regs)                    \
    {                                                                   \
        return sys_##name((t0)A0, (t1)A1, (t2)A2, (t3)A3, (t4)A4,       \
                          (t5)A5);                                      \
    }

typedef uint64_t u64;
typedef int64_t i64;
typedef uint32_t u32;

CALL3(read, int, u64, u64)
CALL3(write, int, u64, u64)
CALL1(close, int)
CALL2(fstat, int, u64)
CALL3(poll, u64, u64, int)
CALL3(lseek, int, i64, int)
CALL6(mmap, u64, u64, int, int, int, i64)
CALL3(mprotect, u64, u64, int)
CALL2(munmap, u64, u64)
CALL1(brk, u64)
CALL4(rt_sigaction, int, u64, u64, u64)
CALL4(rt_sigprocmask, int, u64, u64, u64)
CALL3(ioctl, int, u32, u64)
CALL4(pread64, int, u64, u64, i64)
CALL4(pwrite64, int, u64, u64, i64)
CALL3(readv, int, u64, int)
CALL3(writev, int, u64, int)
CALL5(select, int, u64, u64, u64, u64)
CALL0(sched_yield)
CALL5(mremap, u64, u64, u64, int, u64)
CALL3(madvise, u64, u64, int)
CALL1(dup, int)
CALL2(dup2, int, int)
CALL0(pause)
CALL2(nanosleep, u64, u64)
CALL2(getitimer, int, u64)
CALL1(alarm, u32)
CALL3(setitimer, int, u64, u64)
CALL0(getpid)
CALL3(socket, int, int, int)
CALL3(connect, int, u64, u32)
CALL6(sendto, int, u64, u64, int, u64, u32)
CALL6(recvfrom, int, u64, u64, int, u64, u64)
CALL3(sendmsg, int, u64, int)
CALL3(recvmsg, int, u64, int)
CALL2(shutdown, int, int)
CALL3(bind, int, u64, u32)
CALL2(listen, int, int)
CALL3(getsockname, int, u64, u64)
CALL3(getpeername, int, u64, u64)
CALL5(setsockopt, int, int, int, u64, u32)
CALL5(getsockopt, int, int, int, u64, u64)
CALL1(exit, int)
CALL4(wait4, int, u64, int, u64)
CALL2(kill, int, int)
CALL1(uname, u64)
CALL3(fcntl, int, int, u64)
CALL1(fsync, int)
CALL2(truncate, u64, i64)
CALL2(ftruncate, int, i64)
CALL2(getcwd, u64, u64)
CALL1(chdir, u64)
CALL1(fchdir, int)
CALL2(fchmod, int, u32)
CALL3(fchown, int, u32, u32)
CALL1(umask, u32)
CALL2(gettimeofday, u64, u64)
CALL2(getrlimit, int, u64)
CALL2(getrusage, int, u64)
CALL1(sysinfo, u64)
CALL1(times, u64)
CALL0(getuid)
CALL3(syslog, int, u64, int)
CALL0(getgid)
CALL1(setuid, u32)
CALL1(setgid, u32)
CALL2(setpgid, int, int)
CALL0(getppid)
CALL0(getpgrp)
CALL0(setsid)
CALL2(setreuid, u32, u32)
CALL2(setregid, u32, u32)
CALL2(getgroups, int, u64)
CALL2(setgroups, u64, u64)
CALL3(setresuid, u32, u32, u32)
CALL3(getresuid, u64, u64, u64)
CALL3(setresgid, u32, u32, u32)
CALL3(getresgid, u64, u64, u64)
CALL1(getpgid, int)
CALL1(getsid, int)
CALL2(rt_sigpending, u64, u64)
CALL4(rt_sigtimedwait, u64, u64, u64, u64)
CALL2(rt_sigsuspend, u64, u64)
CALL5(prctl, int, u64, u64, u64, u64)
CALL2(setrlimit, int, u64)
CALL0(sync)
CALL2(settimeofday, u64, u64)
CALL4(reboot, u32, u32, u32, u64)
CALL2(sethostname, u64, u64)
CALL2(setdomainname, u64, u64)
CALL0(gettid)
CALL2(tkill, int, int)
CALL1(time, u64)
CALL3(getdents64, int, u64, u64)
CALL1(set_tid_address, u64)
CALL2(clock_settime, int, u64)
CALL2(clock_gettime, int, u64)
CALL2(clock_getres, int, u64)
CALL4(clock_nanosleep, int, int, u64, u64)
CALL1(exit_group, int)
CALL3(tgkill, int, int, int)
CALL4(openat, int, u64, int, u32)
CALL3(mkdirat, int, u64, u32)
CALL5(fchownat, int, u64, u32, u32, int)
CALL4(newfstatat, int, u64, u64, int)
CALL3(unlinkat, int, u64, int)
CALL5(linkat, int, u64, int, u64, int)
CALL3(symlinkat, u64, int, u64)
CALL4(readlinkat, int, u64, u64, u64)
CALL4(fchmodat, int, u64, u32, int)
CALL6(pselect6, int, u64, u64, u64, u64, u64)
CALL5(ppoll, u64, u64, u64, u64, u64)
CALL2(set_robust_list, u64, u64)
CALL4(accept4, int, u64, u64, int)
CALL3(dup3, int, int, int)
CALL2(pipe2, u64, int)
CALL4(prlimit64, int, int, u64, u64)
CALL5(renameat2, int, u64, int, u64, u32)
CALL3(getrandom, u64, u64, u32)

/*
 * the calls older than the *at family, each the same as its successor
 * with the directory being where the caller stands
 */
static int64_t do_open(struct pt_regs *p_regs)
{
    return sys_openat(AT_FDCWD, A0, (int)A1, (u32)A2);
}

static int64_t do_creat(struct pt_regs *p_regs)
{
    return sys_openat(AT_FDCWD, A0, O_CREAT | O_WRONLY | O_TRUNC, (u32)A1);
}

static int64_t do_stat(struct pt_regs *p_regs)
{
    return sys_newfstatat(AT_FDCWD, A0, A1, 0);
}

static int64_t do_lstat(struct pt_regs *p_regs)
{
    return sys_newfstatat(AT_FDCWD, A0, A1, AT_SYMLINK_NOFOLLOW);
}

static int64_t do_access(struct pt_regs *p_regs)
{
    return sys_faccessat(AT_FDCWD, A0, (int)A1, 0);
}

static int64_t do_faccessat(struct pt_regs *p_regs)
{
    return sys_faccessat((int)A0, A1, (int)A2, 0);
}

static int64_t do_faccessat2(struct pt_regs *p_regs)
{
    return sys_faccessat((int)A0, A1, (int)A2, (int)A3);
}

static int64_t do_pipe(struct pt_regs *p_regs)
{
    return sys_pipe2(A0, 0);
}

static int64_t do_rename(struct pt_regs *p_regs)
{
    return sys_renameat2(AT_FDCWD, A0, AT_FDCWD, A1, 0);
}

static int64_t do_renameat(struct pt_regs *p_regs)
{
    return sys_renameat2((int)A0, A1, (int)A2, A3, 0);
}

static int64_t do_mkdir(struct pt_regs *p_regs)
{
    return sys_mkdirat(AT_FDCWD, A0, (u32)A1);
}

static int64_t do_rmdir(struct pt_regs *p_regs)
{
    return sys_unlinkat(AT_FDCWD, A0, AT_REMOVEDIR);
}

static int64_t do_link(struct pt_regs *p_regs)
{
    return sys_linkat(AT_FDCWD, A0, AT_FDCWD, A1, 0);
}

static int64_t do_unlink(struct pt_regs *p_regs)
{
    return sys_unlinkat(AT_FDCWD, A0, 0);
}

static int64_t do_symlink(struct pt_regs *p_regs)
{
    return sys_symlinkat(A0, AT_FDCWD, A1);
}

static int64_t do_readlink(struct pt_regs *p_regs)
{
    return sys_readlinkat(AT_FDCWD, A0, A1, A2);
}

static int64_t do_chmod(struct pt_regs *p_regs)
{
    return sys_fchmodat(AT_FDCWD, A0, (u32)A1, 0);
}

static int64_t do_chown(struct pt_regs *p_regs)
{
    return sys_fchownat(AT_FDCWD, A0, (u32)A1, (u32)A2, 0);
}

static int64_t do_lchown(struct pt_regs *p_regs)
{
    return sys_fchownat(AT_FDCWD, A0, (u32)A1, (u32)A2, AT_SYMLINK_NOFOLLOW);
}

static int64_t do_accept(struct pt_regs *p_regs)
{
    return sys_accept4((int)A0, A1, A2, 0);
}

/* one id stands for the real and the effective one */
static int64_t do_geteuid(struct pt_regs *p_regs)
{
    (void)p_regs;
    return sys_getuid();
}

static int64_t do_getegid(struct pt_regs *p_regs)
{
    (void)p_regs;
    return sys_getgid();
}

/* nothing is written apart from its data, so the two are one */
static int64_t do_fdatasync(struct pt_regs *p_regs)
{
    return sys_fsync((int)A0);
}

/* every filesystem at once is the one asked for, among them */
static int64_t do_syncfs(struct pt_regs *p_regs)
{
    (void)p_regs;
    return sys_sync();
}

/* where the caller's stack is, which says whether it is on the alternate one */
static int64_t do_sigaltstack(struct pt_regs *p_regs)
{
    return sys_sigaltstack(A0, A1, p_regs->rsp);
}

/*
 * fork, vfork and clone. nothing is copied: the address space is
 * duplicated by marking every page read-only in both and letting the
 * first write fault, so the cost is the page tables and a walk. see
 * addrspace_fork.
 */

/* what the child's first return to ring 3 is to look like, and do first */
struct fork_start {
    struct pt_regs frame;
    uint64_t u_child_tid;       /* CLONE_CHILD_SETTID's word, or 0 */
};

/*
 * the child's first instruction as a thread. it has never been in ring
 * 3, so there is nothing to return *from*: it leaves through a copy of
 * the frame its parent was holding, which fork_return restores whole on
 * the way out, with the zero in rax that tells it it is the child
 */
static void fork_child_start(void *arg)
{
    struct fork_start *p_start = arg;
    struct pt_regs local = p_start->frame;
    uint64_t u_tid_at = p_start->u_child_tid;
    kfree(p_start);
    local.rax = 0;

    /* in its own space now, the word is the child's own copy */
    if (u_tid_at != 0) {
        put_user_i32(u_tid_at, current_pid());
    }
    fork_return(&local);
}

/* CLONE_* that a process of its own can be made with */
#define CLONE_VM             0x00000100
#define CLONE_FS             0x00000200
#define CLONE_FILES          0x00000400
#define CLONE_SIGHAND        0x00000800
#define CLONE_VFORK          0x00004000
#define CLONE_PARENT         0x00008000
#define CLONE_THREAD         0x00010000
#define CLONE_SETTLS         0x00080000
#define CLONE_PARENT_SETTID  0x00100000
#define CLONE_CHILD_CLEARTID 0x00200000
#define CLONE_CHILD_SETTID   0x01000000
#define CSIGNAL              0x000000ff

#define CLONE_KNOWN (CSIGNAL | CLONE_VFORK | CLONE_SETTLS                \
                     | CLONE_PARENT_SETTID | CLONE_CHILD_CLEARTID       \
                     | CLONE_CHILD_SETTID)

/*
 * a new process, a copy of the caller in everything a process holds:
 * its group and session, where it stands, its signal actions and mask,
 * every descriptor pointing at the same file, and its memory, copied as
 * it is written. the child's pid, or why not
 */
static int64_t copy_process(struct pt_regs *p_regs, uint64_t u_flags,
                            uint64_t u_stack, uint64_t u_parent_tid,
                            uint64_t u_child_tid, uint64_t u_tls)
{
    struct thread *p_me = sched_current();
    if (p_me == NULL || p_me->space == NULL || p_me->pid == 0) {
        return -EINVAL;
    }
    int i_parent = p_me->pid;

    /*
     * the frame travels with the child, because the parent's kernel
     * stack, which is where it lives right now, is not the child's
     * to stand on
     */
    struct fork_start *p_start = kmalloc(sizeof *p_start);
    if (p_start == NULL) {
        return -ENOMEM;
    }
    p_start->frame = *p_regs;
    p_start->u_child_tid = (u_flags & CLONE_CHILD_SETTID) ? u_child_tid : 0;
    if (u_stack != 0) {
        p_start->frame.rsp = u_stack;
    }

    int i_pid = process_create(process_name(i_parent), i_parent,
                               process_uid(i_parent), false,
                               pit_uptime_ms());
    if (i_pid == 0) {
        kfree(p_start);
        return -EAGAIN;
    }
    process_inherit(i_pid, i_parent);

    /*
     * written before the copy is made, so the parent's word and the
     * child's both hold it
     */
    if ((u_flags & CLONE_PARENT_SETTID)
        && put_user_i32(u_parent_tid, i_pid) != 0) {
        kfree(p_start);
        process_exited(i_pid, PROCESS_KILLED, pit_uptime_ms());
        process_collect(i_pid, NULL);
        return -EFAULT;
    }

    struct addrspace *p_space = addrspace_fork(p_me->space,
                                               vmm_kernel_pml4());
    if (p_space == NULL) {
        kfree(p_start);
        process_exited(i_pid, PROCESS_KILLED, pit_uptime_ms());
        process_collect(i_pid, NULL);
        return -ENOMEM;
    }

    /*
     * every descriptor, pointing at the same thing. a pipe learns it
     * has another holder, or the parent closing its end would tell the
     * far side there is nobody left when there plainly is
     */
    struct thread *t = process_fds_inherit(i_pid, i_parent)
                       ? thread_create_parked(process_name(i_pid),
                                              fork_child_start, p_start)
                       : NULL;
    if (t == NULL) {
        kfree(p_start);
        addrspace_destroy(p_space);
        process_exited(i_pid, PROCESS_KILLED, pit_uptime_ms());
        process_fds_release(i_pid);
        process_collect(i_pid, NULL);
        return -ENOMEM;
    }
    t->space = p_space;
    t->pid = i_pid;

    /*
     * and the registers the frame does not hold: the fpu's as they are
     * this moment, which is as they were at the call, since the kernel
     * touches none of them, and the fs base, or the one asked for
     */
    fpu_save(&t->arch.fpu);
    t->arch.u_fs_base = (u_flags & CLONE_SETTLS) ? u_tls
                                                 : p_me->arch.u_fs_base;
    process_set_thread(i_pid, t->id);

    uint64_t u_slot_flags;
    struct process *p_child = process_lock_slot(i_pid, &u_slot_flags);
    if (p_child != NULL) {
        if (u_flags & CLONE_CHILD_CLEARTID) {
            p_child->u_clear_tid = u_child_tid;
        }
        p_child->b_vforked = (u_flags & CLONE_VFORK) != 0;
    }
    process_unlock_slot(u_slot_flags);

    /*
     * released only now, with the group, the descriptors and the space
     * all settled, the same reason spawn parks its threads
     */
    sched_wake_thread(t->id);

    /*
     * a vfork's parent waits for the child to be done with the program
     * they started as, by running another or by ending. the memory is
     * the child's own copy all the same, so this is only the order
     * things happen in, which is what a program calling vfork counts on
     */
    while (process_vforking(i_pid)) {
        sleep_ms(1);
    }
    return i_pid;
}

static int64_t do_fork(struct pt_regs *p_regs)
{
    return copy_process(p_regs, SIGCHLD, 0, 0, 0, 0);
}

static int64_t do_vfork(struct pt_regs *p_regs)
{
    return copy_process(p_regs, CLONE_VFORK | SIGCHLD, 0, 0, 0, 0);
}

/*
 * clone, for a process of its own. one that would share the caller's
 * memory, files or signal handlers is a thread, and threads are a
 * version away
 */
static int64_t do_clone(struct pt_regs *p_regs)
{
    uint64_t u_flags = A0;
    if ((u_flags & ~(uint64_t)CLONE_KNOWN) != 0) {
        return -EINVAL;
    }
    if ((u_flags & CSIGNAL) != SIGCHLD) {
        return -EINVAL;
    }
    return copy_process(p_regs, u_flags, A1, A2, A3, A4);
}

/*
 * the caller becomes another program. what it asked for is copied in
 * before anything else happens, since the memory it is in is about to
 * stop existing
 */
static int64_t do_execve(struct pt_regs *p_regs)
{
    char arr_path[PATH_MAX];
    struct vfs_file node;
    int64_t i_why = path_at(AT_FDCWD, A0, arr_path);
    if (i_why == 0) {
        i_why = path_lookup(arr_path, true, &node);
    }
    if (i_why < 0) {
        return i_why;
    }
    if (node.is_dir || !may_access(&node, X_OK)) {
        return -EACCES;
    }

    struct exec_args args;
    i_why = exec_args_copy(A1, A2, &args);
    if (i_why < 0) {
        return i_why;
    }
    struct user_entry e;
    i_why = user_exec(arr_path, &args, &e);
    exec_args_free(&args);
    if (i_why < 0) {
        return i_why;
    }
    process_fds_exec(current_pid());

    /*
     * the frame the exit path restores is the new program's start: its
     * entry, its stack, with argc where rsp points, the flags a fresh
     * program gets, and nothing of the old one's. that goes for the fpu
     * and the fs base as well
     */
    struct thread *p_me = sched_current();
    fpu_init_state(&p_me->arch.fpu);
    fpu_restore(&p_me->arch.fpu);
    p_me->arch.u_fs_base = 0;
    fs_base_load(0);

    uint64_t u_nr = p_regs->vector;
    memset(p_regs, 0, sizeof *p_regs);
    p_regs->vector = u_nr;
    p_regs->rip = e.u_entry;
    p_regs->rsp = e.u_stack_top;
    p_regs->rflags = RFLAGS_IF | 0x2;
    p_regs->cs = GDT_USER_CODE3;
    p_regs->ss = GDT_USER_DATA3;
    return 0;
}

/*
 * where %fs points in ring 3, which the thread keeps and the switch
 * loads, so no program ever runs on another's. gs is not offered: the
 * kernel names the core with it, in both rings
 */
static int64_t do_arch_prctl(struct pt_regs *p_regs)
{
    struct thread *p_me = sched_current();
    uint64_t u_addr = A1;

    switch (A0) {
    case ARCH_SET_FS:
        if (u_addr >= USER_ADDRESS_END) {
            return -EPERM;
        }
        p_me->arch.u_fs_base = u_addr;
        fs_base_load(u_addr);
        return 0;
    case ARCH_GET_FS:
        return (put_user_u64(u_addr, p_me->arch.u_fs_base) == 0) ? 0
                                                                 : -EFAULT;
    default:
        return -EINVAL;
    }
}

/*
 * signals. a handler runs in ring 3 on the program's own stack, and the
 * only place the kernel has the program's registers to hand, and is
 * about to give them back, is the way back to ring 3: a signal happens
 * there and not when it is raised.
 *
 * the frame is the one every x86_64 program's libc expects to find, from
 * the handler's rsp up: the address it returns to, the restorer the libc
 * gave with the action, which makes the rt_sigreturn call; a ucontext,
 * holding the interrupted registers as a sigcontext and the mask to go
 * back to; the siginfo; and the fpu's registers, which the sigcontext
 * points at. the ucontext and the sigcontext in it are the abi's, in
 * <uapi/asm/sigcontext.h>
 */
struct rt_sigframe {
    uint64_t u_return;
    struct ucontext uc;
    struct siginfo info;
};

_Static_assert(offsetof(struct rt_sigframe, uc) == 8,
               "the ucontext follows the return address");
_Static_assert(sizeof(struct siginfo) == 128, "a siginfo is 128 bytes");

/* the red zone: 128 bytes below rsp a leaf function may use unclaimed */
#define RED_ZONE 128

static void regs_to_context(const struct pt_regs *p_regs,
                            struct sigcontext *p_sc)
{
    memset(p_sc, 0, sizeof *p_sc);
    p_sc->r8 = p_regs->r8;
    p_sc->r9 = p_regs->r9;
    p_sc->r10 = p_regs->r10;
    p_sc->r11 = p_regs->r11;
    p_sc->r12 = p_regs->r12;
    p_sc->r13 = p_regs->r13;
    p_sc->r14 = p_regs->r14;
    p_sc->r15 = p_regs->r15;
    p_sc->rdi = p_regs->rdi;
    p_sc->rsi = p_regs->rsi;
    p_sc->rbp = p_regs->rbp;
    p_sc->rbx = p_regs->rbx;
    p_sc->rdx = p_regs->rdx;
    p_sc->rax = p_regs->rax;
    p_sc->rcx = p_regs->rcx;
    p_sc->rsp = p_regs->rsp;
    p_sc->rip = p_regs->rip;
    p_sc->eflags = p_regs->rflags;
    p_sc->cs = (uint16_t)p_regs->cs;
    p_sc->ss = (uint16_t)p_regs->ss;
}

static void context_to_regs(const struct sigcontext *p_sc,
                            struct pt_regs *p_regs)
{
    p_regs->r8 = p_sc->r8;
    p_regs->r9 = p_sc->r9;
    p_regs->r10 = p_sc->r10;
    p_regs->r11 = p_sc->r11;
    p_regs->r12 = p_sc->r12;
    p_regs->r13 = p_sc->r13;
    p_regs->r14 = p_sc->r14;
    p_regs->r15 = p_sc->r15;
    p_regs->rdi = p_sc->rdi;
    p_regs->rsi = p_sc->rsi;
    p_regs->rbp = p_sc->rbp;
    p_regs->rbx = p_sc->rbx;
    p_regs->rdx = p_sc->rdx;
    p_regs->rax = p_sc->rax;
    p_regs->rcx = p_sc->rcx;
    p_regs->rsp = p_sc->rsp;
    p_regs->rip = p_sc->rip;

    /*
     * the frame came out of the program's own memory, so nothing in it
     * is believed that decides who the program is: it goes back to ring
     * 3 with ring 3's selectors, flags a program may set and interrupts
     * on. without this a program could write cs = the kernel's into its
     * own stack and sigreturn into ring 0
     */
    p_regs->cs = GDT_USER_CODE3;
    p_regs->ss = GDT_USER_DATA3;
    p_regs->rflags = (p_sc->eflags & RFLAGS_USER) | RFLAGS_IF | 0x2;
}

/*
 * the handler entered as though it had been called: its frame under the
 * stack the program was on, or at the top of the alternate one, and the
 * fpu fresh for it. false when there was no room the program could be
 * written to, or no restorer to come back through
 */
static bool enter_handler(struct pt_regs *p_regs, int i_sig,
                          const struct sigaction *p_act, uint64_t u_old_mask,
                          const struct signal_origin *p_origin)
{
    if (!(p_act->sa_flags & SA_RESTORER) || p_act->sa_restorer == 0) {
        return false;
    }
    const struct signal_state *p_sig = process_signal_state(current_pid());
    stack_t alt = p_sig->altstack;
    bool b_on_alt = !(alt.ss_flags & SS_DISABLE) && p_regs->rsp > alt.ss_sp
                    && p_regs->rsp <= alt.ss_sp + alt.ss_size;

    uint64_t u_sp = p_regs->rsp - RED_ZONE;
    if ((p_act->sa_flags & SA_ONSTACK) && !(alt.ss_flags & SS_DISABLE)
        && !b_on_alt) {
        u_sp = alt.ss_sp + alt.ss_size;
        b_on_alt = true;
    }

    /* the fpu's registers above the frame, on the boundary fxrstor wants */
    u_sp -= sizeof(struct fpu_state);
    u_sp &= ~(uint64_t)63;
    uint64_t u_fpstate = u_sp;

    /*
     * and the frame, leaving rsp eight past a sixteen boundary, exactly
     * what a handler compiled as an ordinary function expects on entry
     */
    u_sp = ((u_sp - sizeof(struct rt_sigframe)) & ~(uint64_t)15) - 8;
    uint64_t u_frame = u_sp;

    struct rt_sigframe *p_frame = kmalloc(sizeof *p_frame);
    struct fpu_state *p_fpu = kmalloc(sizeof *p_fpu);
    bool b_ok = p_frame != NULL && p_fpu != NULL;
    if (b_ok) {
        memset(p_frame, 0, sizeof *p_frame);
        p_frame->u_return = p_act->sa_restorer;
        p_frame->uc.uc_stack.ss_sp = alt.ss_sp;
        p_frame->uc.uc_stack.ss_size = alt.ss_size;
        p_frame->uc.uc_stack.ss_flags = (alt.ss_flags & SS_DISABLE)
                                        ? SS_DISABLE
                                        : (b_on_alt ? SS_ONSTACK : 0);
        regs_to_context(p_regs, &p_frame->uc.uc_mcontext);
        p_frame->uc.uc_mcontext.err = p_regs->error_code;
        p_frame->uc.uc_mcontext.trapno = p_regs->vector;
        p_frame->uc.uc_mcontext.oldmask = u_old_mask;
        p_frame->uc.uc_mcontext.cr2 = p_origin->u_addr;
        p_frame->uc.uc_mcontext.fpstate = u_fpstate;
        p_frame->uc.uc_sigmask = u_old_mask;
        signal_info(i_sig, p_origin, &p_frame->info);

        /*
         * written through copy_to_user like anything else the kernel
         * puts in a program's memory: a stack the program pointed
         * somewhere it may only read has no room for a frame, and the
         * copy says so rather than faulting in ring 0
         */
        fpu_save(p_fpu);
        b_ok = copy_to_user(u_fpstate, p_fpu, sizeof *p_fpu) == 0
               && copy_to_user(u_frame, p_frame, sizeof *p_frame) == 0;
    }
    kfree(p_frame);
    if (!b_ok) {
        kfree(p_fpu);
        return false;
    }

    p_regs->rsp = u_frame;
    p_regs->rip = p_act->sa_handler;
    p_regs->rdi = (uint64_t)i_sig;
    p_regs->rsi = u_frame + offsetof(struct rt_sigframe, info);
    p_regs->rdx = u_frame + offsetof(struct rt_sigframe, uc);
    p_regs->rax = 0;
    p_regs->rflags &= ~(RFLAGS_DF | RFLAGS_TF | RFLAGS_RF);

    fpu_init_state(p_fpu);
    fpu_restore(p_fpu);
    kfree(p_fpu);
    return true;
}

/*
 * a handler has returned, through its restorer, so the frame is just
 * under where rsp is: the registers go back exactly as they were, the fpu
 * with them, and the mask the handler interrupted
 */
static int64_t do_rt_sigreturn(struct pt_regs *p_regs)
{
    uint64_t u_frame = p_regs->rsp - 8;
    struct ucontext uc;
    struct fpu_state *p_fpu = kmalloc(sizeof *p_fpu);
    int i_me = current_pid();
    struct signal_origin origin = { .i_code = SI_KERNEL };

    if (p_fpu == NULL
        || copy_from_user(&uc, u_frame + offsetof(struct rt_sigframe, uc),
                          sizeof uc) != 0
        || (uc.uc_mcontext.fpstate != 0
            && copy_from_user(p_fpu, uc.uc_mcontext.fpstate,
                              sizeof *p_fpu) != 0)) {
        kfree(p_fpu);
        process_force_signal(i_me, SIGSEGV, &origin);
        return (int64_t)p_regs->rax;
    }
    context_to_regs(&uc.uc_mcontext, p_regs);

    /*
     * the fpu's too, with the mxcsr bits the cpu lacks cleared, since
     * fxrstor faults in ring 0 on any it is handed
     */
    if (uc.uc_mcontext.fpstate != 0) {
        fpu_sanitize(p_fpu);
        fpu_restore(p_fpu);
    }
    kfree(p_fpu);

    process_set_mask(i_me, uc.uc_sigmask, false);

    /* an address the cpu cannot return to is the program's fault */
    if (p_regs->rip >= USER_ADDRESS_END || p_regs->rsp >= USER_ADDRESS_END) {
        process_force_signal(i_me, SIGSEGV, &origin);
    }
    return (int64_t)p_regs->rax;    /* what it was holding, not an answer */
}

#define ENTRY(name) [__NR_##name] = do_##name

static const syscall_fn s_arr_table[NR_SYSCALLS] = {
    ENTRY(read), ENTRY(write), ENTRY(open), ENTRY(close), ENTRY(stat),
    ENTRY(fstat), ENTRY(lstat), ENTRY(poll), ENTRY(lseek), ENTRY(mmap),
    ENTRY(mprotect), ENTRY(munmap), ENTRY(brk), ENTRY(rt_sigaction),
    ENTRY(rt_sigprocmask), ENTRY(rt_sigreturn), ENTRY(ioctl),
    ENTRY(pread64), ENTRY(pwrite64), ENTRY(readv), ENTRY(writev),
    ENTRY(access), ENTRY(pipe), ENTRY(select), ENTRY(sched_yield),
    ENTRY(mremap), ENTRY(madvise), ENTRY(dup), ENTRY(dup2), ENTRY(pause),
    ENTRY(nanosleep), ENTRY(getitimer), ENTRY(alarm), ENTRY(setitimer),
    ENTRY(getpid), ENTRY(socket), ENTRY(connect), ENTRY(accept),
    ENTRY(sendto), ENTRY(recvfrom), ENTRY(sendmsg), ENTRY(recvmsg),
    ENTRY(shutdown), ENTRY(bind), ENTRY(listen), ENTRY(getsockname),
    ENTRY(getpeername), ENTRY(setsockopt), ENTRY(getsockopt), ENTRY(clone),
    ENTRY(fork), ENTRY(vfork), ENTRY(execve), ENTRY(exit), ENTRY(wait4),
    ENTRY(kill), ENTRY(uname), ENTRY(fcntl), ENTRY(fsync),
    ENTRY(fdatasync), ENTRY(truncate), ENTRY(ftruncate), ENTRY(getcwd),
    ENTRY(chdir), ENTRY(fchdir), ENTRY(rename), ENTRY(mkdir), ENTRY(rmdir),
    ENTRY(creat), ENTRY(link), ENTRY(unlink), ENTRY(symlink),
    ENTRY(readlink), ENTRY(chmod), ENTRY(fchmod), ENTRY(chown),
    ENTRY(fchown), ENTRY(lchown), ENTRY(umask), ENTRY(gettimeofday),
    ENTRY(getrlimit), ENTRY(getrusage), ENTRY(sysinfo), ENTRY(times),
    ENTRY(getuid), ENTRY(syslog), ENTRY(getgid), ENTRY(setuid),
    ENTRY(setgid), ENTRY(geteuid), ENTRY(getegid), ENTRY(setpgid),
    ENTRY(getppid), ENTRY(getpgrp), ENTRY(setsid), ENTRY(setreuid),
    ENTRY(setregid), ENTRY(getgroups), ENTRY(setgroups), ENTRY(setresuid),
    ENTRY(getresuid), ENTRY(setresgid), ENTRY(getresgid), ENTRY(getpgid),
    ENTRY(getsid), ENTRY(rt_sigpending), ENTRY(rt_sigtimedwait),
    ENTRY(rt_sigsuspend), ENTRY(sigaltstack), ENTRY(prctl),
    ENTRY(arch_prctl), ENTRY(setrlimit), ENTRY(sync), ENTRY(settimeofday),
    ENTRY(reboot), ENTRY(sethostname), ENTRY(setdomainname), ENTRY(gettid),
    ENTRY(tkill), ENTRY(time), ENTRY(getdents64), ENTRY(set_tid_address),
    ENTRY(clock_settime), ENTRY(clock_gettime), ENTRY(clock_getres),
    ENTRY(clock_nanosleep), ENTRY(exit_group), ENTRY(tgkill),
    ENTRY(openat), ENTRY(mkdirat), ENTRY(fchownat), ENTRY(newfstatat),
    ENTRY(unlinkat), ENTRY(renameat), ENTRY(linkat), ENTRY(symlinkat),
    ENTRY(readlinkat), ENTRY(fchmodat), ENTRY(faccessat), ENTRY(pselect6),
    ENTRY(ppoll), ENTRY(set_robust_list), ENTRY(accept4), ENTRY(dup3),
    ENTRY(pipe2), ENTRY(prlimit64), ENTRY(syncfs), ENTRY(renameat2),
    ENTRY(getrandom), ENTRY(faccessat2),
};

/* the answers a call gives when a signal cut it short, none a program sees */
static bool restartable(int64_t i_result)
{
    return i_result == -ERESTARTSYS || i_result == -ERESTARTNOINTR
           || i_result == -ERESTARTNOHAND;
}

/* the call made again: its number back in rax, and rip on the `syscall` */
static void restart(struct pt_regs *p_regs)
{
    p_regs->rax = p_regs->vector;
    p_regs->rip -= SYSCALL_INSN_LEN;
}

/*
 * everything raised and deliverable, acted on: ignored, stopping or
 * continuing the process, ending it, or one handler entered, the rest
 * waiting for it to return. a call a signal cut short, in b_call, is
 * made again when no handler runs, and when one does is told -EINTR
 * unless its kind and SA_RESTART say to make it again after
 */
static void deliver_signals(struct pt_regs *p_regs, bool b_call)
{
    int i_pid = current_pid();
    if (i_pid <= 0) {
        return;
    }
    bool b_restart = b_call && restartable((int64_t)p_regs->rax);

    for (;;) {
        int i_sig = 0;
        struct sigaction act;
        uint64_t u_old_mask = 0;
        struct signal_origin origin;
        enum signal_action e_act = process_take_signal(i_pid, &i_sig, &act,
                                                       &u_old_mask, &origin);
        switch (e_act) {
        case SIGNAL_ACTION_NONE:
            break;

        case SIGNAL_ACTION_IGNORE:
            continue;

        case SIGNAL_ACTION_STOP:
            /* a stop that was blocked when sent, and is not any more */
            process_stopped(i_pid, i_sig);
            sched_set_stopped(process_thread(i_pid), true);
            sched_yield();
            continue;

        case SIGNAL_ACTION_CONTINUE:
            sched_set_stopped(process_thread(i_pid), false);
            continue;

        case SIGNAL_ACTION_HANDLER:
            if (b_restart) {
                int64_t i_result = (int64_t)p_regs->rax;
                if (i_result == -ERESTARTNOINTR
                    || (i_result == -ERESTARTSYS
                        && (act.sa_flags & SA_RESTART))) {
                    restart(p_regs);
                } else {
                    p_regs->rax = (uint64_t)-EINTR;
                }
                b_restart = false;
            }
            if (enter_handler(p_regs, i_sig, &act, u_old_mask, &origin)) {
                return;         /* one at a time: the rest wait */
            }
            /*
             * nowhere to put the frame, or nothing to return through:
             * the program cannot be told, so it is ended the way a
             * fault ends it, which a SIGSEGV handler of its own would
             * only fail the same way at
             */
            if (i_sig == SIGSEGV) {
                thread_exit(SIGSEGV);
            }
            process_force_signal(i_pid, SIGSEGV, &origin);
            continue;

        case SIGNAL_ACTION_TERMINATE:
            thread_exit(i_sig);     /* the wait status of a signal's death */
        }
        break;
    }

    /*
     * nothing ran a handler: a mask a wait put in place goes back now,
     * as it would have when the handler returned, and the call is made
     * again, which is all its being cut short meant
     */
    uint64_t u_flags;
    struct process *p = process_lock_slot(i_pid, &u_flags);
    if (p != NULL && p->sig.b_suspended) {
        signal_set_mask(&p->sig, p->sig.u_suspend_mask);
        p->sig.b_suspended = false;
        p->interrupted = signal_deliverable(&p->sig);
    }
    process_unlock_slot(u_flags);
    if (b_restart) {
        restart(p_regs);
    }
}

/*
 * one pair of words per core, reached through gs by the entry stub.
 * these cannot be globals any more: two cores in a syscall at the same
 * instant would trample each other's stacks, and the scheduler on one
 * core would be rewriting the kernel stack the other is about to use
 */
struct syscall_percpu {
    uint64_t kernel_rsp;    /* gs:0, keep this first */
    uint64_t scratch_rsp;   /* gs:8 */
};

static struct syscall_percpu syscall_cpu[SMP_MAX_CPUS];

void syscall_set_kernel_rsp(uint64_t rsp)
{
    syscall_cpu[smp_this_cpu()].kernel_rsp = rsp;
}

void syscall_init(void)
{
    /*
     * STAR[47:32] is the kernel selector pair syscall loads: cs from it
     * and ss from it+8. STAR[63:48] is the base sysret computes from,
     * cs = base+16 and ss = base+8, which is why the gdt puts user data
     * below user code
     */
    uint64_t star = ((uint64_t)GDT_KERNEL_CODE << 32)
                  | ((uint64_t)GDT_KERNEL_DATA << 48);
    wrmsr(MSR_STAR, star);

    wrmsr(MSR_LSTAR, (uint64_t)syscall_entry);

    /*
     * flags to clear on entry. IF is the important one: it means the kernel arrives with interrupts off and can swap onto a kernel stack
     * without anything preempting the kernel halfway. DF because the sysv abi
     * insists it be clear, TF so a user single-stepping cannot drag the
     * kernel along with it
     */
    wrmsr(MSR_SFMASK, RFLAGS_IF | RFLAGS_DF | RFLAGS_TF);

    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_SCE);

    /*
     * gs names this core, in both rings, for as long as the machine
     * runs. both halves are set to the same thing so that a stray
     * swapgs, from anywhere, ever, is a no-op rather than a fault
     * three instructions into the next system call.
     *
     * every one of the registers set in this function is per core, which
     * is the whole reason it runs on each of them rather than once
     */
    uint64_t mine = (uint64_t)&syscall_cpu[smp_this_cpu()];
    wrmsr(MSR_GS_BASE, mine);
    wrmsr(MSR_KERNEL_GS_BASE, mine);
}

void syscall_from_user(struct pt_regs *p_regs)
{
    uint64_t u_nr = p_regs->vector;

    /*
     * anything whose alarm has come due, before the delivery below,
     * so a syscall that takes a while and crosses a deadline delivers
     * the signal on its way out rather than on the next call
     */
    process_check_alarms(pit_uptime_ms());

    syscall_fn fn = (u_nr < NR_SYSCALLS) ? s_arr_table[u_nr] : NULL;
    int64_t i_result = (fn != NULL) ? fn(p_regs) : -ENOSYS;

    /*
     * the answer goes into the frame before any handler is entered, so
     * that the frame a handler saves holds it, and sigreturn gives the
     * program the answer to the call it made rather than the question.
     * sigreturn's own answer is what it put back, and it was not cut
     * short by anything
     */
    p_regs->rax = (uint64_t)i_result;
    deliver_signals(p_regs, u_nr != __NR_rt_sigreturn);
}

void exit_to_user(struct pt_regs *p_regs)
{
    deliver_signals(p_regs, false);
}
