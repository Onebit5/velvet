// SPDX-License-Identifier: GPL-2.0-only
/*
 * arch/x86_64/syscall.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the syscall dispatcher.
 */

#include <asm/syscall.h>
#include <velvet/epoch.h>
#include <asm/smp.h>
#include <asm/msr.h>
#include <stdbool.h>
#include <asm/gdt.h>
#include <velvet/input.h>
#include <velvet/pit.h>
#include <velvet/kprintf.h>
#include <velvet/vmm.h>
#include <velvet/pmm.h>
#include <velvet/sched.h>
#include <velvet/thread.h>
#include <velvet/process.h>
#include <velvet/netif.h>
#include <velvet/socket.h>
#include <velvet/usermode.h>
#include <velvet/addrspace.h>
#include <velvet/vfs.h>
#include <velvet/disk.h>
#include <velvet/fsck.h>
#include <velvet/source.h>
#include <velvet/init.h>
#include <velvet/hash.h>
#include <velvet/path.h>
#include <velvet/tty.h>
#include <velvet/console.h>
#include <velvet/input.h>
#include <velvet/pipe.h>
#include <velvet/kmalloc.h>
#include <velvet/addrspace.h>
#include <velvet/vmm.h>
#include <velvet/string.h>
#include <uapi/velvet/tty.h>

#define MSR_STAR   0xc0000081
#define MSR_LSTAR  0xc0000082
#define MSR_SFMASK 0xc0000084

#define EFER_SCE (1ull << 0)    /* without this, `syscall` is #UD */

#define RFLAGS_IF (1ull << 9)
#define RFLAGS_DF (1ull << 10)
#define RFLAGS_TF (1ull << 8)

static uint64_t call_counts[SYSCALL_COUNT];

static const char *const call_names[SYSCALL_COUNT] = {
    "exit", "write", "read", "uptime", "yield", "sleep",
    "open", "close", "getpid", "spawn", "wait", "readdir", "getuid",
    "create", "chdir", "getcwd", "mkdir", "rmdir",
    "unlink", "rename", "stat",
    "getkey", "screen", "cursor", "clear", "fork", "mmap", "munmap",
    "chmod", "chown", "symlink", "readlink", "getenv", "setenv",
    "socket", "sendto", "recvfrom",
};

uint64_t syscall_times_called(unsigned nr)
{
    return (nr < SYSCALL_COUNT) ? call_counts[nr] : 0;
}

const char *syscall_name(unsigned nr)
{
    return (nr < SYSCALL_COUNT) ? call_names[nr] : "?";
}

/* implemented in syscall.asm */
extern void syscall_entry(void);

static int caller_pid(void);

/*
 * whose page tables decide whether a user pointer is real.
 *
 * it must be the *caller's*, not the kernel's. every program has had an
 * address space of its own, and the kernel's tables have no
 * mapping for a program's memory at all, so checking there says no to
 * every pointer that was ever going to be valid, and a program prints
 * nothing for no visible reason. the kernel is running on the caller's cr3 at
 * this moment, so this is also what the cpu would use if the kernel simply
 * dereferenced the thing
 */
static uint64_t caller_pml4(void)
{
    struct thread *me = sched_current();
    if (me != NULL && me->space != NULL) {
        return me->space->pml4;
    }
    return vmm_kernel_pml4();
}

/*
 * a pointer handed to the kernel by ring 3 is a claim, not a fact. check the
 * whole span really is mapped before touching a byte of it, a user
 * program should not be able to make the kernel fault by lying.
 *
 * `for_write` is which way the bytes are about to move, and it matters
 * twice over.
 *
 * it decides what kind of fault to ask for when the page is not there
 * yet, and asking for the wrong one refuses a page the program is
 * perfectly entitled to. this asked for a *write* on every pointer,
 * which meant any read-only region the program had not touched itself
 * was refused, and a program's `.rodata` is exactly that. every
 * string constant `uptime` prints goes straight from .rodata into
 * `write` without the program ever reading it, so the first access is
 * this check, and the check asked to write to it. the program printed
 * its numbers and none of its words.
 *
 * and it decides whether the page has to be *writable* at all. that
 * was not checked before, which let a program hand `read` a pointer
 * into its own .text and have the kernel fill it in, ring 3 editing
 * itself through a syscall, past a mapping that says read-only
 */
static bool user_range_ok_dir(uint64_t addr, uint64_t len, bool for_write)
{
    if (len == 0) {
        return true;
    }
    if (addr + len < addr) {
        return false;           /* wrapped, so it is a lie by construction */
    }
    /*
     * nothing in userspace lives in the higher half, and letting a
     * pointer up there through would hand ring 3 the kernel
     */
    if (addr >= 0xffff800000000000ull || (addr + len) > 0xffff800000000000ull) {
        return false;
    }

    /*
     * TODO: this walks, and can fault, every page of the range, so a
     * megabyte long read is 256 page table walks and up to 256 faults before
     * the copy starts. a walk that returns the longest run sharing the same
     * flags would let the loop skip the rest of a mapping, which is most of
     * the pages in it.
     */
    uint64_t pml4 = caller_pml4();
    struct thread *me = sched_current();

    for (uint64_t p = addr & ~0xfffull; p < addr + len; p += PAGE_SIZE) {
        uint64_t flags = vmm_flags(pml4, p);

        /*
         * not present is not the same as not allowed.
         *
         * memory from `mmap` is *reserved* rather than mapped: the page
         * arrives when the program first touches it, which is how a
         * program can ask for a megabyte and pay for the page it uses.
         * so a page the program has agreed to and not yet written has
         * no entry at all, and this check refused it.
         *
         * that had never shown up because no program had passed
         * malloc'd memory to a syscall: `wordcount` reads a byte at a
         * time into a stack variable and copies it in. `diff` reads a
         * whole file into the heap, which is the first time a reserved
         * page reached here, and every read was refused.
         *
         * so the page is faulted in the same way touching it would,
         * and if the address is in no region at all, that fails and the
         * refusal below stands
         */
        if (!(flags & PTE_PRESENT) && me != NULL && me->space != NULL) {
            if (addrspace_fault(me->space, p, for_write, false)) {
                flags = vmm_flags(pml4, p);
            }
        }

        /*
         * present, not writable, and about to be written to. that is
         * either a page shared with somebody after a fork, which
         * faulting resolves by making the copy, exactly as a store from
         * ring 3 would, or a region that was never the program's to
         * change, which the refusal below then catches
         */
        if (for_write && (flags & PTE_PRESENT) && !(flags & PTE_WRITE)
            && me != NULL && me->space != NULL) {
            if (addrspace_fault(me->space, p, true, true)) {
                flags = vmm_flags(pml4, p);
            }
        }

        if (!(flags & PTE_PRESENT) || !(flags & PTE_USER)) {
            /*
             * say so. a refusal returns -1 to a program that will
             * probably ignore it, and the result is a program that
             * prints nothing for no reason anyone can see, which is
             * exactly how this bug stayed hidden
             */
            kprintf("[kernel] refused a pointer from pid %d: %p is not "
                    "this process's memory\n", caller_pid(), (void *)addr);
            return false;
        }

        if (for_write && !(flags & PTE_WRITE)) {
            kprintf("[kernel] refused a pointer from pid %d: %p is not "
                    "this process's to write to\n",
                    caller_pid(), (void *)addr);
            return false;
        }
    }
    return true;
}

/*
 * the two ways a syscall touches a user buffer, named rather than left
 * as a bare `true` at twenty call sites, `user_range_ok(p, n, true)`
 * reads like a question and answers a different one
 */
static bool user_range_ok(uint64_t addr, uint64_t len)
{
    return user_range_ok_dir(addr, len, false);
}

/* for a buffer the kernel is about to *fill* */
static bool user_range_writable(uint64_t addr, uint64_t len)
{
    return user_range_ok_dir(addr, len, true);
}

#define WRITE_MAX 4096

/*
 * the most a program may ask for in one go. a limit rather than "as
 * much as the address space holds", because the pages are not made
 * until they are touched, so without one, a program could reserve
 * more than the machine has and only find out halfway through using it
 */
#define MMAP_MAX (64ull * 1024 * 1024)

/*
 * which process is asking. everything touching per-process state goes
 * through this rather than assuming
 */
static int caller_pid(void)
{
    struct thread *me = sched_current();
    return (me != NULL) ? me->pid : 0;
}

/*
 * copy a path out of ring 3 into somewhere the kernel can trust it. paths are
 * short by definition, so a fixed buffer is honest rather than lazy
 */
static bool copy_path(uint64_t ptr, uint64_t len, char *out, size_t max)
{
    if (len == 0 || len >= max || !user_range_ok(ptr, len)) {
        return false;
    }
    const char *src = (const char *)ptr;
    for (uint64_t i = 0; i < len; i++) {
        out[i] = src[i];
    }
    out[len] = '\0';
    return true;
}

/*
 * a path, as the caller wrote it, flattened against where the caller is
 * standing. every name that crosses this boundary goes through here,
 * relative or absolute, `..` and all, so that no filesystem below ever
 * sees a path that means something different depending on who asked
 */
static bool copy_path_resolved(uint64_t ptr, uint64_t len,
                               char *out, size_t size)
{
    char raw[PATH_MAX];
    if (!copy_path(ptr, len, raw, sizeof raw)) {
        return false;
    }
    return path_resolve(process_cwd(caller_pid()), raw, out, size);
}

static int64_t sys_write_console(uint64_t ptr, uint64_t len);

