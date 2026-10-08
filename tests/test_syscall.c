// SPDX-License-Identifier: GPL-2.0-only
/*
 * tests/test_syscall.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the syscall dispatcher, and in particular what it refuses.
 */

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdlib.h>

/* what the lock complains through */
void panic(const char *fmt, ...)
{
    printf("PANIC: ");
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    printf("\n");
    exit(1);
}

#include <stdbool.h>
#include <stdlib.h>


static char out[8192];
static size_t out_len;
static void out_reset(void)
{
    out[0] = 0; out_len = 0;
}
/* the bulk path added so a program's output does not parse a format string per character. */
void kwrite(const char *s, size_t n)
{
    for (size_t i = 0; i < n && out_len + 1 < sizeof out; i++) {
        out[out_len++] = s[i];
    }
    out[out_len] = 0;
}

void kprintf(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    out_len += vsnprintf(out + out_len, sizeof out - out_len, fmt, ap);
    va_end(ap);
}

/* two page tables that disagree, which is the whole point. */
#define KERNEL_PML4 0x1000
#define CALLER_PML4 0x2000

/*
 * one page is user, one is present but supervisor-only, the rest is
 * nothing at all
 */
#include "mm/vmm.h"
static uint64_t user_page, kernel_page;
uint64_t vmm_kernel_pml4(void)
{
    return KERNEL_PML4;
}

static uint64_t user_extra;         /* another page the test calls the user's */
static uint64_t user_ro;            /* and one the program may read, not write */
static int ro_present;              /* ... once something has faulted it in */
static uint64_t last_pml4_asked;

uint64_t vmm_flags(uint64_t pml4, uint64_t v)
{
    last_pml4_asked = pml4;
    if (pml4 != CALLER_PML4) {
        return 0;                   /* the kernel's tables know none of this */
    }
    uint64_t p = v & ~0xfffull;
    if (p == user_page)   return PTE_PRESENT | PTE_USER | PTE_WRITE;
    if (user_extra && p == (user_extra & ~0xfffull))
                          return PTE_PRESENT | PTE_USER | PTE_WRITE;
    /*
     * the read-only page is *not there* until something faults it in,
     * which is the whole point of it: a program's .rodata is a region
     * the kernel has agreed to and not yet mapped, and the bug this
     * catches was in what kind of fault the pointer check asked for
     */
    if (user_ro && p == (user_ro & ~0xfffull)) {
        return ro_present ? (PTE_PRESENT | PTE_USER) : 0;  /* no PTE_WRITE */
    }
    if (p == kernel_page) return PTE_PRESENT | PTE_WRITE;
    return 0;
}

#include <setjmp.h>
static jmp_buf jb;
static int exited, exit_code_seen;
void thread_exit(int code)
{
    exited = 1; exit_code_seen = code; longjmp(jb, 1);
}

/*
 * the dispatcher asks who is calling, so there has to be somebody,
 * and that somebody has an address space of its own
 */
#include "sched/thread.h"
#include "mm/addrspace.h"
static struct addrspace my_space = { .pml4 = CALLER_PML4 };
static struct thread me = { .space = &my_space };
struct thread *sched_current(void)
{
    return &me;
}

uint64_t pit_uptime_ms(void)
{
    return 1234;
}

/*
 * the screen, stubbed. the editor's four calls are about *where* things
 * go rather than what they say, so what is checked here is the
 * permission on them, only whoever is being typed at may paint the
 * screen or take a key off it
 */
static int cleared, moved_col, moved_row, keys_taken;
void console_clear(void)
{
    cleared++;
}
void console_move(size_t col, size_t row)
{
    moved_col = (int)col;
    moved_row = (int)row;
}
void console_size(size_t *cols, size_t *rows, size_t *w, size_t *h)
{
    if (cols) *cols = 100;
    if (rows) *rows = 40;
    (void)w; (void)h;
}


/* the pipe blocks by parking on a waitq. */
#include "fs/pipe.h"
void waitq_enqueue(struct waitq *q)
{
    (void)q;
}
void waitq_wake_all(struct waitq *q)
{
    (void)q;
}
void waitq_sleep(void)
{
    printf("FAIL: something blocked that should not have\n");
    exit(1);
}
void sched_yield(void)
{
    kprintf("<YIELD>");
}
void sleep_ms(uint64_t ms)
{
    kprintf("<SLEEP %lu>", ms);
}
static int next_key = 'x';
int input_getchar_blocking(void)
{
    keys_taken++; return next_key;
}

/* syscall_init installs this in an msr; the test never call it here */
void syscall_entry(void)
{
}

/* one core, on a host that has no such thing */
uint32_t smp_this_cpu(void)
{
    return 0;
}

/*
 * the wire, which this suite does not exercise, there is no card and
 * no interface here, so every socket call must refuse rather than reach
 * into a stack that does not exist. what is checked below is that the
 * *pointer* handling is the same as every other call's
 */
#include "net/netif.h"
bool net_is_up(void)
{
    return false;
}
int net_socket_open(int owner, uint16_t port)
{
    (void)owner; (void)port; return -1;
}
bool net_socket_close(int owner, int h)
{
    (void)owner; (void)h; return false;
}
void net_socket_close_all(int owner)
{
    (void)owner;
}
bool net_socket_send(int owner, int h, ipv4 to, uint16_t p,
                     const void *d, size_t n)
{
    (void)owner; (void)h; (void)to; (void)p; (void)d; (void)n;
    return false;
}
int64_t net_socket_take(int owner, int h, ipv4 *f, uint16_t *fp,
                        void *out, size_t max)
{
    (void)owner; (void)h; (void)f; (void)fp; (void)out; (void)max;
    return -1;
}

/*
 * the waiting calls. all of them refuse here, for the reason the note above
 * gives: this suite is about how a syscall treats a *pointer* from ring
 * 3, and a stub that returned data would only test the stub
 */
