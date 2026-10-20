// SPDX-License-Identifier: GPL-2.0-only
/*
 * shell/shell.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the velvet room terminal: the kernel's monitor.
 */

#include <velvet/shell.h>
#include <velvet/input.h>
#include <velvet/console.h>
#include <velvet/tty.h>
#include <velvet/mouse.h>
#include <velvet/pit.h>
#include <velvet/kprintf.h>
#include <velvet/string.h>
#include <velvet/pmm.h>
#include <velvet/pressure.h>
#include <velvet/kmalloc.h>
#include <velvet/slab.h>
#include <velvet/meminfo.h>
#include <velvet/diskinfo.h>
#include <velvet/vmm.h>
#include <velvet/backtrace.h>
#include <velvet/rtc.h>
/*
 * `cpus`, `ioapic`, the syscall counters and the processor name in
 * `arcana` are not features of this shell, they are *reports on this
 * machine*, an io apic to move interrupts onto, a brand string in a
 * cpuid leaf, a table of cores the acpi tables described.
 *
 * on a board with none of those the honest thing is to say so rather
 * than to invent a portable-sounding wrapper that returns nothing on
 * every architecture but one. so they are conditional, and this file
 * stays on checkarch's allow list where it belongs
 */
#include <velvet/ramdisk.h>
#include <velvet/source.h>
#include <velvet/hash.h>
#include <velvet/disk.h>
#include <velvet/fsck.h>
#include <velvet/install.h>
#include <velvet/netif.h>
#include <velvet/ether.h>
#include <velvet/ip.h>
#include <velvet/signal.h>
#include <velvet/e1000.h>
#include <velvet/vfs.h>
#include <velvet/path.h>
#include <velvet/ahci.h>
#include <velvet/pipe.h>
#include <velvet/init.h>

/* and the four that only a pc has. */
#if defined(VELVET_ARCH_X86_64)
#include <asm/cpuinfo.h>
#include <asm/smp.h>
#include <asm/syscall.h>
#include <asm/interrupts.h>
#endif
#include <asm/cpu.h>
#include <velvet/pci.h>
#include <velvet/ksyms.h>
#include <generated/version.h>
#include <velvet/sched.h>
#include <velvet/spinlock.h>
#include <velvet/thread.h>
#include <velvet/process.h>
#include <stdint.h>
#include <stdbool.h>

/*
 * TODO: this one file is the parser, the built ins, job control, the
 * history and the terminal, which is five things and five thousand lines.
 * the built ins have no business here; split them out the way the guide
 * splits a subsystem.
 */
#define LINE_MAX 128

/* TODO: 8 is a silent limit. */
#define ARGV_MAX 8
#define HISTORY_MAX 16

#define COLOR_PROMPT 0x7b8ce0   /* velvet blue */
#define COLOR_TEXT   0xc8c8d0
#define COLOR_WARN   0xe6c245

struct command {
    const char *name;
    const char *help;
    void (*fn)(int argc, char **argv);
    bool takes_file;    /* tab should offer ramdisk names after it */

    /* the shape of the command line, for `help <name>`. */
    const char *usage;
};

static const struct command commands[];    /* defined below, after the handlers */
static void run_argv(int argc, char **argv);

/*
 * what the monitor keeps between one line and the next: where it is
 * standing, what was typed, its variables and how the last command went
 */

#define HISTORY_SESSION_MAX 16

struct session {
    char cwd[PATH_MAX];

    char history[HISTORY_SESSION_MAX][LINE_MAX];
    int  hist_count;

    /*
     * the shell is a kernel thread with no process entry of its own, so
     * its variables live here, in an environment block like a program's
     */
    char env[PROC_ENV_MAX];
    size_t env_len;

    /* what the last thing exited with. `$?`, and what `if` looks at */
    int status;
};

static struct session s_session;

static struct session *me(void)
{
    return &s_session;
}

#define shell_cwd       (me()->cwd)
#define history         (me()->history)
#define hist_count      (me()->hist_count)


/* a session starts here rather than in a static initialiser. */
static void session_init(void)
{
    shell_cwd[0] = '/';
    shell_cwd[1] = '\0';
    hist_count = 0;
    me()->status  = 0;
    me()->env[0]  = '\0';
    me()->env_len = 1;
}
static size_t common_prefix(const char *a, const char *b);



struct persona {
    const char *name;
    const char *line;
    uint64_t    period_ms;
};

static const struct persona personas[] = {
    { "pixie",      "count",   700 },
    { "jack-frost", "hee-ho!", 1300 },
};
#define PERSONA_COUNT (sizeof(personas) / sizeof(personas[0]))

/* how many times a summoned persona speaks before departing. */
#define PERSONA_LINES 8

/*
 * ctrl+c bumps this. every persona remembers what it was when it was
 * summoned, and takes the hint when the number moves. the kernel has no
 * signals and no way to yank a sleeping thread off the run queue, so
 * cancelling is cooperative: a persona notices next time it wakes up,
 * which can be up to one sleep period later
 */
static volatile uint64_t cancel_generation;

static void persona_thread(void *arg)
{
    const struct persona *p = arg;
    uint64_t summoned_at = cancel_generation;

    for (int i = 1; i <= PERSONA_LINES; i++) {
        if (cancel_generation != summoned_at) {
            kprintf("[%s] recalled to the velvet room\n", p->name);
            return;
        }
        kprintf("[%s] %s %d, uptime %lums\n", p->name, p->line, i,
                pit_uptime_ms());
        sleep_ms(p->period_ms);
    }
}



/* `help <name>`. for a builtin, out of the table. */
static void help_one(const char *name)
{
    for (const struct command *c = commands; c->name; c++) {
        if (strcmp(name, c->name) != 0) {
            continue;
        }
        kprintf("%s\n", c->usage != NULL ? c->usage : c->name);
        kprintf("  %s\n", c->help);
        return;
    }

    kprintf("'%s' is not something you can type. `help` lists what is\n", name);
}

static void cmd_help(int argc, char **argv)
{
    if (argc > 1) {
        help_one(argv[1]);
        return;
    }

    /* names, in columns, and nothing else. */
    size_t columns = 0;
    console_size(&columns, NULL, NULL, NULL);
    size_t per_row = (columns > 20) ? (columns - 4) / 12 : 4;
    if (per_row < 2) {
        per_row = 2;
    }

    kprintf("built in\n ");
    size_t n = 0;
    for (const struct command *c = commands; c->name; c++) {
        kprintf(" %s", c->name);
        for (size_t i = strlen(c->name); i < 11; i++) {
            kprintf(" ");
        }
        if (++n % per_row == 0) {
            kprintf("\n ");
        }
    }
    kprintf("\n\n`help <name>` for what one takes\n");
}

static void cmd_clear(int argc, char **argv)
{
    (void)argc; (void)argv;
    console_clear();
}

static void cmd_mem(int argc, char **argv)
{
    (void)argc; (void)argv;
    mm_show_meminfo();
}

/* one mount's worth of it, said the same way whichever mount it is */

static void cmd_disk(int argc, char **argv)
{
    (void)argc; (void)argv;
    disk_show();
}

/* what a drive says it holds. */
static void cmd_parts(int argc, char **argv)
{
    (void)argc; (void)argv;
    disk_show_partitions();
}

/* what a write-back cache costs, and the thing that pays it back. */
static void cmd_sync(int argc, char **argv)
{
    (void)argc; (void)argv;

    if (!disk_ready(DISK_ROOT)) {
        kprintf("no disk, so there is nothing anywhere to lose\n");
        return;
    }
    if (!disk_dirty()) {
        kprintf("nothing waiting, the disk already holds what the kernel does\n");
        return;
    }
    if (!disk_sync()) {
        kprintf("the drive refused something. what is on it is not what the kernel "
                "believes\n");
        return;
    }
    kprintf("written\n");
}

/* growing a filesystem into the partition it is sitting in. */
static void cmd_resize(int argc, char **argv)
{
    size_t which = DISK_ROOT;
    uint64_t want = 0;
    bool said_size = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "work") == 0) {
            which = DISK_WORK;
            continue;
        }
        want = 0;
        said_size = true;
        for (const char *p = argv[i]; *p != '\0'; p++) {
            if (*p < '0' || *p > '9') {
                kprintf("resize [work] [blocks], with no number it fills "
                        "the partition\n");
                return;
            }
            want = want * 10 + (uint64_t)(*p - '0');
        }
    }

    if (!disk_ready(which) || disk_which(which) != DISK_EXT4) {
        kprintf("there is no ext4 filesystem at %s to resize\n",
                disk_mount_point(which));
        return;
    }

    uint64_t room = disk_room(which);
    uint64_t ceiling = disk_ceiling(which);
    if (!said_size) {
        want = room;
        if (want > ceiling) {
            want = ceiling;
        }
    }

    uint64_t used = 0, total = 0;
    disk_usage(which, &used, &total);
    uint32_t per = disk_cluster_bytes(which);

    kprintf("%s holds %lu blocks of %lu, and its partition has room for "
            "%lu\n", disk_mount_point(which), total / per, (uint64_t)per,
            room);
    if (want == total / per) {
        kprintf("which is already the size it is. nothing to do\n");
        return;
    }
    if (want > room) {
        kprintf("%lu blocks is more than the partition holds. a filesystem "
                "longer than\nthe partition under it is one that writes "
                "over its neighbour\n", want);
        return;
    }

    struct ext4_resize r;
    const char *error = NULL;
    if (!disk_resize(which, want, &r, &error)) {
        console_set_colors(COLOR_WARN, 0x101018);
        kprintf("%s\n", error != NULL ? error : "it would not resize");
        console_set_colors(COLOR_TEXT, 0x101018);
        if (r.blocks_in_way > 0 || r.inodes_in_way > 0) {
            kprintf("%lu block(s) and %lu inode(s) are out past where the "
                    "new end would\nbe. nothing is moved here, so this "
                    "refuses rather than losing them.\nempty the tail "
                    "and ask again\n",
                    r.blocks_in_way, r.inodes_in_way);
        } else if (want > ceiling) {
            kprintf("the descriptor table describes every group and sits "
                    "at the front of\nevery group, so it can only grow "
                    "into the room reserved for it when\nthe filesystem "
                    "was made. that stops at %lu blocks\n", ceiling);
        }
        return;
    }

    kprintf("%lu blocks in %lu group(s), was %lu in %lu\n",
            (uint64_t)r.blocks_after, (uint64_t)r.groups_after,
            (uint64_t)r.blocks_before, (uint64_t)r.groups_before);
    if (r.inodes_after != r.inodes_before) {
        kprintf("and %lu inodes, was %lu, a group brings its own, so a "
                "disk that grew\nwithout them would fill up having used a "
                "fraction of itself\n",
                (uint64_t)r.inodes_after, (uint64_t)r.inodes_before);
    }
    kprintf("`fsck` will say whether the kernel is telling the truth\n");
}

/* the checker, on the machine it is for. */
static void cmd_fsck(int argc, char **argv)
{
    bool mend = true;
    size_t which = DISK_ROOT;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0) {
            mend = false;
        } else if (strcmp(argv[i], "work") == 0)
{
            which = DISK_WORK;
        }
    }

    struct fsck_report r;
    const char *error = NULL;
    if (!disk_fsck(which, mend, &r, &error)) {
        kprintf("%s\n", error != NULL ? error : "cannot check it");
        return;
    }

    kprintf("%s\n", disk_mount_point(which));

    kprintf("%u blocks of %u, %u inodes, %u group(s)\n",
            r.blocks, r.block_size, r.inodes, r.groups);

    if (r.total_found == 0) {
        kprintf("nothing wrong with it\n");
        return;
    }
    for (int i = 0; i < FSCK_PROBLEMS; i++) {
        if (r.found[i] == 0) {
            continue;
        }
        kprintf("  %u x %s", r.found[i],
                fsck_problem_name((enum fsck_problem)i));
        if (r.mended[i] > 0) {
            kprintf("  (%u mended)", r.mended[i]);
        } else if (mend) {
            kprintf("  (left alone, no answer to it but yours)");
        }
        kprintf("\n");
    }
    kprintf("%u problem(s), %u mended\n", r.total_found, r.total_mended);
}

static void cmd_cd(int argc, char **argv)
{
    /*
     * a builtin, and it has to be: a program runs as its own process
     * with its own working directory, so a `cd` that was a program
     * would change where *it* was standing and then exit
     */
    const char *want = (argc > 1) ? argv[1] : "/";

    char resolved[PATH_MAX];
    if (!path_resolve(shell_cwd, want, resolved, sizeof resolved)) {
        kprintf("that path is longer than the kernel can hold\n");
        return;
    }

    if (!path_is_root(resolved)) {
        struct vfs_file f;
        if (!vfs_open(resolved, &f)) {
            kprintf("%s: no such place\n", resolved);
            return;
        }
        if (!f.is_dir) {
            kprintf("%s is a file, not somewhere to stand\n", resolved);
            return;
        }
    }

    for (size_t i = 0; i < sizeof shell_cwd; i++) {
        shell_cwd[i] = resolved[i];
        if (resolved[i] == '\0') {
            break;
        }
    }
}