/*
 * the whole of writing, now that a descriptor can be four things.
 *
 * one lookup and a switch. a program asks for descriptor 1 and gets
 * whatever was put there before it started, the screen, a pipe, or a
 * file, and cannot tell which. that is the entire reason `echo hi`,
 * `echo hi | wc` and `echo hi > x` are the same program doing the same
 * thing, and it is why redirection needed 0, 1 and 2 to become real
 * slots rather than three special cases in here
 */
static int64_t sys_write(uint64_t fd, uint64_t ptr, uint64_t len)
{
    /*
     * clamp first, then check what the kernel clamped to. a program asking to
     * write four exabytes gets a short write rather than a refusal,
     * which is the ordinary contract, and the range actually checked
     * is the one actually touched, so an absurd length can never widen
     * what the kernel is willing to read
     */
    if (len > WRITE_MAX) {
        len = WRITE_MAX;
    }
    if (!user_range_ok(ptr, len)) {
        return -1;
    }

    /*
     * a copy rather than a pointer into the table: everything below
     * this may block, and none of it may run while the process table's
     * lock is held
     */
    struct fd f;
    if (!process_fd_get(caller_pid(), (int)fd, &f)) {
        return -1;
    }

    switch (f.kind) {
    case FD_CONSOLE:
        /*
         * the console belongs to whoever is at the front of the
         * terminal, exactly the way the keyboard does. a job put in the
         * background with `&` printing over the prompt is the noise
         * that `&` was supposed to spare you, and worse than noise,
         * because it lands in the middle of a line you are typing.
         *
         * so it is dropped rather than refused. a program has no way to
         * know it is in the background and no sensible thing to do
         * about it, and a write that fails would have it either die or
         * loop; a write that quietly went nowhere is what a terminal
         * nobody is watching actually is.
         *
         * descriptor 2 is the exception, and it is the exception
         * everywhere for the same reason: it is the one that reaches
         * the terminal whatever else is going on, so a job that failed
         * in the background still gets to say so. the same call was
         * made for pipes, errors are not data
         */
        if (fd != FD_STDERR
            && process_pgid(caller_pid()) != tty_foreground()) {
            return (int64_t)len;
        }
        return sys_write_console(ptr, len);

    case FD_PIPE: {
        if (!f.writing) {
            return -1;      /* the reading end. a descriptor goes one way */
        }
        int64_t n = pipe_write(f.pipe, caller_pid(), (const void *)ptr, len);
        if (n < 0) {
            /*
             * nobody is reading and nobody ever will be. unix raises
             * SIGPIPE here and the default disposition is to die; the kernel has no signals, so the process is ended for it, which
             * gets to the same place by a shorter road.
             *
             * this is not tidiness. it is what makes `cat huge | head`
             * stop: head has seen enough and gone, and without this cat
             * would sit blocked on a buffer that will never drain,
             * holding the terminal, forever
             */
            thread_exit(PROCESS_KILLED);
        }
        return n;
    }

    case FD_DISK: {
        /*
         * FIXME: nothing here checks that this descriptor was opened for
         * writing, or that the caller may write this file at all. sys_open
         * hands back an FD_DISK on a read check alone and records no write
         * intent, and the only write guards in this file, in sys_create
         * and in redirect, cover making a new file as uid 0. so a guest
         * opens a root-owned 0644 file for reading and writes through the
         * same descriptor, and the bytes reach the disk: the one promise
         * this system is built to keep, that a uid cannot reach what is
         * not its own, does not hold for a file that already exists. the
         * fd wants a writable bit set at open time, and a vfs_may_write to
         * gate the write the way vfs_may_read already gates the read.
         */
        struct fd_disk d;
        if (!process_fd_disk(caller_pid(), (int)fd, &d)) {
            return -1;
        }

        /*
         * the descriptor remembered where this file's directory record
         * is, which is what lets the new size be written back to the
         * right place without looking the path up all over again
         */
        struct vfs_file vf;
        memset(&vf, 0, sizeof vf);
        vf.kind = VFS_DISK;
        vf.mount = d.mount;
        vf.cluster = d.cluster;
        vf.size = d.size;
        vf.entry_sector = d.entry_sector;
        vf.entry_offset = d.entry_offset;

        int64_t n = vfs_write(&vf, d.pos, (const void *)ptr, len);
        if (n > 0) {
            process_fd_grew(caller_pid(), (int)fd, vf.cluster, vf.size);
            process_fd_advance(caller_pid(), (int)fd, (uint64_t)n);
        }
        return n;
    }

    default:
        /*
         * the keyboard, and a file in read-only memory. neither takes
         * anything, and saying so beats pretending it went somewhere
         */
        return -1;
    }
}

static int64_t sys_write_console(uint64_t ptr, uint64_t len)
{
    /*
     * clamp first, then check what the kernel clamped to. a program asking to
     * write four exabytes gets a short write rather than a refusal,
     * which is the ordinary contract, and the range actually checked
     * below is the one the kernel actually touch, so an absurd length can
     * never widen what the kernel is willing to read
     */
    if (len > WRITE_MAX) {
        len = WRITE_MAX;
    }
    if (!user_range_ok(ptr, len)) {
        return -1;
    }
    /*
     * one call rather than one per byte. this was `kprintf("%c", ...)`
     * in a loop, which parses a format string and walks a variadic list
     * for every character a program prints, two thousand of each to
     * redraw a screen
     */
    kwrite((const char *)ptr, (size_t)len);
    return (int64_t)len;
}

/*
 * the terminal does the echoing and the line editing, because a program
 * in ring 3 cannot, the keys never pass through it
 */
static int64_t sys_read(uint64_t fd, uint64_t ptr, uint64_t len)
{
    if (len == 0 || !user_range_writable(ptr, len)) {
        return -1;
    }

    struct fd f;
    if (!process_fd_get(caller_pid(), (int)fd, &f)) {
        return -1;
    }

    switch (f.kind) {
    case FD_KEYBOARD:
        /*
         * the keyboard, unless somebody put something else there. a
         * program reading stdin neither knows nor cares whether a
         * person or a `cat` is on the other end
         */
        return tty_read_line(caller_pid(), (char *)ptr, len);

    case FD_PIPE:
        if (f.writing) {
            return -1;
        }
        return pipe_read(f.pipe, caller_pid(), (void *)ptr, len);

    case FD_DISK: {
        /*
         * the bytes are not in memory, so the descriptor's position and
         * the file's first cluster are enough to go and get them
         */
        struct fd_disk d;
        if (!process_fd_disk(caller_pid(), (int)fd, &d)) {
            return -1;
        }
        if (d.remaining == 0) {
            return 0;           /* the end, which is not an error */
        }
        if (len > d.remaining) {
            len = d.remaining;
        }
        struct vfs_file vf;
        memset(&vf, 0, sizeof vf);
        vf.kind = VFS_DISK;
        vf.mount = d.mount;
        vf.cluster = d.cluster;
        vf.size = d.size;

        int64_t n = vfs_read(&vf, d.pos, (void *)ptr, len);
        if (n > 0) {
            process_fd_advance(caller_pid(), (int)fd, (uint64_t)n);
        }
        return n;
    }

    case FD_SOURCE: {
        /*
         * the bytes are on the boot medium, and the archive knows how to
         * go and get them, so, like the disk, the descriptor remembers
         * where the file starts and nothing else
         */
        struct fd_source s;
        if (!process_fd_source(caller_pid(), (int)fd, &s)) {
            return -1;
        }
        if (s.remaining == 0) {
            return 0;
        }
        if (len > s.remaining) {
            len = s.remaining;
        }
        struct vfs_file vf;
        memset(&vf, 0, sizeof vf);
        vf.kind = VFS_SOURCE;
        vf.medium_at = s.medium_at;
        vf.size = s.size;

        int64_t n = vfs_read(&vf, s.pos, (void *)ptr, len);
        if (n > 0) {
            process_fd_advance(caller_pid(), (int)fd, (uint64_t)n);
        }
        return n;
    }

    case FD_MEMORY: {
        /*
         * the bytes are already in memory, the descriptor only says
         * how far through them the kernel had got
         */
        const void *data = NULL;
        uint64_t left = 0;
        if (!process_fd_peek(caller_pid(), (int)fd, &data, &left)) {
            return -1;
        }
        if (left < len) {
            len = left;         /* a short read at the end, as usual */
        }
        memcpy((void *)ptr, data, len);
        process_fd_advance(caller_pid(), (int)fd, len);
        return (int64_t)len;
    }

    case FD_PROC: {
        /*
         * written afresh from where the last read stopped, so a reader
         * taking it a piece at a time sees one file, as long as nothing
         * in it changed in between
         */
        struct vfs_file vf;
        memset(&vf, 0, sizeof vf);
        vf.kind = VFS_PROC;
        vf.sz_proc = f.sz_proc;

        int64_t i_got = vfs_read(&vf, f.pos, (void *)ptr, len);
        if (i_got > 0) {
            process_fd_advance(caller_pid(), (int)fd, (uint64_t)i_got);
        }
        return i_got;
    }

    default:
        return -1;      /* the console. there is nothing to read from it */
    }
}

/*
 * closing a descriptor with a pipe in it lets go of one end, which
 * wakes whoever is waiting on the other, so the pipe comes back out
 * of the table first and is closed here, holding nothing
 */
/* one end of a pipe the caller no longer has, let go of */
static void let_go(const struct pipe_end *p_end)
{
    if (p_end->p == NULL) {
        return;
    }
    if (p_end->writing) {
        pipe_close_write(p_end->p);
    } else {
        pipe_close_read(p_end->p);
    }
}