int64_t net_socket_wait(int owner, int h, ipv4 *f, uint16_t *fp,
                        void *out, size_t max, int64_t t)
{
    (void)owner; (void)h; (void)f; (void)fp; (void)out; (void)max; (void)t;
    return -1;
}
size_t net_socket_ready(int owner, const int *hs, size_t n, int64_t t,
                        bool *ready)
{
    (void)owner; (void)hs; (void)t;
    for (size_t i = 0; i < n; i++) { ready[i] = false; }
    return 0;
}
int net_tcp_open(int owner, ipv4 to, uint16_t port)
{
    (void)owner; (void)to; (void)port; return -1;
}
int net_tcp_serve(int owner, uint16_t port)
{
    (void)owner; (void)port; return -1;
}
int net_tcp_accept(int owner, int h, int64_t t)
{
    (void)owner; (void)h; (void)t; return -1;
}
int64_t net_tcp_send(int owner, int h, const void *d, size_t n)
{
    (void)owner; (void)h; (void)d; (void)n; return -1;
}
int64_t net_tcp_recv(int owner, int h, void *o, size_t max, int64_t t)
{
    (void)owner; (void)h; (void)o; (void)max; (void)t; return -1;
}
bool net_tcp_shut(int owner, int h)
{
    (void)owner; (void)h; return false;
}
void net_tcp_close_all(int owner)
{
    (void)owner;
}

#include "sched/process.h"
#include "fs/ramdisk.h"

/* a one-file ramdisk, so `open` has something to find */
static const char motd[] = "hee-ho, from a file\n";
/* motd is readable by anyone; the secret is not. */
static const struct ramdisk_file rd[] = {
    { "./motd.txt",   motd, sizeof motd - 1, 0644 },
    { "./bin/",       "",   0,               0755 },
    { "./bin/cat",    motd, 4,               0755 },
    { "./secret.txt", motd, 4,               0600 },
};
bool ramdisk_may_read(const struct ramdisk_file *f, int uid)
{
    return uid == 0 || (f->mode & 0004) != 0;
}
bool ramdisk_open(const char *name, struct ramdisk_file *out)
{
    for (size_t i = 0; i < 4; i++) {
        const char *n = rd[i].name + 2;     /* past the ./ */
        if (strcmp(name, n) == 0) { *out = rd[i]; return true; }
    }
    return false;
}
bool ramdisk_present(void)
{
    return true;
}
bool ramdisk_stat(size_t i, struct ramdisk_file *out)
{
    if (i >= 4) return false;
    *out = rd[i];
    return true;
}

/*
 * the disk. the real one wants a sata controller, and what this file is
 * responsible for is not fat32, test_fat32 runs that against a real
 * image, but the *routing*: that a path under the mount point reaches
 * the disk and one that is not reaches the ramdisk. so this stub
 * records what it was asked, and tests check who got asked
 */
#include "fs/disk.h"
static const char disk_text[] = "on the disk\n";
static int disk_lookups, disk_creates;
static bool disk_is_ready = true;

/* one stub filesystem, standing in for both mounts. */
bool disk_ready(size_t which)
{
    return which == DISK_ROOT && disk_is_ready;
}
const char *disk_model(void)
{
    return "STUB";
}
void *kmalloc(size_t n)
{
    return malloc(n);
}
void kfree(void *p)
{
    free(p);
}
bool disk_lookup(size_t which, const char *path, struct disk_entry *out)
{
    (void)which;
    disk_lookups++;
    if (!disk_is_ready || strcmp(path, "/hello.txt") != 0) {
        return false;
    }
    memset(out, 0, sizeof *out);
    strcpy(out->name, "hello.txt");
    out->size = sizeof disk_text - 1;
    out->cluster = 7;
    out->entry_sector = 100;
    return true;
}
bool disk_readdir(size_t which, const char *path, size_t index,
                  struct disk_entry *out)
{
    (void)which;
    if (!disk_is_ready || strcmp(path, "/") != 0 || index > 0) {
        return false;
    }
    memset(out, 0, sizeof *out);
    strcpy(out->name, "hello.txt");
    return true;
}
int64_t disk_read(size_t which, uint32_t cluster, uint64_t size,
                  uint64_t offset, void *buf, uint64_t len)
{
    (void)which; (void)size;
    if (cluster != 7 || offset >= sizeof disk_text - 1) return 0;
    uint64_t left = (sizeof disk_text - 1) - offset;
    if (len > left) len = left;
    memcpy(buf, disk_text + offset, len);
    return (int64_t)len;
}
bool disk_create(size_t which, const char *path, struct disk_entry *out)
{
    (void)which;
    disk_creates++;
    if (!disk_is_ready) return false;
    (void)path;
    memset(out, 0, sizeof *out);
    strcpy(out->name, "new.txt");
    out->entry_sector = 200;
    return true;
}
bool disk_mkdir(size_t w, const char *path)
{
    (void)w; (void)path; return disk_is_ready;
}
bool disk_rmdir(size_t w, const char *path)
{
    (void)w; (void)path; return disk_is_ready;
}
bool disk_unlink(size_t w, const char *path)
{
    (void)w; (void)path; return disk_is_ready;
}
/* the things a filesystem with opinions can be told. */
bool disk_lookup_nofollow(size_t which, const char *path,
                          struct disk_entry *out)
{
    return disk_lookup(which, path, out);
}
bool disk_readlink(size_t w, const char *p, char *o, size_t n)
{
    (void)w; (void)p; (void)o; (void)n; return false;
}
bool disk_chmod(size_t w, const char *p, uint32_t m)
{
    (void)w; (void)p; (void)m; return false;
}
/* which filesystem answered. */
const char *disk_kind_name(size_t w)
{
    (void)w; return "stub";
}
enum disk_kind disk_which(size_t w)
{
    (void)w; return DISK_FAT32;
}
bool disk_chown(size_t w, const char *p, uint32_t u, uint32_t g)
{
    (void)w; (void)p; (void)u; (void)g; return false;
}
bool disk_symlink(size_t w, const char *p, const char *t)
{
    (void)w; (void)p; (void)t; return false;
}

bool disk_rename(size_t w, const char *from, const char *to)
{
    (void)w; (void)from; (void)to; return disk_is_ready;
}
int64_t disk_write_at(size_t w, struct disk_entry *e, uint64_t offset,
                      const void *buf, uint64_t len)
{
    (void)w; (void)buf;
    e->size = offset + len;
    return (int64_t)len;
}

/* the terminal, whose real version needs a scheduler to wake threads */
#include "drivers/tty.h"
static int foreground_pid = TTY_SHELL;
int tty_foreground(void)
{
    return foreground_pid;
}

/* the line discipline is the tty's own business and has its own suite. */
static bool tty_was_read;
int64_t tty_read_line(int pid, char *buf, uint64_t len)
{
    tty_was_read = true;
    if (pid != foreground_pid || len == 0) return -1;
    if (process_take_interrupt(pid)) return -1;
    buf[0] = (char)next_key;
    return 1;
}