static void cmd_pwd(int argc, char **argv)
{
    (void)argc; (void)argv;
    kprintf("%s\n", shell_cwd);
}

static void cmd_locks(int argc, char **argv)
{
    (void)argc; (void)argv;

    kprintf("%-10s %-5s %-6s %s\n", "lock", "rank", "held", "times waited");

    uint64_t total = 0;
    for (size_t i = 0; i < spin_count(); i++) {
        const struct spinlock *l = spin_at(i);
        kprintf("%-10s %-5d %-6s %lu\n", l->name, (int)l->rank,
                l->held ? "yes" : "no", l->contended);
        total += l->contended;
    }

    kprintf("\n%zu locks. ", spin_count());
    if (total == 0) {
        kprintf("nothing has ever waited on one, which is what a\n");
        kprintf("machine running kernel code on one core looks like.\n");
    } else {
        kprintf("waited %lu times in total.\n", total);
    }
    kprintf("\nrank is the order they may be taken in: a lock may only be\n");
    kprintf("taken while holding lower-ranked ones. it is read off the call\n");
    kprintf("graph, tty calls the scheduler, the scheduler reaches into\n");
    kprintf("the process table, all of them allocate, and anything may\n");
    kprintf("print. two locks taken in opposite orders by two cores is a\n");
    kprintf("machine that stops with nothing to say, so it is checked.\n");
}

static void cmd_cpus(int argc, char **argv)
{
    (void)argc; (void)argv;
    cpu_show();
}

/* mount a particular partition instead of whichever answered first. */
static void cmd_mount_at(int argc, char **argv)
{
    /*
     * a bare `mount` used to walk straight into argv[1], which on a
     * one-word line is whatever the splitter last left there. the
     * compiler had been pointing at this the whole time by way of an
     * unused `argc`, the parameter is unused precisely because the
     * check that should have used it was missing
     */
    if (argc < 2) {
        kprintf("mount [work] <number>, `parts` lists them\n");
        return;
    }

    /* `mount 2` puts a partition at /, and `mount work 2` puts one at the other place. */
    int arg = 1;
    size_t where = DISK_ROOT;
    if (strcmp(argv[1], "work") == 0) {
        where = DISK_WORK;
        arg = 2;
    }
    if (arg >= argc) {
        kprintf("mount work <number>, `parts` lists them\n");
        return;
    }

    size_t which = 0;
    for (const char *p = argv[arg]; *p != '\0'; p++) {
        if (*p < '0' || *p > '9') {
            kprintf("mount [work] <number>, `parts` lists them\n");
            return;
        }
        which = which * 10 + (size_t)(*p - '0');
    }

    if (which >= disk_part_count()) {
        kprintf("there is no partition %lu. `parts` lists them\n",
                (uint64_t)which);
        return;
    }
    if ((int)which == disk_mounted_part(where)) {
        kprintf("partition %lu is already the one at %s\n",
                (uint64_t)which, disk_mount_point(where));
        return;
    }

    if (!disk_mount_part(where, which)) {
        kprintf("partition %lu will not go at %s. either there is nothing "
                "on it the kernel\n", (uint64_t)which, disk_mount_point(where));
        kprintf("recognise, the table says what it is *meant* to hold, "
                "which is a\n");
        kprintf("different question, or it is on another drive, or it is "
                "already\n");
        kprintf("mounted at the other place. one filesystem in two places "
                "is two\n");
        kprintf("caches' worth of state disagreeing about one disk\n");
        return;
    }

    kprintf("partition %lu is now at %s, holding %s\n",
            (uint64_t)which, disk_mount_point(where), disk_kind_name(where));
    kprintf("anything that was open on the old one is stale. there is no "
            "reference\n");
    kprintf("counting here that could have done better, so this says so "
            "instead\n");
}

static void cmd_mount(int argc, char **argv)
{
    if (argc > 1) {
        cmd_mount_at(argc, argv);
        return;
    }
    vfs_show_mounts();

    kprintf("\na name with no leading slash is looked for on the disk "
            "first and\n");
    kprintf("%s second, so a disk may supply its own copy of anything "
            "and a\n", VFS_BOOT);
    kprintf("machine without one still finds what it booted with.\n");
}

static void cmd_slabs(int argc, char **argv)
{
    (void)argc; (void)argv;
    mm_show_slabinfo();
}

static void cmd_ps(int argc, char **argv)
{
    (void)argc; (void)argv;
    sched_show_threads();
}

static void cmd_summon(int argc, char **argv)
{
    if (argc < 2) {
        kprintf("summon whom? the register holds:\n");
        for (size_t i = 0; i < PERSONA_COUNT; i++) {
            kprintf("  %s\n", personas[i].name);
        }
        return;
    }

    for (size_t i = 0; i < PERSONA_COUNT; i++) {
        if (strcmp(argv[1], personas[i].name) == 0) {
            struct thread *t = thread_create(personas[i].name, persona_thread,
                                             (void *)&personas[i]);
            if (t == NULL) {
                kprintf("the summoning failed, no memory for a new soul\n");
                return;
            }
            console_set_colors(COLOR_PROMPT, 0x101018);
            kprintf("I am thou... thou art I...\n");
            kprintf("%s has answered thy call (thread %d)\n",
                    personas[i].name, t->id);
            console_set_colors(COLOR_TEXT, 0x101018);
            return;
        }
    }
    kprintf("no persona by the name '%s' dwells here\n", argv[1]);
}

/* hex, with or without the 0x. returns false if its not a number */
static bool parse_hex(const char *s, uint64_t *out)
{
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
    }
    if (*s == '\0') {
        return false;
    }
    uint64_t v = 0;
    for (; *s; s++) {
        uint64_t d;
        if (*s >= '0' && *s <= '9')      d = *s - '0';
        else if (*s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'F') d = *s - 'A' + 10;
        else return false;
        v = v * 16 + d;
    }
    *out = v;
    return true;
}

static void cmd_vmm(int argc, char **argv)
{
    if (argc >= 2) {
        uint64_t addr;
        if (!parse_hex(argv[1], &addr)) {
            kprintf("'%s' is not a hex address\n", argv[1]);
            return;
        }
        vmm_dump(addr);
        return;
    }

    vmm_show();
}

/* recurse until the stack runs out. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winfinite-recursion"
static uint64_t eat_stack(uint64_t depth)
{
    volatile uint64_t padding[64];
    for (int i = 0; i < 64; i++) {
        padding[i] = depth;
    }
    return padding[0] + eat_stack(depth + 1);
}
#pragma GCC diagnostic pop

static void cmd_stackoverflow(int argc, char **argv)
{
    (void)argc; (void)argv;
    console_set_colors(COLOR_WARN, 0x101018);
    kprintf("running off the end of this thread's stack on purpose...\n");
    console_set_colors(COLOR_TEXT, 0x101018);
    kprintf("returned %lu, which should have been impossible\n", eat_stack(0));
}

static void cmd_bt(int argc, char **argv)
{
    (void)argc; (void)argv;
    /* the same walker a panic uses, just with nothing on fire */
    kbacktrace(0, 0);
}


/* a live picture, redrawn until somebody presses a key. */
/*
 * the half that cannot be proved before it is trusted, so it
 * is asked for from a shell that already works rather than done to you
 * at boot. if the keyboard goes quiet afterwards, a reboot undoes it
 */
static void cmd_lspci(int argc, char **argv)
{
    (void)argc; (void)argv;
    pci_show();
}

static void cmd_ioapic(int argc, char **argv)
{
    (void)argc; (void)argv;

#if !defined(VELVET_ARCH_X86_64)
    kprintf("there are no apics on this machine. the interrupts it has "
            "arrive the only way they can\n");
#else
    if (!interrupts_on_apic()) {
        kprintf("interrupts are still on the 8259; there is nothing to "
                "move them from\n");
        return;
    }

    kprintf("moving the keyboard and serial onto the io apic.\n");
    kprintf("if this was a mistake, the keyboard will simply stop and a\n");
    kprintf("reboot will put everything back.\n\n");

    if (interrupts_use_ioapic()) {
        kprintf("\npress a key. if this echoes, it worked.\n");
    }
#endif
}

static void cmd_top(int argc, char **argv)
{
    (void)argc; (void)argv;

    while (!input_haskey()) {
        console_clear();

        console_set_colors(COLOR_PROMPT, 0x101018);
        kprintf("velvet %s, press any key to stop watching\n\n",
                VELVET_VERSION);
        console_set_colors(COLOR_TEXT, 0x101018);

        uint64_t ms = pit_uptime_ms();
        kprintf("up %luh %lum %lus     %zu threads, %zu processes\n",
                ms / 3600000, (ms / 60000) % 60, (ms / 1000) % 60,
                sched_thread_count(), process_count());

        kprintf("memory  %lu / %lu MiB in use, peaked at %lu MiB\n",
                pmm_used_bytes() / (1024 * 1024),
                pmm_total_bytes() / (1024 * 1024),
                pmm_peak_bytes() / (1024 * 1024));
        kprintf("heap    %lu KiB claimed, %lu bytes handed out\n\n",
                kheap_total_bytes() / 1024, kheap_used_bytes());

        sched_dump();

        /* which doors ring 3 actually uses. */
#if defined(VELVET_ARCH_X86_64)
        kprintf("\nsyscalls\n ");
        bool any = false;
        for (unsigned i = 0; i < SYSCALL_COUNT; i++) {
            uint64_t n = syscall_times_called(i);
            if (n > 0) {
                kprintf(" %s=%lu", syscall_name(i), n);
                any = true;
            }
        }
        kprintf("%s\n", any ? "" : " none yet");
#endif

        sleep_ms(500);
    }

    (void)input_getchar();      /* the key that stopped the kernel is not a command */
    console_clear();
}

static void cmd_whoami(int argc, char **argv)
{
    (void)argc; (void)argv;
    kprintf("monitor, uid 0\n");
    kprintf("the velvet room answers to thee\n");
}

static void cmd_dmesg(int argc, char **argv)
{
    (void)argc; (void)argv;
    /* everything the boot said while the screen was being kept quiet */
    klog_dump();
}



static void cmd_arcana(int argc, char **argv)
{
    (void)argc; (void)argv;

    console_set_colors(COLOR_PROMPT, 0x101018);
    kprintf("\nThou art I... And I am thou...\n\n");
    console_set_colors(COLOR_TEXT, 0x101018);

    kprintf("  THE COMPUTER ARCANA\n");
    kprintf("  rank %s, the bond deepens with every commit\n\n",
            VELVET_VERSION);

    kprintf("  version    velvet %s\n", VELVET_VERSION);
    kprintf("  forged     %s, %s\n", __DATE__, __TIME__);
    kprintf("  by         gcc %s\n", __VERSION__);
    kprintf("  known      %lu functions by name\n", ksym_count);
    if (ramdisk_present()) {
        kprintf("  carrying   %zu files in the ramdisk\n", ramdisk_count());
    }
    kprintf("\n");
}

/* fastfetch, if fastfetch had read a tarot deck */
static void cmd_persona(int argc, char **argv)
{
    (void)argc; (void)argv;

    static const char *mask[] = {
        "     .-\"\"\"\"\"-.     ",
        "   .'  _     _  '.   ",
        "  /   (o)   (o)   \\  ",
        " |       ---       | ",
        " |    \\  ___  /    | ",
        "  \\    '.___.'    /  ",
        "   '.           .'   ",
        "     '-._____.-'     ",
    };

    char brand[49];
#if defined(VELVET_ARCH_X86_64)
    cpu_brand(brand);
#else
    /*
     * cpuid is an x86 instruction and there is no portable equivalent
     * to reach for, what other architectures have is a part number
     * rather than a name somebody chose.
     *
     * memcpy rather than strcpy because this kernel's string.h has never
     * had one: six functions, and every one is there because something
     * needed it
     */
    memcpy(brand, "unknown", sizeof "unknown");
#endif

    size_t cols = 0, rows = 0, w = 0, h = 0;
    console_size(&cols, &rows, &w, &h);

    uint64_t ms = pit_uptime_ms();
    uint64_t total = pmm_total_bytes() / (1024 * 1024);
    uint64_t used  = pmm_used_bytes() / (1024 * 1024);

    /* the info column, one line per line of the mask */
    const int LINES = 8;
    for (int i = 0; i < LINES; i++) {
        console_set_colors(COLOR_PROMPT, 0x101018);
        kprintf("%s", mask[i]);
        console_set_colors(COLOR_TEXT, 0x101018);

        switch (i) {
        case 0:
            kprintf("monitor@velvet");
            break;
        case 1:
            kprintf("--------------");
            break;
        case 2:
            kprintf("arcana    the Computer, rank %s", VELVET_VERSION);
            break;
        case 3:
            kprintf("persona   %s", brand);
            break;
        case 4:
            kprintf("awakened  %luh %lum %lus",
                    ms / 3600000, (ms / 60000) % 60, (ms / 1000) % 60);
            break;
        case 5:
            kprintf("souls     %zu threads bound to the wheel",
                    sched_thread_count());
            break;
        case 6:
            kprintf("memory    %lu / %lu MiB", used, total);
            break;
        case 7:
            kprintf("vision    %zux%zu (%zux%zu of glyphs)", w, h, cols, rows);
            break;
        }
        kprintf("\n");
    }
    kprintf("\n");
}

