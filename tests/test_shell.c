// SPDX-License-Identifier: GPL-2.0-only
/*
 * tests/test_shell.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * host-side test for the shell's line splitting and command dispatch.
 * includes shell.c directly so the static helpers are reachable, and stubs
 * out every piece of kernel it leans on.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
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

/* the mouse constants the stubs below hand back */
#include <velvet/mouse.h>


static char out[4096];
static size_t out_len;

static void out_reset(void)
{
    out[0] = 0; out_len = 0;
}

void kprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    out_len += vsnprintf(out + out_len, sizeof(out) - out_len, fmt, ap);
    va_end(ap);
}


void console_clear(void)
{
    kprintf("<CLEAR>");
}

/* which console this session is. */
static unsigned my_console;
unsigned tty_my_console(void)
{
    return my_console;
}

static unsigned shown;
static int switches;
unsigned console_active(void)
{
    return shown;
}
void console_switch(unsigned n)
{
    shown = n; switches++;
}
void console_scroll_back(int lines)
{
    (void)lines;
}

/* a mouse the test can switch on and off. */
static bool have_mouse = true;
bool mouse_present(void)
{
    return have_mouse;
}
bool mouse_has_wheel(void)
{
    return true;
}
void mouse_position(size_t *col, size_t *row)
{
    if (col) *col = 12;
    if (row) *row = 7;
}
uint8_t mouse_buttons(void)
{
    return MOUSE_LEFT;
}
static uint64_t fake_resyncs;
uint64_t mouse_packets(void)
{
    return 42;
}
uint64_t mouse_resyncs(void)
{
    return fake_resyncs;
}
size_t console_scrollback_lines(void)
{
    return 0;
}
void console_set_colors(uint32_t f, uint32_t b)
{
    (void)f; (void)b;
}
bool console_ready(void)
{
    return true;
}
int input_getchar_blocking(void)
{
    return '\n';
}

/*
 * a script checks between lines for a ctrl+c, because a loop may
 * legitimately run forever and stopping one has to be possible
 */
static int pending_key = -1;
int input_peek(void)
{
    return pending_key;
}
/* /proc writes its files through a capture, which nothing here reads */
struct kcapture;
void sched_capture(struct kcapture *p_cap)
{
    (void)p_cap;
}

uint64_t pit_uptime_ms(void)
{
    return 12345;
}
uint64_t pit_ticks(void)
{
    return 1234;
}
/* init, as far as the shell is concerned. */
#include <setjmp.h>
#include <velvet/init.h>
static jmp_buf stopped_here;
static int stops;
static enum init_stop stopped_how;

void init_stop_machine(enum init_stop how)
{
    stops++;
    stopped_how = how;
    kprintf(how == INIT_REBOOT ? "<REBOOT>" : "<POWEROFF>");
    longjmp(stopped_here, 1);
}

/* a machine on its way down, or not. */
static bool machine_stopping;
bool init_stopping(void)
{
    return machine_stopping;
}

/* two services, one of them in the state worth noticing */
void init_snapshot(struct init_table *out)
{
    init_table_reset(out);
    init_add(out, "flusher", NULL, NULL, 0, true);
    init_add(out, "tty1", NULL, NULL, 0, true);
    init_started(out, 0, 11, 0);
    init_started(out, 1, 12, 0);
    init_died(out, 1, 1);
}

static const char *restarted;
bool init_restart(const char *name)
{
    restarted = name;
    return strcmp(name, "tty1") == 0;
}
uint64_t pmm_total_bytes(void)
{
    return 2046ull * 1024 * 1024;
}
uint64_t pmm_used_bytes(void)
{
    return 100ull * 1024;
}
uint64_t pmm_free_bytes(void)
{
    return 2045ull * 1024 * 1024;
}
uint64_t kheap_total_bytes(void)
{
    return 36 * 1024;
}
uint64_t kheap_used_bytes(void)
{
    return 512;
}
void sched_dump(void)
{
    kprintf("<PS>");
}

void sched_show_threads(void)
{
    sched_dump();
}
size_t process_count(void)
{
    return 0;
}

/* the process table, as the `signal` command sees it. */
static struct process fake_proc;
static bool fake_proc_exists;
static int signals_sent, last_signal;

const struct process *process_find(int pid)
{
    if (!fake_proc_exists) { return NULL; }
    fake_proc.pid = pid;
    return &fake_proc;
}
bool process_signal(int pid, int sig)
{
    (void)pid;
    if (sig <= 0 || sig >= SIGNAL_MAX) { return false; }
    signals_sent++;
    last_signal = sig;
    return true;
}
uint64_t pmm_peak_bytes(void)
{
    return 0;
}
uint64_t pmm_metadata_bytes(void)
{
    return 64 * 1024;
}
uint64_t pmm_blocks_at(unsigned order)
{
    return order == 10 ? 511 : 0;
}
struct slab_cache *slab_first_cache(void)
{
    return NULL;
}

/* the disk, which the shell only ever asks about */
#include <velvet/disk.h>
bool disk_ready(size_t w)
{
    return w == DISK_ROOT;
}

/* the installer, which the shell drives and this test does not. */
#include <velvet/install.h>
size_t disk_drive_count(void)
{
    return 1;
}
uint64_t disk_drive_sectors(unsigned d)
{
    (void)d; return 40960;
}
const char *disk_drive_model(unsigned d)
{
    (void)d; return "stand-in";
}
int disk_mounted_drive(void)
{
    return 0;
}
int disk_system_drive(void)
{
    return -1;
}

bool disk_raw_read(unsigned drive, uint64_t lba, uint32_t count, void *buf)
{
    (void)drive; (void)lba;
    memset(buf, 0, (size_t)count * 512);
    return true;
}
bool disk_raw_write(unsigned drive, uint64_t lba, uint32_t count,
                    const void *buf)
{
    (void)drive; (void)lba; (void)count; (void)buf;
    return true;
}

/*
 * the real install.c is linked in, so what is exercised here is the
 * actual sequence and not a description of it. only the half that needs
 * a mounted filesystem is stubbed, because that half is #ifdef'd out of
 * a host build anyway
 */
size_t install_copy_tree(const char *from, const char *to,
                         void (*report)(const char *what),
                         const char **error)
{
    (void)from; (void)to; (void)report; (void)error;
    return 0;
}

bool disk_mount(void)
{
    return true;
}

/*
 * the stamp the build writes into the kernel, which a host link has no
 * generated file to supply. the shell only ever prints it and compares
 * it against what a medium hashes to, and both of those are the same
 * work whatever the number is
 */
const char source_stamp[41] = "0000000000000000000000000000000000000000";
const uint64_t source_stamp_bytes = 0;

/* the wire, as far as the shell is concerned. */
#include <velvet/netif.h>
#include <velvet/e1000.h>

/* the card, as the shell sees it. */
static bool card_there, card_up, card_interrupts;
static uint8_t card_irq;
static ipv4 card_router;
static uint64_t card_ints, card_not_ours;

bool e1000_present(void)
{
    return card_there;
}
const char *e1000_model(void)
{
    return "none";
}
static struct mac no_mac;
const struct mac *e1000_mac(void)
{
    return &no_mac;
}
void e1000_get_stats(struct e1000_stats *out)
{
    memset(out, 0, sizeof *out);
    out->interrupts = card_ints;
    out->not_ours = card_not_ours;
}
bool e1000_interrupts_working(void)
{
    return card_interrupts;
}
uint8_t e1000_irq_line(void)
{
    return card_irq;
}
void e1000_on_arrival(void (*fn)(void))
{
    (void)fn;
}

bool net_is_up(void)
{
    return card_up;
}

ipv4 net_router(void)
{
    return card_router;
}