/* spawn and wait live in usermode.c, which needs a real cpu */
static int started_pid = -1;
void user_start(int pid)
{
    started_pid = pid;
}

/* fork's moving parts, recorded rather than done. */
/*
 * both defined in headers included further down, and both only ever
 * pointed at here
 */
struct user_regs;
struct spawn_io;
static int forked_spaces, forked_threads, freed_spaces;

struct addrspace *addrspace_fork(const struct addrspace *from, uint64_t kp)
{
    (void)from; (void)kp;
    forked_spaces++;
    return &my_space;
}
void addrspace_destroy(struct addrspace *as)
{
    (void)as; freed_spaces++;
}

/* mmap, recorded rather than done. */
static uint64_t reserved_len;
static uint64_t next_reservation = 0x600000000000ull;
static uint64_t dropped_at;
static bool drop_ok = true;

uint64_t addrspace_reserve(struct addrspace *as, uint64_t len, uint64_t flags)
{
    (void)as; (void)flags;
    reserved_len = len;
    return next_reservation;
}
bool addrspace_drop_region(struct addrspace *as, uint64_t start)
{
    (void)as;
    dropped_at = start;
    return drop_ok;
}
uint64_t vmm_nx(void)
{
    return 0;
}
uint64_t vmm_kernel_pml4(void);

static struct thread child_thread;
struct thread *thread_create_parked(const char *name, void (*entry)(void *),
                                    void *arg)
{
    (void)name; (void)entry;
    forked_threads++;
    child_thread.id = 77;
    /* the frame is the child's to free, and nothing here runs it */
    kfree(arg);
    return &child_thread;
}
void fork_return(struct user_regs *frame)
{
    (void)frame;
}

/* when the child was let loose. */
static int woke_thread = -1;
static int pgid_at_wake = -1, fds_at_wake = -1, space_at_wake = -1;

void sched_wake_thread(int id)
{
    woke_thread = id;

    /* what the child looked like at the moment it was let loose. */
    for (size_t i = 0; ; i++) {
        const struct process *p = process_at(i);
        if (p == NULL) break;
        if (p->thread_id != id) continue;
        pgid_at_wake = p->pgid;
        fds_at_wake = (int)process_fd_count(p->pid);
        space_at_wake = forked_spaces;
    }
}

static int spawned_parent = -1;
static const char *spawned_path;
static int spawned_uid = -1;
static const char *spawned_cwd;
int user_spawn(const char *path, int argc, const char *const argv[],
               const char *cwd, int parent, int uid, bool announce,
               const struct spawn_io *io, const char **error)
{
    (void)argc; (void)argv; (void)error; (void)announce; (void)io;
    spawned_path = path; spawned_parent = parent; spawned_uid = uid;
    spawned_cwd = cwd;
    return 77;
}
static int wait_code = 5;
bool user_wait(int pid, int *code)
{
    (void)pid; if (code) *code = wait_code; return true;
}

#include "arch/x86_64/syscall.h"
extern int64_t syscall_dispatch(uint64_t nr, uint64_t a0, uint64_t a1,
                                uint64_t a2, uint64_t a3, uint64_t a4,
                                struct user_regs *regs);

/*
 * what ring 3 was holding, which the entry stub writes down on every
 * call so that fork has something to hand a child. nothing else reads
 * it, so one frame with recognisable values in it is plenty
 */
static struct user_regs caller_frame = {
    .r15 = 15, .r14 = 14, .r13 = 13, .r12 = 12, .rbx = 3, .rbp = 5,
    .r9 = 9, .r8 = 8, .r10 = 10, .rdx = 2, .rsi = 6, .rdi = 7,
    .r11 = 0x202, .rcx = 0x400123, .rsp = 0x6ffffffff000ull,
};

static int failures;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); failures++; } } while (0)

static int64_t call(uint64_t nr, uint64_t a0, uint64_t a1)
{
    out_reset();
    return syscall_dispatch(nr, a0, a1, 0, 0, 0, &caller_frame);
}

static int64_t call3(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2)
{
    out_reset();
    return syscall_dispatch(nr, a0, a1, a2, 0, 0, &caller_frame);
}

/* readdir takes a path in the last two, so it needs all five */
static int64_t call5(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2,
                     uint64_t a3, uint64_t a4)
{
    out_reset();
    return syscall_dispatch(nr, a0, a1, a2, a3, a4, &caller_frame);
}

/* read and write take a descriptor first now, the way they do everywhere else. */
static int64_t write_to(uint64_t fd, uint64_t ptr, uint64_t len)
{
    return call3(SYS_WRITE, fd, ptr, len);
}
static int64_t read_from(uint64_t fd, uint64_t ptr, uint64_t len)
{
    return call3(SYS_READ, fd, ptr, len);
}

/*
 * signals are delivered on the way out of a syscall, which means the
 * dispatch can now stop or continue a process. neither is what this
 * suite is about, it is about how a syscall treats a pointer from
 * ring 3, so both are recorded rather than done
 */
static int stops;
void sched_set_stopped(int id, bool stopped)
{
    (void)id; (void)stopped; stops++;
}

/* the terminal is a mode. */
#include "drivers/termios.h"
static struct term_mode test_mode = TERM_COOKED;
void tty_set_mode(const struct term_mode *m)
{
    test_mode = *m;
}
struct term_mode tty_get_mode(void)
{
    return test_mode;
}
void tty_restore_mode(void)
{
    test_mode = (struct term_mode)TERM_COOKED;
}

/*
 * a pointer check faults a reserved page in rather than
 * refusing it, memory from mmap is reserved and arrives on first
 * touch. this suite builds its page tables by hand, so every page it
 * uses is already present and there is nothing to fault: the answer
 * here is "no region", which is exactly what a bad pointer gets
 */
static int faults_asked;
bool addrspace_fault(struct addrspace *as, uint64_t virt, bool write,
                     bool present)
{
    (void)as; (void)present;
    faults_asked++;

    /*
     * the read-only region behaves as the real one does: a read fault
     * populates it, and a write fault against a region that was never
     * writable is refused. that refusal is what the old pointer check
     * walked into on every string a program tried to print
     */
    if (user_ro && (virt & ~0xfffull) == (user_ro & ~0xfffull)) {
        if (write) {
            return false;
        }
        ro_present = 1;
        return true;
    }
    return false;
}