static int64_t sys_close(int fd)
{
    struct pipe_end e;
    if (!process_fd_close(caller_pid(), fd, &e)) {
        return -1;
    }
    let_go(&e);
    return 0;
}

/*
 * spawning was the only way to make a process and it built one from a
 * file every time. this makes one from a process instead: the same
 * program, the same open files, the same place in it, two of
 * everything except the answer, which is how either half knows which it
 * is.
 *
 * nothing is copied. the address space is duplicated by marking every
 * page read-only in both and letting the first write fault, so the cost
 * is the page tables and a walk. see addrspace_fork.
 */
static void fork_child_start(void *arg);

static int64_t sys_fork(struct user_regs *regs)
{
    if (regs == NULL) {
        return -1;
    }

    struct thread *me = sched_current();
    if (me == NULL || me->space == NULL || me->pid == 0) {
        return -1;      /* a kernel thread has nothing to fork */
    }

    int parent = me->pid;

    /*
     * the frame travels with the child, because the parent's kernel
     * stack, which is where it lives right now, is not the child's
     * to stand on
     */
    struct user_regs *frame = kmalloc(sizeof *frame);
    if (frame == NULL) {
        return -1;
    }
    *frame = *regs;

    struct addrspace *space = addrspace_fork(me->space, vmm_kernel_pml4());
    if (space == NULL) {
        kfree(frame);
        return -1;
    }

    int pid = process_create(process_name(parent), parent,
                             process_uid(parent), false, pit_uptime_ms());
    if (pid == 0) {
        kfree(frame);
        addrspace_destroy(space);
        return -1;
    }

    /*
     * a copy of the parent in everything a process holds: its job, so
     * ctrl+c reaches the child too, where it stands, its environment,
     * and what it does with each signal. a child that lost the
     * environment lost PATH, and one that lost a handler died of the
     * first ctrl+c its parent had arranged to survive
     */
    process_inherit(pid, parent);

    /*
     * every descriptor, pointing at the same thing. a pipe learns it
     * has another holder, or the parent closing its end would tell the
     * far side there is nobody left when there plainly is
     */
    if (!process_fds_inherit(pid, parent)) {
        kfree(frame);
        addrspace_destroy(space);
        process_exited(pid, PROCESS_KILLED, pit_uptime_ms());
        pipe_release_for(pid);
        process_collect(pid, NULL);
        return -1;
    }

    struct thread *t = thread_create_parked(process_name(pid),
                                            fork_child_start, frame);
    if (t == NULL) {
        kfree(frame);
        addrspace_destroy(space);
        process_exited(pid, PROCESS_KILLED, pit_uptime_ms());
        pipe_release_for(pid);
        process_collect(pid, NULL);
        return -1;
    }
    t->space = space;
    t->pid = pid;
    process_set_thread(pid, t->id);

    /*
     * released only now, with the group, the descriptors and the space
     * all settled, the same reason spawn parks its threads
     */
    sched_wake_thread(t->id);

    /*
     * and the two answers. the parent's is the return value below; the
     * child's is the zero fork_return writes into rax
     */
    return pid;
}

/*
 * a program may read and change its own, and what it changes is
 * inherited by anything it starts and by nothing else. that is not a
 * limitation, it is the definition: a child cannot reach up into its
 * parent, which is exactly why `export` is a shell builtin everywhere
 * and has been since 1977, a command could only ever have changed
 * its own
 */
static int64_t sys_getenv(uint64_t ptr, uint64_t len, uint64_t out_ptr,
                          uint64_t out_size)
{
    if (len == 0 || len >= ENV_NAME_MAX || !user_range_ok(ptr, len)) {
        return -1;
    }
    if (out_size == 0 || !user_range_writable(out_ptr, out_size)) {
        return -1;
    }

    char name[ENV_NAME_MAX];
    memcpy(name, (const void *)ptr, len);
    name[len] = '\0';

    char value[ENV_VALUE_MAX];
    if (!process_env_get(caller_pid(), name, value, sizeof value)) {
        return -1;      /* not set, which is not the same as set to nothing */
    }

    uint64_t n = strlen(value);
    if (n >= out_size) {
        n = out_size - 1;
    }
    memcpy((void *)out_ptr, value, n);
    ((char *)out_ptr)[n] = '\0';
    return (int64_t)n;
}

static int64_t sys_setenv(uint64_t ptr, uint64_t len, uint64_t vptr,
                          uint64_t vlen)
{
    if (len == 0 || len >= ENV_NAME_MAX || !user_range_ok(ptr, len)) {
        return -1;
    }
    char name[ENV_NAME_MAX];
    memcpy(name, (const void *)ptr, len);
    name[len] = '\0';

    /*
     * a name with an equals in it would make an entry that could never
     * be looked up again, since the first equals ends the name
     */
    for (uint64_t i = 0; i < len; i++) {
        if (name[i] == '=') {
            return -1;
        }
    }

    if (vptr == 0) {
        return process_env_set(caller_pid(), name, NULL) ? 0 : -1;
    }
    if (vlen >= ENV_VALUE_MAX || !user_range_ok(vptr, vlen)) {
        return -1;
    }
    char value[ENV_VALUE_MAX];
    memcpy(value, (const void *)vptr, vlen);
    value[vlen] = '\0';

    return process_env_set(caller_pid(), name, value) ? 0 : -1;
}

/*
 * all of these answer -1 on the ramdisk and on a fat disk, because
 * neither has anywhere to record the answer. a chmod that quietly did
 * nothing would be worse than one that refuses
 */
static int64_t sys_chmod(uint64_t ptr, uint64_t len, uint64_t mode)
{
    char path[PATH_MAX];
    if (!copy_path_resolved(ptr, len, path, sizeof path)) {
        return -1;
    }
    /*
     * only the owner or the master. otherwise anybody could hand
     * themselves a file by making it world-writable first
     */
    struct vfs_file f;
    int uid = process_uid(caller_pid());
    if (!vfs_open_nofollow(path, &f)) {
        return -1;
    }
    if (uid != 0 && (uint32_t)uid != f.uid) {
        kprintf("[kernel] pid %d (uid %d) does not own that\n",
                caller_pid(), uid);
        return -1;
    }
    return vfs_chmod(path, (uint32_t)(mode & 0xfff)) ? 0 : -1;
}

static int64_t sys_chown(uint64_t ptr, uint64_t len, uint64_t uid,
                         uint64_t gid)
{
    char path[PATH_MAX];
    if (!copy_path_resolved(ptr, len, path, sizeof path)) {
        return -1;
    }
    /*
     * giving a file away is the master's alone. if anybody could, then
     * a quota is a suggestion and so is an owner
     */
    if (process_uid(caller_pid()) != 0) {
        kprintf("[kernel] pid %d (uid %d) may not give files away\n",
                caller_pid(), process_uid(caller_pid()));
        return -1;
    }
    return vfs_chown(path, (uint32_t)uid, (uint32_t)gid) ? 0 : -1;
}

static int64_t sys_symlink(uint64_t ptr, uint64_t len, uint64_t tptr,
                           uint64_t tlen)
{
    char path[PATH_MAX];
    if (!copy_path_resolved(ptr, len, path, sizeof path)) {
        return -1;
    }
    if (tlen == 0 || tlen >= PATH_MAX || !user_range_ok(tptr, tlen)) {
        return -1;
    }
    /*
     * the target is *not* resolved. a symlink holds whatever text it
     * was given and means it wherever it is later read from, which is
     * the difference between a link and a copy of an answer
     */
    char target[PATH_MAX];
    memcpy(target, (const void *)tptr, tlen);
    target[tlen] = '\0';

    if (process_uid(caller_pid()) != 0) {
        return -1;
    }
    return vfs_symlink(path, target) ? 0 : -1;
}

static int64_t sys_readlink(uint64_t ptr, uint64_t len, uint64_t out_ptr,
                            uint64_t out_size)
{
    char path[PATH_MAX];
    if (!copy_path_resolved(ptr, len, path, sizeof path)) {
        return -1;
    }
    if (out_size == 0 || !user_range_writable(out_ptr, out_size)) {
        return -1;
    }
    char target[PATH_MAX];
    if (!vfs_readlink(path, target, sizeof target)) {
        return -1;
    }
    uint64_t n = strlen(target);
    if (n >= out_size) {
        n = out_size - 1;
    }
    memcpy((void *)out_ptr, target, n);
    ((char *)out_ptr)[n] = '\0';
    return (int64_t)n;
}

/*
 * everything a program had until now was decided before it started: its
 * segments and a stack. this is the other way round, it asks, and
 * gets a range of addresses that are its to use.
 *
 * not one page of it is made here. the range is agreed to and the pages
 * arrive as they are touched, so asking for a megabyte and using four
 * bytes of it costs one page. that is not an optimisation bolted on
 * afterwards, it is the only thing that makes asking for a lot
 * reasonable in the first place.
 */
static int64_t sys_mmap(uint64_t len)
{
    struct thread *me = sched_current();
    if (me == NULL || me->space == NULL || len == 0) {
        return 0;
    }
    if (len > MMAP_MAX) {
        return 0;       /* an absurd ask is refused rather than served */
    }
    return (int64_t)addrspace_reserve(me->space, len,
                                      PTE_USER | PTE_WRITE | vmm_nx());
}

static int64_t sys_munmap(uint64_t addr)
{
    struct thread *me = sched_current();
    if (me == NULL || me->space == NULL) {
        return -1;
    }
    /*
     * by the address it was handed out at, not by any address inside
     * it. partial unmapping is a thing unix does and nothing here
     * needs, and pretending to support it would be worse than saying so
     */
    return addrspace_drop_region(me->space, addr) ? 0 : -1;
}