/* the connection table the shell walks. */
static struct tcp_conn shell_conns[TCP_CONN_MAX];
struct tcp_conn *net_tcp_at(int i)
{
    return (i >= 0 && i < TCP_CONN_MAX) ? &shell_conns[i] : NULL;
}
int net_tcp_connect(ipv4 to, uint16_t port)
{
    (void)to; (void)port; return -1;
}
int net_tcp_listen(uint16_t port)
{
    (void)port; return -1;
}
bool net_tcp_shut(int owner, int h)
{
    (void)owner; (void)h; return false;
}
int net_tcp_open(int owner, ipv4 to, uint16_t port)
{
    (void)owner; (void)to; (void)port; return -1;
}
int net_tcp_accept(int owner, int h, int64_t t)
{
    (void)owner; (void)h; (void)t; return -1;
}
int64_t net_tcp_send(int owner, int h, const void *d, size_t n)
{
    (void)owner; (void)h; (void)d; (void)n; return -1;
}
int64_t net_tcp_recv(int owner, int h, void *o, size_t m, int64_t t)
{
    (void)owner; (void)h; (void)o; (void)m; (void)t; return -1;
}

void net_tcp_forget(int i)
{
    if (i >= 0 && i < TCP_CONN_MAX) { memset(&shell_conns[i], 0, sizeof shell_conns[i]); }
}
bool net_up(ipv4 a, ipv4 m, ipv4 r)
{
    (void)a; (void)m; (void)r; return false;
}
ipv4 net_address(void)
{
    return 0;
}
ipv4 net_netmask(void)
{
    return 0;
}
void net_get_stats(struct net_stats *out)
{
    memset(out, 0, sizeof *out);
}
bool net_arp_request(ipv4 who)
{
    (void)who; return false;
}
bool net_ping_send(ipv4 to, uint16_t seq)
{
    (void)to; (void)seq; return false;
}
bool net_ping_seen(uint16_t seq, uint64_t *when)
{
    (void)seq; (void)when; return false;
}
static struct arp_cache empty_cache;
const struct arp_cache *net_arp_cache(void)
{
    return &empty_cache;
}

/*
 * a small tree, so completion has directories to descend into and
 * names that share prefixes to be careful about
 */
static const struct { const char *dir, *name; bool is_dir; } disk_tree[] = {
    { "/", "welcome.txt",     false },
    { "/", "notes",           true  },
    { "/", "hello.txt",       false },
    { "/", "big.bin",         false },
    { "/notes", "deep.txt",   false },
    { "/notes", "deeper.txt", false },
};
bool disk_lookup(size_t w, const char *path, struct disk_entry *out)
{
    (void)w;
    for (size_t i = 0; i < sizeof disk_tree / sizeof disk_tree[0]; i++) {
        char full[160];
        snprintf(full, sizeof full, "%s%s%s", disk_tree[i].dir,
                 disk_tree[i].dir[1] == '\0' ? "" : "/", disk_tree[i].name);
        if (strcmp(full, path) != 0) continue;
        memset(out, 0, sizeof *out);
        strcpy(out->name, disk_tree[i].name);
        out->is_dir = disk_tree[i].is_dir;
        return true;
    }
    return false;
}
int64_t disk_read(size_t w, uint32_t c, uint64_t s, uint64_t o, void *b,
                  uint64_t l)
{
    (void)w;
    (void)c; (void)s; (void)o; (void)b; (void)l; return -1;
}
bool disk_create(size_t w, const char *path, struct disk_entry *out)
{
    (void)w;
    (void)path; (void)out; return false;
}
bool disk_mkdir(size_t w, const char *path)
{
    (void)w; (void)path; return true;
}
bool disk_rmdir(size_t w, const char *path)
{
    (void)w; (void)path; return true;
}
bool disk_unlink(size_t w, const char *path)
{
    (void)w; (void)path; return true;
}
/* the things a filesystem with opinions can be told. */
bool disk_lookup_nofollow(size_t w, const char *path,
                          struct disk_entry *out)
{
    return disk_lookup(w, path, out);
}
bool disk_readlink(size_t w, const char *p, char *o, size_t n)
{
    (void)w;
    (void)p; (void)o; (void)n; return false;
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
bool disk_journalled(size_t w)
{
    (void)w; return false;
}

/* a drive with two partitions on it, so `parts` and `mount <n>` have something to say. */
static struct disk_part fake_parts[2] = {
    { { 2048, 8192, 0, 1, 0x83, true, "", "linux" }, PART_MBR, true, "ext4" },
    { { 10240, 4096, 0, 2, 0x0c, false, "", "fat32" }, PART_MBR, false, "" },
};
static int mounted_part = 0;
static bool mount_part_ok = true;

size_t disk_part_count(void)
{
    return 2;
}
bool disk_part_at(size_t i, struct disk_part *out)
{
    if (i >= 2) return false;
    *out = fake_parts[i];
    return true;
}
int disk_mounted_part(size_t w)
{
    return w == DISK_ROOT ? mounted_part : -1;
}
bool disk_mount_part(size_t w, size_t i)
{
    if (!mount_part_ok || w != DISK_ROOT) return false;
    mounted_part = (int)i;
    return true;
}
const char *disk_mount_point(size_t w)
{
    return w == DISK_WORK ? DISK_WORK_AT : "/";
}
enum disk_kind disk_which(size_t w)
{
    return w == DISK_ROOT ? DISK_FAT32 : DISK_NONE;
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
    (void)w; (void)from; (void)to; return true;
}
int64_t disk_write_at(size_t w, struct disk_entry *e, uint64_t o,
                      const void *b, uint64_t l)
{
    (void)w; (void)e; (void)o; (void)b; (void)l; return -1;
}
bool disk_readdir(size_t w, const char *path, size_t index,
                  struct disk_entry *out)
{
    (void)w;
    size_t seen = 0;
    for (size_t i = 0; i < sizeof disk_tree / sizeof disk_tree[0]; i++) {
        if (strcmp(disk_tree[i].dir, path) != 0) continue;
        if (seen++ != index) continue;
        memset(out, 0, sizeof *out);
        strcpy(out->name, disk_tree[i].name);
        out->is_dir = disk_tree[i].is_dir;
        return true;
    }
    return false;
}
/*
 * the cache, as far as the shell is concerned: something that can be
 * dirty and can be told to stop being. what is under test here is that
 * the two ways this machine stops both write first, a reboot that
 * does not sync throws away whatever had not reached the drive
 */
static bool cache_dirty = true;
static int  syncs;
static bool sync_ok = true;

bool disk_sync(void)
{
    syncs++; if (sync_ok) cache_dirty = false; return sync_ok;
}
bool disk_dirty(void)
{
    return cache_dirty;
}

/*
 * the checker is real code with a real suite of its own, what the
 * shell owes is only that it asks and prints the answer, so here the
 * answer is a fixed one
 */
/*
 * the filesystem's size, as far as the shell is concerned: a number it
 * asks for, a number it may ask for, and a call that says yes. what is
 * under test here is what the *command* says, since growing a
 * filesystem has a suite of its own that does it for real
 */
static uint64_t stub_blocks = 16384;
uint64_t disk_room(size_t w)
{
    return w == DISK_ROOT ? 65536 : 0;
}
uint64_t disk_ceiling(size_t w)
{
    return w == DISK_ROOT ? 262144 : 0;
}
bool disk_resize(size_t w, uint64_t blocks, struct ext4_resize *out,
                 const char **error)
{
    memset(out, 0, sizeof *out);
    *error = NULL;
    if (w != DISK_ROOT) {
        *error = "nothing there";
        return false;
    }
    out->blocks_before = (uint32_t)stub_blocks;
    out->groups_before = 2;
    out->inodes_before = 1024;
    stub_blocks = blocks;
    out->blocks_after = (uint32_t)blocks;
    out->groups_after = 8;
    out->inodes_after = 4096;
    return true;
}

bool disk_fsck(size_t w, bool mend, struct fsck_report *out,
               const char **error)
{
    (void)w; (void)mend;
    *error = NULL;
    memset(out, 0, sizeof *out);
    out->blocks = 16384;
    out->block_size = 1024;
    out->inodes = 1024;
    out->groups = 2;
    return true;
}
void disk_cache_stats(struct bcache_stats *out)
{
    memset(out, 0, sizeof *out);
    out->held = 3;
    out->hits = 90;
    out->misses = 10;
}

const char *disk_label(size_t w)
{
    (void)w; return "VELVET";
}
const char *disk_model(void)
{
    return "QEMU HARDDISK";
}
uint64_t disk_bytes(void)
{
    return 64ull * 1024 * 1024;
}
uint32_t disk_cluster_bytes(size_t w)
{
    (void)w; return 512;
}
bool disk_usage(size_t w, uint64_t *used, uint64_t *total)
{
    if (w != DISK_ROOT) return false;
    *used = 32 * 1024; *total = 64ull * 1024 * 1024;
    return true;
}
uint64_t ahci_sectors(void)
{
    return 131072;
}

/* the processors, which the shell only ever asks about */
#include <asm/smp.h>
static struct cpu fake_cpus[2] = {
    { .index = 0, .apic_id = 0, .reported_id = 0, .online = true,
      .bootstrap = true, .scheduling = true },
    { .index = 1, .apic_id = 1, .reported_id = 1, .online = true,
      .bootstrap = false, .scheduling = true },
};
size_t smp_cpu_count(void)
{
    return 2;
}
size_t smp_online_count(void)
{
    return 2;
}
const struct cpu *smp_cpu_at(size_t i)
{
    return (i < 2) ? &fake_cpus[i] : NULL;
}
const char *sched_cpu_running(unsigned cpu)
{
    return cpu == 0 ? "shell" : "idle1";
}
size_t sched_cores_scheduling(void)
{
    return 2;
}
uint64_t syscall_times_called(unsigned n)
{
    (void)n; return 0;
}
const char *syscall_name(unsigned n)
{
    (void)n; return "x";
}
bool input_haskey(void)
{
    return true;
}
bool interrupts_on_apic(void)
{
    return false;
}
bool interrupts_use_ioapic(void)
{
    return false;
}
int input_getchar(void)
{
    int c = pending_key; pending_key = -1; return c;
}
void klog_dump(void)
{
    kprintf("<DMESG>");
}
size_t pipe_count(void)
{
    return 0;
}
void vmm_show(void)
{
}

void vmm_dump(uint64_t v)
{
    kprintf("<VMM %#lx>", v);
}
void kbacktrace(uint64_t rbp, uint64_t rip)
{
    (void)rbp; (void)rip; kprintf("<BT>");
}
uint64_t vmm_translate(uint64_t pml4, uint64_t v)
{
    (void)pml4; (void)v; return v;
}
#include <velvet/rtc.h>
void rtc_read(struct rtc_time *t)
{
    t->second = 5; t->minute = 4; t->hour = 3;
    t->day = 2; t->month = 1; t->year = 2026;
}
void cpu_brand(char *buf)
{
    strcpy(buf, "Imaginary CPU @ 1 Hz");
}
void console_size(size_t *c, size_t *r, size_t *w, size_t *h)
{
    if (c) *c = 160;
    if (r) *r = 50;
    if (w) *w = 1280;
    if (h) *h = 800;
}
const unsigned long ksym_count = 442;

/* the real ramdisk parser, mounted on the real archive the build produces. */
#include <velvet/ramdisk.h>

#include <velvet/sched.h>
size_t sched_thread_count(void)
{
    return 4;
}
static enum sched_kill_result kill_answer = SCHED_KILL_OK;
static int killed_id = -1;
enum sched_kill_result sched_kill(int id)
{
    killed_id = id; return kill_answer;
}
uint64_t vmm_kernel_pml4(void)
{
    return 0x1000;
}
void *kmalloc(size_t n)
{
    return malloc(n);
}
void kfree(void *p)
{
    free(p);
}
void sleep_ms(uint64_t ms)
{
    (void)ms;
}

/*
 * cmd_summon reads t->id off whatever the test hands back, so hand back
 * something real rather than a poked-in pointer value
 */
#include <velvet/thread.h>
static struct thread spawned = { .id = 42 };
static int created;
static const char *created_name;
struct thread *thread_create(const char *n, void (*e)(void *), void *a)
{
    (void)e; (void)a;
    created++; created_name = n;
    return &spawned;
}

#include "shell/shell.c"


static int failures;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); failures++; } } while (0)

