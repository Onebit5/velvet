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
#include <asm/fpu.h>
#include <asm/uaccess.h>
#include <uapi/velvet/errno.h>
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
#include <velvet/init.h>
#include <velvet/path.h>
#include <velvet/tty.h>
#include <velvet/console.h>
#include <velvet/input.h>
#include <velvet/pipe.h>
#include <velvet/file.h>
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

/*
 * the flags a program may hand back through sigreturn: the arithmetic
 * ones, direction, trap, resume and alignment check. iopl and the rest
 * stay the kernel's
 */
#define RFLAGS_USER 0x50dd5ull

/* implemented in syscall.asm */
extern void syscall_entry(void);

static int caller_pid(void);

/*
 * a string the kernel holds, handed to a program's buffer of u_size,
 * cut short to fit with its NUL kept: its length as handed over, or -1
 */
static int64_t copy_string_out(uint64_t u_out, uint64_t u_size,
                               const char *str_from)
{
    if (u_size == 0) {
        return -1;
    }
    uint64_t u_len = strlen(str_from);
    if (u_len >= u_size) {
        u_len = u_size - 1;
    }
    if (copy_to_user(u_out, str_from, u_len) != 0
        || clear_user(u_out + u_len, 1) != 0) {
        return -1;
    }
    return (int64_t)u_len;
}

#define WRITE_MAX 4096