static void cmd_date(int argc, char **argv)
{
    (void)argc; (void)argv;
    static const char *months[] = { "", "january", "february", "march",
        "april", "may", "june", "july", "august", "september", "october",
        "november", "december" };

    struct rtc_time t;
    rtc_read(&t);
    kprintf("%02u:%02u:%02u on the %u%s of %s, %u\n",
            t.hour, t.minute, t.second, t.day,
            (t.day / 10 == 1) ? "th"
              : (t.day % 10 == 1) ? "st"
              : (t.day % 10 == 2) ? "nd"
              : (t.day % 10 == 3) ? "rd" : "th",
            months[t.month <= 12 ? t.month : 0], t.year);
}

static void cmd_hexdump(int argc, char **argv)
{
    if (argc < 2) {
        kprintf("hexdump <hex address> [bytes]\n");
        return;
    }
    uint64_t addr, count = 64;
    if (!parse_hex(argv[1], &addr)) {
        kprintf("'%s' is not a hex address\n", argv[1]);
        return;
    }
    if (argc >= 3 && !parse_hex(argv[2], &count)) {
        kprintf("'%s' is not a hex length\n", argv[2]);
        return;
    }
    if (count > 1024) {
        count = 1024;       /* you did not mean that */
    }

    for (uint64_t off = 0; off < count; off += 16) {
        uint64_t base = addr + off;

        /* ask the page tables before touching anything. */
        /*
         * FIXME: this asks about one byte and then reads sixteen. a row
         * beginning in the last fifteen bytes of a mapped page runs into
         * whatever follows, and where that is unmapped the read faults in
         * ring 0 and takes the machine with it, from a builtin any
         * logged-in user can type with no more than an address. the
         * backtrace walker asks the same question of both ends of its
         * range for exactly this reason. check the row's last byte too,
         * and skip or shorten the row when it is not there.
         */
        if (vmm_translate(vmm_kernel_pml4(), base) == VMM_NO_MAPPING) {
            kprintf("%p  <not mapped>\n", (void *)base);
            continue;
        }

        const unsigned char *p = (const unsigned char *)base;
        kprintf("%p ", (void *)base);
        for (int i = 0; i < 16; i++) {
            kprintf(" %02x", p[i]);
        }
        kprintf("  ");
        for (int i = 0; i < 16; i++) {
            kprintf("%c", (p[i] >= ' ' && p[i] <= '~') ? p[i] : '.');
        }
        kprintf("\n");
    }
}

static void cmd_kill(int argc, char **argv)
{
    if (argc < 2) {
        kprintf("kill <thread id>, see `ps`\n");
        return;
    }
    uint64_t id = 0;
    for (const char *p = argv[1]; *p; p++) {
        if (*p < '0' || *p > '9') {
            kprintf("'%s' is not a thread id\n", argv[1]);
            return;
        }
        id = id * 10 + (uint64_t)(*p - '0');
    }
    switch (sched_kill((int)id)) {
    case SCHED_KILL_OK:
        kprintf("thread %lu returns to the sea of souls\n", id);
        break;
    case SCHED_KILL_NO_SUCH:
        kprintf("no thread %lu walks this realm\n", id);
        break;
    case SCHED_KILL_SELF:
        kprintf("i will not unmake myself while thou art still speaking\n");
        break;
    case SCHED_KILL_PROTECTED:
        kprintf("that one keeps the wheel turning. leave it be\n");
        break;
    }
}

static void cmd_history(int argc, char **argv);   /* needs the history array */

static void cmd_time(int argc, char **argv)
{
    if (argc < 2) {
        kprintf("time <command>, how long it takes\n");
        return;
    }
    uint64_t start = pit_uptime_ms();
    run_argv(argc - 1, argv + 1);
    kprintf("[%lums]\n", pit_uptime_ms() - start);
}

/*
 * the tree this kernel was built from is written into the boot
 * image after the ramdisk, and stays there: philemon does not load it
 * and neither does the kernel. `ls /boot/src` reads it off the drive a
 * sector at a time, which is what makes it free to carry.
 *
 * this command is the three things a listing cannot say, how much of
 * it there is, whether it is the source *this* kernel was built from,
 * and how to get a copy you are allowed to edit
 */

static void src_report(const char *what)
{
    kprintf("  %s\n", what);
}

static void cmd_src(int argc, char **argv)
{
    if (!source_present()) {
        kprintf("this image carries no source tree. it was either built "
                "without one or\n");
        kprintf("this machine was started by something that is not "
                "philemon\n");
        return;
    }

    if (argc >= 2 && strcmp(argv[1], "verify") == 0) {
        /* every sector of it, hashed, against what the build wrote into the kernel. */
        kprintf("reading %lu KiB off the medium\n", source_bytes() / 1024);

        uint8_t digest[20];
        if (!source_digest(digest)) {
            kprintf("the medium stopped answering part way through. what "
                    "is out there is not\nall there\n");
            return;
        }

        char hex[41];
        sha1_hex(digest, hex);
        if (strcmp(hex, source_stamp) == 0) {
            kprintf("%s\n", hex);
            kprintf("this is the source this kernel was built from\n");
        } else {
            console_set_colors(COLOR_WARN, 0x101018);
            kprintf("the medium says %s\n", hex);
            kprintf("this kernel says %s\n", source_stamp);
            kprintf("these are not the same tree. the archive, the kernel, "
                    "or both came from\nsomewhere else\n");
            console_set_colors(COLOR_TEXT, 0x101018);
        }
        return;
    }

    if (argc >= 2 && strcmp(argv[1], "unpack") == 0) {
        /* TODO: the boot test unpacks the whole tree, so this third word is not exercised. */
        const char *to = (argc >= 3) ? argv[2] : "/src";
        char from[VFS_NAME_MAX];

        /*
         * a third word takes a part of the tree rather than all of it,
         * and it is not a convenience: /boot/src/boot is a real
         * directory to the listing code, so unpacking a corner of the
         * archive is the same walk with a shorter prefix
         */
        size_t n = 0;
        for (const char *p = VFS_SRC; *p != '\0' && n < sizeof from - 2; p++) {
            from[n++] = *p;
        }
        if (argc >= 4) {
            from[n++] = '/';
            for (const char *p = argv[3]; *p != '\0' && n < sizeof from - 1; p++) {
                from[n++] = *p;
            }
        }
        from[n] = '\0';

        struct vfs_file dir;
        if (!vfs_open(from, &dir)) {
            kprintf("there is nothing called %s in the archive\n", from);
            return;
        }

        kprintf("writing %s to %s\n", from, to);

        const char *error = NULL;
        size_t files = install_copy_tree(from, to, src_report, &error);
        if (error != NULL) {
            console_set_colors(COLOR_WARN, 0x101018);
            kprintf("stopped after %zu files: %s\n", files, error);
            console_set_colors(COLOR_TEXT, 0x101018);
            return;
        }
        if (files == 0) {
            kprintf("nothing was copied. %s holds no files\n", from);
            return;
        }
        if (!disk_sync()) {
            kprintf("%zu files written, but the drive would not flush\n",
                    files);
            return;
        }
        kprintf("%zu files under %s, and they are yours to edit\n",
                files, to);
        return;
    }

    if (argc >= 2) {
        kprintf("src [verify|unpack [where [what]]]\n");
        return;
    }

    kprintf("%zu files, %lu KiB, at sector %lu of the boot medium\n",
            source_count(), source_bytes() / 1024, source_lba());
    kprintf("built as %s\n", source_stamp);

    /*
     * the cheap half of `src verify`: the length is in the table and
     * costs one sector to read, and an archive that is not even the
     * right size is one nobody needs to hash three megabytes to doubt
     */
    if (source_bytes() != source_stamp_bytes) {
        console_set_colors(COLOR_WARN, 0x101018);
        kprintf("but this kernel was built with an archive of %lu KiB, "
                "which is not the\none out there. `src verify` says how "
                "far apart they are\n", source_stamp_bytes / 1024);
        console_set_colors(COLOR_TEXT, 0x101018);
    }
    kprintf("\n");
    kprintf("none of it is in memory: %s is read off the drive when "
            "somebody looks.\n", VFS_SRC);
    kprintf("`src verify` hashes the whole of it against what this "
            "kernel was built\nfrom. `src unpack` writes a copy "
            "somewhere you can change it\n");
}

/* both of the ways this machine stops are asked of init now. */

/* read_line is defined with the rest of the line editing, a long way below this. */
static void read_line(char *buf, size_t max);

/* the installer talks to two drives by number. */
struct install_context {
    unsigned src, dst;
};

static bool install_src_read(void *ctx, uint64_t lba, uint32_t count,
                             void *buf)
{
    const struct install_context *c = ctx;
    return disk_raw_read(c->src, lba, count, buf);
}

static bool install_dst_read(void *ctx, uint64_t lba, uint32_t count,
                             void *buf)
{
    const struct install_context *c = ctx;
    return disk_raw_read(c->dst, lba, count, buf);
}

static bool install_dst_write(void *ctx, uint64_t lba, uint32_t count,
                              const void *buf)
{
    const struct install_context *c = ctx;
    return disk_raw_write(c->dst, lba, count, buf);
}

static void install_say(void *ctx, const char *what)
{
    (void)ctx;
    kprintf("  %s\n", what);
}

/* every file, which on a ramdisk this size is a wall of text. */
static void install_report(const char *what)
{
    kprintf("  %s\n", what);
}

/*
 * the ramdisk has been kept on the argument that it is what
 * makes the machine work when the disk does not. that makes this machine
 * a live medium by default, and there was never a way to say so.
 *
 * a drive is carrying the system if it has philemon's table at sector
 * 32. the machine is *installed* if that drive is also the one the root
 * filesystem is mounted from, one disk that both starts the machine
 * and holds it. anything else is live: running from something that
 * carries a system but is not the system's home
 */