/*
 * one key, unechoed and untranslated, for a program that is painting
 * its own screen. only the foreground may have it, the same rule the
 * line discipline follows, keys belong to whoever is being typed at
 */
/*
 * the child's first instruction as a thread. it has never been in ring
 * 3, so there is nothing to return *from*, it leaves through a copy
 * of the frame its parent was holding, which fork_return restores whole
 * and then sysrets out of
 */
static void fork_child_start(void *arg)
{
    struct user_regs *frame = arg;
    struct user_regs local = *frame;
    kfree(frame);

    /*
     * on this thread's own kernel stack now, which is the one stack
     * that is certainly the kernel's and certainly not going anywhere
     */
    fork_return(&local);
}

static int64_t sys_getkey(void)
{
    if (process_pgid(caller_pid()) != tty_foreground()) {
        return -1;
    }
    if (process_take_interrupt(caller_pid())) {
        return -1;
    }

    /* -1 is the wait ended by an interrupt, with no key taken */
    int key = input_getchar_blocking();
    if (key < 0) {
        process_take_interrupt(caller_pid());
    }
    return key;
}

static int64_t sys_screen(uint64_t cols_ptr, uint64_t rows_ptr)
{
    if (!user_range_writable(cols_ptr, 4) || !user_range_writable(rows_ptr, 4)) {
        return -1;
    }
    size_t c = 0, r = 0;
    console_size(&c, &r, NULL, NULL);

    /*
     * a machine with no framebuffer answers zero, and a program that
     * believed it would divide by it. eighty by twenty-four is what
     * every terminal has been since the vt100 and is a better guess
     * than nothing
     */
    if (c == 0 || r == 0) {
        c = 80;
        r = 24;
    }
    *(uint32_t *)cols_ptr = (uint32_t)c;
    *(uint32_t *)rows_ptr = (uint32_t)r;
    return 0;
}

static int64_t sys_cursor(uint64_t col, uint64_t row)
{
    if (process_pgid(caller_pid()) != tty_foreground()) {
        return -1;
    }
    console_move((size_t)col, (size_t)row);
    return 0;
}

static int64_t sys_clear(void)
{
    if (process_pgid(caller_pid()) != tty_foreground()) {
        return -1;
    }
    console_clear();
    return 0;
}

static int64_t sys_open(uint64_t ptr, uint64_t len)
{
    char path[PATH_MAX];
    if (!copy_path_resolved(ptr, len, path, sizeof path)) {
        return -1;
    }

    /*
     * which filesystem a name means is the vfs's problem now, not this
     * one's. there used to be two branches here and a prefix test
     */
    struct vfs_file f;
    if (!vfs_open(path, &f) || f.is_dir) {
        return -1;
    }

    /*
     * the boundary, in one line. the mode came off the file and the uid
     * off the process, and neither is anything ring 3 can reach in and
     * change
     */
    if (!vfs_may_read(&f, process_uid(caller_pid()))) {
        kprintf("[kernel] pid %d (uid %d) may not read %s\n",
                caller_pid(), process_uid(caller_pid()), path);
        return -1;
    }

    if (f.kind == VFS_DISK) {
        return process_fd_open_disk(caller_pid(), f.cluster, f.size,
                                    f.entry_sector, f.entry_offset, f.mount);
    }
    if (f.kind == VFS_SOURCE) {
        return process_fd_open_source(caller_pid(), f.medium_at, f.size);
    }
    if (f.kind == VFS_PROC) {
        return process_fd_open_proc(caller_pid(), f.sz_proc);
    }
    return process_fd_open(caller_pid(), f.data, f.size);
}

/*
 * make a file on the disk, or open one that is there, for writing. the
 * ramdisk cannot do this and says so, it is a tar file in read-only
 * memory, and there is nowhere for a new file to go
 */
static int64_t sys_create(uint64_t ptr, uint64_t len)
{
    char path[PATH_MAX];
    if (!copy_path_resolved(ptr, len, path, sizeof path)) {
        return -1;
    }
    if (process_uid(caller_pid()) != 0) {
        kprintf("[kernel] pid %d (uid %d) may not write here\n",
                caller_pid(), process_uid(caller_pid()));
        return -1;
    }

    struct vfs_file f;
    if (!vfs_create(path, &f)) {
        return -1;
    }
    return process_fd_open_disk(caller_pid(), f.cluster, f.size,
                                f.entry_sector, f.entry_offset, f.mount);
}


static int64_t sys_chdir(uint64_t ptr, uint64_t len)
{
    char path[PATH_MAX];
    if (!copy_path_resolved(ptr, len, path, sizeof path)) {
        return -1;
    }

    /*
     * it has to exist and it has to be a directory, otherwise every
     * name used afterwards would resolve against somewhere that is not
     * there, and fail for a reason nowhere near the mistake
     */
    struct vfs_file f;
    if (!path_is_root(path)) {
        if (!vfs_open(path, &f) || !f.is_dir) {
            return -1;
        }
    }

    process_set_cwd(caller_pid(), path);
    return 0;
}

static int64_t sys_getcwd(uint64_t ptr, uint64_t len)
{
    if (len == 0 || !user_range_writable(ptr, len)) {
        return -1;
    }
    const char *here = process_cwd(caller_pid());
    uint64_t n = strlen(here);
    if (n >= len) {
        n = len - 1;
    }
    memcpy((void *)ptr, here, n);
    ((char *)ptr)[n] = '\0';
    return (int64_t)n;
}

/*
 * the log is copied into a buffer of the kernel's under the print lock,
 * and only then to the caller: a write to a user page can fault, and a
 * fault is not something to take with every core's printing stopped
 */
static int64_t sys_syslog(uint64_t u_type, uint64_t u_ptr, uint64_t u_len)
{
    if (u_type == SYSLOG_SIZE_BUFFER) {
        return KLOG_SIZE;
    }
    if (u_type != SYSLOG_READ_ALL || !user_range_writable(u_ptr, u_len)) {
        return -1;
    }

    char *str_log = kmalloc(KLOG_SIZE);
    if (str_log == NULL) {
        return -1;
    }
    size_t sz_want = (u_len < KLOG_SIZE) ? (size_t)u_len : KLOG_SIZE;
    size_t sz_got = klog_copy(str_log, sz_want);
    memcpy((void *)u_ptr, str_log, sz_got);
    kfree(str_log);
    return (int64_t)sz_got;
}

static int64_t sys_sync(void)
{
    if (!disk_ready(DISK_ROOT) || !disk_dirty()) {
        return 0;
    }
    return disk_sync() ? 0 : -1;
}

/* which of the disk's filesystems is mounted at `str_path`, or -1 */
static int mounted_at(const char *str_path)
{
    for (size_t sz_which = 0; sz_which < DISK_MOUNTS; sz_which++) {
        if (disk_ready(sz_which)
            && strcmp(disk_mount_point(sz_which), str_path) == 0) {
            return (int)sz_which;
        }
    }
    return -1;
}

/* a reason, cut to what fits, for a program to print */
static void say_why(char *str_why, size_t sz_size, const char *str_reason)
{
    strncpy(str_why, str_reason, sz_size - 1);
    str_why[sz_size - 1] = '\0';
}

static int64_t sys_fsck(uint64_t u_ptr, uint64_t u_len, uint64_t u_flags,
                        uint64_t u_out)
{
    struct user_fsck *p_out = (struct user_fsck *)u_out;
    char arr_path[PATH_MAX];

    if (!user_range_writable(u_out, sizeof *p_out)) {
        return -1;
    }
    memset(p_out, 0, sizeof *p_out);
    if (!copy_path_resolved(u_ptr, u_len, arr_path, sizeof arr_path)) {
        say_why(p_out->arr_why, sizeof p_out->arr_why, "that is not a path");
        return -1;
    }

    bool b_mend = (u_flags & FSCK_MEND) != 0;
    if (b_mend && process_uid(caller_pid()) != 0) {
        say_why(p_out->arr_why, sizeof p_out->arr_why,
                "only the master may mend a filesystem");
        return -1;
    }
    int i_which = mounted_at(arr_path);
    if (i_which < 0) {
        say_why(p_out->arr_why, sizeof p_out->arr_why,
                "no filesystem of the disk's is mounted there");
        return -1;
    }

    struct fsck_report r;
    const char *str_error = NULL;
    if (!disk_fsck((size_t)i_which, b_mend, &r, &str_error)) {
        say_why(p_out->arr_why, sizeof p_out->arr_why,
                str_error != NULL ? str_error : "cannot check it");
        return -1;
    }
    for (int i_kind = 0; i_kind < FSCK_PROBLEMS; i_kind++) {
        p_out->arr_found[i_kind] = r.found[i_kind];
        p_out->arr_mended[i_kind] = r.mended[i_kind];
    }
    p_out->u_total_found = r.total_found;
    p_out->u_total_mended = r.total_mended;
    p_out->u_blocks = r.blocks;
    p_out->u_inodes = r.inodes;
    p_out->u_groups = r.groups;
    p_out->u_block_size = r.block_size;
    p_out->u_free_blocks = r.free_blocks;
    p_out->u_free_inodes = r.free_inodes;
    return 0;
}