static void check_split(const char *input, int want_argc, const char *want0,
                        const char *want1)
{
    char buf[128];
    char *argv[ARGV_MAX];
    snprintf(buf, sizeof buf, "%s", input);
    int argc = split(buf, argv, ARGV_MAX);
    if (argc != want_argc) {
        printf("FAIL split(\"%s\"): argc=%d want %d\n", input, argc, want_argc);
        failures++;
        return;
    }
    if (want0 && (argc < 1 || strcmp(argv[0], want0) != 0)) {
        printf("FAIL split(\"%s\"): argv[0]=\"%s\" want \"%s\"\n",
               input, argc > 0 ? argv[0] : "(none)", want0);
        failures++;
    }
    if (want1 && (argc < 2 || strcmp(argv[1], want1) != 0)) {
        printf("FAIL split(\"%s\"): argv[1]=\"%s\" want \"%s\"\n",
               input, argc > 1 ? argv[1] : "(none)", want1);
        failures++;
    }
}

/* a quoted word is one word. */
static void check_quotes(void)
{
    check_split("git commit \"a message\"", 3, "git", "commit");

    char buf[128];
    char *argv[ARGV_MAX];
    snprintf(buf, sizeof buf, "git commit \"a message\"");
    int argc = split(buf, argv, ARGV_MAX);
    if (argc != 3 || strcmp(argv[2], "a message") != 0) {
        printf("FAIL quoted argument came out as \"%s\"\n",
               argc > 2 ? argv[2] : "(none)");
        failures++;
    }

    /*
     * single quotes the same way, a quoted word alone on a line, and
     * punctuation inside quotes staying inside them rather than
     * becoming a pipe
     */
    snprintf(buf, sizeof buf, "echo 'two words' | wc");
    argc = split(buf, argv, ARGV_MAX);
    if (argc != 4 || strcmp(argv[1], "two words") != 0
        || strcmp(argv[2], "|") != 0) {
        printf("FAIL single quotes: argc=%d argv[1]=\"%s\"\n",
               argc, argc > 1 ? argv[1] : "(none)");
        failures++;
    }

    /*
     * and one that is never closed, which must be the rest of the line
     * rather than a walk off the end of it
     */
    snprintf(buf, sizeof buf, "echo \"never closed");
    argc = split(buf, argv, ARGV_MAX);
    if (argc != 2 || strcmp(argv[1], "never closed") != 0) {
        printf("FAIL unterminated quote: argc=%d argv[1]=\"%s\"\n",
               argc, argc > 1 ? argv[1] : "(none)");
        failures++;
    }
}

static void run(const char *line)
{
    char buf[128];
    snprintf(buf, sizeof buf, "%s", line);
    out_reset();

    /* somewhere for a command that never returns to come back to */
    if (setjmp(stopped_here) == 0) {
        run_line(buf);
    }
}

/* a variable's value in the session, or "" when it is not set */
static const char *var(const char *name)
{
    static char value[ENV_VALUE_MAX];
    if (!env_block_get(me()->env, me()->env_len, name, value, sizeof value)) {
        value[0] = '\0';
    }
    return value;
}

/*
 * the monitor: the shell's commands with nobody logged in, and none of
 * what is about programs, since running them is the system's
 */