static void cmd_install(int argc, char **argv)
{
    if (disk_drive_count() == 0) {
        kprintf("there are no drives here to install onto\n");
        return;
    }

    /* which drive carries this system. */
    int source = disk_system_drive();
    if (source < 0) {
        kprintf("this machine is not running from a boot medium, so "
                "there is nothing to copy\n");
        return;
    }

    if (argc < 2) {
        kprintf("install <drive>, copy this system onto a drive and make "
                "it bootable\n\n");
        kprintf("%-6s %-22s %10s   %s\n", "drive", "model", "MiB", "");
        for (size_t i = 0; i < disk_drive_count(); i++) {
            uint64_t sectors = disk_drive_sectors((unsigned)i);
            const char *note = "";
            if ((int)i == source) {
                note = "carries this system, the one you booted";
            } else if ((int)i == disk_mounted_drive()) {
                note = "currently mounted at /";
            }
            kprintf("%-6zu %-22s %10lu   %s\n", i, disk_drive_model((unsigned)i),
                    sectors / 2048, note);
        }
        kprintf("\neverything on the drive you name is destroyed.\n");
        return;
    }

    /*
     * two partitions unless told otherwise: the system on one and
     * somewhere to work on the other, so that filling the second does
     * not stop the machine. `install <n> whole` gives the older
     * arrangement, one filesystem over the whole drive
     */
    bool split = true;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "whole") == 0) {
            split = false;
        }
    }

    size_t target = 0;
    for (const char *p = argv[1]; *p != '\0'; p++) {
        if (*p < '0' || *p > '9') {
            kprintf("'%s' is not a drive number. `install` lists them\n",
                    argv[1]);
            return;
        }
        target = target * 10 + (size_t)(*p - '0');
    }
    if (target >= disk_drive_count()) {
        kprintf("there is no drive %zu\n", target);
        return;
    }
    if ((int)target == source) {
        kprintf("that is the drive this system is running from. installing "
                "onto it would overwrite what is being read\n");
        return;
    }

    /* the cache is bound to the mounted drive and the installer talks to another one. */
    if (disk_dirty() && !disk_sync()) {
        kprintf("could not flush what is still in memory. not starting\n");
        return;
    }

    console_set_colors(COLOR_WARN, 0x101018);
    kprintf("everything on drive %zu is about to be destroyed.\n", target);
    console_set_colors(COLOR_TEXT, 0x101018);
    kprintf("type the drive number again to go ahead: ");

    char confirm[8];
    read_line(confirm, sizeof confirm);
    if (strcmp(confirm, argv[1]) != 0) {
        kprintf("nothing was written\n");
        return;
    }

    struct install_context ctx = { (unsigned)source, (unsigned)target };

    struct install_io io = {
        .src_read    = install_src_read,
        .dst_read    = install_dst_read,
        .dst_write   = install_dst_write,
        .ctx         = &ctx,
        .dst_sectors = disk_drive_sectors((unsigned)target),
        .say         = install_say,
    };

    struct install_result r;
    const char *error = NULL;
    if (!install_system(&io, (uint32_t)(pit_uptime_ms() / 1000), split,
                        &r, &error)) {
        console_set_colors(COLOR_WARN, 0x101018);
        kprintf("install failed: %s\n", error);
        console_set_colors(COLOR_TEXT, 0x101018);
        return;
    }

    kprintf("  system %lu sectors, partition at %lu, %lu MiB of ext4\n",
            r.system_sectors, r.part_first_lba, r.part_sectors / 2048);
    if (r.work_sectors > 0) {
        kprintf("  and %lu MiB more at %lu, which will be %s, so that "
                "filling it\n", r.work_sectors / 2048, r.work_first_lba,
                DISK_WORK_AT);
        kprintf("  does not leave the system with nowhere to write\n");
    } else if (split) {
        kprintf("  one partition: the drive is not big enough for a second "
                "one worth\n  having, and two filesystems too small to "
                "hold anything is worse\n  than one that is merely "
                "small\n");
    }

    /*
     * and now the files. the new filesystem has to be mounted to be
     * written to, which means letting go of the one at /, so the
     * ramdisk at /boot is the source, since it is memory and does not
     * care which disk is selected
     */
    kprintf("mounting the new filesystem\n");
    if (!disk_mount()) {
        kprintf("the new disk did not come back as mountable. "
                "the system is on it but there is nothing in the filesystem\n");
        return;
    }

    size_t files = install_copy_tree(VFS_BOOT, "/", install_report, &error);
    if (error != NULL) {
        console_set_colors(COLOR_WARN, 0x101018);
        kprintf("copying stopped after %zu files: %s\n", files, error);
        console_set_colors(COLOR_TEXT, 0x101018);
        return;
    }

    /* and the source, expanded into /src. */
    if (source_present()) {
        kprintf("writing the source out to /src\n");
        size_t src_files = install_copy_tree(VFS_SRC, "/src", install_report,
                                             &error);
        if (error != NULL) {
            console_set_colors(COLOR_WARN, 0x101018);
            kprintf("the system is installed, but the source stopped after "
                    "%zu files: %s\n", src_files, error);
            console_set_colors(COLOR_TEXT, 0x101018);
            return;
        }
        files += src_files;
    }

    if (!disk_sync()) {
        kprintf("the drive would not flush. what was written may not be "
                "all of it\n");
        return;
    }

    console_set_colors(COLOR_PROMPT, 0x101018);
    kprintf("\n%zu files copied. drive %zu will boot on its own now.\n",
            files, target);
    console_set_colors(COLOR_TEXT, 0x101018);
}




static void cmd_ifconfig(int argc, char **argv)
{
    if (!e1000_present()) {
        kprintf("no network card. `lspci` says what is on the bus\n");
        return;
    }

    char text[32];

    if (argc >= 2) {
        ipv4 addr;
        if (!ipv4_parse(argv[1], &addr)) {
            kprintf("'%s' is not an address\n", argv[1]);
            return;
        }
        ipv4 mask = IPV4(255, 255, 255, 0);
        if (argc >= 3 && !ipv4_parse(argv[2], &mask)) {
            kprintf("'%s' is not a netmask\n", argv[2]);
            return;
        }
        ipv4 router = 0;
        if (argc >= 4 && !ipv4_parse(argv[3], &router)) {
            kprintf("'%s' is not a router's address\n", argv[3]);
            return;
        }
        if (!net_up(addr, mask, router)) {
            kprintf("the card would not come up\n");
            return;
        }
        ipv4_format(addr, text, sizeof text);
        kprintf("address %s", text);
        ipv4_format(mask, text, sizeof text);
        kprintf(" netmask %s", text);
        if (router != 0) {
            ipv4_format(router, text, sizeof text);
            kprintf(" router %s", text);
        }
        kprintf("\n");
        return;
    }

    mac_format(e1000_mac(), text, sizeof text);
    kprintf("card    : %s, hardware address %s\n", e1000_model(), text);

    struct e1000_stats es;
    e1000_get_stats(&es);

    /*
     * which way the card is being noticed, and above the address rather
     * than below it: the interrupt is arranged when the card is found
     * and has nothing to do with whether anything has been addressed
     * yet. it lived under the statistics for exactly one version, where
     * a machine with no address returned before reaching it, and
     * `ifconfig` with no argument is the first thing anybody types,
     * which is where a diagnostic has to be if it is going to be read.
     *
     * the whole point of it: a machine on the fallback timer works, and
     * looks exactly like one that does not, until it is asked
     */
    if (e1000_irq_line() == 0) {
        console_set_colors(COLOR_WARN, 0x101018);
        kprintf("woken   : on a timer. the firmware gave the card no "
                "interrupt line, this works, half a second behind\n");
        console_set_colors(COLOR_TEXT, 0x101018);
    } else if (!e1000_interrupts_working()) {
        kprintf("woken   : armed on irq %u, nothing has arrived yet. "
                "send something and look again\n", e1000_irq_line());
    } else {
        kprintf("woken   : by the card on irq %u, %lu times",
                e1000_irq_line(), es.interrupts);
        if (es.not_ours > 0) {
            kprintf(" (%lu on the line were somebody else's)", es.not_ours);
        }
        kprintf("\n");
    }

    if (!net_is_up()) {
        kprintf("address : none. `ifconfig <address> [netmask [router]]` "
                "sets one\n");
        return;
    }
    ipv4_format(net_address(), text, sizeof text);
    kprintf("address : %s", text);
    ipv4_format(net_netmask(), text, sizeof text);
    kprintf(" netmask %s\n", text);
    if (net_router() != 0) {
        ipv4_format(net_router(), text, sizeof text);
        kprintf("gateway : %s, anything not on this wire goes to "
                "it\n", text);
    }

    struct net_stats ns;
    net_get_stats(&ns);

    kprintf("frames  : %lu in, %lu out\n", ns.frames_in, ns.frames_out);
    kprintf("of those: %lu arp, %lu ip (%lu icmp, %lu udp)\n",
            ns.arp_in, ns.ip_in, ns.icmp_in, ns.udp_in);

    /* the two numbers worth watching. */
    kprintf("dropped : %lu not for this machine, %lu malformed\n",
            ns.dropped_not_mine, ns.dropped_bad);

    if (es.send_dropped > 0 || es.receive_dropped > 0) {
        console_set_colors(COLOR_WARN, 0x101018);
        kprintf("card    : %lu could not be sent, %lu arrived with nowhere "
                "to go, the rings are too small for this much traffic\n",
                es.send_dropped, es.receive_dropped);
        console_set_colors(COLOR_TEXT, 0x101018);
    }
}

/* defined below, beside `ping`, and used up here by `tcp connect`. */
static bool address_of(const char *what, ipv4 *out);

static int shell_number(const char *sp);

/* send a signal to a process, by pid. */
static void cmd_signal(int argc, char **argv)
{
    if (argc < 2) {
        kprintf("signal <pid> [name], int, term, kill, stop, cont, hup\n");
        kprintf("the default is `term`, which asks; `kill` does not ask\n");
        return;
    }

    int pid = shell_number(argv[1]);
    if (pid <= 0) {
        kprintf("'%s' is not a pid, `ps` lists them\n", argv[1]);
        return;
    }

    static const struct { const char *name; int sig; } names[] = {
        { "hup",  SIGHUP },  { "int",  SIGINT },  { "quit", SIGQUIT },
        { "kill", SIGKILL }, { "term", SIGTERM }, { "stop", SIGSTOP },
        { "cont", SIGCONT }, { "tstp", SIGTSTP }, { "pipe", SIGPIPE },
    };

    int sig = SIGTERM;
    if (argc >= 3) {
        sig = 0;
        for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
            if (strcmp(argv[2], names[i].name) == 0) {
                sig = names[i].sig;
                break;
            }
        }
        if (sig == 0) {
            /* a number is allowed too, for the ones with no name here */
            sig = shell_number(argv[2]);
        }
        if (sig <= 0 || sig >= SIGNAL_MAX) {
            kprintf("'%s' is not a signal\n", argv[2]);
            return;
        }
    }

    const struct process *p = process_find(pid);
    if (p == NULL) {
        kprintf("there is no process %d\n", pid);
        return;
    }

    if (!process_signal(pid, sig)) {
        kprintf("it would not take it\n");
        return;
    }
    kprintf("%s sent to %d\n", signal_name(sig), pid);

    if (sig == SIGKILL || sig == SIGSTOP) {
        kprintf("that one cannot be caught or blocked, which is the whole "
                "reason it exists\n");
    }
}

/* a decimal number, or -1. */
static int shell_number(const char *sp)
{
    if (*sp == '\0') {
        return -1;
    }
    long v = 0;
    for (const char *p = sp; *p; p++) {
        if (*p < '0' || *p > '9') {
            return -1;
        }
        v = v * 10 + (*p - '0');
        if (v > 65535) {
            return -1;
        }
    }
    return (int)v;
}

/*
 * tcp, driven by hand.
 *
 * there is no ring-3 interface to this yet, a blocking read having no
 * caller, so the shell is how a connection gets opened, written to
 * and closed. that is enough to prove the state machine against a real
 * stack, which is the only thing that can find what its own tests cannot:
 * they check this implementation against its reading of rfc 793, and a
 * peer written by somebody else checks it against everybody else's
 */
static void cmd_tcp(int argc, char **argv)
{
    if (!net_is_up()) {
        kprintf("the wire is not up\n");
        return;
    }

    if (argc < 2) {
        /*
         * the table. `state` is the whole point, a connection stuck in
         * one of the closing states is the symptom the roadmap named,
         * and it is invisible unless something prints it
         */
        bool any = false;
        for (int i = 0; i < TCP_CONN_MAX; i++) {
            const struct tcp_conn *c = net_tcp_at(i);
            if (c->state == TCP_CLOSED && c->error == NULL) {
                continue;
            }
            any = true;

            char a[32];
            ipv4_format(c->remote, a, sizeof a);
            kprintf("%d: %s:%u -> %s:%u  %s\n", i,
                    c->local_port == 0 ? "*" : "this machine", c->local_port,
                    c->remote == 0 ? "*" : a, c->remote_port,
                    tcp_state_name(c->state));
            kprintf("   %u to read, %u waiting to be acknowledged\n",
                    (unsigned)tcp_readable(c), (unsigned)c->send_len);
            if (c->error != NULL) {
                kprintf("   %s\n", c->error);
            }
        }
        if (!any) {
            kprintf("no connections. `tcp connect <address> <port>`\n");
        }
        return;
    }

    if (strcmp(argv[1], "connect") == 0) {
        if (argc < 4) {
            kprintf("tcp connect <address> <port>\n");
            return;
        }
        ipv4 to;
        if (!address_of(argv[2], &to)) {
            return;
        }
        int port = shell_number(argv[3]);
        if (port <= 0 || port > 65535) {
            kprintf("'%s' is not a port\n", argv[3]);
            return;
        }
        int h = net_tcp_connect(to, (uint16_t)port);
        if (h < 0) {
            kprintf("no room for another connection\n");
            return;
        }
        kprintf("connection %d, connecting. `tcp` says how it went\n", h);
        return;
    }

    if (strcmp(argv[1], "listen") == 0) {
        if (argc < 3) {
            kprintf("tcp listen <port>\n");
            return;
        }
        int port = shell_number(argv[2]);
        if (port <= 0 || port > 65535) {
            kprintf("'%s' is not a port\n", argv[2]);
            return;
        }
        int h = net_tcp_listen((uint16_t)port);
        if (h < 0) {
            kprintf("no room for another connection\n");
            return;
        }
        kprintf("connection %d, listening on %d\n", h, port);
        return;
    }

    /* everything below names a connection */
    if (argc < 3) {
        kprintf("tcp <send|read|close> <number> [text]\n");
        return;
    }
    int h = shell_number(argv[2]);
    struct tcp_conn *c = net_tcp_at(h);
    if (c == NULL) {
        kprintf("there is no connection %d\n", h);
        return;
    }

    if (strcmp(argv[1], "send") == 0) {
        if (argc < 4) {
            kprintf("tcp send <number> <text...>\n");
            return;
        }
        /*
         * the words back together, with a newline, almost everything
         * on the other end of a tcp connection is line-oriented
         */
        char line[256];
        size_t at = 0;
        for (int i = 3; i < argc && at < sizeof line - 2; i++) {
            if (i > 3) { line[at++] = ' '; }
            for (const char *p = argv[i]; *p && at < sizeof line - 2; p++) {
                line[at++] = *p;
            }
        }
        line[at++] = '\n';

        size_t n = tcp_write(c, line, at);
        if (n == 0) {
            kprintf("it would not take it, %s\n", tcp_state_name(c->state));
            return;
        }
        kprintf("%u bytes queued\n", (unsigned)n);
        return;
    }

    if (strcmp(argv[1], "read") == 0) {
        char buf[512];
        size_t n = tcp_read(c, buf, sizeof buf - 1);
        if (n == 0) {
            kprintf("nothing waiting\n");
            return;
        }
        buf[n] = '\0';
        kprintf("%s", buf);
        if (buf[n - 1] != '\n') {
            kprintf("\n");
        }
        return;
    }

    if (strcmp(argv[1], "close") == 0) {
        if (tcp_finished(c, pit_uptime_ms())) {
            /*
             * it ended already, and saying "closing" about a connection
             * that finished minutes ago is a machine telling you
             * something is happening when nothing is. so this tidies
             * the entry away instead, which is the only thing left to
             * do with it
             */
            net_tcp_forget(h);
            kprintf("that one had already finished. forgotten\n");
            return;
        }
        tcp_close(c);
        kprintf("closing. it is not finished until the other end agrees, "
                "which `tcp` will show\n");
        return;
    }

    kprintf("`tcp`, `tcp connect`, `tcp listen`, `tcp send`, `tcp read` "
            "or `tcp close`\n");
}