static int64_t sys_resize(uint64_t u_ptr, uint64_t u_len, uint64_t u_blocks,
                          uint64_t u_out)
{
    struct user_resize *p_out = (struct user_resize *)u_out;
    char arr_path[PATH_MAX];

    if (!user_range_writable(u_out, sizeof *p_out)) {
        return -1;
    }
    memset(p_out, 0, sizeof *p_out);
    if (!copy_path_resolved(u_ptr, u_len, arr_path, sizeof arr_path)) {
        say_why(p_out->arr_why, sizeof p_out->arr_why, "that is not a path");
        return -1;
    }
    if (process_uid(caller_pid()) != 0) {
        say_why(p_out->arr_why, sizeof p_out->arr_why,
                "only the master may change the size of a filesystem");
        return -1;
    }
    int i_which = mounted_at(arr_path);
    if (i_which < 0) {
        say_why(p_out->arr_why, sizeof p_out->arr_why,
                "no filesystem of the disk's is mounted there");
        return -1;
    }

    size_t sz_which = (size_t)i_which;
    p_out->u_room = disk_room(sz_which);
    p_out->u_ceiling = disk_ceiling(sz_which);
    p_out->u_block_size = disk_cluster_bytes(sz_which);

    /* 0 is as far as the partition and the table both allow */
    if (u_blocks == 0) {
        u_blocks = (p_out->u_room < p_out->u_ceiling) ? p_out->u_room
                                                      : p_out->u_ceiling;
    }

    struct ext4_resize r;
    const char *str_error = NULL;
    bool b_done = disk_resize(sz_which, u_blocks, &r, &str_error);

    p_out->u_blocks_before = r.blocks_before;
    p_out->u_blocks_after = r.blocks_after;
    p_out->u_groups_before = r.groups_before;
    p_out->u_groups_after = r.groups_after;
    p_out->u_inodes_before = r.inodes_before;
    p_out->u_inodes_after = r.inodes_after;
    p_out->u_blocks_in_way = r.blocks_in_way;
    p_out->u_inodes_in_way = r.inodes_in_way;
    if (!b_done) {
        say_why(p_out->arr_why, sizeof p_out->arr_why,
                str_error != NULL ? str_error : "it would not resize");
        return -1;
    }
    return 0;
}

static int64_t sys_source(uint64_t u_flags, uint64_t u_out)
{
    struct user_source *p_out = (struct user_source *)u_out;

    if (!user_range_writable(u_out, sizeof *p_out)) {
        return -1;
    }
    memset(p_out, 0, sizeof *p_out);
    memcpy(p_out->arr_built, source_stamp, sizeof p_out->arr_built);
    p_out->u_built_bytes = source_stamp_bytes;
    if (!source_present()) {
        return -1;
    }
    p_out->u_files = source_count();
    p_out->u_bytes = source_bytes();
    p_out->u_lba = source_lba();

    if ((u_flags & SOURCE_HASH) != 0) {
        uint8_t arr_digest[20];
        if (!source_digest(arr_digest)) {
            return -1;
        }
        sha1_hex(arr_digest, p_out->arr_medium);
    }
    return 0;
}

/*
 * a string of the caller's whose length it did not say, into `str_out`.
 * false when it runs past `sz_size` or off the end of what the caller
 * owns; each page is asked about once, as the walk reaches it
 */
static bool copy_user_string(uint64_t u_str, char *str_out, size_t sz_size)
{
    for (size_t sz_at = 0; sz_at < sz_size; sz_at++) {
        uint64_t u_addr = u_str + sz_at;
        if ((sz_at == 0 || (u_addr & 0xfff) == 0)
            && !user_range_ok(u_addr, 1)) {
            return false;
        }
        str_out[sz_at] = *(const char *)u_addr;
        if (str_out[sz_at] == '\0') {
            return true;
        }
    }
    return false;
}

/* how much of the arguments the kernel carries across, strings and all */
#define EXEC_ARGS_MAX 2048

static int64_t sys_execve(struct user_regs *regs, uint64_t u_ptr,
                          uint64_t u_len, uint64_t u_argv)
{
    char arr_path[PATH_MAX];
    if (regs == NULL
        || !copy_path_resolved(u_ptr, u_len, arr_path, sizeof arr_path)) {
        return -1;
    }

    /*
     * the arguments are copied out before anything else happens, since
     * the memory they are in is about to stop existing
     */
    char *str_args = kmalloc(EXEC_ARGS_MAX);
    if (str_args == NULL) {
        return -1;
    }
    const char *arr_argv[MAX_ARGS];
    int i_argc = 0;
    size_t sz_used = 0;
    bool b_ok = true;

    if (u_argv == 0) {
        arr_argv[i_argc++] = arr_path;
    }
    while (u_argv != 0) {
        uint64_t u_slot = u_argv + (uint64_t)i_argc * 8;
        if (!user_range_ok(u_slot, 8)) {
            b_ok = false;
            break;
        }
        uint64_t u_str = *(const uint64_t *)u_slot;
        if (u_str == 0) {
            break;
        }
        /*
         * refused rather than cut short: a program run with half its
         * arguments does the wrong thing quietly
         */
        if (i_argc == MAX_ARGS
            || !copy_user_string(u_str, str_args + sz_used,
                                 EXEC_ARGS_MAX - sz_used)) {
            b_ok = false;
            break;
        }
        arr_argv[i_argc++] = str_args + sz_used;
        sz_used += strlen(str_args + sz_used) + 1;
    }

    struct user_entry e;
    const char *str_why = NULL;
    if (!b_ok || i_argc == 0
        || !user_exec(arr_path, i_argc, arr_argv, &e, &str_why)) {
        kfree(str_args);
        return -1;
    }
    kfree(str_args);

    /*
     * the frame the exit path restores is the new program's start: its
     * entry, its stack, its arguments where _start looks for them, the
     * flags a fresh program gets, and nothing of the old one's
     */
    memset(regs, 0, sizeof *regs);
    regs->rcx = e.u_entry;
    regs->rsp = e.u_stack_top;
    regs->rdi = e.u_argc;
    regs->rsi = e.u_argv;
    regs->r11 = RFLAGS_IF | 0x2;
    return 0;
}

static int64_t sys_pipe(uint64_t u_fds)
{
    int i_pid = caller_pid();

    if (!user_range_writable(u_fds, 2 * sizeof(int32_t))) {
        return -1;
    }
    struct pipe *p_pipe = pipe_create();
    if (p_pipe == NULL) {
        return -1;
    }

    struct fd end;
    memset(&end, 0, sizeof end);
    end.kind = FD_PIPE;
    end.pipe = p_pipe;
    int i_read = process_fd_place(i_pid, &end);
    end.writing = true;
    int i_write = (i_read >= 0) ? process_fd_place(i_pid, &end) : -1;

    if (i_write < 0) {
        /* no room for both, so neither end was ever the caller's */
        struct pipe_end taken;
        if (i_read >= 0) {
            process_fd_close(i_pid, i_read, &taken);
        }
        pipe_close_read(p_pipe);
        pipe_close_write(p_pipe);
        return -1;
    }
    ((int32_t *)u_fds)[0] = i_read;
    ((int32_t *)u_fds)[1] = i_write;
    return 0;
}

static int64_t sys_dup2(uint64_t u_from, uint64_t u_to)
{
    int i_pid = caller_pid();
    struct fd src;

    if (u_to >= MAX_FDS || !process_fd_get(i_pid, (int)u_from, &src)) {
        return -1;
    }
    if (u_from == u_to) {
        return (int64_t)u_to;
    }

    /* counted before it is installed, so no moment has it held and uncounted */
    if (src.kind == FD_PIPE) {
        pipe_share(src.pipe, src.writing);
    }
    struct pipe_end closing = { NULL, false };
    process_fd_close(i_pid, (int)u_to, &closing);
    let_go(&closing);
    process_fd_install(i_pid, (int)u_to, &src);
    return (int64_t)u_to;
}

static int64_t sys_mkdir(uint64_t ptr, uint64_t len)
{
    char path[PATH_MAX];
    if (!copy_path_resolved(ptr, len, path, sizeof path)) {
        return -1;
    }
    if (process_uid(caller_pid()) != 0) {
        kprintf("[kernel] pid %d (uid %d) may not make directories\n",
                caller_pid(), process_uid(caller_pid()));
        return -1;
    }
    return vfs_mkdir(path) ? 0 : -1;
}

static int64_t sys_rmdir(uint64_t ptr, uint64_t len)
{
    char path[PATH_MAX];
    if (!copy_path_resolved(ptr, len, path, sizeof path)) {
        return -1;
    }
    if (process_uid(caller_pid()) != 0) {
        kprintf("[kernel] pid %d (uid %d) may not remove directories\n",
                caller_pid(), process_uid(caller_pid()));
        return -1;
    }
    return vfs_rmdir(path) ? 0 : -1;
}

static int64_t sys_unlink(uint64_t ptr, uint64_t len)
{
    char path[PATH_MAX];
    if (!copy_path_resolved(ptr, len, path, sizeof path)) {
        return -1;
    }
    if (process_uid(caller_pid()) != 0) {
        kprintf("[kernel] pid %d (uid %d) may not remove files\n",
                caller_pid(), process_uid(caller_pid()));
        return -1;
    }
    return vfs_unlink(path) ? 0 : -1;
}

static int64_t sys_rename(uint64_t from_ptr, uint64_t from_len,
                          uint64_t to_ptr, uint64_t to_len)
{
    char from[PATH_MAX], to[PATH_MAX];
    if (!copy_path_resolved(from_ptr, from_len, from, sizeof from) ||
        !copy_path_resolved(to_ptr, to_len, to, sizeof to)) {
        return -1;
    }
    if (process_uid(caller_pid()) != 0) {
        kprintf("[kernel] pid %d (uid %d) may not rename files\n",
                caller_pid(), process_uid(caller_pid()));
        return -1;
    }
    return vfs_rename(from, to) ? 0 : -1;
}