/*
 * the clock. this suite has no timer and no cmos chip, so
 * it answers what a machine that has not been told the time answers,
 * which is the state the syscall is meant to be able to report
 */
uint64_t epoch_now(void)
{
    return 0;
}

int main(void)
{
    /*
     * the caller is a process, since half of these calls are about what
     * that process owns
     */
    me.pid = process_create("tester", 0, 0, false, 0);
    foreground_pid = me.pid;    /* it holds the terminal, mostly */

    /* a page a user program could legitimately own */
    char *page = aligned_alloc(4096, 8192);
    user_page = (uint64_t)page;
    kernel_page = (uint64_t)page + 4096;
    strcpy(page, "hello from ring 3");

    /*
     * this is an old regression: every
     * program got an address space of its own, but the checker kept
     * consulting the kernel's, where a program's memory is not mapped
     * at all. the symptom was every syscall taking a pointer quietly
     * returning -1, so programs printed nothing whatsoever
     */
    last_pml4_asked = 0;
    CHECK(write_to(FD_STDOUT, user_page, 4) == 4,
          "a valid user pointer is accepted");
    CHECK(last_pml4_asked == CALLER_PML4,
          "and it was the caller's page tables that were asked, not the "
          "kernel's, which do not map user memory at all");

    /*
     * a kernel thread has no space of its own and falls back to the
     * kernel's, which is right for it
     */
    me.space = NULL;
    last_pml4_asked = 0;
    (void)write_to(FD_STDOUT, user_page, 4);
    CHECK(last_pml4_asked == KERNEL_PML4,
          "a kernel thread is judged by the kernel's tables");
    me.space = &my_space;


    CHECK(call(SYS_UPTIME, 0, 0) == 1234, "SYS_UPTIME returns the clock");
    CHECK(call(SYS_YIELD, 0, 0) == 0 && strstr(out, "<YIELD>"),
          "SYS_YIELD reaches the scheduler");
    CHECK(call(SYS_SLEEP, 250, 0) == 0 && strstr(out, "<SLEEP 250>"),
          "SYS_SLEEP passes its argument through");

    /*
     * the exact failure that made a working kernel print nonsense: if
     * the entry stub does not shuffle registers, `nr` arrives holding
     * an address instead of a number
     */
    CHECK(call(0x400ada, 0, 0) == -1, "a nonsense number is refused");
    CHECK(strstr(out, "does not exist") != NULL, "and complained about");


    CHECK(write_to(FD_STDOUT, user_page, 17) == 17, "SYS_WRITE writes user memory");
    CHECK(strcmp(out, "hello from ring 3") == 0, "and writes exactly it");

    CHECK(write_to(FD_STDOUT, user_page, 0) == 0, "a zero-length write is fine");

    /*
     * this is `.rodata`, and it is where a real machine found a bug no
     * host suite had: the pointer check asked to *write* to every
     * address it was handed, whichever direction the bytes were going.
     * a read-only region the program had not touched itself was then
     * refused, and every string constant a program prints is exactly
     * that, handed straight to `write` without the program ever reading
     * it. `uptime` printed its numbers and none of its words.
     *
     * so both directions are checked here. reading out of a read-only
     * page is what a program does every time it prints anything
     */
    {
        char *ro = aligned_alloc(4096, 4096);
        memcpy(ro, "out of read-only\n", 17);
        user_ro = (uint64_t)ro;
        ro_present = 0;         /* not mapped yet, like every .rodata */

        CHECK(write_to(FD_STDOUT, user_ro, 16) == 16,
              "a program may print a string out of its own .rodata");
        CHECK(strcmp(out, "out of read-only") == 0, "and it arrives whole");

        /*
         * and the other direction, which was never checked at all: the
         * kernel filling a buffer the program cannot write to itself is
         * ring 3 editing its own .text through a syscall
         */
        CHECK(call3(SYS_READ, FD_STDIN, user_ro, 4) == -1,
              "and may not have the kernel write into it");

        CHECK(ro_present == 1,
              "and the page was faulted in as a read rather than a write");
        user_ro = 0;
        ro_present = 0;
        free(ro);
    }

    /*
     * a job put in the background with `&` printing over the prompt is
     * the noise `&` was supposed to spare you, and worse than noise,
     * it lands in the middle of a line somebody is typing
     */
    {
        strcpy(page, "hello from ring 3");
        int was_fg = foreground_pid;

        foreground_pid = 999;       /* somebody else is at the front */
        out_reset();
        CHECK(write_to(FD_STDOUT, user_page, 17) == 17,
              "a background write is accepted rather than refused");
        CHECK(out[0] == '\0', "and goes nowhere");

        /*
         * accepted rather than refused on purpose: a program has no way
         * to know it is in the background and nothing sensible to do
         * about it, so a failure would only make it die or spin
         */

        out_reset();
        CHECK(write_to(FD_STDERR, user_page, 17) == 17, "stderr still writes");
        CHECK(strcmp(out, "hello from ring 3") == 0,
              "and reaches the terminal anyway, so a job that failed in "
              "the background still says so");

        foreground_pid = was_fg;
        out_reset();
        CHECK(write_to(FD_STDOUT, user_page, 17) == 17, "back at the front");
        CHECK(strcmp(out, "hello from ring 3") == 0, "and printing again");
        out_reset();
    }

    /*
     * a pipeline works by 0 and 1 pointing somewhere other than the
     * terminal, and the program not being able to tell. so: give this
     * process a pipe and check that the same SYS_WRITE that reached the
     * console a moment ago now reaches the buffer instead
     */
    {
        static struct pipe pout, pin;
        static _Alignas(4096) char pbuf[64];
        pipe_reset(&pout);
        pipe_reset(&pin);

        strcpy(page, "down the pipe");

        /*
         * 0 and 1 are real descriptors now, so putting a pipe on one is
         * putting a pipe in a slot, the same act as `> file`, which
         * is why one mechanism does both
         */
        struct fd slot;
        memset(&slot, 0, sizeof slot);
        slot.kind = FD_PIPE;
        slot.pipe = &pin;
        slot.writing = false;
        process_fd_install(me.pid, FD_STDIN, &slot);
        slot.pipe = &pout;
        slot.writing = true;
        process_fd_install(me.pid, FD_STDOUT, &slot);

        out_reset();
        CHECK(write_to(FD_STDOUT, user_page, 13) == 13,
              "a write to stdout with a pipe on it succeeds");
        CHECK(out[0] == '\0', "and not one byte of it reaches the console");
        CHECK(pipe_pending(&pout) == 13, "all of it is in the pipe");

        char got[32];
        memset(got, 0, sizeof got);
        pipe_get(&pout, got, 13);
        CHECK(strcmp(got, "down the pipe") == 0, "byte for byte");

        /*
         * stderr never goes down a pipe, deliberately: `cat missing |
         * head` should put its complaint on the screen rather than
         * feeding it to head as though it were data
         */
        out_reset();
        CHECK(write_to(FD_STDERR, user_page, 13) == 13, "stderr still writes");
        CHECK(strcmp(out, "down the pipe") == 0,
              "and goes to the console even with a pipe on stdout");
        CHECK(pipe_pending(&pout) == 0, "with nothing added to the pipe");

        /*
         * reading stdin takes from the pipe rather than the keyboard,
         * and the program has no way to know which it got
         */
        pipe_put(&pin, "from upstream\n", 14);
        user_extra = (uint64_t)pbuf;
        memset(pbuf, 0, sizeof pbuf);
        CHECK(call3(SYS_READ, FD_STDIN, (uint64_t)pbuf, sizeof pbuf) == 14,
              "reading stdin takes from the pipe");
        CHECK(memcmp(pbuf, "from upstream\n", 14) == 0, "exactly what was in it");
        CHECK(!tty_was_read, "and the keyboard was never asked");
        user_extra = 0;

        /*
         * end of file: the writer goes, and the read returns zero
         * rather than waiting for somebody who has already left
         */
        pin.writers = 0;
        user_extra = (uint64_t)pbuf;
        CHECK(call3(SYS_READ, FD_STDIN, (uint64_t)pbuf, sizeof pbuf) == 0,
              "and reads zero once the writer has gone, which is end of file");
        user_extra = 0;

        /* a write with no reader left ends the process. */
        pout.readers = 0;
        exited = 0;
        if (setjmp(jb) == 0) {
            (void)write_to(FD_STDOUT, user_page, 13);
            CHECK(false, "a write to a pipe nobody reads should not return");
        }
        CHECK(exited, "it ends the process instead");
        CHECK(exit_code_seen == PROCESS_KILLED, "recorded as killed");

        memset(&slot, 0, sizeof slot);
        slot.kind = FD_KEYBOARD;
        process_fd_install(me.pid, FD_STDIN, &slot);
        slot.kind = FD_CONSOLE;
        process_fd_install(me.pid, FD_STDOUT, &slot);
        out_reset();
        CHECK(write_to(FD_STDOUT, user_page, 13) == 13,
              "and with the pipes gone, stdout is the console again");
        CHECK(strcmp(out, "down the pipe") == 0, "printing there once more");
    }

    strcpy(page, "hello from ring 3");
    out_reset();
    (void)write_to(FD_STDOUT, user_page, 17);
    out_reset();


    CHECK(write_to(FD_STDOUT, kernel_page, 8) == -1,
          "a page that is present but not user is refused, this is the "
          "one that would let ring 3 read the kernel");
    CHECK(strstr(out, "refused a pointer") != NULL,
          "and the kernel says so rather than failing in silence, which is "
          "how this class of bug stays hidden");
    CHECK(strstr(out, "hello from ring 3") == NULL,
          "but not one byte of what it asked for gets through");

    CHECK(write_to(FD_STDOUT, (uint64_t)page + 0x100000, 8) == -1,
          "an unmapped pointer is refused");

    CHECK(write_to(FD_STDOUT, 0xffffffff80000000ull, 8) == -1,
          "a higher-half pointer is refused outright");
    CHECK(write_to(FD_STDOUT, 0xffff800000000000ull, 8) == -1,
          "including the very bottom of the kernel half");

    /*
     * an absurd length becomes a short write, not a refusal, and
     * crucially the clamped span is the one that gets validated, so it
     * can never be used to widen what the kernel will read
     */
    CHECK(write_to(FD_STDOUT, user_page, ~0ull) == 4096,
          "an absurd length is clamped to a short write");
    CHECK(out_len == 4096, "and exactly that much is written");

    /* the same trick aimed at kernel memory still gets nowhere */
    CHECK(write_to(FD_STDOUT, kernel_page, ~0ull) == -1,
          "clamping does not help a pointer that was never allowed");

    /* a span starting in user memory but running out of it */
    CHECK(write_to(FD_STDOUT, user_page + 4090, 16) == -1,
          "a write running off the end of its page is refused, not truncated");


    next_key = 'q';
    char *buf = page;
    CHECK(read_from(FD_STDIN, (uint64_t)buf, 1) == 1, "reading fd 0 takes a key");
    CHECK(buf[0] == 'q', "from the keyboard");
    CHECK(read_from(FD_STDIN, kernel_page, 4) == -1, "and refuses kernel memory");
    CHECK(read_from(FD_STDIN, user_page, 0) == -1, "a zero-length read is refused");

    /*
     * a background program helping itself would take keys from whoever
     * is actually being typed at
     */
    foreground_pid = 999;               /* somebody else is at the front */
    CHECK(read_from(FD_STDIN, (uint64_t)buf, 1) == -1,
          "a background process is refused the keyboard");
    foreground_pid = me.pid;
    next_key = 'z';
    CHECK(read_from(FD_STDIN, (uint64_t)buf, 1) == 1,
          "and the foreground one is not");

    /*
     * `process_interrupt` raises SIGINT now, and an *unhandled* SIGINT
     * kills the process on the way back to ring 3, which is correct,
     * and is not what these checks are about. they are about a blocking
     * call being cut short, so the signal is set to be ignored and the
     * cutting-short is what is left to observe.
     *
     * that this kernel lets an ignored signal interrupt a call at all
     * is a deliberate divergence: `interrupted` predates signals here
     * and means "stop waiting", which is a different question from
     * "what should happen to this process"
     */
    {
        struct signal_state *st = process_signal_state(me.pid);
        CHECK(st != NULL, "the process has signal state");
        signal_set_handler(st, SIGINT, SIG_IGNORE);
    }

    /* an interrupt delivered before a read means the read never starts */
    process_interrupt(me.pid);
    CHECK(read_from(FD_STDIN, (uint64_t)buf, 1) == -1,
          "a pending interrupt cuts a read short before it begins");
    CHECK(!process_interrupt_pending(me.pid), "and is consumed by it");

    /* a sleep says how it ended */
    CHECK(call(SYS_SLEEP, 1, 0) == 0, "an uninterrupted sleep returns 0");
    process_interrupt(me.pid);
    CHECK(call(SYS_SLEEP, 1, 0) == -1,
          "and an interrupted one says so, rather than pretending time passed");


    CHECK(write_to(3, user_page, 4) == -1,
          "nothing here is writable but the console, and a file says so");
    CHECK(write_to(FD_STDERR, user_page, 4) == 4, "stderr goes the same place");


    strcpy(page, "motd.txt");
    int64_t fd = call(SYS_OPEN, (uint64_t)page, 8);
    CHECK(fd >= FD_FIRST_FILE, "open finds a file and gives it a number");
    CHECK(call(SYS_OPEN, (uint64_t)page, 3) == -1, "a wrong name finds nothing");
    CHECK(call(SYS_OPEN, kernel_page, 8) == -1,
          "and a path the caller does not own is refused before it is read");

    /* page aligned, and that is not fussiness. */
    /* page aligned, and that is not fussiness. */
    static _Alignas(4096) char sink[64];
    memset(sink, 0, sizeof sink);
    user_extra = (uint64_t)sink;
    CHECK(read_from(fd, (uint64_t)sink, 6) == 6, "a short read works");
    CHECK(memcmp(sink, "hee-ho", 6) == 0, "and gives the first bytes");
    CHECK(read_from(fd, (uint64_t)sink, 6) == 6, "reading again continues");
    CHECK(memcmp(sink, ", from", 6) == 0, "from where the last one stopped");

    while (read_from(fd, (uint64_t)sink, 8) > 0) { }
    CHECK(read_from(fd, (uint64_t)sink, 8) == 0, "the end of a file reads zero");

    CHECK(call(SYS_CLOSE, fd, 0) == 0, "closing works");
    CHECK(call(SYS_CLOSE, fd, 0) == -1, "but only once");
    CHECK(read_from(fd, (uint64_t)sink, 4) == -1, "and the fd is dead after");


    CHECK(call(SYS_GETPID, 0, 0) == me.pid, "getpid says who is asking");

    strcpy(page, "bin/thing");
    CHECK(call(SYS_SPAWN, (uint64_t)page, 9) == 77, "spawn returns the new pid");
    CHECK(spawned_parent == me.pid,
          "and records the caller as its parent, so only it may wait");

    int other = process_create("someone else's", 999, 0, false, 0);
    CHECK(call(SYS_WAIT, other, 0) == -1,
          "waiting for another process's child is refused, otherwise the "
          "exit code would go to the wrong place");

    int mine = process_create("the test's", me.pid, 0, false, 0);
    wait_code = 42;
    int codeout = 0;
    user_extra = (uint64_t)&codeout;
    CHECK(call(SYS_WAIT, mine, (uint64_t)&codeout) == mine, "the test's works");
    CHECK(codeout == 42, "and fills in how it went");
    CHECK(call(SYS_WAIT, 4242, 0) == -1, "waiting for nothing is refused");

    /* the mode came out of the tar header and the uid off the process. */
    CHECK(call(SYS_GETUID, 0, 0) == 0, "getuid reports what the process runs as");

    strcpy(page, "secret.txt");
    CHECK(call(SYS_OPEN, (uint64_t)page, 10) >= FD_FIRST_FILE,
          "uid 0 may open a file nobody else may");

    /* the same call, from a process that is not the master */
    {
        int guest = process_create("guest", 0, 1000, false, 0);
        int was = me.pid;
        me.pid = guest;
        foreground_pid = guest;

        out_reset();
        CHECK(syscall_dispatch(SYS_OPEN, (uint64_t)page, 10, 0, 0, 0, &caller_frame) == -1,
              "and a guest may not");
        CHECK(strstr(out, "may not read") != NULL, "and is told so plainly");

        strcpy(page, "motd.txt");
        CHECK(syscall_dispatch(SYS_OPEN, (uint64_t)page, 8, 0, 0, 0, &caller_frame) >= FD_FIRST_FILE,
              "but may still read what is readable by anyone");

        CHECK(syscall_dispatch(SYS_GETUID, 0, 0, 0, 0, 0, &caller_frame) == 1000,
              "and getuid says who it really is");

        /*
         * a spawned child gets the uid of whoever started it, a
         * program picking its own would make the whole idea decorative
         */
        strcpy(page, "bin/thing");
        (void)syscall_dispatch(SYS_SPAWN, (uint64_t)page, 9, 0, 0, 0, &caller_frame);
        CHECK(spawned_uid == 1000, "a child inherits the uid it was started with");

        me.pid = was;
        foreground_pid = was;
        process_exited(guest, 0, 0);
        process_collect(guest, NULL);
    }
    strcpy(page, "motd.txt");

    /* `ls -l` wants a size and a date for every name in a directory. */
    {
        static _Alignas(4096) struct user_stat st;
        user_extra = (uint64_t)&st;
        memset(&st, 0xaa, sizeof st);

        strcpy(page, "motd.txt");
        CHECK(call3(SYS_STAT, (uint64_t)page, 8, (uint64_t)&st) == 0,
              "stat answers about a file");
        CHECK(st.size > 0, "with a size");
        CHECK(st.is_dir == 0, "and says it is not a directory");
        /*
         * the ramdisk keeps no dates worth having, so it says so rather
         * than making one up
         */
        CHECK(st.year == 0, "and no date, which is what the ramdisk knows");

        strcpy(page, "nothing-at-all");
        CHECK(call3(SYS_STAT, (uint64_t)page, 14, (uint64_t)&st) == -1,
              "and refuses a name that is not there");

        /*
         * the struct comes back through a pointer ring 3 handed over,
         * and a pointer into the kernel is the whole reason that check
         * exists
         */
        strcpy(page, "motd.txt");
        CHECK(call3(SYS_STAT, (uint64_t)page, 8, kernel_page) == -1,
              "and will not write the answer into the kernel");
        user_extra = 0;
    }

    /*
     * the disk stub says yes to both, so what is under test here is the
     * dispatcher: that the paths are copied in safely and that a guest
     * is stopped before either reaches a filesystem
     */
    strcpy(page, "/gone.txt");
    CHECK(call(SYS_UNLINK, (uint64_t)page, 9) == 0, "unlink reaches the disk");
    CHECK(call(SYS_UNLINK, kernel_page, 9) == -1,
          "but not with a path in the kernel");

    {
        static _Alignas(4096) char to[32];
        strcpy(to, "/there.txt");
        user_extra = (uint64_t)to;
        CHECK(call5(SYS_RENAME, (uint64_t)page, 9, (uint64_t)to, 10, 0) == 0,
              "rename takes two paths");
        CHECK(call5(SYS_RENAME, (uint64_t)page, 9, kernel_page, 10, 0) == -1,
              "and checks the second one as carefully as the first");
        user_extra = 0;
    }

    {
        int guest = process_create("guest2", 0, 1000, false, 0);
        int was = me.pid;
        me.pid = guest;
        foreground_pid = guest;

        out_reset();
        strcpy(page, "/gone.txt");
        CHECK(syscall_dispatch(SYS_UNLINK, (uint64_t)page, 9, 0, 0, 0, &caller_frame) == -1,
              "a guest may not remove a file");
        CHECK(strstr(out, "may not remove") != NULL, "and is told why");

        me.pid = was;
        foreground_pid = was;
        process_exited(guest, 0, 0);
        process_collect(guest, NULL);
    }
    strcpy(page, "motd.txt");

    /*
     * a shell wants none of these: it prints a prompt, reads a line,
     * prints an answer, and the console keeps the cursor wherever the
     * printing left it. a program painting a whole screen cannot work
     * that way, and every one of these is about *where* rather than
     * what, so what is worth checking is the permission on them
     */
    {
        uint32_t c = 0, r = 0;
        static _Alignas(4096) uint32_t size_out[2];
        user_extra = (uint64_t)size_out;
        CHECK(call(SYS_SCREEN, (uint64_t)&size_out[0],
                   (uint64_t)&size_out[1]) == 0, "the screen has a size");
        c = size_out[0];
        r = size_out[1];
        CHECK(c == 100 && r == 40, "and it is the one the console reports");
        user_extra = 0;

        CHECK(call(SYS_SCREEN, kernel_page, kernel_page) == -1,
              "and it will not be written into the kernel");

        cleared = 0;
        CHECK(call(SYS_CLEAR, 0, 0) == 0, "the foreground may clear the screen");
        CHECK(cleared == 1, "and it happens");

        CHECK(call(SYS_CURSOR, 7, 3) == 0, "and put the cursor somewhere");
        CHECK(moved_col == 7 && moved_row == 3, "exactly where it asked");

        keys_taken = 0;
        next_key = 'z';
        CHECK(call(SYS_GETKEY, 0, 0) == 'z', "and take one key, unechoed");
        CHECK(keys_taken == 1, "off the input queue");

        /*
         * an interrupt has to be noticed after the wait as well as
         * before it, ctrl+c arrives by waking the thread, so a check
         * only on the way in would miss every one that mattered
         */
        process_interrupt(me.pid);
        CHECK(call(SYS_GETKEY, 0, 0) == -1, "and gives up when interrupted");

        /* the screen belongs to whoever is being typed at. */
        int other = process_create("background", 0, 0, false, 0);
        int was = me.pid;
        me.pid = other;

        cleared = 0;
        CHECK(syscall_dispatch(SYS_CLEAR, 0, 0, 0, 0, 0, &caller_frame) == -1,
              "a background process may not clear the screen");
        CHECK(cleared == 0, "and does not");
        CHECK(syscall_dispatch(SYS_CURSOR, 1, 1, 0, 0, 0, &caller_frame) == -1,
              "nor move the cursor");
        CHECK(syscall_dispatch(SYS_GETKEY, 0, 0, 0, 0, 0, &caller_frame) == -1,
              "nor help itself to a key meant for somebody else");

        me.pid = was;
        process_exited(other, 0, 0);
        process_collect(other, NULL);
    }

    /* spawning was the only way to make a process and it built one from a file every time. */
    {
        strcpy(page, "motd.txt");
        long held = call(SYS_OPEN, (uint64_t)page, 8);
        CHECK(held >= FD_FIRST_FILE, "the parent has a file open");

        process_set_pgid(me.pid, me.pid);

        forked_spaces = forked_threads = 0;
        woke_thread = -1;
        pgid_at_wake = -1;
        fds_at_wake = -1;

        int64_t child = call(SYS_FORK, 0, 0);
        CHECK(child > 0, "fork answers the parent with a pid");
        CHECK(child != me.pid, "which is not the parent's own");
        CHECK(forked_spaces == 1, "one address space was made");
        CHECK(forked_threads == 1, "and one thread to run in it");

        const struct process *kid = process_find((int)child);
        CHECK(kid != NULL, "and there is a process there");

        CHECK(kid->parent == me.pid, "whose parent is the one that forked");
        CHECK(kid->pgid == me.pid,
              "in the same job, so ctrl+c reaches it, otherwise a forked "
              "child is a process no key can touch");
        CHECK(kid->uid == process_uid(me.pid), "running as the same user");
        CHECK(strcmp(process_cwd((int)child), process_cwd(me.pid)) == 0,
              "standing where its parent was standing");

        CHECK(process_fd_count((int)child) == process_fd_count(me.pid),
              "holding every file its parent held");
        struct fd inherited;
        CHECK(process_fd_get((int)child, (int)held, &inherited),
              "including that one");

        /*
         * the ordering, which is the part that is easy to get wrong and
         * silent when you do
         */
        CHECK(woke_thread == 77, "the child was let loose");
        CHECK(pgid_at_wake == me.pid,
              "but only once its group was settled");
        CHECK(fds_at_wake > 0, "and its descriptors with it");

        /*
         * the child is a real process and must not be left in the table
         *, nothing here is going to run it
         */
        process_exited((int)child, 0, 0);
        process_collect((int)child, NULL);
        call(SYS_CLOSE, (uint64_t)held, 0);
    }

    /* everything a program had until now was decided before it started. */
    {
        reserved_len = 0;
        int64_t at = call(SYS_MMAP, 8192, 0);
        CHECK(at != 0, "a program can ask for memory");
        CHECK(reserved_len == 8192, "and asks for exactly what it said");

        CHECK(call(SYS_MMAP, 0, 0) == 0, "asking for nothing gets nothing");
        CHECK(call(SYS_MMAP, 1ull << 40, 0) == 0,
              "and an absurd length is refused rather than served, the "
              "pages are not made until they are touched, so without a "
              "limit a program could reserve more than the machine has");

        dropped_at = 0;
        CHECK(call(SYS_MUNMAP, (uint64_t)at, 0) == 0, "and give it back");
        CHECK(dropped_at == (uint64_t)at, "by the address it was handed");

        drop_ok = false;
        CHECK(call(SYS_MUNMAP, 0x123000, 0) == -1,
              "an address nobody handed out is refused");
        drop_ok = true;
    }

    /*
     * `ls` was the only command that needed something new to leave the
     * kernel: `open` can only answer about a name you already know.
     * with one namespace it names a directory, and no path means the
     * root, which is the disk, plus the mounts standing on it
     */
    user_extra = (uint64_t)sink;
    memset(sink, 0, sizeof sink);
    CHECK(call3(SYS_READDIR, 0, (uint64_t)sink, sizeof sink) == 9,
          "readdir with no path lists the root");
    CHECK(strcmp(sink, "hello.txt") == 0, "starting with what is on the disk");

    /*
     * past the disk's own entries, /boot is standing there, and it has
     * to come back marked as somewhere you can descend into
     */
    CHECK(call3(SYS_READDIR, 1, (uint64_t)sink, sizeof sink) == 5,
          "and then the mount point");
    CHECK(strcmp(sink, "boot/") == 0,
          "with a trailing slash, since it is a directory");

    CHECK(call3(SYS_READDIR, 99, (uint64_t)sink, sizeof sink) == -1,
          "past the end says so rather than inventing a name");
    CHECK(call3(SYS_READDIR, 0, kernel_page, 64) == -1,
          "and a buffer the caller does not own is refused");

    /*
     * naming /boot reaches the ramdisk, whose names are paths that
     * `open` would take, with tar's leading ./ stripped off
     */
    strcpy(page, "/boot");
    user_extra = (uint64_t)sink;
    CHECK(call5(SYS_READDIR, 0, (uint64_t)sink, sizeof sink,
                (uint64_t)page, 5) == 8,
          "and /boot lists the ramdisk");
    CHECK(strcmp(sink, "motd.txt") == 0, "with the leading ./ stripped");

    /*
     * index 1, not 2: tar's own directory record for ./bin/ is skipped,
     * since a name with nothing behind it is no use to anyone
     */
    CHECK(call5(SYS_READDIR, 1, (uint64_t)sink, sizeof sink,
                (uint64_t)page, 5) == 7, "later ones by index");
    CHECK(strcmp(sink, "bin/cat") == 0, "including nested paths");

    /* a name longer than the buffer is truncated, not written past */
    memset(sink, 0xaa, sizeof sink);
    CHECK(call5(SYS_READDIR, 0, (uint64_t)sink, 4, (uint64_t)page, 5) == 3,
          "a short buffer takes what fits");
    CHECK(strcmp(sink, "mot") == 0, "terminated, with nothing beyond it");
    strcpy(page, "motd.txt");

    /*
     * every path that crosses this boundary is now read from wherever
     * the caller happens to be, so the same six characters typed by two
     * processes can mean two different files
     */
    user_extra = (uint64_t)sink;
    memset(sink, 0, sizeof sink);
    CHECK(call3(SYS_GETCWD, (uint64_t)sink, sizeof sink, 0) == 1,
          "a process starts at the root");
    CHECK(strcmp(sink, "/") == 0, "which is spelled with one slash");

    /*
     * moving somewhere that is not there must fail rather than leaving
     * the process standing nowhere, every later name would resolve
     * against a place that does not exist, and fail nowhere near here
     */
    strcpy(page, "/nowhere");
    CHECK(call3(SYS_CHDIR, (uint64_t)page, 8, 0) == -1,
          "moving somewhere that is not there is refused");
    memset(sink, 0, sizeof sink);
    call3(SYS_GETCWD, (uint64_t)sink, sizeof sink, 0);
    CHECK(strcmp(sink, "/") == 0, "and leaves the test where the test was");

    strcpy(page, "/boot");
    CHECK(call3(SYS_CHDIR, (uint64_t)page, 5, 0) == 0,
          "and moving somewhere real is allowed");
    memset(sink, 0, sizeof sink);
    call3(SYS_GETCWD, (uint64_t)sink, sizeof sink, 0);
    CHECK(strcmp(sink, "/boot") == 0, "which is then where the test is");

    /* and now a bare name means something different than it did */
    strcpy(page, "motd.txt");
    CHECK(call3(SYS_OPEN, (uint64_t)page, 8, 0) >= 0,
          "a bare name is read from where the test is standing");

    /*
     * back up, and out of the tree entirely: `..` from the root is the
     * root, so this cannot name anything outside it
     */
    strcpy(page, "../../..");
    CHECK(call3(SYS_CHDIR, (uint64_t)page, 8, 0) == 0, "climbing out is fine");
    memset(sink, 0, sizeof sink);
    call3(SYS_GETCWD, (uint64_t)sink, sizeof sink, 0);
    CHECK(strcmp(sink, "/") == 0, "and lands at the root, not above it");

    strcpy(page, "motd.txt");

    /* every socket call must refuse rather than reaching into a stack that is not there. */

    CHECK(syscall_dispatch(SYS_SOCKET, 5000, 0, 0, 0, 0, &caller_frame) == -1,
          "socket refuses when the wire is down");

    CHECK(syscall_dispatch(SYS_SENDTO, 0, 0x0a000202, 7,
                           0xffff800000000000ull, 8, &caller_frame) == -1,
          "sendto refuses a buffer that is not the caller's");

    CHECK(syscall_dispatch(SYS_RECVFROM, 0, 0xffff800000000000ull,
                           0xffff800000000000ull, 64, 0, &caller_frame) == -1,
          "and recvfrom refuses to write into one");


    if (setjmp(jb) == 0) {
        syscall_dispatch(SYS_EXIT, 0, 0, 0, 0, 0, &caller_frame);
        printf("FAIL: SYS_EXIT returned\n");
        failures++;
    }
    CHECK(exited, "SYS_EXIT ends the thread");

    if (!failures) printf("all good\n");
    return failures;
}