/* who else is out there. */
static void cmd_arp(int argc, char **argv)
{
    if (!net_is_up()) {
        kprintf("the wire is not up. `ifconfig <address>` first\n");
        return;
    }

    if (argc >= 2) {
        ipv4 who;
        if (!ipv4_parse(argv[1], &who)) {
            kprintf("'%s' is not an address\n", argv[1]);
            return;
        }
        net_arp_request(who);
        kprintf("asked who has %s. `arp` shows what answered\n", argv[1]);
        return;
    }

    const struct arp_cache *c = net_arp_cache();
    uint64_t now = pit_uptime_ms();
    size_t n = arp_count(c, now);

    if (n == 0) {
        kprintf("nobody has said anything yet. `arp <address>` asks\n");
        return;
    }

    console_set_colors(COLOR_PROMPT, 0x101018);
    kprintf("%-16s %-18s %s\n", "address", "hardware", "heard");
    console_set_colors(COLOR_TEXT, 0x101018);

    for (size_t i = 0; i < n; i++) {
        struct arp_entry e;
        if (!arp_at(c, i, now, &e)) {
            break;
        }
        char ip[20], mac[20];
        ipv4_format(e.ip, ip, sizeof ip);
        mac_format(&e.mac, mac, sizeof mac);
        kprintf("%-16s %-18s %lus ago\n", ip, mac,
                (now - e.learned_ms) / 1000);
    }
}

/* an address, for the commands that take one. */
static bool address_of(const char *what, ipv4 *out)
{
    if (ipv4_parse(what, out)) {
        return true;
    }
    kprintf("'%s' is not an address\n", what);
    return false;
}

/*
 * the smallest possible round trip, and therefore the best thing to
 * debug with: when it works the card, arp, ip and the checksum are all
 * working, and when it does not the fault is in one of four things
 */
static void cmd_ping(int argc, char **argv)
{
    if (!net_is_up()) {
        kprintf("the wire is not up. `ifconfig <address>` first\n");
        return;
    }
    if (argc < 2) {
        kprintf("ping <address>\n");
        return;
    }
    ipv4 to;
    if (!address_of(argv[1], &to)) {
        return;
    }

    unsigned sent = 0, back = 0;

    for (unsigned i = 1; i <= 4; i++) {
        uint64_t at = pit_uptime_ms();

        /*
         * the first one usually fails, and that is not a failure: the
         * hardware address is not known yet, so the send has gone off to
         * ask instead. trying again is the whole of address resolution
         * as far as anything up here sees it
         */
        if (!net_ping_send(to, (uint16_t)i)) {
            sleep_ms(200);
            if (!net_ping_send(to, (uint16_t)i)) {
                char text[32];
                ipv4_format(to, text, sizeof text);
                kprintf("no route to %s\n", text);

                if (ip_next_hop(to, net_address(), net_netmask(),
                                net_router()) == 0) {
                    kprintf("it is not on this wire and there is no "
                            "gateway to hand it to\n");
                }
                return;
            }
        }
        sent++;

        uint64_t when = 0;
        bool got = false;
        for (int wait = 0; wait < 20; wait++) {
            if (net_ping_seen((uint16_t)i, &when)) {
                got = true;
                break;
            }
            sleep_ms(50);
        }

        if (got) {
            back++;
            kprintf("%s: seq %u, %lums\n", argv[1], i,
                    when > at ? when - at : 0);
        } else {
            kprintf("%s: seq %u, no answer\n", argv[1], i);
        }

        /* the same way a script notices. */
        if (input_peek() == KEY_CTRL_C) {
            (void)input_getchar();
            kprintf("stopped\n");
            break;
        }
    }

    kprintf("%u sent, %u back\n", sent, back);
}

static void cmd_poweroff(int argc, char **argv)
{
    (void)argc; (void)argv;
    init_stop_machine(INIT_POWEROFF);
}

static void cmd_crash(int argc, char **argv)
{
    (void)argc; (void)argv;
    console_set_colors(COLOR_WARN, 0x101018);
    kprintf("tempting fate: reading from 0xdeadbeef...\n");
    console_set_colors(COLOR_TEXT, 0x101018);

    volatile uint64_t *bad = (volatile uint64_t *)0xdeadbeef;
    uint64_t got = *bad;

    kprintf("read back %lx, which should have been impossible\n", got);
}

static void cmd_reboot(int argc, char **argv)
{
    (void)argc; (void)argv;
    init_stop_machine(INIT_REBOOT);
}

/* what init is looking after, and how it has been going. */
static void cmd_init(int argc, char **argv)
{
    if (argc >= 3 && strcmp(argv[1], "start") == 0) {
        if (init_restart(argv[2])) {
            kprintf("%s started\n", argv[2]);
        } else {
            kprintf("init looks after no '%s'\n", argv[2]);
        }
        return;
    }
    if (argc >= 2) {
        kprintf("init           , what is being looked after\n");
        kprintf("init start NAME, start one that is down\n");
        return;
    }

    struct init_table t;
    init_snapshot(&t);

    console_set_colors(COLOR_PROMPT, 0x101018);
    kprintf("%-10s %-9s %6s %7s\n", "service", "state", "thread", "starts");
    console_set_colors(COLOR_TEXT, 0x101018);

    for (size_t i = 0; i < t.count; i++) {
        const struct service *s = &t.s[i];
        if (s->state == SERVICE_GIVEN_UP) {
            console_set_colors(COLOR_WARN, 0x101018);
        }
        kprintf("%-10s %-9s %6d %7u\n", s->name, init_state_name(s->state),
                s->thread_id, s->starts);
        console_set_colors(COLOR_TEXT, 0x101018);
    }
}

/* switch screens from the keyboard-less side of the machine. */
static void cmd_chvt(int argc, char **argv)
{
    if (argc < 2) {
        kprintf("this is console %u of %d.\n",
                tty_my_console() + 1, VCONSOLE_COUNT);
        kprintf("  chvt <n>        show another one\n");
        kprintf("  alt+1 .. alt+4  the same, from a keyboard\n");
        kprintf("  alt+f1 .. f4    also, where the host does not eat "
                "them first\n");
        kprintf("  ctrl+\\ then n   the same, over a serial line\n");
        kprintf("  shift+pageup    look back up this one\n");
        return;
    }

    int n = 0;
    for (const char *p = argv[1]; *p != '\0'; p++) {
        if (*p < '0' || *p > '9') {
            kprintf("chvt <n>, where n is 1 to %d\n", VCONSOLE_COUNT);
            return;
        }
        n = n * 10 + (*p - '0');
    }
    if (n < 1 || n > VCONSOLE_COUNT) {
        kprintf("there are %d consoles, numbered from 1\n", VCONSOLE_COUNT);
        return;
    }

    console_switch((unsigned)(n - 1));

    /*
     * said on *this* console, which is the one nobody is looking at any
     * more, so it is there when they come back rather than printed
     * over whatever they switched to
     */
    kprintf("showing console %d\n", n);
}

/* what the pointer is doing, and whether it is doing it at all. */
static void cmd_mouse(int argc, char **argv)
{
    (void)argc; (void)argv;

    if (!mouse_present()) {
        kprintf("no mouse. qemu wants -device usb-mouse or the ps/2 one it\n");
        kprintf("gives you by default; a machine with none carries on "
                "perfectly well\n");
        return;
    }

    size_t col = 0, row = 0;
    mouse_position(&col, &row);
    uint8_t held = mouse_buttons();

    kprintf("ps/2 mouse%s\n", mouse_has_wheel() ? " with a wheel" : "");
    kprintf("  at        column %lu, row %lu\n",
            (uint64_t)col, (uint64_t)row);
    kprintf("  held      %s%s%s%s\n",
            (held & MOUSE_LEFT) ? "left " : "",
            (held & MOUSE_MIDDLE) ? "middle " : "",
            (held & MOUSE_RIGHT) ? "right" : "",
            held ? "" : "nothing");
    kprintf("  packets   %lu\n", mouse_packets());
    kprintf("  out of step %lu\n", mouse_resyncs());

    if (mouse_resyncs() > 0) {
        kprintf("\nbytes arriving where a packet could not start. a few at "
                "boot are\n");
        kprintf("ordinary, the controller had some queued before anybody "
                "was listening.\n");
        kprintf("a number that keeps climbing is a mouse losing sync, and "
                "the pointer\n");
        kprintf("will be flying off in straight lines.\n");
    }

    kprintf("\ndrag over text to select it, middle button types it back.\n");
    kprintf("the wheel looks back up this console, same as shift+pageup.\n");
}

/*
 * `set` rather than sh's bare `NAME=value`, because a bare assignment
 * means the shell has to decide whether a word with an equals in it is
 * an assignment or an argument, and every shell that took that on has
 * a page of rules about when it is which. one word at the front is
 * unambiguous and reads no worse.
 *
 * there is no separate `export`. everything set here is inherited by
 * everything started here, because a variable that was not would be a
 * variable this shell could see and nothing else could, which is a
 * feature nobody has asked for
 */
static void cmd_set(int argc, char **argv)
{
    if (argc == 1) {
        /* with nothing to set, say what is set. */
        size_t at = 0;
        int shown = 0;
        while (at < me()->env_len && me()->env[at] != '\0') {
            kprintf("%s\n", &me()->env[at]);
            while (me()->env[at] != '\0') {
                at++;
            }
            at++;
            shown++;
        }
        if (shown == 0) {
            kprintf("nothing is set. `set NAME value`\n");
        }
        return;
    }

    if (argc == 2) {
        /* one word: show that one, or say it is not set. */
        char value[ENV_VALUE_MAX];
        if (env_block_get(me()->env, me()->env_len, argv[1], value,
                          sizeof value)) {
            kprintf("%s\n", value);
        } else {
            kprintf("%s is not set\n", argv[1]);
        }
        return;
    }

    /* everything after the name, joined with spaces. */
    char value[ENV_VALUE_MAX];
    size_t n = 0;
    for (int i = 2; i < argc && n + 1 < sizeof value; i++) {
        if (i > 2) {
            value[n++] = ' ';
        }
        for (const char *p = argv[i]; *p != '\0' && n + 1 < sizeof value; p++) {
            value[n++] = *p;
        }
    }
    value[n] = '\0';

    for (const char *p = argv[1]; *p != '\0'; p++) {
        if (*p == '=') {
            kprintf("a name cannot have an equals in it, that is what "
                    "separates it\n");
            kprintf("from its value, so one inside could never be looked "
                    "up again\n");
            return;
        }
    }

    if (me()->env_len == 0) {
        me()->env[0] = '\0';
        me()->env_len = 1;
    }
    if (!env_block_set(me()->env, &me()->env_len, PROC_ENV_MAX, argv[1],
                       value)) {
        kprintf("no room. this session may hold about a kilobyte of "
                "environment\n");
    }
}

static void cmd_unset(int argc, char **argv)
{
    if (argc < 2) {
        kprintf("unset <name>\n");
        return;
    }
    for (int i = 1; i < argc; i++) {
        env_block_set(me()->env, &me()->env_len, PROC_ENV_MAX, argv[i], NULL);
    }
}