/*
 * what a file is, without opening it. `ls -l` wants a size and a date
 * for every name in a directory, and opening each one to find out would
 * mean a descriptor per file for information that is in the directory
 * entry the readdir already read
 */
static int64_t sys_stat(uint64_t ptr, uint64_t len, uint64_t out_ptr)
{
    char path[PATH_MAX];
    if (!copy_path_resolved(ptr, len, path, sizeof path)) {
        return -1;
    }
    if (!user_range_writable(out_ptr, sizeof(struct user_stat))) {
        return -1;
    }

    /*
     * without following a symlink at the end. `ls -l` wants the link
     * rather than what it points at, and so does anything about to
     * remove one
     */
    struct vfs_file f;
    if (!vfs_open_nofollow(path, &f)) {
        return -1;
    }
    if (!vfs_may_read(&f, process_uid(caller_pid()))) {
        return -1;
    }

    struct user_stat st;
    memset(&st, 0, sizeof st);
    st.size = f.size;
    st.mode = f.mode;
    st.is_dir = f.is_dir ? 1 : 0;
    st.uid = f.uid;
    st.gid = f.gid;
    st.is_symlink = f.is_symlink ? 1 : 0;
    st.year = f.written.year;
    st.month = f.written.month;
    st.day = f.written.day;
    st.hour = f.written.hour;
    st.minute = f.written.minute;
    st.second = f.written.second;

    memcpy((void *)out_ptr, &st, sizeof st);
    return 0;
}

/*
 * the nth file in the ramdisk, by name. this is the whole of readdir:
 * there are no directories to descend into, so an index and a name is
 * the entire interface. `ls` needed exactly this and nothing else,
 * it was the only thing keeping it inside the kernel
 */
static int64_t sys_readdir(uint64_t index, uint64_t ptr, uint64_t len,
                           uint64_t path_ptr, uint64_t path_len)
{
    if (len == 0 || !user_range_writable(ptr, len)) {
        return -1;
    }

    /*
     * no path means the root, which is where anyone looking around
     * would start. there is only one namespace now, so this needs no
     * idea of which filesystem it is walking
     */
    char path[PATH_MAX] = "/";
    if (path_len > 0
        && !copy_path_resolved(path_ptr, path_len, path, sizeof path)) {
        return -1;
    }
    if (path_len == 0) {
        /* no path means "where the kernel is", which is now a real answer */
        const char *here = process_cwd(caller_pid());
        size_t i = 0;
        while (here[i] != '\0' && i < sizeof path - 1) {
            path[i] = here[i];
            i++;
        }
        path[i] = '\0';
    }

    struct vfs_file f;
    if (!vfs_readdir(path, (size_t)index, &f)) {
        return -1;      /* past the end */
    }

    /*
     * a directory comes back with a trailing slash, which is how
     * everyone has said "this one can be descended into" since long
     * before any of this. no protocol needed
     */
    uint64_t n = strlen(f.name);
    bool slash = f.is_dir && (n == 0 || f.name[n - 1] != '/');
    if (slash) {
        n++;
    }
    if (n >= len) {
        n = len - 1;
    }
    /*
     * FIXME: the subtraction below underflows. when the entry is a
     * directory, slash is true, and a caller whose buffer is one byte
     * clamps n to len - 1, which is zero, so slash ? n - 1 is (size_t)-1
     * and the memcpy copies four exabytes until it runs off the end of
     * the caller's space and faults in ring 0. a one line program that
     * calls readdir on "/" with a one byte buffer halts the machine, and
     * the subtraction happens after the clamp so the clamp cannot save
     * it. clamp the copy length itself, or take the slash off before
     * bounding n against len rather than after.
     */
    memcpy((void *)ptr, f.name, slash ? n - 1 : n);
    if (slash && n > 0) {
        ((char *)ptr)[n - 1] = '/';
    }
    ((char *)ptr)[n] = '\0';
    return (int64_t)n;
}

static int64_t sys_spawn(uint64_t ptr, uint64_t len)
{
    char path[PATH_MAX];
    if (!copy_path_resolved(ptr, len, path, sizeof path)) {
        return -1;
    }
    /*
     * a spawned program gets its own path as argv[0], the way a shell
     * would give it. richer arguments want a syscall that can carry
     * them, which is not this one.
     *
     * it also inherits its uid rather than choosing one: a program that
     * could pick its own user would make the whole idea decorative
     */
    const char *why = NULL;
    const char *argv[1] = { path };
    int pid = user_spawn(path, 1, argv, process_cwd(caller_pid()), caller_pid(),
                         process_uid(caller_pid()), false, &why);
    if (pid == 0) {
        return -1;
    }

    /*
     * the environment goes with it. that is the whole of what an
     * environment is: a thing a child is born holding, which is why
     * setting one is worth doing at all
     */
    {
        /*
         * XXX: one buffer, shared by every core that spawns. two
         * programs starting at the same instant on two cores fill the
         * same bytes, and either can hand the other's environment to its
         * child. it wants to be a local, which is what sys_select does
         * for its handles for exactly this reason, or room made in the
         * child itself.
         */
        static char block[PROC_ENV_MAX];
        size_t n = process_get_env(caller_pid(), block, sizeof block);
        process_set_env(pid, block, n);
    }

    /*
     * nothing else to settle for a child spawned by a program, it
     * inherits its parent's group and keeps the descriptors it was born
     * with, so it goes straight away
     */
    user_start(pid);
    return pid;
}

/*
 * block until a child ends, then hand back how it went. a program may
 * only wait for something it started, otherwise one process could
 * collect another's child, and the exit code would go to the wrong
 * place entirely
 */
static int64_t sys_wait(uint64_t u_pid, uint64_t u_status, uint64_t u_flags)
{
    int i_me = caller_pid();
    int i_pid = (int)u_pid;

    if (i_pid == 0 || i_pid < -1
        || (u_status != 0 && !user_range_writable(u_status, sizeof(int)))) {
        return -1;
    }

    /*
     * this polls every 20ms, as waiting always has here, which nobody
     * will notice and which needs no queue for a child to wake
     */
    for (;;) {
        int i_status = 0;
        bool b_ended = false;
        int i_who = process_reportable(i_me, i_pid,
                                       (u_flags & WUNTRACED) != 0,
                                       &i_status, &b_ended);
        if (i_who < 0) {
            return -1;
        }
        if (i_who > 0) {
            if (b_ended) {
                user_collect(i_who, NULL);
            }
            if (u_status != 0) {
                *(int *)u_status = i_status;
            }
            return i_who;
        }
        if ((u_flags & WNOHANG) != 0) {
            return 0;
        }
        if (process_take_interrupt(i_me)) {
            return -1;
        }
        sleep_ms(20);
    }
}

/*
 * the number is in rax, arguments in rdi rsi rdx rcx (the asm moved r10
 * there for the kernel) and r8. returns into rax
 */
/*
 * three calls over the socket table in net/socket.c. what they do about
 * *pointers* is the same as every other call here: nothing ring 3 hands
 * over is touched until user_range_ok has said the whole span belongs to
 * the caller
 */

static int64_t sys_socket(uint16_t port)
{
    if (!net_is_up()) {
        return -1;
    }
    return net_socket_open(caller_pid(), port);
}

static int64_t sys_sendto(int handle, uint32_t to, uint16_t port,
                          uint64_t buf, uint64_t len)
{
    if (!net_is_up() || len > SOCKET_DATAGRAM) {
        return -1;
    }
    if (!user_range_ok(buf, len)) {
        return -1;
    }
    return net_socket_send(caller_pid(), handle, to, port,
                           (const void *)buf, len) ? (int64_t)len : -1;
}

static int64_t sys_recvfrom(int handle, uint64_t from, uint64_t buf,
                            uint64_t len)
{
    if (!net_is_up() || len > SOCKET_DATAGRAM) {
        return -1;
    }
    if (!user_range_writable(buf, len)
        || !user_range_writable(from, sizeof(struct user_from))) {
        return -1;
    }

    ipv4 who = 0;
    uint16_t whoport = 0;
    int64_t n = net_socket_take(caller_pid(), handle, &who, &whoport,
                                (void *)buf, len);
    if (n < 0) {
        return -1;     /* nothing waiting, which the program sees as
                        * `not yet` rather than as a failure */
    }

    struct user_from *f = (struct user_from *)from;
    f->address = who;
    f->port = whoport;
    f->reserved = 0;
    return n;
}


static int64_t sys_recvwait(int handle, uint64_t from, uint64_t buf,
                            uint64_t len, int64_t timeout)
{
    if (!net_is_up() || len > SOCKET_DATAGRAM) {
        return -1;
    }
    if (!user_range_writable(buf, len)
        || !user_range_writable(from, sizeof(struct user_from))) {
        return -1;
    }

    ipv4 who = 0;
    uint16_t whoport = 0;
    int64_t n = net_socket_wait(caller_pid(), handle, &who, &whoport,
                                (void *)buf, len, timeout);
    if (n < 0) {
        return -1;
    }
    struct user_from *f = (struct user_from *)from;
    f->address = who;
    f->port = whoport;
    f->reserved = 0;
    return n;
}