/* how much of a read goes through the kernel at a time */
#define READ_CHUNK 4096

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
    if (len == 0 || len >= max || copy_from_user(out, ptr, len) != 0) {
        return false;
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

/*
 * a file just opened, at the caller's lowest free descriptor: which, or
 * -1 with the file let go of when there is none, or no file at all
 */
static int64_t install_file(struct file *p_file)
{
    if (p_file == NULL) {
        return -1;
    }
    int i_fd = process_fd_add(caller_pid(), p_file, 0, false);
    if (i_fd < 0) {
        file_put(p_file);
        return -1;
    }
    return i_fd;
}

/*
 * the whole of writing: one lookup and a call through the file. a
 * program asks for descriptor 1 and gets whatever was put there before
 * it started, the console, a pipe, or a file, and cannot tell which.
 * that is the entire reason `echo hi`, `echo hi | wc` and `echo hi > x`
 * are the same program doing the same thing
 */
static int64_t sys_write(uint64_t fd, uint64_t ptr, uint64_t len)
{
    struct file *p_file = process_fd_file(caller_pid(), (int)fd);
    if (p_file == NULL) {
        return -1;
    }
    if (!(p_file->u_mode & FILE_WRITE) || p_file->p_ops->write == NULL) {
        file_put(p_file);
        return -1;      /* opened to be read, or a thing that only reads */
    }

    /*
     * clamp first, then copy what the kernel clamped to. a program asking
     * to write four exabytes gets a short write rather than a refusal,
     * which is the ordinary contract, and an absurd length can never
     * widen what the kernel is willing to read
     */
    if (len > WRITE_MAX) {
        len = WRITE_MAX;
    }

    /*
     * the console belongs to whoever is at the front of the terminal,
     * exactly the way the keyboard does. a job put in the background
     * with `&` printing over the prompt is the noise that `&` was
     * supposed to spare you, so it is dropped rather than refused: a
     * write that quietly went nowhere is what a terminal nobody is
     * watching actually is. descriptor 2 is the exception, so that a job
     * that failed in the background still gets to say so
     */
    if (p_file->e_type == FILE_TYPE_TTY && fd != FD_STDERR
        && process_pgid(caller_pid()) != tty_foreground()) {
        file_put(p_file);
        return (int64_t)len;
    }

    char *p_buf = kmalloc(WRITE_MAX);
    int64_t n = -1;
    if (p_buf != NULL && copy_from_user(p_buf, ptr, len) == 0) {
        n = p_file->p_ops->write(p_file, p_buf, len);
    }
    kfree(p_buf);
    file_put(p_file);

    if (n == -EPIPE) {
        /*
         * nobody is reading and nobody ever will be. unix raises SIGPIPE
         * here and the default disposition is to die, and this ends the
         * process for it, which gets to the same place by a shorter road.
         *
         * this is not tidiness. it is what makes `cat huge | head` stop:
         * head has seen enough and gone, and without this cat would sit
         * blocked on a buffer that will never drain, holding the
         * terminal, forever
         */
        thread_exit(PROCESS_KILLED);
    }
    return (n < 0) ? -1 : n;
}

/*
 * a read a piece at a time through the kernel's own buffer. a regular
 * file is read until the caller has what it asked for or the file ends,
 * the way a program expects a file to be read; a pipe or the keyboard
 * answers with what there is. the terminal does the echoing and the
 * line editing, because a program in ring 3 cannot, the keys never pass
 * through it
 */
static int64_t sys_read(uint64_t fd, uint64_t ptr, uint64_t len)
{
    if (len == 0 || !access_ok(ptr, len)) {
        return -1;
    }
    struct file *p_file = process_fd_file(caller_pid(), (int)fd);
    if (p_file == NULL) {
        return -1;
    }
    if (!(p_file->u_mode & FILE_READ) || p_file->p_ops->read == NULL) {
        file_put(p_file);
        return -1;
    }

    char *p_buf = kmalloc(READ_CHUNK);
    uint64_t u_done = 0;
    int64_t i_result = (p_buf != NULL) ? 0 : -1;
    while (p_buf != NULL && u_done < len) {
        uint64_t u_want = len - u_done;
        if (u_want > READ_CHUNK) {
            u_want = READ_CHUNK;
        }
        int64_t i_got = p_file->p_ops->read(p_file, p_buf, u_want);
        if (i_got <= 0) {
            i_result = i_got;
            break;
        }
        if (copy_to_user(ptr + u_done, p_buf, (size_t)i_got) != 0) {
            i_result = -1;
            break;
        }
        u_done += (uint64_t)i_got;
        if (p_file->e_type != FILE_TYPE_REGULAR || (uint64_t)i_got < u_want) {
            break;
        }
    }
    kfree(p_buf);
    file_put(p_file);
    if (u_done > 0) {
        return (int64_t)u_done;
    }
    return (i_result < 0) ? -1 : 0;
}

/*
 * the descriptor goes at once, and the file with the last descriptor on
 * it, anywhere: a pipe's end closes when every copy of it a fork or a
 * dup made has closed too
 */
static int64_t sys_close(int fd)
{
    struct file *p_file = process_fd_take(caller_pid(), fd);
    if (p_file == NULL) {
        return -1;
    }
    file_put(p_file);
    return 0;
}

static int64_t sys_lseek(int fd, int64_t i_offset, int i_whence)
{
    struct file *p_file = process_fd_file(caller_pid(), fd);
    if (p_file == NULL) {
        return -1;
    }
    int64_t i_to = (p_file->p_ops->seek != NULL)
                 ? p_file->p_ops->seek(p_file, i_offset, i_whence) : -1;
    file_put(p_file);
    return (i_to < 0) ? -1 : i_to;
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

static int64_t sys_fork(struct pt_regs *regs)
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
    struct pt_regs *frame = kmalloc(sizeof *frame);
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
        process_fds_release(pid);
        process_collect(pid, NULL);
        return -1;
    }

    struct thread *t = thread_create_parked(process_name(pid),
                                            fork_child_start, frame);
    if (t == NULL) {
        kfree(frame);
        addrspace_destroy(space);
        process_exited(pid, PROCESS_KILLED, pit_uptime_ms());
        process_fds_release(pid);
        process_collect(pid, NULL);
        return -1;
    }
    t->space = space;
    t->pid = pid;

    /*
     * and the registers the frame does not hold: the fpu's as they are
     * this moment, which is as they were at the call, since the kernel
     * touches none of them, and the same fs base
     */
    fpu_save(&t->arch.fpu);
    t->arch.u_fs_base = me->arch.u_fs_base;
    process_set_thread(pid, t->id);

    /*
     * released only now, with the group, the descriptors and the space
     * all settled, the same reason spawn parks its threads
     */
    sched_wake_thread(t->id);

    /*
     * and the two answers. the parent's is the return value below; the
     * child's is the zero fork_child_start writes into its frame's rax
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
    char name[ENV_NAME_MAX];
    if (len == 0 || len >= ENV_NAME_MAX || out_size == 0
        || copy_from_user(name, ptr, len) != 0) {
        return -1;
    }
    name[len] = '\0';

    char value[ENV_VALUE_MAX];
    if (!process_env_get(caller_pid(), name, value, sizeof value)) {
        return -1;      /* not set, which is not the same as set to nothing */
    }

    return copy_string_out(out_ptr, out_size, value);
}