/*
 * a builtin rather than a program, and that is the whole reason `if`
 * can do anything useful: without it a condition could only ever be
 * "did that program succeed", which is a fine thing to ask and not
 * enough to write anything with.
 *
 * the spellings are the ones test has had since v7 unix, because
 * inventing new ones would mean everybody has to learn them twice
 */
static bool test_says_yes(int argc, char **argv)
{
    if (argc == 2) {
        return argv[1][0] != '\0';     /* a non-empty word is true */
    }

    if (argc == 3) {
        if (strcmp(argv[1], "-n") == 0) return argv[2][0] != '\0';
        if (strcmp(argv[1], "-z") == 0) return argv[2][0] == '\0';

        struct vfs_file f;
        char path[PATH_MAX];
        if (!path_resolve(shell_cwd, argv[2], path, sizeof path)) {
            return false;
        }
        if (strcmp(argv[1], "-e") == 0) return vfs_open(path, &f);
        if (strcmp(argv[1], "-f") == 0) return vfs_open(path, &f) && !f.is_dir;
        if (strcmp(argv[1], "-d") == 0) return vfs_open(path, &f) && f.is_dir;
        return false;
    }

    if (argc == 4) {
        if (strcmp(argv[2], "=") == 0)  return strcmp(argv[1], argv[3]) == 0;
        if (strcmp(argv[2], "!=") == 0) return strcmp(argv[1], argv[3]) != 0;

        /*
         * the numeric ones. a word that is not a number compares as
         * zero, which is what test has always done and is worth
         * knowing rather than being surprised by
         */
        long a = 0, b = 0;
        for (const char *p = argv[1]; *p >= '0' && *p <= '9'; p++) {
            a = a * 10 + (*p - '0');
        }
        for (const char *p = argv[3]; *p >= '0' && *p <= '9'; p++) {
            b = b * 10 + (*p - '0');
        }
        if (strcmp(argv[2], "-eq") == 0) return a == b;
        if (strcmp(argv[2], "-ne") == 0) return a != b;
        if (strcmp(argv[2], "-lt") == 0) return a < b;
        if (strcmp(argv[2], "-gt") == 0) return a > b;
        if (strcmp(argv[2], "-le") == 0) return a <= b;
        if (strcmp(argv[2], "-ge") == 0) return a >= b;
    }
    return false;
}

static void cmd_test(int argc, char **argv)
{
    if (argc < 2) {
        kprintf("test <word>            true if it is not empty\n");
        kprintf("test -n/-z <word>      not empty / empty\n");
        kprintf("test -e/-f/-d <path>   exists / is a file / is a "
                "directory\n");
        kprintf("test <a> = / != <b>    the same text, or not\n");
        kprintf("test <a> -eq -ne -lt -gt -le -ge <b>   as numbers\n");
        kprintf("\nit says nothing and sets $?, which is what `if` "
                "reads\n");
        me()->status = 2;
        return;
    }
    me()->status = test_says_yes(argc, argv) ? 0 : 1;
}

static void run_line(char *line);

/* a file of commands, with `if` and `while` in it. */

#define SCRIPT_MAX_LINES 512
#define SCRIPT_MAX_DEPTH 8
#define SCRIPT_MAX_STEPS 100000

enum block_kind { BLOCK_IF, BLOCK_WHILE };

struct block {
    enum block_kind kind;
    size_t at;              /* the line the block began on */
    bool   running;         /* are the lines inside being executed */
    bool   taken;           /* has a branch of this if already run */
    bool   outer;           /* was anything running when it began */
};

/* run one line and say whether it succeeded. */
static bool line_succeeded(char *line)
{
    me()->status = 0;
    run_line(line);
    return me()->status == 0;
}

/*
 * the first word of a line, without disturbing the line, the
 * interpreter has to look at it before deciding whether to run it, and
 * running it needs the line intact
 */
static void first_word(const char *line, char *out, size_t max)
{
    while (*line == ' ' || *line == '\t') {
        line++;
    }
    size_t n = 0;
    while (*line != '\0' && *line != ' ' && *line != '\t' && n + 1 < max) {
        out[n++] = *line++;
    }
    out[n] = '\0';
}

/*
 * everything after the first word, which for `if` and `while` is the
 * command that decides
 */
static char *rest_of(char *line)
{
    while (*line == ' ' || *line == '\t') {
        line++;
    }
    while (*line != '\0' && *line != ' ' && *line != '\t') {
        line++;
    }
    while (*line == ' ' || *line == '\t') {
        line++;
    }
    return line;
}

static void run_script_text(char *text, const char *name)
{
    /* chop it into lines in place. */
    static char *lines[SCRIPT_MAX_LINES];
    size_t count = 0;

    char *p = text;
    lines[count++] = p;
    for (; *p != '\0'; p++) {
        if (*p == '\n') {
            *p = '\0';
            if (count >= SCRIPT_MAX_LINES) {
                kprintf("%s: more than %d lines, which is more than the kernel "
                        "read\n", name, SCRIPT_MAX_LINES);
                return;
            }
            lines[count++] = p + 1;
        } else if (*p == '\r') {
            *p = '\0';     /* written on another machine. drop them quietly */
        }
    }

    struct block stack[SCRIPT_MAX_DEPTH];
    int depth = 0;
    bool running = true;

    size_t pc = 0;
    unsigned long steps = 0;

    while (pc < count) {
        /*
         * a script may loop forever quite legitimately, so this is not
         * a limit on what may be written, it is a way out of one that
         * was not meant to. ctrl+c is the other, and is the one for
         * loops that were
         */
        if (++steps > SCRIPT_MAX_STEPS) {
            kprintf("%s: stopped after %d steps. if that was on purpose, "
                    "it wants\n", name, SCRIPT_MAX_STEPS);
            kprintf("a program rather than a script\n");
            return;
        }
        if (input_peek() == KEY_CTRL_C) {
            (void)input_getchar();
            kprintf("%s: stopped\n", name);
            return;
        }

        char *line = lines[pc];
        char word[16];
        first_word(line, word, sizeof word);

        if (strcmp(word, "if") == 0) {
            if (depth == SCRIPT_MAX_DEPTH) {
                kprintf("%s: nested deeper than %d\n", name,
                        SCRIPT_MAX_DEPTH);
                return;
            }
            bool cond = running && line_succeeded(rest_of(line));
            stack[depth].kind = BLOCK_IF;
            stack[depth].at = pc;
            stack[depth].outer = running;
            stack[depth].taken = cond;
            stack[depth].running = running && cond;
            running = stack[depth].running;
            depth++;

        } else if (strcmp(word, "else") == 0) {
            if (depth == 0 || stack[depth - 1].kind != BLOCK_IF) {
                kprintf("%s: an else with no if\n", name);
                return;
            }
            stack[depth - 1].running = stack[depth - 1].outer
                                    && !stack[depth - 1].taken;
            running = stack[depth - 1].running;

        } else if (strcmp(word, "while") == 0) {
            if (depth == SCRIPT_MAX_DEPTH) {
                kprintf("%s: nested deeper than %d\n", name,
                        SCRIPT_MAX_DEPTH);
                return;
            }
            bool cond = running && line_succeeded(rest_of(line));
            stack[depth].kind = BLOCK_WHILE;
            stack[depth].at = pc;
            stack[depth].outer = running;
            stack[depth].taken = cond;
            stack[depth].running = running && cond;
            running = stack[depth].running;
            depth++;

        } else if (strcmp(word, "end") == 0) {
            if (depth == 0) {
                kprintf("%s: an end with nothing open\n", name);
                return;
            }
            depth--;
            if (stack[depth].kind == BLOCK_WHILE && stack[depth].running) {
                /* back to the `while`, which will test again and open the block again. */
                pc = stack[depth].at;
                running = stack[depth].outer;
                continue;
            }
            running = (depth > 0) ? stack[depth].outer : true;
            if (depth > 0) {
                running = stack[depth - 1].running;
            }

        } else if (word[0] != '\0' && running) {
            run_line(line);
        }

        pc++;
    }

    if (depth != 0) {
        kprintf("%s: the file ended with %d block(s) still open\n",
                name, depth);
    }
}

/* read a file and run it. */
static bool run_script(const char *path)
{
    const void *data = NULL;
    uint64_t size = 0;
    bool owned = false;

    if (!vfs_slurp(path, &data, &size, &owned)) {
        return false;
    }

    /*
     * FIXME: this buffer is static, and so is the line table the caller
     * builds over it, so a `source` inside a sourced file writes the
     * inner file over the text the outer one is still walking. the
     * outer's lines[] then point into the inner file's bytes, and its
     * program counter indexes whatever the inner call left in the
     * table, so the rest of the outer script is whatever happens to be
     * sitting there rather than what was written. the block depth is
     * capped at 8 and nothing caps the file depth. a buffer per call, or
     * a refusal while one is already running.
     */
    static char text[16 * 1024];
    if (size >= sizeof text) {
        kprintf("%s: larger than %lu bytes, which is more than the kernel read\n",
                path, (uint64_t)sizeof text - 1);
        vfs_release(data, owned);
        return true;
    }

    memcpy(text, data, size);
    text[size] = '\0';
    vfs_release(data, owned);

    run_script_text(text, path);
    return true;
}

static void cmd_source(int argc, char **argv)
{
    if (argc < 2) {
        kprintf("source <file>, run a file of commands in this shell\n");
        return;
    }
    char path[PATH_MAX];
    if (!path_resolve(shell_cwd, argv[1], path, sizeof path)
        || !run_script(path)) {
        kprintf("no such file: %s\n", argv[1]);
        me()->status = 127;
    }
}

static const struct command commands[] = {
    { "help",   "list what thou may command",           cmd_help, false, "help [name]" },
    { "clear",  "wipe the screen clean",                cmd_clear, false, NULL },
    { "whoami", "who thou art, and what that permits",  cmd_whoami, false, NULL },
    { "dmesg",  "everything boot said while you werent looking", cmd_dmesg, false, NULL },
    { "arcana", "the rank of this bond, and its making", cmd_arcana, false, NULL },
    { "persona","the face this machine wears",          cmd_persona, false, NULL },
    { "mem",    "frames and heap, honestly counted",    cmd_mem, false, NULL },
    { "ps",     "the threads that walk this realm",     cmd_ps, false, NULL },
    { "top",    "the same, but watched rather than asked", cmd_top, false, NULL },
    { "lspci",  "what is plugged into this machine",    cmd_lspci, false, NULL },
    { "slabs",  "the object caches, and what they hold", cmd_slabs, false, NULL },
    { "disk",   "the drive, and the filesystem on it",  cmd_disk, false, NULL },
    { "sync",   "put what is in memory onto the disk",  cmd_sync, false, NULL },
    { "fsck",   "check the filesystem, and mend what has one answer", cmd_fsck, true, "fsck [work] [-n]" },
    { "resize", "grow a filesystem into the partition it sits in", cmd_resize, false, "resize [work] [blocks]" },
    { "mount",  "which filesystem is where; mount <n> moves it", cmd_mount, false, "mount [number]" },
    { "parts",  "what each drive says it holds",        cmd_parts, false, NULL },
    { "chvt",   "show another console; alt+f1..f5 too", cmd_chvt, false, "chvt [1-5]" },
    { "mouse",  "the pointer, and whether it is well",  cmd_mouse, false, NULL },
    { "set",    "a variable, or all of them",           cmd_set, false, "set [NAME [value...]]" },
    { "unset",  "forget one",                           cmd_unset, false, "unset <name>" },
    { "env",    "what is set", cmd_set, false, NULL },
    { "test",   "answer a question in $?, for `if`",    cmd_test, false, "test <words...>" },
    { "source", "run a file of commands in this shell", cmd_source, true, "source <file>" },
    { "cpus",   "the processors, and which are awake",  cmd_cpus, false, NULL },
    { "locks",  "what guards what, and what waits",     cmd_locks, false, NULL },
    { "cd",     "go somewhere; no argument means the root", cmd_cd, true, "cd [directory]" },
    { "pwd",    "where the kernel is standing",                  cmd_pwd, false, NULL },
    { "ioapic", "move external interrupts off the 8259 (risky)", cmd_ioapic, false, NULL },
    { "summon", "call forth a persona thread (in the background)", cmd_summon, false, "summon <name>" },
    { "vmm",    "what the page tables say about an address", cmd_vmm, false, "vmm <address>" },
    { "bt",     "who called whom to get here",          cmd_bt, false, NULL },
    { "date",   "what the battery-backed clock believes", cmd_date, false, NULL },
    { "hexdump","look at memory, safely",                cmd_hexdump, false, "hexdump <address>" },
    { "kill",   "end a thread by id",                    cmd_kill, false, "kill <id>" },
    { "signal", "send a signal to a process by pid",     cmd_signal, false, "signal <pid> [int|term|kill|stop|cont]" },
    { "history","what thou hast said before",            cmd_history, false, NULL },
    { "time",   "how long a command takes",              cmd_time, false, "time <command...>" },
    { "crash",  "tempt fate with a wild pointer",       cmd_crash, false, NULL },
    { "smash",  "run off the end of the stack on purpose", cmd_stackoverflow, false, NULL },
    { "init",   "what init looks after, and how it fares", cmd_init, false, "init [start <name>]" },
    { "install","copy this system onto a drive and make it boot", cmd_install, false, "install [drive] [whole]" },
    { "src",    "the source this was built from, still on the medium", cmd_src, false, "src [verify|unpack [where [what]]]" },
    { "ifconfig","the card, and the address on it",     cmd_ifconfig, false, "ifconfig [address [netmask [router]]]" },
    { "tcp",    "connections, and what state they are in", cmd_tcp, false, "tcp [connect <address> <port>|listen <port>|send <n> <text>|read <n>|close <n>]" },
    { "ping",   "the smallest round trip there is",     cmd_ping, false, "ping <address>" },
    { "arp",    "who else is out there",                cmd_arp, false, "arp [address]" },
    { "reboot", "sever the bond and begin anew",        cmd_reboot, false, NULL },
    { "poweroff","let the velvet room fade",             cmd_poweroff, false, NULL },
    { NULL, NULL, NULL, false, NULL },
};