/* wait on several at once, which is what every server is */
static int64_t sys_select(uint64_t handles, uint64_t count, int64_t timeout,
                          uint64_t ready)
{
    if (count == 0 || count > SOCKET_MAX) {
        return -1;
    }
    if (!user_range_ok(handles, count * sizeof(int32_t))
        || !user_range_writable(ready, count)) {
        return -1;
    }

    /*
     * copied out of user memory before use rather than read in place.
     * the check above says the range is the caller's; it does not stop
     * another thread in the same process rewriting it underneath, and a
     * handle that changes between the check and the use is the oldest
     * trick there is
     */
    int local[SOCKET_MAX];
    bool flags[SOCKET_MAX];
    const int32_t *u = (const int32_t *)handles;
    for (uint64_t i = 0; i < count; i++) {
        local[i] = u[i];
    }

    size_t n = net_socket_ready(caller_pid(), local, count, timeout, flags);

    uint8_t *out = (uint8_t *)ready;
    for (uint64_t i = 0; i < count; i++) {
        out[i] = flags[i] ? 1 : 0;
    }
    return (int64_t)n;
}


static int64_t sys_connect(uint64_t ip, uint64_t port)
{
    if (!net_is_up() || port == 0 || port > 65535) {
        return -1;
    }
    return net_tcp_open(caller_pid(), (ipv4)ip, (uint16_t)port);
}

static int64_t sys_listen(uint64_t port)
{
    if (!net_is_up() || port == 0 || port > 65535) {
        return -1;
    }
    return net_tcp_serve(caller_pid(), (uint16_t)port);
}

static int64_t sys_accept(int handle, int64_t timeout)
{
    return net_tcp_accept(caller_pid(), handle, timeout);
}

static int64_t sys_send(int handle, uint64_t buf, uint64_t len)
{
    if (len == 0 || len > TCP_SEND_BUF || !user_range_ok(buf, len)) {
        return -1;
    }
    return net_tcp_send(caller_pid(), handle, (const void *)buf, len);
}

static int64_t sys_recv(int handle, uint64_t buf, uint64_t len,
                        int64_t timeout)
{
    if (len == 0 || len > TCP_RECV_BUF || !user_range_writable(buf, len)) {
        return -1;
    }
    return net_tcp_recv(caller_pid(), handle, (void *)buf, len, timeout);
}

/*
 * this is where a signal actually happens. everything before it was
 * bookkeeping: a bit set in a word, and a sleeping thread woken so that
 * it comes back here rather than staying asleep.
 *
 * it has to be *here* and not at the moment the signal is raised,
 * because a handler runs in ring 3 with the program's own stack and
 * registers, and the only place the kernel has those to hand, and is
 * about to give them back, is on the way out of a syscall.
 */

/*
 * set the program up to run its handler, and to come back afterwards.
 *
 * the saved registers go on the program's own stack, below the red
 * zone, and the handler is entered as though it had been *called*,
 * with a return address pointing at a stub in the program.
 *
 * that stub has to come from the program because there is nowhere else
 * to put it: the stack is not executable, so the kernel cannot write
 * three instructions there, and there is no page of kernel code mapped
 * into every program to point at either. so `signal()` hands the
 * address over at install time
 */
static bool enter_handler(struct user_regs *regs, int sig, uint64_t handler)
{
    uint64_t tramp = process_trampoline(caller_pid());
    if (tramp == 0 || handler == 0) {
        return false;
    }

    uint64_t sp = regs->rsp;

    /*
     * the red zone: 128 bytes below rsp that a leaf function may be
     * using without having claimed them. writing there would corrupt
     * whatever the program was in the middle of
     */
    sp -= 128;
    sp &= ~(uint64_t)15;

    /*
     * XXX: the frame is checked as readable and then written, and the
     * return address further down is checked the same way. that holds up
     * only because a write from ring 0 to a page that is present and
     * read-only is caught by the page fault handler and turned into a
     * copy on the spot, which is exactly what a forked child's stack
     * pages are until it writes to them itself. tighten the fault
     * handler so that only ring 3 faults consult the regions, and this
     * becomes a kernel panic the first time a child is signalled.
     */
    sp -= sizeof(struct user_regs);
    uint64_t frame = sp;
    if (!user_range_ok(frame, sizeof(struct user_regs))) {
        return false;
    }
    memcpy((void *)frame, regs, sizeof *regs);

    /*
     * and the return address, which leaves rsp eight past a sixteen
     * boundary, exactly what a handler compiled as an ordinary
     * function expects to find on entry
     */
    sp -= 8;
    if (!user_range_ok(sp, 8)) {
        return false;
    }
    *(uint64_t *)sp = tramp;

    regs->rsp = sp;
    regs->rdi = (uint64_t)sig;      /* the handler's argument */
    regs->rcx = handler;            /* where `sysret` will go */
    return true;
}

void syscall_end_if_fatal(void)
{
    int i_pid = caller_pid();
    int i_sig = (i_pid > 0) ? process_take_fatal(i_pid) : 0;

    if (i_sig != 0) {
        process_died_by_signal(i_pid, i_sig);
        thread_exit(128 + i_sig);       /* never returns */
    }
}

static void deliver_signals(struct user_regs *regs)
{
    int pid = caller_pid();
    if (pid <= 0) {
        return;
    }

    for (;;) {
        int sig = 0;
        uint64_t handler = 0;
        enum signal_action a = process_take_signal(pid, &sig, &handler);

        switch (a) {
        case SIGNAL_ACTION_NONE:
            return;

        case SIGNAL_ACTION_IGNORE:
            continue;           /* and look for the next one */

        case SIGNAL_ACTION_STOP:
            /* a stop that was blocked when sent, and is not any more */
            process_stopped(pid, sig);
            sched_set_stopped(process_thread(pid), true);
            sched_yield();
            continue;

        case SIGNAL_ACTION_CONTINUE:
            sched_set_stopped(process_thread(pid), false);
            continue;

        case SIGNAL_ACTION_HANDLER:
            if (regs != NULL && enter_handler(regs, sig, handler)) {
                return;         /* one at a time: the rest wait */
            }
            /*
             * no trampoline, or a stack that could not be written.
             * falling through to the default is the only honest answer
             *, pretending the handler ran would be worse
             */
            /* fall through */

        case SIGNAL_ACTION_TERMINATE:
            process_died_by_signal(pid, sig);
            thread_exit(128 + sig);     /* never returns */
        }
    }
}

/*
 * install one. the trampoline comes with it rather than being a call of
 * its own, so that a program cannot arrange to have handlers without
 * having somewhere for them to return to
 */
/*
 * a signal goes only where the sender has a say: to its own processes,
 * or to anybody's when the sender is root. a guest able to stop root's
 * programs, init among them, is a guest able to stop the machine
 */
static bool may_signal(int i_uid, int i_pid)
{
    const struct process *p_target = process_find(i_pid);
    return p_target != NULL && !p_target->exited
           && (i_uid == 0 || i_uid == p_target->uid);
}

/*
 * a pid, or a whole group as minus its pgid, which reaches every member
 * the sender may signal. -1 is not a group: elsewhere it means everyone,
 * and this kernel does not offer that
 */
static int64_t sys_sigsend(uint64_t u_pid, uint64_t u_sig)
{
    int i_pid = (int)u_pid;
    int i_uid = process_uid(caller_pid());
    bool b_any = false;

    if (i_pid >= 0) {
        b_any = may_signal(i_uid, i_pid) && process_signal(i_pid, (int)u_sig);
    } else if (i_pid != -1) {
        int arr_pids[MAX_PROCESSES];
        size_t sz_n = process_group_pids(-i_pid, arr_pids, MAX_PROCESSES);
        for (size_t sz_at = 0; sz_at < sz_n; sz_at++) {
            if (may_signal(i_uid, arr_pids[sz_at])) {
                b_any |= process_signal(arr_pids[sz_at], (int)u_sig);
            }
        }
    }

    /* a sender that stopped itself stops here, not some instructions on */
    struct thread *p_me = sched_current();
    if (p_me != NULL && p_me->stopped) {
        sched_yield();
    }
    return b_any ? 0 : -1;
}

/*
 * put a process in a group: the caller or a child of its own, into a
 * group named after it or one that already has a member of the same
 * user. 0 for the pid means the caller and 0 for the group means the
 * pid's own
 */
static int64_t sys_setpgid(uint64_t u_pid, uint64_t u_pgid)
{
    int i_me = caller_pid();
    int i_pid = (u_pid == 0) ? i_me : (int)u_pid;
    int i_pgid = (u_pgid == 0) ? i_pid : (int)u_pgid;
    const struct process *p_target = process_find(i_pid);

    if (p_target == NULL || p_target->exited || i_pgid <= 0
        || (i_pid != i_me && p_target->parent != i_me)) {
        return -1;
    }
    if (i_pgid != i_pid) {
        int i_member = 0;
        if (process_group_pids(i_pgid, &i_member, 1) == 0
            || process_uid(i_member) != process_uid(i_me)) {
            return -1;
        }
    }
    process_set_pgid(i_pid, i_pgid);
    return 0;
}

static int64_t sys_signal(uint64_t sig, uint64_t handler, uint64_t tramp)
{
    struct signal_state *st = process_signal_state(caller_pid());
    if (st == NULL) {
        return -1;
    }
    if (handler != SIG_DEFAULT && handler != SIG_IGNORE) {
        if (!user_range_ok(handler, 1) || tramp == 0
            || !user_range_ok(tramp, 1)) {
            return -1;
        }
        process_set_trampoline(caller_pid(), tramp);
    }
    return signal_set_handler(st, (int)sig, handler) ? 0 : -1;
}

/*
 * a handler has returned. the saved registers go back exactly as they
 * were, so the program carries on from wherever it was interrupted,
 * which may be in the middle of a syscall that has to be retried, and
 * that is the program's problem to notice
 */