static int64_t sys_setenv(uint64_t ptr, uint64_t len, uint64_t vptr,
                          uint64_t vlen)
{
    char name[ENV_NAME_MAX];
    if (len == 0 || len >= ENV_NAME_MAX
        || copy_from_user(name, ptr, len) != 0) {
        return -1;
    }
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
    char value[ENV_VALUE_MAX];
    if (vlen >= ENV_VALUE_MAX || copy_from_user(value, vptr, vlen) != 0) {
        return -1;
    }
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
    char target[PATH_MAX];
    if (tlen == 0 || tlen >= PATH_MAX
        || copy_from_user(target, tptr, tlen) != 0) {
        return -1;
    }
    /*
     * the target is *not* resolved. a symlink holds whatever text it
     * was given and means it wherever it is later read from, which is
     * the difference between a link and a copy of an answer
     */
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
    char target[PATH_MAX];
    if (out_size == 0 || !vfs_readlink(path, target, sizeof target)) {
        return -1;
    }
    return copy_string_out(out_ptr, out_size, target);
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
 * on the way out
 */
static void fork_child_start(void *arg)
{
    struct pt_regs *frame = arg;
    struct pt_regs local = *frame;
    kfree(frame);
    local.rax = 0;                  /* "you are the child" */

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
    uint32_t u_cols = (uint32_t)c;
    uint32_t u_rows = (uint32_t)r;
    if (copy_to_user(cols_ptr, &u_cols, sizeof u_cols) != 0
        || copy_to_user(rows_ptr, &u_rows, sizeof u_rows) != 0) {
        return -1;
    }
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

    /*
     * for reading, and only reading: the check above is the one that was
     * made, so it is the one the descriptor carries
     */
    return install_file(vfs_open_file(&f, FILE_READ));
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
    return install_file(vfs_open_file(&f, FILE_READ | FILE_WRITE));
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
    return copy_string_out(ptr, len, process_cwd(caller_pid()));
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
    if (u_type != SYSLOG_READ_ALL || !access_ok(u_ptr, u_len)) {
        return -1;
    }

    char *str_log = kmalloc(KLOG_SIZE);
    if (str_log == NULL) {
        return -1;
    }
    size_t sz_want = (u_len < KLOG_SIZE) ? (size_t)u_len : KLOG_SIZE;
    size_t sz_got = klog_copy(str_log, sz_want);
    int64_t i_copied = copy_to_user(u_ptr, str_log, sz_got);
    kfree(str_log);
    return (i_copied == 0) ? (int64_t)sz_got : -1;
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

static int64_t resize_into(uint64_t u_ptr, uint64_t u_len, uint64_t u_blocks,
                           struct user_resize *p_out)
{
    char arr_path[PATH_MAX];

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

/* the answer is written out whether it worked or not, since it says why */
static int64_t sys_resize(uint64_t u_ptr, uint64_t u_len, uint64_t u_blocks,
                          uint64_t u_out)
{
    struct user_resize out;
    if (!access_ok(u_out, sizeof out)) {
        return -1;
    }
    int64_t i_result = resize_into(u_ptr, u_len, u_blocks, &out);
    return (copy_to_user(u_out, &out, sizeof out) == 0) ? i_result : -1;
}

/*
 * a string of the caller's whose length it did not say, into `str_out`.
 * false when it runs past `sz_size` or off the end of what the caller
 * owns; each page is asked about once, as the walk reaches it
 */
static bool copy_user_string(uint64_t u_str, char *str_out, size_t sz_size)
{
    return strncpy_from_user(str_out, u_str, sz_size) >= 0;
}

/* how much of the arguments the kernel carries across, strings and all */
#define EXEC_ARGS_MAX 2048

static int64_t sys_execve(struct pt_regs *regs, uint64_t u_ptr,
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
        uint64_t u_str = 0;
        if (get_user_u64(&u_str, u_argv + (uint64_t)i_argc * 8) != 0) {
            b_ok = false;
            break;
        }
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
    process_fds_exec(caller_pid());

    /*
     * the frame the exit path restores is the new program's start: its
     * entry, its stack, its arguments where _start looks for them, the
     * flags a fresh program gets, and nothing of the old one's. that
     * goes for the fpu and the fs base as well
     */
    struct thread *me = sched_current();
    fpu_init_state(&me->arch.fpu);
    fpu_restore(&me->arch.fpu);
    me->arch.u_fs_base = 0;
    fs_base_load(0);

    memset(regs, 0, sizeof *regs);
    regs->rip = e.u_entry;
    regs->rsp = e.u_stack_top;
    regs->rdi = e.u_argc;
    regs->rsi = e.u_argv;
    regs->rflags = RFLAGS_IF | 0x2;
    regs->cs = GDT_USER_CODE3;
    regs->ss = GDT_USER_DATA3;
    return 0;
}

static int64_t sys_pipe(uint64_t u_fds)
{
    int i_pid = caller_pid();

    if (!access_ok(u_fds, 2 * sizeof(int32_t))) {
        return -1;
    }
    struct file *p_read = NULL;
    struct file *p_write = NULL;
    if (!pipe_open(&p_read, &p_write)) {
        return -1;
    }
    int i_read = process_fd_add(i_pid, p_read, 0, false);
    int i_write = (i_read >= 0) ? process_fd_add(i_pid, p_write, 0, false)
                                : -1;
    if (i_write < 0) {
        /* no room for both, so neither end was ever the caller's */
        if (i_read >= 0) {
            process_fd_take(i_pid, i_read);
        }
        file_put(p_read);
        file_put(p_write);
        return -1;
    }
    int32_t arr_fds[2] = { i_read, i_write };
    if (copy_to_user(u_fds, arr_fds, sizeof arr_fds) != 0) {
        /* nowhere to say which they are, so they are no use to anybody */
        sys_close(i_read);
        sys_close(i_write);
        return -1;
    }
    return 0;
}

/*
 * another descriptor on the same file, closing whatever `to` held: the
 * two share a position from here on, as two descriptors on one open
 * file always do
 */
static int64_t sys_dup2(uint64_t u_from, uint64_t u_to)
{
    int i_pid = caller_pid();

    if (u_to >= MAX_FDS) {
        return -1;
    }
    struct file *p_file = process_fd_file(i_pid, (int)u_from);
    if (p_file == NULL) {
        return -1;
    }
    if (u_from == u_to) {
        file_put(p_file);
        return (int64_t)u_to;
    }
    struct file *p_old = NULL;
    process_fd_replace(i_pid, (int)u_to, p_file, false, &p_old);
    file_put(p_old);
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
    if (!access_ok(out_ptr, sizeof(struct user_stat))) {
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

    return (copy_to_user(out_ptr, &st, sizeof st) == 0) ? 0 : -1;
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
    if (len == 0 || !access_ok(ptr, len)) {
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
    char arr_out[VFS_NAME_MAX + 2];
    memcpy(arr_out, f.name, slash ? n - 1 : n);
    if (slash && n > 0) {
        arr_out[n - 1] = '/';
    }
    arr_out[n] = '\0';
    return (copy_to_user(ptr, arr_out, n + 1) == 0) ? (int64_t)n : -1;
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
        || (u_status != 0 && !access_ok(u_status, sizeof(int)))) {
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
            if (u_status != 0 && put_user_i32(u_status, i_status) != 0) {
                return -1;
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
 * three calls over the socket table in net/socket.c. what they do about
 * *pointers* is the same as every other call here: what ring 3 hands
 * over is copied in or out through uaccess, never read where it lies
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
    uint8_t arr_data[SOCKET_DATAGRAM];
    if (copy_from_user(arr_data, buf, len) != 0) {
        return -1;
    }
    return net_socket_send(caller_pid(), handle, to, port,
                           arr_data, len) ? (int64_t)len : -1;
}

/* a datagram taken off a socket, and who sent it, handed to ring 3 */
static int64_t deliver_datagram(uint64_t u_from, ipv4 who, uint16_t u_port,
                                uint64_t u_buf, const uint8_t *p_data,
                                int64_t i_len)
{
    struct user_from from = { .address = who, .port = u_port,
                              .reserved = 0 };
    if (copy_to_user(u_buf, p_data, (size_t)i_len) != 0
        || copy_to_user(u_from, &from, sizeof from) != 0) {
        return -1;
    }
    return i_len;
}

static int64_t sys_recvfrom(int handle, uint64_t from, uint64_t buf,
                            uint64_t len)
{
    if (!net_is_up() || len > SOCKET_DATAGRAM) {
        return -1;
    }
    if (!access_ok(buf, len) || !access_ok(from, sizeof(struct user_from))) {
        return -1;
    }

    ipv4 who = 0;
    uint16_t whoport = 0;
    uint8_t arr_data[SOCKET_DATAGRAM];
    int64_t n = net_socket_take(caller_pid(), handle, &who, &whoport,
                                arr_data, len);
    if (n < 0) {
        return -1;     /* nothing waiting, which the program sees as
                        * `not yet` rather than as a failure */
    }

    return deliver_datagram(from, who, whoport, buf, arr_data, n);
}


static int64_t sys_recvwait(int handle, uint64_t from, uint64_t buf,
                            uint64_t len, int64_t timeout)
{
    if (!net_is_up() || len > SOCKET_DATAGRAM) {
        return -1;
    }
    if (!access_ok(buf, len) || !access_ok(from, sizeof(struct user_from))) {
        return -1;
    }

    ipv4 who = 0;
    uint16_t whoport = 0;
    uint8_t arr_data[SOCKET_DATAGRAM];
    int64_t n = net_socket_wait(caller_pid(), handle, &who, &whoport,
                                arr_data, len, timeout);
    if (n < 0) {
        return -1;
    }
    return deliver_datagram(from, who, whoport, buf, arr_data, n);
}

/* wait on several at once, which is what every server is */
static int64_t sys_select(uint64_t handles, uint64_t count, int64_t timeout,
                          uint64_t ready)
{
    if (count == 0 || count > SOCKET_MAX) {
        return -1;
    }
    /*
     * copied out of user memory before use rather than read in place, so
     * that nothing rewriting it underneath can change a handle between
     * the check and the use, the oldest trick there is
     */
    int32_t arr_handles[SOCKET_MAX];
    if (copy_from_user(arr_handles, handles, count * sizeof(int32_t)) != 0
        || !access_ok(ready, count)) {
        return -1;
    }
    int local[SOCKET_MAX];
    bool flags[SOCKET_MAX];
    for (uint64_t i = 0; i < count; i++) {
        local[i] = arr_handles[i];
    }

    size_t n = net_socket_ready(caller_pid(), local, count, timeout, flags);

    uint8_t arr_ready[SOCKET_MAX];
    for (uint64_t i = 0; i < count; i++) {
        arr_ready[i] = flags[i] ? 1 : 0;
    }
    return (copy_to_user(ready, arr_ready, count) == 0) ? (int64_t)n : -1;
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
    if (len == 0 || len > TCP_SEND_BUF) {
        return -1;
    }
    uint8_t *p_data = kmalloc(TCP_SEND_BUF);
    if (p_data == NULL) {
        return -1;
    }
    int64_t n = -1;
    if (copy_from_user(p_data, buf, len) == 0) {
        n = net_tcp_send(caller_pid(), handle, p_data, len);
    }
    kfree(p_data);
    return n;
}

static int64_t sys_recv(int handle, uint64_t buf, uint64_t len,
                        int64_t timeout)
{
    if (len == 0 || len > TCP_RECV_BUF || !access_ok(buf, len)) {
        return -1;
    }
    uint8_t *p_data = kmalloc(TCP_RECV_BUF);
    if (p_data == NULL) {
        return -1;
    }
    int64_t n = net_tcp_recv(caller_pid(), handle, p_data, len, timeout);
    if (n > 0 && copy_to_user(buf, p_data, (size_t)n) != 0) {
        n = -1;
    }
    kfree(p_data);
    return n;
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
/* what a handler's frame holds: the registers, then the fpu's */
#define SIGNAL_FRAME_SIZE (sizeof(struct pt_regs) + sizeof(struct fpu_state))

static bool enter_handler(struct pt_regs *regs, int sig, uint64_t handler)
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

    /*
     * the fpu's registers go above the frame, on the boundary fxrstor
     * wants. the handler is given a fresh set, so that what it computes
     * cannot disturb what it interrupted, and sigreturn puts these back
     */
    sp -= sizeof(struct fpu_state);
    sp &= ~(uint64_t)15;

    /*
     * written through copy_to_user like anything else the kernel puts in
     * a program's memory: a stack the program pointed somewhere it may
     * only read has no room for a frame, and the copy says so rather
     * than faulting in ring 0, and a page a forked child still shares
     * with its parent becomes the child's own on the way
     */
    sp -= sizeof(struct pt_regs);
    uint64_t frame = sp;
    struct fpu_state fpu;
    fpu_save(&fpu);
    if (copy_to_user(frame, regs, sizeof *regs) != 0
        || copy_to_user(frame + sizeof *regs, &fpu, sizeof fpu) != 0) {
        return false;
    }

    /*
     * and the return address, which leaves rsp eight past a sixteen
     * boundary, exactly what a handler compiled as an ordinary
     * function expects to find on entry
     */
    sp -= 8;
    if (put_user_u64(sp, tramp) != 0) {
        return false;
    }

    regs->rsp = sp;
    regs->rdi = (uint64_t)sig;      /* the handler's argument */
    regs->rip = handler;            /* where `iretq` will go */

    fpu_init_state(&fpu);
    fpu_restore(&fpu);
    return true;
}

static void deliver_signals(struct pt_regs *regs)
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
        if (!access_ok(handler, 1) || tramp == 0 || !access_ok(tramp, 1)) {
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
static int64_t sys_sigreturn(uint64_t frame, struct pt_regs *regs)
{
    struct signal_state *st = process_signal_state(caller_pid());
    struct pt_regs saved;
    struct fpu_state fpu;
    if (st == NULL || regs == NULL
     || copy_from_user(&saved, frame, sizeof saved) != 0
     || copy_from_user(&fpu, frame + sizeof saved, sizeof fpu) != 0) {
        return -1;
    }
    *regs = saved;

    /*
     * the fpu's too, with the mxcsr bits the cpu lacks cleared, since
     * fxrstor faults in ring 0 on any it is handed
     */
    fpu_sanitize(&fpu);
    fpu_restore(&fpu);

    /*
     * the frame came out of the program's own memory, so nothing in it is
     * believed that decides who the program is: it goes back to ring 3
     * with ring 3's selectors, flags a program may set and interrupts on,
     * and an address the cpu can return to. without this a program could
     * write cs = the kernel's into its own stack and sigreturn into ring 0
     */
    regs->cs = GDT_USER_CODE3;
    regs->ss = GDT_USER_DATA3;
    regs->rflags = (regs->rflags & RFLAGS_USER) | RFLAGS_IF | 0x2;
    if (regs->rip >= USER_ADDRESS_END || regs->rsp >= USER_ADDRESS_END) {
        process_force_signal(caller_pid(), SIGSEGV);
    }
    signal_handler_returned(st);
    return (int64_t)regs->rax;      /* what it was holding, not an answer */
}

/*
 * where %fs points in ring 3, which the thread keeps and the switch
 * loads, so no program ever runs on another's. gs is not offered: the
 * kernel names the core with it, in both rings
 */
static int64_t sys_arch_prctl(uint64_t u_code, uint64_t u_addr)
{
    struct thread *me = sched_current();
    if (me == NULL || me->pid == 0) {
        return -1;
    }

    switch (u_code) {
    case ARCH_SET_FS:
        if (u_addr >= USER_ADDRESS_END) {
            return -1;
        }
        me->arch.u_fs_base = u_addr;
        fs_base_load(u_addr);
        return 0;
    case ARCH_GET_FS:
        return (put_user_u64(u_addr, me->arch.u_fs_base) == 0) ? 0 : -1;
    default:
        return -1;
    }
}

static int64_t syscall_do(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2,
                          uint64_t a3, uint64_t a4, struct pt_regs *regs)
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
        size_t c = 0, r = 0;
        console_size(&c, &r, NULL, NULL);
        uint32_t u_cols = (uint32_t)c;
        uint32_t u_rows = (uint32_t)r;
        if (copy_to_user(a0, &u_cols, sizeof u_cols) != 0
         || copy_to_user(a1, &u_rows, sizeof u_rows) != 0) {
            return -1;
        }
        return 0;
    }
    case SYS_SIGMASK: {
        struct signal_state *st = process_signal_state(caller_pid());
        if (st == NULL) {
            return -1;
        }
        /*
         * this call's mask has a bit per signal number, 1 to 31, where
         * the kernel keeps signal n at bit n - 1
         */
        uint32_t was = (uint32_t)(signal_get_mask(st) << 1);
        if (a0 != 0) {          /* 0 asks without setting */
            signal_set_mask(st, (uint64_t)(uint32_t)a1 >> 1);
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
    case SYS_RESIZE:
        return sys_resize(a0, a1, a2, a3);
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
        return sys_lseek((int)a0, (int64_t)a1, (int)a2);
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
    case SYS_ARCH_PRCTL:
        return sys_arch_prctl(a0, a1);
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
                         uint64_t a3, uint64_t a4, struct pt_regs *regs)
{
    if (nr < SYSCALL_COUNT) {
    }

    /*
     * anything whose alarm has come due, before the delivery below,
     * so a syscall that takes a while and crosses a deadline delivers
     * the signal on its way out rather than on the next call
     */
    process_check_alarms(pit_uptime_ms());

    int64_t result = syscall_do(nr, a0, a1, a2, a3, a4, regs);

    /*
     * the answer goes into the frame before any handler is entered, so
     * that the frame a handler saves holds it, and sigreturn gives the
     * program the answer to the call it made rather than the question
     */
    if (regs != NULL) {
        regs->rax = (uint64_t)result;
    }

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

void syscall_from_user(struct pt_regs *regs)
{
    (void)syscall_dispatch(regs->vector, regs->rdi, regs->rsi, regs->rdx,
                           regs->r10, regs->r8, regs);
}

void exit_to_user(struct pt_regs *regs)
{
    deliver_signals(regs);
}