/* chop a line into argv in place. */
static char *punctuation(char *p, size_t *eaten)
{
    static char bar[] = "|", in[] = "<", out[] = ">", app[] = ">>";

    if (*p == '|') { *eaten = 1; return bar; }
    if (*p == '<') { *eaten = 1; return in; }
    if (*p == '>') {
        if (p[1] == '>') { *eaten = 2; return app; }
        *eaten = 1;
        return out;
    }
    *eaten = 0;
    return NULL;
}

static int split(char *line, char **argv, int max)
{
    int argc = 0;
    char *p = line;

    for (;;) {
        while (*p == ' ') {
            p++;
        }
        if (*p == '\0' || argc == max) {
            break;
        }

        size_t eaten;
        char *punct = punctuation(p, &eaten);
        if (punct != NULL) {
            argv[argc++] = punct;
            p += eaten;
            continue;
        }

        /* a quoted word is one word, spaces and all. */
        if (*p == '"' || *p == '\'') {
            char quote = *p;
            argv[argc++] = ++p;
            while (*p != '\0' && *p != quote) {
                p++;
            }
            if (*p == quote) {
                *p++ = '\0';
            }
            continue;       /* unterminated: the rest of the line, and no crash */
        }

        argv[argc++] = p;
        while (*p != '\0' && *p != ' ' && punctuation(p, &eaten) == NULL) {
            p++;
        }
        if (*p == ' ') {
            *p++ = '\0';
        } else if (*p != '\0') {
            /* punctuation right up against the word. */
            punct = punctuation(p, &eaten);
            *p = '\0';
            p += eaten;
            if (argc < max) {
                argv[argc++] = punct;
            }
        }
    }
    return argc;
}

/* is this word a builtin? */
static const struct command *builtin_named(const char *name)
{
    for (const struct command *c = commands; c->name; c++) {
        if (strcmp(name, c->name) == 0) {
            return c;
        }
    }
    return NULL;
}

/* dispatch an already-split command. */
static void run_argv(int argc, char **argv)
{
    if (argc == 0) {
        return;
    }
    const struct command *c = builtin_named(argv[0]);
    if (c != NULL) {
        c->fn(argc, argv);
        return;
    }

    /* before giving up, see if they nearly typed something real. */
    const struct command *near = NULL;
    size_t best = 0;
    int ties = 0;
    for (const struct command *c = commands; c->name; c++) {
        size_t n = common_prefix(c->name, argv[0]);
        if (n > best) {
            best = n;
            near = c;
            ties = 1;
        } else if (n == best && best > 0) {
            ties++;
        }
    }

    /*
     * 127 is what every shell says for a name that is not a command,
     * and `if` reads it, so a script can branch on whether something
     * exists at all
     */
    me()->status = 127;

    if (best >= 2 && ties == 1) {
        kprintf("'%s' means nothing here. didst thou mean '%s'?\n",
                argv[0], near->name);
    } else {
        /* running programs is the system's, and the monitor is the kernel's */
        kprintf("'%s' is not one of the monitor's commands, and the monitor "
                "runs no programs. `help` lists what it has\n", argv[0]);
    }
}

/*
 * a bar or an arrow: what a shell that runs programs joins them with.
 * the monitor has nothing to join, and its builtins print straight at
 * the screen, with no output to hand anybody
 */
static bool needs_wiring(int argc, char **argv)
{
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "|") == 0 || strcmp(argv[i], "<") == 0
            || strcmp(argv[i], ">") == 0 || strcmp(argv[i], ">>") == 0) {
            return true;
        }
    }
    return false;
}

/*
 * expansion happens to the whole line before it is split into words,
 * which is what sh does and is worth knowing about: a variable holding
 * "a b" becomes two words, not one word with a space in it. that is
 * occasionally infuriating and is the behaviour everybody expects,
 * including the infuriating part.
 *
 * a name that is not set expands to nothing, which is also sh's answer
 * and is the only one that lets `cat $MAYBE` mean "cat" rather than
 * "cat, and also fail"
 */
static void expand(const char *in, char *out, size_t max)
{
    size_t at = 0;

    for (size_t i = 0; in[i] != '\0' && at + 1 < max; ) {
        if (in[i] != '$') {
            out[at++] = in[i++];
            continue;
        }
        i++;

        /*
         * $? is what the last thing exited with, and is the only
         * variable here that is not in the environment, it belongs to
         * the shell rather than to anything it starts
         */
        if (in[i] == '?') {
            i++;
            char digits[16];
            int n = 0;
            int v = me()->status;
            if (v < 0) {
                out[at++] = '-';
                v = -v;
            }
            if (v == 0) {
                digits[n++] = '0';
            }
            while (v > 0 && n < 15) {
                digits[n++] = (char)('0' + v % 10);
                v /= 10;
            }
            while (n-- > 0 && at + 1 < max) {
                out[at++] = digits[n];
            }
            continue;
        }

        bool braced = (in[i] == '{');
        if (braced) {
            i++;
        }

        char name[ENV_NAME_MAX];
        size_t n = 0;
        while (in[i] != '\0' && n + 1 < sizeof name
               && ((in[i] >= 'a' && in[i] <= 'z')
                   || (in[i] >= 'A' && in[i] <= 'Z')
                   || (in[i] >= '0' && in[i] <= '9')
                   || in[i] == '_')) {
            name[n++] = in[i++];
        }
        name[n] = '\0';

        if (braced && in[i] == '}') {
            i++;
        }

        if (n == 0) {
            /* a lone dollar is a dollar. */
            out[at++] = '$';
            continue;
        }

        char value[ENV_VALUE_MAX];
        if (env_block_get(me()->env, me()->env_len, name, value,
                          sizeof value)) {
            for (size_t k = 0; value[k] != '\0' && at + 1 < max; k++) {
                out[at++] = value[k];
            }
        }
        /* and a name nobody set expands to nothing at all */
    }

    out[at] = '\0';
}

static void run_line(char *line)
{
    char *argv[ARGV_MAX];

    /* a comment is the rest of the line, and a line that is only a comment is nothing at all. */
    for (size_t i = 0; line[i] != '\0'; i++) {
        if (line[i] == '#' && (i == 0 || line[i - 1] == ' ')) {
            line[i] = '\0';
            break;
        }
    }

    /* expansion, before the split. */
    static char expanded[LINE_MAX];
    expand(line, expanded, sizeof expanded);
    line = expanded;

    int argc = split(line, argv, ARGV_MAX);

    if (needs_wiring(argc, argv)) {
        kprintf("the monitor joins nothing with | and redirects nothing\n");
        me()->status = 2;
        return;
    }
    run_argv(argc, argv);   /* argc 0 just means they pressed enter */
}

static void prompt(void)
{
    console_set_colors(COLOR_PROMPT, 0x101018);
    kprintf("monitor@velvet[%u]:%s# ", tty_my_console() + 1, shell_cwd);
    console_set_colors(COLOR_TEXT, 0x101018);
}




static void history_add(const char *line)
{
    if (line[0] == '\0') {
        return;             /* dont remember the user pressing enter */
    }
    if (hist_count > 0 && strcmp(history[hist_count - 1], line) == 0) {
        return;             /* dont remember the same thing twice running */
    }

    if (hist_count == HISTORY_MAX) {
        /* oldest falls off the end */
        for (int i = 1; i < HISTORY_MAX; i++) {
            for (int j = 0; j < LINE_MAX; j++) {
                history[i - 1][j] = history[i][j];
            }
        }
        hist_count--;
    }

    size_t n = 0;
    while (line[n] && n < LINE_MAX - 1) {
        history[hist_count][n] = line[n];
        n++;
    }
    history[hist_count][n] = '\0';
    hist_count++;
}

static void cmd_history(int argc, char **argv)
{
    (void)argc; (void)argv;
    for (int i = 0; i < hist_count; i++) {
        kprintf("  %2d  %s\n", i + 1, history[i]);
    }
}

/*
 * the console treats \b as pure cursor-left now, same as any terminal,
 * so these work identically on the framebuffer and down the wire
 */

static void move_left(size_t n)
{
    while (n-- > 0) {
        kprintf("\b");
    }
}

/*
 * reprint everything from pos onward, plus a space to cover a character
 * that just shifted off the end, then come back to where the kernel was
 */
static void redraw_tail(const char *line, size_t len, size_t pos)
{
    for (size_t i = pos; i < len; i++) {
        kprintf("%c", line[i]);
    }
    kprintf(" ");
    move_left(len - pos + 1);
}

/* throw away what is on screen and put something else there. */
static void replace_line(char *line, size_t *len, size_t *pos, const char *with)
{
    move_left(*pos);                    /* back to the start of the line */
    size_t old = *len;

    size_t n = 0;
    while (with[n] && n < LINE_MAX - 1) {
        line[n] = with[n];
        kprintf("%c", with[n]);
        n++;
    }
    line[n] = '\0';

    for (size_t i = n; i < old; i++) {  /* cover whatever was longer */
        kprintf(" ");
    }
    move_left(old > n ? old - n : 0);

    *len = n;
    *pos = n;
}

static bool is_word_char(char c)
{
    return c != ' ';
}



/* how many leading characters two strings share */
static size_t common_prefix(const char *a, const char *b)
{
    size_t n = 0;
    while (a[n] != '\0' && a[n] == b[n]) {
        n++;
    }
    return n;
}

/*
 * the word the cursor is sitting in, and whether it is in command
 * position, meaning a builtin's name is what belongs there, rather than
 * a filename. returns where that word starts.
 */
static size_t word_start(const char *line, size_t pos, bool *first_word)
{
    size_t start = pos;
    while (start > 0 && line[start - 1] != ' ') {
        start--;
    }

    *first_word = true;
    for (size_t i = start; i-- > 0; ) {
        if (line[i] != ' ') {
            *first_word = false;
            break;
        }
    }
    return start;
}

/* replace the word under the cursor with `with`, redrawing what follows */
static void replace_word(char *line, size_t *len, size_t *pos,
                         size_t start, const char *with)
{
    size_t old_word = *pos - start;
    size_t tail_len = *len - *pos;
    size_t new_word = strlen(with);

    if (start + new_word + tail_len + 1 >= LINE_MAX) {
        return;
    }

    /*
     * shuffle whatever came after the word out of the way. it overlaps
     * itself whenever the word changes by less than its length, which is
     * what memmove is for
     */
    memmove(line + start + new_word, line + *pos, tail_len);
    for (size_t i = 0; i < new_word; i++) {
        line[start + i] = with[i];
    }
    *len = start + new_word + tail_len;
    line[*len] = '\0';

    move_left(old_word);
    for (size_t i = start; i < *len; i++) {
        kprintf("%c", line[i]);
    }
    /* blank anything the shorter word left behind */
    size_t was = start + old_word + tail_len;
    for (size_t i = *len; i < was; i++) {
        kprintf(" ");
    }
    *pos = start + new_word;
    move_left((*len > was ? *len : was) - *pos);
}

/* which command the line begins with, or NULL if it is not one the kernel knows. */
static const struct command *command_for_line(const char *line)
{
    size_t i = 0;
    while (line[i] == ' ') {
        i++;
    }
    size_t start = i;
    while (line[i] != '\0' && line[i] != ' ') {
        i++;
    }
    size_t n = i - start;

    for (const struct command *c = commands; c->name; c++) {
        if (strlen(c->name) == n && memcmp(c->name, line + start, n) == 0) {
            return c;
        }
    }
    return NULL;
}