static int64_t sys_sigreturn(uint64_t frame, struct user_regs *regs)
{
    struct signal_state *st = process_signal_state(caller_pid());
    if (st == NULL || regs == NULL
     || !user_range_ok(frame, sizeof(struct user_regs))) {
        return -1;
    }
    memcpy(regs, (const void *)frame, sizeof *regs);
    signal_handler_returned(st);
    return (int64_t)regs->rdi;      /* whatever rax will be overwritten by */
}

static int64_t syscall_do(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2,
                          uint64_t a3, uint64_t a4, struct user_regs *regs)
{
    switch (nr) {
    case SYS_EXIT:
        thread_exit((int)a0);   /* never returns */
    case SYS_WRITE:
        return sys_write(a0, a1, a2);
    case SYS_READ:
        return sys_read(a0, a1, a2);
    case SYS_OPEN:
        return sys_open(a0, a1);
    case SYS_CLOSE:
        return sys_close((int)a0);
    case SYS_SOCKET:
        return sys_socket((uint16_t)a0);
    case SYS_SENDTO:
        return sys_sendto((int)a0, (uint32_t)a1, (uint16_t)a2, a3, a4);
    case SYS_RECVFROM:
        return sys_recvfrom((int)a0, a1, a2, a3);
    case SYS_RECVWAIT:
        return sys_recvwait((int)a0, a1, a2, a3, (int64_t)a4);
    case SYS_SELECT:
        return sys_select(a0, a1, (int64_t)a2, a3);
    case SYS_CONNECT:
        return sys_connect(a0, a1);
    case SYS_LISTEN:
        return sys_listen(a0);
    case SYS_ACCEPT:
        return sys_accept((int)a0, (int64_t)a1);
    case SYS_SEND:
        return sys_send((int)a0, a1, a2);
    case SYS_RECV:
        return sys_recv((int)a0, a1, a2, (int64_t)a3);
    case SYS_SHUTDOWN:
        return net_tcp_shut(caller_pid(), (int)a0) ? 0 : -1;
    case SYS_SIGNAL:
        return sys_signal(a0, a1, a2);
    case SYS_SIGSEND:
        return sys_sigsend(a0, a1);
    case SYS_SIGRETURN:
        return sys_sigreturn(a0, regs);
    case SYS_TTYMODE: {
        /*
         * three flags in one number, and set and get in one call. a
         * program that wants raw mode has to put the old one back
         * afterwards, so being handed the previous value by the call
         * that changes it is what makes that possible without a second
         * syscall and a race between them
         */
        struct term_mode was = tty_get_mode();
        int64_t previous = (was.raw ? TTY_RAW : 0) | (was.echo ? TTY_ECHO : 0)
                         | (was.signals ? TTY_SIGNALS : 0);
        if (a0 != 0) {
            struct term_mode m = {
                .raw     = (a1 & TTY_RAW) != 0,
                .echo    = (a1 & TTY_ECHO) != 0,
                .signals = (a1 & TTY_SIGNALS) != 0,
            };
            tty_set_mode(&m);
        }
        return previous;
    }
    case SYS_NOW:
        return (int64_t)epoch_now();
    case SYS_ALARM:
        /*
         * the deadline is in uptime rather than in wall-clock seconds.
         * an alarm is "wake the kernel in five seconds" and must survive
         * somebody setting the date, which a wall-clock deadline
         * would not
         */
        return (int64_t)process_set_alarm(caller_pid(), a0, pit_uptime_ms());
    case SYS_WINSIZE: {
        /*
         * FIXME: a0 is checked as readable and then written to a few
         * lines down; a1, beside it, uses the check that is right. a
         * program handing over a pointer into its own .rodata has the
         * kernel write through it, and that survives only because a
         * supervisor write to a read-only page is caught by the fault
         * handler and quietly turned into a copy. sys_screen above does
         * this same pair correctly, which is what makes this a slip
         * rather than a decision.
         */
        if (!user_range_ok(a0, sizeof(uint32_t))
         || !user_range_writable(a1, sizeof(uint32_t))) {
            return -1;
        }
        size_t c = 0, r = 0;
        console_size(&c, &r, NULL, NULL);
        *(uint32_t *)a0 = (uint32_t)c;
        *(uint32_t *)a1 = (uint32_t)r;
        return 0;
    }
    case SYS_SIGMASK: {
        struct signal_state *st = process_signal_state(caller_pid());
        if (st == NULL) {
            return -1;
        }
        uint32_t was = signal_get_mask(st);
        if (a0 != 0) {          /* 0 asks without setting */
            signal_set_mask(st, (uint32_t)a1);
        }
        return (int64_t)was;
    }
    case SYS_GETPID:
        return caller_pid();
    case SYS_SPAWN:
        return sys_spawn(a0, a1);
    case SYS_WAIT:
        return sys_wait(a0, a1, a2);
    case SYS_READDIR:
        return sys_readdir(a0, a1, a2, a3, a4);
    case SYS_CREATE:
        return sys_create(a0, a1);
    case SYS_CHDIR:
        return sys_chdir(a0, a1);
    case SYS_GETCWD:
        return sys_getcwd(a0, a1);
    case SYS_MKDIR:
        return sys_mkdir(a0, a1);
    case SYS_RMDIR:
        return sys_rmdir(a0, a1);
    case SYS_UNLINK:
        return sys_unlink(a0, a1);
    case SYS_RENAME:
        return sys_rename(a0, a1, a2, a3);
    case SYS_STAT:
        return sys_stat(a0, a1, a2);
    case SYS_GETKEY:
        return sys_getkey();
    case SYS_SCREEN:
        return sys_screen(a0, a1);
    case SYS_CURSOR:
        return sys_cursor(a0, a1);
    case SYS_CLEAR:
        return sys_clear();
    case SYS_FORK:
        return sys_fork(regs);
    case SYS_MMAP:
        return sys_mmap(a0);
    case SYS_MUNMAP:
        return sys_munmap(a0);
    case SYS_CHMOD:
        return sys_chmod(a0, a1, a2);
    case SYS_CHOWN:
        return sys_chown(a0, a1, a2, a3);
    case SYS_SYMLINK:
        return sys_symlink(a0, a1, a2, a3);
    case SYS_READLINK:
        return sys_readlink(a0, a1, a2, a3);
    case SYS_GETENV:
        return sys_getenv(a0, a1, a2, a3);
    case SYS_SETENV:
        return sys_setenv(a0, a1, a2, a3);
    case SYS_GETUID:
        return process_uid(caller_pid());
    case SYS_UPTIME:
        return (int64_t)pit_uptime_ms();
    case SYS_SYSLOG:
        return sys_syslog(a0, a1, a2);
    case SYS_SYNC:
        return sys_sync();
    case SYS_FSCK:
        return sys_fsck(a0, a1, a2, a3);
    case SYS_RESIZE:
        return sys_resize(a0, a1, a2, a3);
    case SYS_SOURCE:
        return sys_source(a0, a1);
    case SYS_EXECVE:
        return sys_execve(regs, a0, a1, a2);
    case SYS_PIPE:
        return sys_pipe(a0);
    case SYS_DUP2:
        return sys_dup2(a0, a1);
    case SYS_SETPGID:
        return sys_setpgid(a0, a1);
    case SYS_TTYATTACH:
        return (process_uid(caller_pid()) == 0
                && tty_attach(caller_pid(), (unsigned)a0)) ? 0 : -1;
    case SYS_TCSETPGRP:
        return tty_hand_to(caller_pid(), (int)a0) ? 0 : -1;
    case SYS_TCGETPGRP:
        return tty_foreground();
    case SYS_LSEEK:
        return process_fd_seek(caller_pid(), (int)a0, (int64_t)a1, (int)a2);
    case SYS_REBOOT:
        if (process_uid(caller_pid()) != 0
            || (a0 != REBOOT_RESTART && a0 != REBOOT_POWER_OFF)) {
            return -1;
        }
        init_stop_machine(a0 == REBOOT_RESTART ? INIT_REBOOT
                                               : INIT_POWEROFF);
    case SYS_SETUID: {
        int i_uid = process_uid(caller_pid());
        if ((int)a0 < 0 || (i_uid != 0 && (int)a0 != i_uid)) {
            return -1;
        }
        process_set_uid(caller_pid(), (int)a0);
        return 0;
    }
    case SYS_YIELD:
        sched_yield();
        return 0;
    case SYS_SLEEP:
        sleep_ms(a0);
        /*
         * an interrupt wakes a sleeper early, and it should be able to
         * tell that is what happened rather than think time passed
         */
        return process_take_interrupt(caller_pid()) ? -1 : 0;
    default:
        kprintf("[kernel] thread asked for syscall %lu, which does not exist\n",
                nr);
        return -1;
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

int64_t syscall_dispatch(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2,
                         uint64_t a3, uint64_t a4, struct user_regs *regs)
{
    if (nr < SYSCALL_COUNT) {
        call_counts[nr]++;
    }

    /*
     * anything whose alarm has come due, before the delivery below,
     * so a syscall that takes a while and crosses a deadline delivers
     * the signal on its way out rather than on the next call
     */
    process_check_alarms(pit_uptime_ms());

    int64_t result = syscall_do(nr, a0, a1, a2, a3, a4, regs);

    /*
     * and then, on the way out, whatever has been raised since.
     *
     * here rather than at the moment a signal is raised, because a
     * handler runs in ring 3 on the program's own stack, and the only
     * place the kernel has those to hand, and is about to give them
     * back, is exactly here.
     *
     * sigreturn is the one call that must not be followed by this: it
     * has just put the interrupted registers back, and delivering
     * another signal on top would push a second frame over the first
     * before the program has run a single instruction of what it was
     * doing
     */
    if (nr != SYS_SIGRETURN) {
        deliver_signals(regs);
    }
    return result;
}