static void monitor_tests(void)
{

    run("bin/hello");
    CHECK(strstr(out, "runs no programs") != NULL && me()->status == 127,
          "the monitor runs no program by name");
    run("run bin/hello");
    CHECK(strstr(out, "runs no programs") != NULL, "nor with run");
    run("jobs");
    CHECK(strstr(out, "runs no programs") != NULL, "and has no jobs");
    run("echo a | wc");
    CHECK(strstr(out, "joins nothing") != NULL,
          "and joins nothing with a pipe");
    run("help");
    CHECK(strstr(out, "dmesg") != NULL && strstr(out, " run ") == NULL
          && strstr(out, "jobs") == NULL && strstr(out, "ring 3") == NULL,
          "help lists the monitor's commands, and no programs");
    run("help hello");
    CHECK(strstr(out, "not something you can type") != NULL,
          "and help for a program does not run the program to ask it");
    run("pwd");
    CHECK(strstr(out, "/") != NULL && strstr(out, "monitor") == NULL,
          "while its own commands work as they always did");

}

int main(void)
{
    check_quotes();

    /*
     * a session starts here rather than in a static initialiser, since
     * there is one per console now and a zeroed struct is not a started
     * session. shell_run does this on the real machine
     */
    session_init();

    /*
     * mount the medium the build just made, so completion is exercised
     * against names the kernel really looks up
     */
    {
        FILE *fp = fopen("bin/ramdisk.tar", "rb");
        if (fp == NULL) {
            printf("FAIL: no bin/ramdisk.tar, run `make bin/ramdisk.tar`\n");
            return 1;
        }
        static uint8_t tarbytes[1024 * 1024];
        size_t n = fread(tarbytes, 1, sizeof tarbytes, fp);
        fclose(fp);
        ramdisk_mount(tarbytes, n);
        if (!ramdisk_present()) {
            printf("FAIL: the archive did not parse\n");
            return 1;
        }
    }


    check_split("help", 1, "help", NULL);
    check_split("echo hello", 2, "echo", "hello");
    check_split("   echo   hello   ", 2, "echo", "hello");
    check_split("", 0, NULL, NULL);
    check_split("      ", 0, NULL, NULL);
    check_split("summon jack-frost", 2, "summon", "jack-frost");
    /* more words than ARGV_MAX must clamp, not scribble past the array */
    check_split("a b c d e f g h i j k l", ARGV_MAX, "a", "b");

    /* a bar is its own word however it was typed. */
    check_split("cat x | head", 4, "cat", "x");
    check_split("cat x|head", 4, "cat", "x");
    check_split("cat x |head", 4, "cat", "x");
    check_split("|", 1, "|", NULL);
    check_split("echo hi > f", 4, "echo", "hi");
    check_split("echo hi>f", 4, "echo", "hi");
    check_split("echo hi>>f", 4, "echo", "hi");
    check_split("sort <a >b", 5, "sort", "<");
    {
        char buf[64] = "echo hi>>f";
        char *argv[ARGV_MAX];
        int n = split(buf, argv, ARGV_MAX);
        CHECK(n == 4 && strcmp(argv[2], ">>") == 0 && strcmp(argv[3], "f") == 0,
              "two arrows with no spaces are still one word, not two");
    }
    {
        char buf[64] = "a|b";
        char *argv[ARGV_MAX];
        int n = split(buf, argv, ARGV_MAX);
        CHECK(n == 3 && strcmp(argv[0], "a") == 0 && strcmp(argv[1], "|") == 0
              && strcmp(argv[2], "b") == 0,
              "a bar with no spaces round it still separates the two sides");
    }

    /*
     * an environment is a thing inherited rather than asked for, which
     * is why `export` is a shell builtin everywhere and has been since
     * 1977: a command could only ever have changed its own
     */

    my_console = 0;
    session_init();

    run("set");
    CHECK(strstr(out, "nothing is set") != NULL, "a new session has none");

    run("set GREETING hee-ho");
    run("set");
    CHECK(strstr(out, "GREETING=hee-ho") != NULL, "one can be set");
    run("set GREETING");
    CHECK(strstr(out, "hee-ho") != NULL, "and read back by name");

    /*
     * everything after the name, joined, `set X a b` is one variable
     * rather than a complaint about arguments
     */
    run("set PHRASE the bond endures");
    run("set PHRASE");
    CHECK(strstr(out, "the bond endures") != NULL,
          "and a value may have spaces in it");

    /*
     * setting twice replaces rather than leaving two of it, which a
     * walk would then find whichever came first
     */
    run("set GREETING different");
    out_reset();
    run("set");
    CHECK(strstr(out, "hee-ho") == NULL, "setting one again replaces it");
    CHECK(strstr(out, "GREETING=different") != NULL, "with the new value");

    run("set BAD=NAME x");
    CHECK(strstr(out, "equals") != NULL,
          "a name with an equals in it is refused, the first equals is "
          "what separates the name from the value, so one inside could "
          "never be looked up again");

    run("unset GREETING");
    run("set GREETING");
    CHECK(strstr(out, "not set") != NULL, "and one can be forgotten");

    /* not set and set-to-nothing are different answers */
    run("set EMPTY");
    CHECK(strstr(out, "not set") != NULL, "a name nobody set is not set");



    /* what a variable expanded to, caught by setting another to it */
    run("set NAME velvet");
    run("set OUT $NAME");
    CHECK(strcmp(var("OUT"), "velvet") == 0, "$NAME expands");

    run("set OUT ${NAME}room");
    CHECK(strcmp(var("OUT"), "velvetroom") == 0,
          "and braces say where the name stops");

    run("set OUT x$NOTHING");
    CHECK(strcmp(var("OUT"), "x") == 0,
          "a name nobody set expands to nothing at all");

    /* expansion happens before the split, so a value with a space in it becomes two words. */
    {
        char expanded[LINE_MAX];
        char *words[ARGV_MAX];
        expand("x $PHRASE", expanded, sizeof expanded);
        CHECK(split(expanded, words, ARGV_MAX) == 4,
              "a value with spaces becomes several words");
    }

    run("set OUT $");
    CHECK(strcmp(var("OUT"), "$") == 0,
          "and a lone dollar is a dollar rather than eating what follows");

    run("test a = a");
    run("set OUT $?");
    CHECK(strcmp(var("OUT"), "0") == 0,
          "$? is what the last thing exited with");

    run("test a = b");
    run("set OUT $?");
    CHECK(strcmp(var("OUT"), "1") == 0, "whatever that was");

    run("nosuchcommand");
    run("set OUT $?");
    CHECK(strcmp(var("OUT"), "127") == 0,
          "and 127 for a name that is not a command, as everywhere else");

    run("test a = a");
    CHECK(me()->status == 0, "test says yes by exiting zero");
    run("test a = b");
    CHECK(me()->status == 1, "and no by exiting one");
    run("test -z ''");
    run("test 3 -lt 5");
    CHECK(me()->status == 0, "numbers compare as numbers");
    run("test 30 -lt 5");
    CHECK(me()->status == 1, "rather than as text, 30 is not less than 5");
    run("test -f /hello.txt");
    CHECK(me()->status == 0, "and a file can be asked about");
    run("test -f /nosuchfile");
    CHECK(me()->status == 1, "including one that is not there");
    run("test -d /notes");
    CHECK(me()->status == 0, "and a directory");



    /* the interpreter is where the real logic is, so it is driven directly rather than through a file. */
    {
        static char script[2048];
        #define SCRIPT(text) do { \
            strcpy(script, text); \
            out_reset(); \
            run_script_text(script, "<test>"); \
        } while (0)

        SCRIPT("set A one\nset B two\n");
        run("set A");
        CHECK(strstr(out, "one") != NULL, "a script runs its lines");

        /*
         * and in this session, which is what makes one worth having at
         * login: a script's `set` sets *these* variables
         */
        run("set B");
        CHECK(strstr(out, "two") != NULL, "in the session that ran it");



        SCRIPT("if test 1 -eq 1\n"
               "  set TAKEN yes\n"
               "end\n");
        run("set TAKEN");
        CHECK(strstr(out, "yes") != NULL, "a true if runs its body");

        run("unset TAKEN");
        SCRIPT("if test 1 -eq 2\n"
               "  set TAKEN yes\n"
               "end\n");
        run("set TAKEN");
        CHECK(strstr(out, "not set") != NULL, "and a false one does not");

        SCRIPT("if test 1 -eq 2\n"
               "  set WHICH then\n"
               "else\n"
               "  set WHICH else\n"
               "end\n");
        run("set WHICH");
        CHECK(strstr(out, "else") != NULL, "else runs when the if did not");

        SCRIPT("if test 1 -eq 1\n"
               "  set WHICH then\n"
               "else\n"
               "  set WHICH else\n"
               "end\n");
        run("set WHICH");
        CHECK(strstr(out, "then") != NULL, "and does not when it did");

        /*
         * nesting, and the part that is easy to get wrong: a block
         * inside one that is not running must not run, however true its
         * own condition is
         */
        run("unset INNER");
        SCRIPT("if test 1 -eq 2\n"
               "  if test 1 -eq 1\n"
               "    set INNER yes\n"
               "  end\n"
               "end\n");
        run("set INNER");
        CHECK(strstr(out, "not set") != NULL,
              "a true if inside a false one does not run, which is the "
              "thing a flag rather than a stack gets wrong");

        run("unset INNER");
        SCRIPT("if test 1 -eq 1\n"
               "  if test 1 -eq 1\n"
               "    set INNER yes\n"
               "  end\n"
               "end\n");
        run("set INNER");
        CHECK(strstr(out, "yes") != NULL, "and a true one inside a true one does");

        /* and the condition itself must not run either. */
        run("unset SKIPPED");
        SCRIPT("if test 1 -eq 2\n"
               "  if set SKIPPED yes\n"
               "  end\n"
               "end\n");
        CHECK(var("SKIPPED")[0] == '\0',
              "a condition inside a block that is not running is not even "
              "evaluated, a command is a command, and one in a skipped "
              "branch would still have done whatever it does");

        SCRIPT("if test 1 -eq 2\n"
               "  while set SKIPPED yes\n"
               "  end\n"
               "end\n");
        CHECK(var("SKIPPED")[0] == '\0', "and neither is a while's");



        SCRIPT("set N 0\n"
               "while test $N -lt 3\n"
               "  set N 1\n"
               "  set N 2\n"
               "  set N 3\n"
               "end\n"
               "set DONE yes\n");
        run("set N");
        CHECK(strstr(out, "3") != NULL, "a while loops until its test fails");
        run("set DONE");
        CHECK(strstr(out, "yes") != NULL, "and the script carries on after it");

        /* a while whose condition is false from the start runs nothing */
        run("unset NEVER");
        SCRIPT("while test 1 -eq 2\n"
               "  set NEVER yes\n"
               "end\n");
        run("set NEVER");
        CHECK(strstr(out, "not set") != NULL,
              "a while that was never true runs nothing at all");

        /* one that never ends is stopped rather than hanging the machine. */
        SCRIPT("while test 1 -eq 1\n"
               "  set SPIN yes\n"
               "end\n");
        CHECK(strstr(out, "stopped after") != NULL,
              "a loop that never ends is stopped rather than hanging the "
              "machine");



        run("unset C");
        SCRIPT("# this is a comment\n"
               "\n"
               "set C yes   # and so is this\n");
        run("set C");
        CHECK(strstr(out, "yes") != NULL, "comments and blank lines are skipped");
        CHECK(strstr(out, "#") == NULL, "and a trailing one is not part of the value");



        SCRIPT("end\n");
        CHECK(strstr(out, "nothing open") != NULL, "an end with nothing open");

        SCRIPT("else\n");
        CHECK(strstr(out, "no if") != NULL, "an else with no if");

        SCRIPT("if test 1 -eq 1\n  echo hi\n");
        CHECK(strstr(out, "still open") != NULL,
              "and a file that ends with a block open says so rather than "
              "quietly doing half of it");

        #undef SCRIPT
    }

    /* the resync count is the interesting number. */

    have_mouse = true;
    fake_resyncs = 0;
    run("mouse");
    CHECK(strstr(out, "column 12") != NULL, "mouse says where the pointer is");
    CHECK(strstr(out, "left") != NULL, "and what is held");
    CHECK(strstr(out, "middle button") != NULL,
          "and what it is for, since a pointer on a text console is not "
          "obvious");
    CHECK(strstr(out, "flying off") == NULL,
          "and says nothing about losing sync when it has not");

    fake_resyncs = 900;
    run("mouse");
    CHECK(strstr(out, "flying off") != NULL,
          "but explains the symptom when the count is climbing");

    have_mouse = false;
    run("mouse");
    CHECK(strstr(out, "no mouse") != NULL,
          "a machine with none says so plainly");
    CHECK(strstr(out, "carries on") != NULL,
          "and that it is not a problem");
    have_mouse = true;

    /* the prompt says whose it is, and which console that is */
    my_console = 4;
    out_reset();
    prompt();
    CHECK(strstr(out, "monitor@velvet[5]") != NULL,
          "the prompt says it is the monitor, on the fifth console");
    my_console = 0;
    session_init();

    shown = 0;
    switches = 0;
    run("chvt 3");
    CHECK(shown == 2 && switches == 1, "chvt shows another console");
    run("chvt 1");
    CHECK(shown == 0, "and comes back");

    run("chvt 9");
    CHECK(strstr(out, "numbered from 1") != NULL,
          "a console that does not exist is refused");
    run("chvt x");
    CHECK(strstr(out, "chvt <n>") != NULL, "and so is something that is not a number");

    run("chvt");
    CHECK(strstr(out, "console 1") != NULL,
          "and with no argument it says which one you are on, alt+f1 "
          "does the same thing from a keyboard, and somebody on a serial "
          "line has neither an alt key nor a function key");

    /* a disk is not a filesystem. */

    run("parts");
    CHECK(strstr(out, "2048") != NULL, "parts lists where each one starts");
    CHECK(strstr(out, "linux") && strstr(out, "fat32"),
          "and what the table says each holds");
    CHECK(strstr(out, "mbr") != NULL, "and which kind of table said so");
    CHECK(strstr(out, "[ext4]") != NULL,
          "and what was actually found on it, which is a different "
          "question from what the table claims");
    CHECK(strstr(out, "*") != NULL, "with a mark on the one that is mounted");

    mounted_part = 0;
    run("mount 1");
    CHECK(mounted_part == 1, "mount <n> moves the mount");
    CHECK(strstr(out, "stale") != NULL,
          "and says what that costs, there is no reference counting "
          "here that could do better, so it says so instead");

    run("mount 1");
    CHECK(strstr(out, "already") != NULL, "mounting the one already there says so");

    run("mount 9");
    CHECK(strstr(out, "no partition 9") != NULL,
          "a number nobody handed out is refused");
    run("mount x");
    CHECK(strstr(out, "parts") != NULL,
          "and something that is not a number points at the list");

    mount_part_ok = false;
    mounted_part = 0;
    run("mount 1");
    CHECK(mounted_part == 0, "a partition with nothing on it is not mounted");
    CHECK(strstr(out, "different question") != NULL,
          "and the difference between what a table claims and what is "
          "there is spelled out");
    mount_part_ok = true;

    /* the bare command still does what it always did */
    run("mount");
    CHECK(strstr(out, "/boot") != NULL, "and `mount` alone still lists them");

    /* a write reached the drive as it was made, so a reboot lost nothing by definition. */

    cache_dirty = true;
    syncs = 0;
    run("sync");
    CHECK(syncs == 1, "sync writes what is waiting");
    CHECK(strstr(out, "written") != NULL, "and says so");

    run("sync");
    CHECK(strstr(out, "nothing waiting") != NULL,
          "and asking again says there is nothing to do");

    cache_dirty = true;
    sync_ok = false;
    run("sync");
    CHECK(strstr(out, "not what the kernel believes") != NULL,
          "a drive that refuses is reported, since the disk and the "
          "machine now disagree and somebody should know");
    sync_ok = true;

    /* both ways of stopping are asked of init now, and neither of them writes anything here. */

    cache_dirty = true;
    syncs = 0;
    stops = 0;
    run("reboot");
    CHECK(stops == 1, "reboot asks init to stop the machine");
    CHECK(stopped_how == INIT_REBOOT, "saying which way");
    CHECK(syncs == 0,
          "and does not sync on its own, doing that here would write "
          "out whatever happened to be dirty while three other sessions "
          "are still adding to it");

    cache_dirty = true;
    stops = 0;
    run("poweroff");
    CHECK(stops == 1, "and poweroff asks the same way");
    CHECK(stopped_how == INIT_POWEROFF, "saying the other");
    CHECK(syncs == 0, "and improvises no more than reboot does");

    /* all three commands have to say so rather than printing an empty table. */

    run("ifconfig");
    CHECK(strstr(out, "no network card") != NULL,
          "`ifconfig` with no card says there is no card");

    run("ping 10.0.2.2");
    CHECK(strstr(out, "not up") != NULL,
          "`ping` says the wire is not up rather than timing out");

    run("arp");
    CHECK(strstr(out, "not up") != NULL, "and so does `arp`");

    /*
     * the card is not polled twenty times a second, it will
     * raise an interrupt instead, keeping the old loop on a half-second
     * timer as a fallback. the fallback is the problem: a machine whose
     * interrupt never arrives *works*, a little late, and looks exactly
     * like one where it does. so `ifconfig` has to say which, and the
     * warning is the branch worth getting right, the quiet one is the
     * one that would be missed
     */
    card_there = true;

    /* a card with no address still says how it is being noticed. */
    card_up = false;
    card_interrupts = false;
    card_irq = 0;
    run("ifconfig");
    CHECK(strstr(out, "on a timer") != NULL,
          "the mode shows on a card that has no address yet");

    card_up = true;

    /* three states, not two. */
    card_irq = 0;
    run("ifconfig");
    CHECK(strstr(out, "no interrupt line") != NULL,
          "a card the firmware gave no line says so specifically");

    card_irq = 11;
    card_interrupts = false;
    run("ifconfig");
    CHECK(strstr(out, "nothing has arrived") != NULL,
          "and an armed card with no traffic says that instead");
    CHECK(strstr(out, "11") != NULL, "naming the line it is armed on");
    CHECK(strstr(out, "on a timer") == NULL,
          "which is not the same claim as having no line at all");
    CHECK(strstr(out, "by the card") == NULL,
          "and does not claim both at once");

    /* where anything not on this wire goes, when something said. */
    card_router = IPV4(10, 0, 2, 2);
    run("ifconfig");
    CHECK(strstr(out, "gateway : 10.0.2.2") != NULL,
          "`ifconfig` names the router it hands the rest of the world to");
    card_router = 0;
    run("ifconfig");
    CHECK(strstr(out, "gateway") == NULL,
          "and has no gateway line when there is none");

    run("ifconfig 10.0.2.15 255.255.255.0 nowhere");
    CHECK(strstr(out, "not a router's address") != NULL,
          "a router that is not an address is refused before anything "
          "is set");

    card_interrupts = true;
    card_ints = 41;
    card_not_ours = 0;
    run("ifconfig");
    CHECK(strstr(out, "by the card") != NULL, "and says so when it has");
    CHECK(strstr(out, "41") != NULL, "with the count");
    CHECK(strstr(out, "on a timer") == NULL, "and drops the warning");
    CHECK(strstr(out, "somebody else") == NULL,
          "a line nobody else is on says nothing about sharing it");

    card_not_ours = 7;
    run("ifconfig");
    CHECK(strstr(out, "somebody else") != NULL,
          "a shared line is reported rather than looking like a fault");

    /* a name is a program's to look up, so the monitor takes addresses. */
    run("ping nowhere.example");
    CHECK(strstr(out, "not an address") != NULL,
          "`ping` refuses a name rather than sending it anywhere");

    /* the shell's job is to turn a name into a number and to refuse what should be refused. */
    fake_proc_exists = true;
    fake_proc.uid = 0;

    signals_sent = 0;
    run("signal 7 int");
    CHECK(signals_sent == 1 && last_signal == SIGINT,
          "a signal is sent by name");
    CHECK(strstr(out, "interrupt") != NULL, "and named back");

    run("signal 7");
    CHECK(last_signal == SIGTERM,
          "with no name it is term, the one that asks rather than the "
          "one that does not");

    run("signal 7 kill");
    CHECK(last_signal == SIGKILL, "kill is kill");
    CHECK(strstr(out, "cannot be caught") != NULL,
          "and says so, because that is the whole reason it exists");

    signals_sent = 0;
    run("signal 7 wibble");
    CHECK(signals_sent == 0, "a name nobody uses sends nothing");
    CHECK(strstr(out, "not a signal") != NULL, "and says which word");

    run("signal nought int");
    CHECK(signals_sent == 0, "and neither does a pid that is not a number");

    fake_proc_exists = false;
    run("signal 99 int");
    CHECK(signals_sent == 0, "or one nobody holds");
    CHECK(strstr(out, "no process") != NULL, "which is said plainly");

    card_there = false;
    card_up = false;
    card_interrupts = false;
    card_irq = 0;
    card_ints = card_not_ours = 0;

    /*
     * the address parsing is linked for real, so a bad address must be
     * refused before anything touches a card
     */
    run("ping nonsense");
    CHECK(strstr(out, "not up") != NULL,
          "and the wire being down is checked before the address is");

    /*
     * the drives this test offers carry nothing, so `install` must say
     * there is no system to copy rather than copying zeroes onto a disk
     * and reporting success. that is the branch a machine booted some
     * other way takes, and it is the one worth having covered here,
     * the sequence itself has its own suite
     */

    run("install");
    CHECK(strstr(out, "not running from a boot medium") != NULL,
          "`install` with nothing to copy says so rather than trying");

    run("install 0");
    CHECK(strstr(out, "not running from a boot medium") != NULL,
          "and naming a drive does not change that");

    run("install nonsense");
    CHECK(strstr(out, "not running from a boot medium") != NULL,
          "nor does naming something that is not a drive");



    run("init");
    CHECK(strstr(out, "flusher") != NULL && strstr(out, "tty1") != NULL,
          "`init` lists the services");
    CHECK(strstr(out, "running") != NULL, "with what each one is doing");
    CHECK(strstr(out, "stopped") != NULL,
          "including the ones that are not, a supervisor you cannot "
          "look at is one you have to take on faith");

    restarted = NULL;
    run("init start tty1");
    CHECK(restarted != NULL && strcmp(restarted, "tty1") == 0,
          "and one that is down can be started by name");
    CHECK(strstr(out, "started") != NULL, "which says so");

    run("init start nosuch");
    CHECK(strstr(out, "no '") != NULL,
          "and asking for one that does not exist is a plain no");

    run("help");
    CHECK(strstr(out, "summon") && strstr(out, "reboot"),
          "help lists the commands");

    /* the list is the builtins, and the names only */
    out_reset();
    run("help");
    CHECK(strstr(out, "cd") != NULL, "help lists a builtin");
    CHECK(strstr(out, "ring 3") == NULL && strstr(out, "$PATH") == NULL,
          "and no programs, since the monitor runs none");
    CHECK(strstr(out, "help <name>") != NULL,
          "and points at where one thing is actually explained");
    CHECK(strstr(out, "go somewhere; no argument") == NULL,
          "without a description beside every single one");

    out_reset();
    run("help cd");
    CHECK(strstr(out, "cd [directory]") != NULL,
          "a builtin explains itself out of the table");
    CHECK(strstr(out, "go somewhere") != NULL, "with what it is for");

    out_reset();
    run("help nonsense");
    CHECK(strstr(out, "not something you can type") != NULL,
          "and a name that is neither says so");

    /* a file is not a command, however much it is there */
    run("/init");
    CHECK(strstr(out, "not one of the monitor's commands") != NULL
          && me()->status == 127,
          "a file on the medium is not a command");

    run("");
    CHECK(out_len == 0, "empty line does nothing at all");

    run("     ");
    CHECK(out_len == 0, "whitespace-only line does nothing");

    run("clear");
    CHECK(strcmp(out, "<CLEAR>") == 0, "clear reaches the console");

    run("ps");
    CHECK(strcmp(out, "<PS>") == 0, "ps reaches the scheduler");

    run("mem");
    CHECK(strstr(out, "2046") && strstr(out, "36"),
          "mem reports both pmm and heap");

    run("bt");
    CHECK(strcmp(out, "<BT>") == 0, "bt reaches the stack walker");

    run("nonsense");
    CHECK(strstr(out, "nonsense") && strstr(out, "help"),
          "unknown command names itself and points at help");

    /* summon: known, unknown, and bare */
    created = 0;
    run("summon pixie");
    CHECK(created == 1 && strcmp(created_name, "pixie") == 0,
          "summon pixie spawns a thread named pixie");

    created = 0;
    run("summon gorgon");
    CHECK(created == 0 && strstr(out, "gorgon") != NULL,
          "unknown persona spawns nothing and says so");

    created = 0;
    run("summon");
    CHECK(created == 0 && strstr(out, "pixie") && strstr(out, "jack-frost"),
          "bare summon lists the register");


    hist_count = 0;
    history_add("mem");
    history_add("ps");
    CHECK(hist_count == 2, "two commands remembered");
    CHECK(strcmp(history[0], "mem") == 0 && strcmp(history[1], "ps") == 0,
          "history is in the order they were typed");

    history_add("ps");
    CHECK(hist_count == 2, "the same command twice running is remembered once");

    history_add("");
    CHECK(hist_count == 2, "a bare enter is not remembered");

    history_add("mem");
    CHECK(hist_count == 3, "a repeat that isnt adjacent still counts");

    /* overflow: fill past HISTORY_MAX and check the oldest fall off */
    hist_count = 0;
    char tmp[32];
    for (int i = 0; i < HISTORY_MAX + 5; i++) {
        snprintf(tmp, sizeof tmp, "cmd%d", i);
        history_add(tmp);
    }
    CHECK(hist_count == HISTORY_MAX, "history caps at HISTORY_MAX");
    CHECK(strcmp(history[HISTORY_MAX - 1], "cmd20") == 0,
          "newest command is at the bottom");
    CHECK(strcmp(history[0], "cmd5") == 0,
          "oldest survivor is the right one after rotation");

    /* a line longer than LINE_MAX must be truncated, not overflow */
    hist_count = 0;
    char big[LINE_MAX * 2];
    memset(big, 'x', sizeof big - 1);
    big[sizeof big - 1] = 0;
    history_add(big);
    CHECK(strlen(history[0]) == LINE_MAX - 1, "overlong line truncated safely");


    {
        char line[LINE_MAX] = "hello";
        size_t len = 5, pos = 5;
        out_reset();
        replace_line(line, &len, &pos, "ps");
        CHECK(strcmp(line, "ps") == 0 && len == 2 && pos == 2,
              "replace_line swaps the content and leaves the cursor at the end");
        /*
         * walk back over the old text, print the new, blank the excess,
         * then step back over the blanks
         */
        CHECK(strcmp(out, "\b\b\b\b\b" "ps" "   " "\b\b\b") == 0,
              "replace_line covers the longer line it replaced");

        out_reset();
        replace_line(line, &len, &pos, "");
        CHECK(line[0] == 0 && len == 0 && pos == 0,
              "replace_line can clear the line entirely");

        len = 0; pos = 0; line[0] = 0;
        out_reset();
        replace_line(line, &len, &pos, "uptime");
        CHECK(strcmp(out, "uptime") == 0 && len == 6 && pos == 6,
              "growing from an empty line just prints");
    }


    {
        out_reset();
        move_left(3);
        CHECK(strcmp(out, "\b\b\b") == 0, "move_left is pure backspaces");

        char line[LINE_MAX] = "abcd";
        out_reset();
        redraw_tail(line, 4, 2);
        CHECK(strcmp(out, "cd " "\b\b\b") == 0,
              "redraw_tail reprints the tail, blanks one, and comes back");
    }


    {
        char line[LINE_MAX]; size_t len, pos;

        strcpy(line, "hex"); len = 3; pos = 3;
        out_reset();
        complete(line, &len, &pos);
        CHECK(strcmp(line, "hexdump") == 0 && len == 7 && pos == 7,
              "a unique prefix completes to the whole command");

        /* several candidates sharing no more letters: list them */
        strcpy(line, "c"); len = 1; pos = 1;
        out_reset();
        complete(line, &len, &pos);
        CHECK(strcmp(line, "c") == 0 && len == 1,
              "an ambiguous prefix leaves the line alone");
        CHECK(strstr(out, "clear") && strstr(out, "crash"),
              "and shows what it could have meant");

        /* several candidates that DO share letters: fill those in and say nothing. */
        strcpy(line, "po"); len = 2; pos = 2;
        out_reset();
        complete(line, &len, &pos);
        CHECK(strcmp(line, "poweroff") == 0, "a unique-enough prefix fills in");

        /* an empty word must do nothing at all, thats what help is for */
        strcpy(line, ""); len = 0; pos = 0;
        out_reset();
        complete(line, &len, &pos);
        CHECK(out_len == 0 && len == 0,
              "a bare tab lists nothing, since `help` exists");

        /*
         * a bare tab after a command that takes a filename should
         * answer, unlike a bare tab in the command position, there is
         * no `help` listing files
         */
        strcpy(line, "source "); len = 7; pos = 7;
        out_reset();
        complete(line, &len, &pos);
        CHECK(out_len > 0, "source<tab> with nothing typed offers something");

        /* the monitor runs no programs, so their names complete to nothing */
        strcpy(line, "upt"); len = 3; pos = 3;
        out_reset();
        complete(line, &len, &pos);
        CHECK(strcmp(line, "upt") == 0 && out_len == 0,
              "a program's name is not offered");

        /*
         * the ramdisk is flat and has no leading slash on anything, so
         * a word that starts with one is the disk's business, walked a
         * directory at a time
         */

        /*
         * "/b" is ambiguous, big.bin is also there, so it must fill
         * in no further and list the two instead of picking one
         */
        strcpy(line, "source /b"); len = 9; pos = 9;
        out_reset();
        complete(line, &len, &pos);
        CHECK(strcmp(line, "source /b") == 0,
              "an ambiguous prefix completes no further");
        CHECK(out_len > 0, "and lists what it could have been");

        strcpy(line, "source /bo"); len = 10; pos = 10;
        out_reset();
        complete(line, &len, &pos);
        CHECK(strcmp(line, "source /boot/") == 0,
              "one more character reaches the mount point");

        strcpy(line, "source /boot"); len = 12; pos = 12;
        out_reset();
        complete(line, &len, &pos);
        CHECK(strcmp(line, "source /boot/") == 0,
              "and so does the mount point with no slash yet");

        strcpy(line, "source /w"); len = 9; pos = 9;
        out_reset();
        complete(line, &len, &pos);
        CHECK(strcmp(line, "source /welcome.txt") == 0,
              "a file on the disk completes to its whole path");

        /*
         * a directory has to come back with a slash, so that tabbing
         * again carries on into it rather than stopping at a name that
         * cannot be opened
         */
        strcpy(line, "cd /n"); len = 5; pos = 5;
        out_reset();
        complete(line, &len, &pos);
        CHECK(strcmp(line, "cd /notes/") == 0,
              "a directory completes with a trailing slash");

        /* and then straight on into it */
        strcpy(line, "source /notes/deep"); len = 18; pos = 18;
        out_reset();
        complete(line, &len, &pos);
        CHECK(strcmp(line, "source /notes/deep") == 0,
              "two files sharing a prefix fill in no further");
        CHECK(out_len > 0, "and the pair gets listed instead");

        strcpy(line, "source /notes/deeper"); len = 20; pos = 20;
        out_reset();
        complete(line, &len, &pos);
        CHECK(strcmp(line, "source /notes/deeper.txt") == 0,
              "and one more character is enough to settle it");

        strcpy(line, "source x /h"); len = 11; pos = 11;
        out_reset();
        complete(line, &len, &pos);
        CHECK(strcmp(line, "source x /hello.txt") == 0,
              "a filename anywhere in the arguments completes");

        /* a bare tab after the mount point lists what is there */
        strcpy(line, "cd /"); len = 4; pos = 4;
        out_reset();
        complete(line, &len, &pos);
        CHECK(out_len > 0, "a bare tab at the root lists it");

        /* a path that is not the test's must not be answered with the disk */
        strcpy(line, "source /nowhere/pass"); len = 20; pos = 20;
        out_reset();
        complete(line, &len, &pos);
        CHECK(strcmp(line, "source /nowhere/pass") == 0,
              "a path into a directory that is not there completes to nothing");

        /* a relative path completes the same as an absolute one */
        strcpy(line, "cd notes/d"); len = 10; pos = 10;
        out_reset();
        complete(line, &len, &pos);
        CHECK(strcmp(line, "cd notes/deep") == 0,
              "a relative path completes against the tree, as far as two agree");

        strcpy(line, "source notes/deeper"); len = 19; pos = 19;
        out_reset();
        complete(line, &len, &pos);
        CHECK(strcmp(line, "source notes/deeper.txt") == 0,
              "and settles when one more character is given");

        /* the ramdisk's own names are whole paths, nested ones included */
        strcpy(line, "source selftests/sched/wa"); len = 25; pos = 25;
        out_reset();
        complete(line, &len, &pos);
        CHECK(strcmp(line, "source selftests/sched/wait") == 0,
              "a nested name on the ramdisk completes from a partial path");

        /* signal and setuid share only the s, and stop does too */
        strcpy(line, "source selftests/sched/s"); len = 24; pos = 24;
        out_reset();
        complete(line, &len, &pos);
        CHECK(strcmp(line, "source selftests/sched/s") == 0,
              "names sharing a prefix complete only as far as they agree");

        strcpy(line, "source selftests/sched/si"); len = 25; pos = 25;
        out_reset();
        complete(line, &len, &pos);
        CHECK(strcmp(line, "source selftests/sched/signal") == 0,
              "and one more character settles it");

        /*
         * a word that grows by less than what follows it: the rest of
         * the line moves up over itself, and has to be moved from its
         * far end or it overwrites what it has yet to move
         */
        strcpy(line, "cat mo done and more"); len = 20; pos = 6;
        out_reset();
        replace_word(line, &len, &pos, 4, "motd");
        CHECK(strcmp(line, "cat motd done and more") == 0 && pos == 8
              && len == 22,
              "a longer word keeps all of the line after it");
        strcpy(line, "cat motd.txt done and more"); len = 26; pos = 12;
        out_reset();
        replace_word(line, &len, &pos, 4, "mo");
        CHECK(strcmp(line, "cat mo done and more") == 0 && pos == 6,
              "and so does a shorter one");

        /* filenames after a builtin that takes one */
        strcpy(line, "source in"); len = 9; pos = 9;
        out_reset();
        complete(line, &len, &pos);
        CHECK(strcmp(line, "source init") == 0 && pos == 11,
              "source completes a filename out of the ramdisk");

        /* completing mid-line keeps whatever followed */
        strcpy(line, "source in done"); len = 14; pos = 9;
        out_reset();
        complete(line, &len, &pos);
        CHECK(strcmp(line, "source init done") == 0,
              "and the rest of the line survives the insert");

        strcpy(line, "zzz"); len = 3; pos = 3;
        out_reset();
        complete(line, &len, &pos);
        CHECK(strcmp(line, "zzz") == 0 && out_len == 0,
              "an unmatchable prefix is left in peace");

        /* a builtin that takes no filename offers none */
        strcpy(line, "bt in"); len = 5; pos = 5;
        out_reset();
        complete(line, &len, &pos);
        CHECK(strcmp(line, "bt in") == 0 && out_len == 0,
              "a builtin that takes no file completes nothing");

        /* nor does a name that is not a builtin */
        strcpy(line, "zzz in"); len = 6; pos = 6;
        out_reset();
        complete(line, &len, &pos);
        CHECK(out_len == 0, "an unknown command has no filenames to offer");

        /*
         * complete() must cope with a line the editor has not
         * terminated, which is the state it is really called in
         */
        memset(line, 'X', LINE_MAX);
        line[0]='c'; line[1]='d'; line[2]=' ';
        line[3]='i'; line[4]='n';
        len = 5; pos = 5;
        out_reset();
        complete(line, &len, &pos);
        CHECK(strcmp(line, "cd init") == 0,
              "an unterminated buffer still completes correctly");
    }


    run("date");
    CHECK(strstr(out, "03:04:05") && strstr(out, "january") && strstr(out, "2026"),
          "date reports what the clock said");
    CHECK(strstr(out, "2nd") != NULL, "and gets the ordinal right");

    run("history");
    CHECK(out_len > 0, "history prints something");

    killed_id = -1; kill_answer = SCHED_KILL_OK;
    run("kill 3");
    CHECK(killed_id == 3 && strstr(out, "sea of souls"),
          "kill passes the id through and reports success");

    kill_answer = SCHED_KILL_PROTECTED;
    run("kill 1");
    CHECK(strstr(out, "wheel turning") != NULL, "idle is protected");

    killed_id = -1;
    run("kill notanumber");
    CHECK(killed_id == -1 && strstr(out, "not a thread id"),
          "a non-numeric id never reaches the scheduler");

    run("time ps");
    CHECK(strstr(out, "<PS>") && strstr(out, "ms]"),
          "time runs the command and reports how long it took");

    run("hexdump");
    CHECK(strstr(out, "hexdump <hex address>") != NULL, "hexdump explains itself");

    run("dmesg");
    CHECK(strcmp(out, "<DMESG>") == 0, "dmesg reaches the log");

    /* a near miss gets a suggestion rather than a shrug */
    run("dmseg");
    CHECK(strstr(out, "didst thou mean 'dmesg'") != NULL,
          "one candidate by first letter earns a suggestion");
    run("qqq");
    CHECK(strstr(out, "`help` lists what it has") != NULL,
          "no candidate falls back to pointing at help");

    run("arcana");
    CHECK(strstr(out, "COMPUTER ARCANA") && strstr(out, VELVET_VERSION),
          "arcana names the arcana and the version");
    CHECK(strstr(out, "442") != NULL, "and how many symbols it carries");

    run("persona");
    CHECK(strstr(out, "monitor@velvet") != NULL, "persona has a header");
    CHECK(strstr(out, "Imaginary CPU") != NULL, "and reports the cpu");
    CHECK(strstr(out, "1280x800") != NULL, "and the resolution");
    CHECK(strstr(out, "4 threads") != NULL, "and the thread count");

    monitor_tests();

    if (!failures) printf("all good\n");
    return failures;
}