/*
 * candidates come from one of three places depending on where you are:
 * builtin names in the first word, ramdisk filenames after a builtin
 * that takes one, and, once a word starts looking like a path, the
 * disk, which unlike the ramdisk is a real tree and has to be walked a
 * directory at a time.
 *
 * they are whole words rather than bare names, because a whole word is
 * what gets replaced: completing `notes` in `cat /disk/no` has to put
 * back `/disk/notes/`, not `notes`
 */
#define MAX_CANDIDATES 24
#define CAND_MAX       96

struct candidates {
    char items[MAX_CANDIDATES][CAND_MAX];
    int  count;
};

static void add_candidate(struct candidates *c, const char *dir,
                          const char *name, bool is_dir)
{
    if (c->count >= MAX_CANDIDATES) {
        return;
    }
    char *out = c->items[c->count];
    size_t n = 0;

    while (*dir != '\0' && n < CAND_MAX - 3) {
        out[n++] = *dir++;
    }
    if (name != NULL) {
        if (n > 0 && out[n - 1] != '/' && n < CAND_MAX - 3) {
            out[n++] = '/';
        }
        while (*name != '\0' && n < CAND_MAX - 3) {
            out[n++] = *name++;
        }
    }
    /*
     * a directory gets a slash, so tab again carries straight on into
     * it rather than stopping at a name you cannot open
     */
    if (is_dir && n < CAND_MAX - 2) {
        out[n++] = '/';
    }

    out[n] = '\0';
    c->count++;
}

/*
 * an absolute word names a place in the one namespace, so it completes
 * against whatever is mounted there, the disk at /, the ramdisk at
 * /boot, without this having to know which is which
 */
static void gather_path(struct candidates *c, const char *word, size_t wlen)
{
    /*
     * split at the last slash: what comes before names the directory to
     * look in, what comes after is the part being matched
     */
    size_t cut = 0;
    bool have_slash = false;
    for (size_t i = 0; i < wlen; i++) {
        if (word[i] == '/') {
            cut = i;
            have_slash = true;
        }
    }
    if (!have_slash) {
        return;
    }

    char dir[CAND_MAX];
    size_t dlen = (cut == 0) ? 1 : cut;      /* "/" when the slash is first */
    if (dlen >= sizeof dir) {
        return;
    }
    memcpy(dir, word, dlen);
    dir[dlen] = '\0';

    const char *partial = word + cut + 1;
    size_t plen = wlen - cut - 1;

    struct vfs_file e;
    for (size_t i = 0; vfs_readdir(dir, i, &e); i++) {
        size_t n = strlen(e.name);
        if (n < plen || common_prefix(e.name, partial) < plen) {
            continue;
        }
        add_candidate(c, dir, e.name, e.is_dir);
        if (c->count >= MAX_CANDIDATES) {
            break;
        }
    }
}

static void gather(struct candidates *c, const char *line, size_t start,
                   size_t pos, bool first_word)
{
    c->count = 0;
    size_t plen = pos - start;
    const char *prefix = line + start;

    if (first_word) {
        for (const struct command *cmd = commands; cmd->name; cmd++) {
            if (strlen(cmd->name) >= plen
                && common_prefix(cmd->name, prefix) >= plen) {
                add_candidate(c, cmd->name, NULL, false);
            }
        }

        return;
    }

    /*
     * filenames only after a command that takes one: completing `bt
     * in<tab>` into a filename would be surprising
     */
    const struct command *cmd = command_for_line(line);
    if (cmd == NULL || !cmd->takes_file) {
        return;
    }

    /*
     * any word with a slash in it names a place in the tree, whether or
     * not it starts at the root, `boot/mo` is as much a path as
     * `/boot/mo` is
     */
    bool is_path = false;
    for (size_t i = 0; i < plen; i++) {
        if (prefix[i] == '/') {
            is_path = true;
            break;
        }
    }

    if (is_path) {
        gather_path(c, prefix, plen);
    } else {
        /*
         * a bare name is looked for the way vfs_open looks for one: the
         * disk's root first, then the ramdisk. offered bare, since bare
         * is what was typed
         */
        struct vfs_file v;
        for (size_t i = 0;
             vfs_readdir("/", i, &v) && c->count < MAX_CANDIDATES; i++) {
            size_t n = strlen(v.name);
            if (n >= plen && common_prefix(v.name, prefix) >= plen) {
                add_candidate(c, v.name, NULL, v.is_dir);
            }
        }
    }

    /*
     * and the ramdisk's own names, which are whole paths carrying no
     * leading slash, `selftests/s` completes to `selftests/sched/...`
     * out of this and out of nothing else
     */
    struct ramdisk_file f;
    for (size_t i = 0; ramdisk_stat(i, &f) && c->count < MAX_CANDIDATES; i++) {
        const char *name = f.name;
        if (name[0] == '.' && name[1] == '/') {
            name += 2;
        }
        size_t n = strlen(name);
        if (n == 0 || name[n - 1] == '/') {
            continue;       /* tar's directory records have nothing behind them */
        }
        if (n >= plen && common_prefix(name, prefix) >= plen) {
            add_candidate(c, name, NULL, false);
        }
    }
}

static void complete(char *line, size_t *len, size_t *pos)
{
    /*
     * the editor does not keep the line terminated while you are typing
     *, it only does that on enter, and everything below wants a
     * string. terminate it here, where len is known
     */
    line[*len] = '\0';

    bool first_word;
    size_t start = word_start(line, *pos, &first_word);

    /*
     * a bare tab in the command position does nothing on purpose:
     * dumping the whole command list is what `help` is for. after a
     * command that takes a filename there is no such list to consult,
     * so an empty word there is worth answering
     */
    if (*pos == start && first_word) {
        return;
    }

    /*
     * two kilobytes, which is more than the shell thread's stack wants
     * to spare, and the shell is the only thing that completes anything
     */
    static struct candidates c;
    gather(&c, line, start, *pos, first_word);

    if (c.count == 0) {
        return;
    }
    if (c.count == 1) {
        replace_word(line, len, pos, start, c.items[0]);
        return;
    }

    /*
     * several: fill in as far as they all agree, and only if that adds
     * nothing do the kernel shows the list
     */
    size_t shared = strlen(c.items[0]);
    for (int i = 1; i < c.count; i++) {
        size_t n = common_prefix(c.items[0], c.items[i]);
        if (n < shared) {
            shared = n;
        }
    }
    if (shared > *pos - start) {
        char partial[LINE_MAX];
        for (size_t i = 0; i < shared; i++) {
            partial[i] = c.items[0][i];
        }
        partial[shared] = '\0';
        replace_word(line, len, pos, start, partial);
        return;
    }

    kprintf("\n");
    for (int i = 0; i < c.count; i++) {
        kprintf("  %s", c.items[i]);
    }
    kprintf("\n");
    prompt();
    for (size_t i = 0; i < *len; i++) {
        kprintf("%c", line[i]);
    }
    move_left(*len - *pos);
}

/* read a line for the kernel's own prompts. */
static void read_line(char *buf, size_t max)
{
    size_t len = 0;
    for (;;) {
        int c = input_getchar_blocking();

        if (c == '\n') {
            kprintf("\n");
            break;
        }
        if (c == '\b') {
            if (len > 0) {
                len--;
                kprintf("\b \b");
            }
            continue;
        }
        if (c < ' ' || c > '~') {
            continue;
        }
        if (len + 1 < max) {
            buf[len++] = (char)c;
            kprintf("%c", (char)c);
        }
    }
    buf[len] = '\0';
}

static void session_loop(void);

void shell_monitor(void)
{
    session_init();

    console_set_colors(COLOR_TEXT, 0x101018);
    kprintf("velvet v%s, the monitor\n\n", VELVET_VERSION);
    kprintf("the kernel's own commands, with nobody logged in and no "
            "programs run.\n`help` lists them. alt+1..4 go back to the "
            "system\n");
    session_loop();
}

/* the monitor's prompt, for as long as the machine is up */
static void session_loop(void)
{
    char line[LINE_MAX];

    for (;;) {
        size_t len = 0;     /* characters in the line */
        size_t pos = 0;     /* where the cursor sits within them */
        int hist_pos = hist_count;

        /* the machine is going down and this screen is not going to be typed at again. */
        if (init_stopping()) {
            for (;;) {
                sleep_ms(1000);
            }
        }

        prompt();

        for (;;) {
            int c = input_getchar_blocking();

            if (c == '\n') {
                /*
                 * print the tail the kernel was sitting in front of, so the
                 * finished line reads properly before the kernel moves on
                 */
                for (size_t i = pos; i < len; i++) {
                    kprintf("%c", line[i]);
                }
                kprintf("\n");
                break;
            }

            if (c == KEY_CTRL_C) {
                for (size_t i = pos; i < len; i++) {
                    kprintf("%c", line[i]);
                }
                kprintf("^C\n");
                len = 0;
                cancel_generation++;    /* and call back any personas */
                break;
            }


            if (c == KEY_LEFT || c == 0x02) {           /* ctrl+b */
                if (pos > 0) { move_left(1); pos--; }
                continue;
            }
            if (c == KEY_RIGHT || c == 0x06) {          /* ctrl+f */
                if (pos < len) { kprintf("%c", line[pos]); pos++; }
                continue;
            }
            if (c == 0x01) {                            /* ctrl+a, home */
                move_left(pos);
                pos = 0;
                continue;
            }
            if (c == 0x05) {                            /* ctrl+e, end */
                while (pos < len) { kprintf("%c", line[pos]); pos++; }
                continue;
            }


            if (c == KEY_UP) {
                if (hist_pos > 0) {
                    hist_pos--;
                    replace_line(line, &len, &pos, history[hist_pos]);
                }
                continue;
            }
            if (c == KEY_DOWN) {
                if (hist_pos < hist_count) {
                    hist_pos++;
                    replace_line(line, &len, &pos,
                                 hist_pos == hist_count ? "" : history[hist_pos]);
                }
                continue;
            }


            if (c == '\b') {                            /* backspace */
                if (pos > 0) {
                    for (size_t i = pos - 1; i < len - 1; i++) {
                        line[i] = line[i + 1];
                    }
                    len--; pos--;
                    move_left(1);
                    redraw_tail(line, len, pos);
                }
                continue;
            }
            if (c == KEY_DELETE || c == 0x04) {         /* del, ctrl+d */
                if (pos < len) {
                    for (size_t i = pos; i < len - 1; i++) {
                        line[i] = line[i + 1];
                    }
                    len--;
                    redraw_tail(line, len, pos);
                }
                continue;
            }
            if (c == 0x15) {                            /* ctrl+u, kill line */
                replace_line(line, &len, &pos, "");
                continue;
            }
            if (c == 0x0b) {                            /* ctrl+k, kill to end */
                for (size_t i = pos; i < len; i++) { kprintf(" "); }
                move_left(len - pos);
                len = pos;
                line[len] = '\0';
                continue;
            }
            if (c == 0x17) {                            /* ctrl+w, kill a word */
                size_t start = pos;
                while (start > 0 && !is_word_char(line[start - 1])) start--;
                while (start > 0 && is_word_char(line[start - 1]))  start--;
                size_t removed = pos - start;
                if (removed > 0) {
                    for (size_t i = start; i + removed < len; i++) {
                        line[i] = line[i + removed];
                    }
                    len -= removed;
                    pos = start;
                    move_left(removed);
                    for (size_t i = pos; i < len; i++) kprintf("%c", line[i]);
                    for (size_t i = 0; i < removed; i++) kprintf(" ");
                    move_left(len - pos + removed);
                }
                continue;
            }

            if (c == 0x0c) {                            /* ctrl+l */
                /*
                 * wipe the screen and put the prompt back with whatever
                 * was half-typed, cursor where it was. same as every
                 * terminal, and it does not disturb the line
                 */
                console_clear();
                prompt();
                for (size_t i = 0; i < len; i++) {
                    kprintf("%c", line[i]);
                }
                move_left(len - pos);
                continue;
            }

            if (c == '\t') {
                complete(line, &len, &pos);
                continue;
            }


            if (c < ' ' || c > '~') {
                continue;       /* anything else non-printable is not the kernel's */
            }
            if (len + 1 < LINE_MAX) {
                for (size_t i = len; i > pos; i--) {
                    line[i] = line[i - 1];
                }
                line[pos] = (char)c;
                len++;
                /*
                 * print from here to the end, then step back to just
                 * after the character the kernel inserted
                 */
                for (size_t i = pos; i < len; i++) {
                    kprintf("%c", line[i]);
                }
                pos++;
                move_left(len - pos);
            }
        }

        line[len] = '\0';
        history_add(line);
        run_line(line);
    }

    /*
     * somebody logged out. the thread this ran on ends, init notices,
     * and a new session starts on this console, with nothing at all
     * carried over, which is the whole difference
     */
}
