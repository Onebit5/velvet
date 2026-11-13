// SPDX-License-Identifier: GPL-2.0-only
/*
 * fs/proc_pid.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * /proc/<pid>: a directory for every process, and what is in it.
 */

#include "proc_node.h"

#include <asm/cpu.h>
#include <velvet/addrspace.h>
#include <velvet/dev.h>
#include <velvet/file.h>
#include <velvet/kmalloc.h>
#include <velvet/kprintf.h>
#include <velvet/namei.h>
#include <velvet/pipe.h>
#include <velvet/pmm.h>
#include <velvet/process.h>
#include <velvet/sched.h>
#include <velvet/socket.h>
#include <velvet/string.h>
#include <velvet/tty.h>
#include <velvet/usermode.h>
#include <velvet/vfs.h>
#include <velvet/vmm.h>
#include <uapi/velvet/fcntl.h>
#include <uapi/velvet/prctl.h>
#include <uapi/velvet/signal.h>
#include <uapi/velvet/stat.h>

/*
 * these are read by programs that were written for another kernel, and
 * that parse them: ps, top, a libc's sysconf, anything that finds out
 * about itself by opening /proc/self. so each is laid out field for
 * field the way those programs expect, numbers they cannot have from
 * here being 0, and every field there all the same, since a program
 * counts its way along a line to the one it wants
 */

/* what a clock tick is to a program, sysconf's _SC_CLK_TCK and AT_CLKTCK */
#define USER_HZ 100

/* how wide maps pads a line before the name of what is mapped */
#define MAPS_NAME_COLUMN 73

/* every capability there is, which root has, as a capability set */
#define CAPS_ALL ((uint64_t)0x000001ffffffffff)

/* the signal a process's parent is told it ended with */
#define EXIT_SIGNAL 17

/*
 * what a report is made of: the process as one of its threads sees it,
 * and that thread as the scheduler does, when it still runs
 */
struct seen {
    struct process_view view;
    struct sched_info info;
    bool b_info;
};

static bool see(const struct proc_ref *p_ref, struct seen *p_seen)
{
    if (!process_view(p_ref->i_pid, p_ref->i_tid, &p_seen->view)) {
        return false;
    }
    p_seen->b_info = p_seen->view.i_thread != 0
                     && sched_info(p_seen->view.i_thread, &p_seen->info);
    return true;
}

bool proc_owner(int i_pid, uint32_t *p_uid, uint32_t *p_gid)
{
    struct process_view view;
    if (!process_view(i_pid, i_pid, &view)) {
        return false;
    }
    *p_uid = (uint32_t)view.i_uid;
    *p_gid = (uint32_t)view.i_gid;
    return true;
}

/* a count of ticks as the clock ticks a program counts in */
static uint64_t clock_ticks(uint64_t u_ticks)
{
    return u_ticks * USER_HZ / CLOCK_TICK_HZ;
}

/*
 * the letter for what it is doing: running or able to, asleep, stopped,
 * or ended and not yet waited for
 */
static char state_of(const struct seen *p_seen)
{
    if (p_seen->view.b_exited) {
        return 'Z';
    }
    if (p_seen->view.i_stopped_by != 0
        || (p_seen->b_info && p_seen->info.b_stopped)) {
        return 'T';
    }
    if (!p_seen->b_info) {
        return 'S';
    }
    switch (p_seen->info.e_state) {
    case THREAD_RUNNING:
    case THREAD_READY:
        return 'R';
    case THREAD_DEAD:
        return 'X';
    default:
        return 'S';
    }
}

static const char *state_name(char c_state)
{
    switch (c_state) {
    case 'R': return "R (running)";
    case 'T': return "T (stopped)";
    case 'Z': return "Z (zombie)";
    case 'X': return "X (dead)";
    default:  return "S (sleeping)";
    }
}

/*
 * what it is called, cut to what a program may set it to: the process's
 * name, or the thread's own for a thread's directory
 */
static void comm_of(const struct proc_ref *p_ref, const struct seen *p_seen,
                    char *str_out)
{
    const char *str_name = (p_ref->b_thread && p_seen->b_info)
                           ? p_seen->info.arr_name : p_seen->view.arr_name;
    strncpy(str_out, str_name, TASK_COMM_LEN - 1);
    str_out[TASK_COMM_LEN - 1] = '\0';
}

/*
 * the time it has had of the cpus: the whole process's for a process's
 * directory, and the one thread's for a thread's
 */
static struct cpu_usage usage_of(const struct proc_ref *p_ref,
                                 const struct seen *p_seen,
                                 struct cpu_usage *p_children)
{
    struct cpu_usage used = sched_process_usage(p_ref->i_pid, p_children);
    if (p_ref->b_thread) {
        used = p_seen->b_info ? p_seen->info.cpu : (struct cpu_usage){ 0 };
    }
    return used;
}

/*
 * where a program's image lies: the executable part, and the part of it
 * that may be written, which is its data
 */
struct image_bounds {
    uint64_t u_code_start;
    uint64_t u_code_end;
    uint64_t u_data_start;
    uint64_t u_data_end;
};

static void bounds_of(struct addrspace *p_space, struct image_bounds *p_out)
{
    memset(p_out, 0, sizeof *p_out);
    struct vma *p_regions = kmalloc(VMA_MAX * sizeof *p_regions);
    if (p_regions == NULL) {
        return;
    }
    size_t sz_count = addrspace_regions(p_space, p_regions, VMA_MAX);
    for (size_t sz_at = 0; sz_at < sz_count && sz_at < VMA_MAX; sz_at++) {
        const struct vma *v = &p_regions[sz_at];
        bool b_exec = (v->flags & PTE_USER) && !(v->flags & PTE_NX);
        if (v->kind != VMA_FILE) {
            continue;
        }
        uint64_t *p_start = b_exec ? &p_out->u_code_start
                                   : &p_out->u_data_start;
        uint64_t *p_end = b_exec ? &p_out->u_code_end : &p_out->u_data_end;
        if ((b_exec || (v->flags & PTE_WRITE)) && *p_start == 0) {
            *p_start = v->start;
        }
        if (b_exec || (v->flags & PTE_WRITE)) {
            *p_end = v->end;
        }
    }
    kfree(p_regions);
}

/* what /proc/<pid>/stat and statm say of a process's memory */
struct memory {
    bool b_any;                 /* it has a space: it has not ended */
    struct addrspace_sizes sizes;
    struct image_bounds bounds;
    uint64_t u_faults;
    uint64_t u_stack_start;
    uint64_t u_brk_start;
    uint64_t u_arg_start;
    uint64_t u_arg_end;
    uint64_t u_env_start;
    uint64_t u_env_end;
};

static void memory_of(int i_pid, struct memory *p_out)
{
    memset(p_out, 0, sizeof *p_out);
    struct addrspace *p_space = sched_space_of(i_pid);
    if (p_space == NULL) {
        return;
    }
    p_out->b_any = true;
    addrspace_sizes(p_space, &p_out->sizes);
    bounds_of(p_space, &p_out->bounds);
    p_out->u_faults = p_space->u_faults;
    p_out->u_stack_start = p_space->u_stack_start;
    p_out->u_brk_start = p_space->u_brk_start;
    p_out->u_arg_start = p_space->u_arg_start;
    p_out->u_arg_end = p_space->u_arg_end;
    p_out->u_env_start = p_space->u_env_start;
    p_out->u_env_end = p_space->u_env_end;
    addrspace_put(p_space);
}

/*
 * stat: one line, every field in its place, the fields a program finds
 * by counting spaces after the name in brackets
 */
static void show_stat(const struct proc_ref *p_ref)
{
    struct seen seen;
    if (!see(p_ref, &seen)) {
        return;
    }
    const struct process_view *v = &seen.view;
    char arr_comm[TASK_COMM_LEN];
    comm_of(p_ref, &seen, arr_comm);
    struct cpu_usage children;
    struct cpu_usage used = usage_of(p_ref, &seen, &children);
    uint32_t u_tty = 0;
    int i_front = -1;
    if (!tty_of_session(v->i_sid, &u_tty, &i_front)) {
        u_tty = 0;
        i_front = -1;
    }
    struct memory mem;
    memory_of(v->i_pid, &mem);
    uint64_t u_rss_limit = v->arr_limits[RLIMIT_RSS].rlim_cur;
    bool b_leader = v->i_tid == v->i_pid;
    int i_cpu = (seen.b_info && seen.info.i_cpu >= 0) ? seen.info.i_cpu : 0;

    kprintf("%d (%s) %c %d %d %d %u %d 0 ", v->i_tid, arr_comm,
            state_of(&seen), v->i_ppid, v->i_pgid, v->i_sid, u_tty, i_front);
    kprintf("%lu 0 0 0 %lu %lu %lu %lu 20 0 %lu 0 %lu ",
            p_ref->b_thread ? 0 : mem.u_faults, clock_ticks(used.u_user),
            clock_ticks(used.u_system), clock_ticks(children.u_user),
            clock_ticks(children.u_system), (uint64_t)v->sz_threads,
            v->u_started_ms * USER_HZ / 1000);
    kprintf("%lu %lu %lu %lu %lu %lu 0 0 ",
            mem.sizes.u_size * PAGE_SIZE, mem.sizes.u_resident, u_rss_limit,
            mem.bounds.u_code_start, mem.bounds.u_code_end,
            mem.u_stack_start);
    kprintf("%lu %lu %lu %lu 0 0 0 %d %d 0 0 0 0 0 ",
            v->u_pending & 0x7fffffffu, v->u_blocked & 0x7fffffffu,
            v->u_ignored & 0x7fffffffu, v->u_caught & 0x7fffffffu,
            (b_leader || !p_ref->b_thread) ? EXIT_SIGNAL : -1, i_cpu);
    kprintf("%lu %lu %lu %lu %lu %lu %lu %d\n",
            mem.bounds.u_data_start, mem.bounds.u_data_end, mem.u_brk_start,
            mem.u_arg_start, mem.u_arg_end, mem.u_env_start, mem.u_env_end,
            v->b_exited ? v->i_wait_status : 0);
}

/*
 * statm: the space in pages, all of it, what is there, what of that is
 * the image's, the image's code, nothing for libraries, its data and
 * stack, and nothing for what nobody has used since 2.6
 */
static void show_statm(const struct proc_ref *p_ref)
{
    struct memory mem;
    memory_of(p_ref->i_pid, &mem);
    kprintf("%lu %lu %lu %lu 0 %lu 0\n", mem.sizes.u_size,
            mem.sizes.u_resident, mem.sizes.u_file, mem.sizes.u_exe,
            mem.sizes.u_data + mem.sizes.u_stack);
}

/* pages as the kilobytes status counts in */
static uint64_t kb(uint64_t u_pages)
{
    return u_pages * (PAGE_SIZE / 1024);
}

/* how many bits a set has */
static unsigned bits_in(uint64_t u_set)
{
    unsigned u_count = 0;
    for (; u_set != 0; u_set &= u_set - 1) {
        u_count++;
    }
    return u_count;
}

/* a set of cores as a list of runs, 0-3,6 */
static void show_core_list(uint64_t u_cores)
{
    const char *str_sep = "";
    for (unsigned u_from = 0; u_from < 64; u_from++) {
        if ((u_cores >> u_from & 1) == 0) {
            continue;
        }
        unsigned u_to = u_from;
        while (u_to + 1 < 64 && (u_cores >> (u_to + 1) & 1) != 0) {
            u_to++;
        }
        kprintf((u_to == u_from) ? "%s%u" : "%s%u-%u", str_sep, u_from, u_to);
        str_sep = ",";
        u_from = u_to;
    }
    kprintf("\n");
}

static void show_status_memory(const struct memory *p_mem)
{
    const struct addrspace_sizes *s = &p_mem->sizes;
    /*
     * no high-water mark is kept, so the peak is what it has now, and
     * nothing is locked, pinned, shared or swapped
     */
    kprintf("VmPeak:\t%8lu kB\nVmSize:\t%8lu kB\nVmLck:\t%8lu kB\n"
            "VmPin:\t%8lu kB\n", kb(s->u_size), kb(s->u_size), (uint64_t)0,
            (uint64_t)0);
    kprintf("VmHWM:\t%8lu kB\nVmRSS:\t%8lu kB\nRssAnon:\t%8lu kB\n"
            "RssFile:\t%8lu kB\nRssShmem:\t%8lu kB\n", kb(s->u_resident),
            kb(s->u_resident), kb(s->u_resident - s->u_file), kb(s->u_file),
            0ul);
    kprintf("VmData:\t%8lu kB\nVmStk:\t%8lu kB\nVmExe:\t%8lu kB\n"
            "VmLib:\t%8lu kB\nVmPTE:\t%8lu kB\nVmSwap:\t%8lu kB\n",
            kb(s->u_data), kb(s->u_stack), kb(s->u_exe), 0ul,
            kb(s->u_tables), 0ul);
    kprintf("HugetlbPages:\t%8lu kB\nCoreDumping:\t0\nTHP_enabled:\t0\n",
            0ul);
}

/*
 * status: the same facts and more, a name and a tab before each, which
 * is what a person reads and what a good deal of code greps
 */
static void show_status(const struct proc_ref *p_ref)
{
    struct seen seen;
    if (!see(p_ref, &seen)) {
        return;
    }
    const struct process_view *v = &seen.view;
    char arr_comm[TASK_COMM_LEN];
    comm_of(p_ref, &seen, arr_comm);
    struct memory mem;
    memory_of(v->i_pid, &mem);

    kprintf("Name:\t%s\nUmask:\t%04o\nState:\t%s\n", arr_comm, v->u_umask,
            state_name(state_of(&seen)));
    kprintf("Tgid:\t%d\nNgid:\t0\nPid:\t%d\nPPid:\t%d\nTracerPid:\t0\n",
            v->i_pid, v->i_tid, v->i_ppid);
    kprintf("Uid:\t%d\t%d\t%d\t%d\nGid:\t%d\t%d\t%d\t%d\nFDSize:\t%d\n",
            v->i_uid, v->i_uid, v->i_uid, v->i_uid, v->i_gid, v->i_gid,
            v->i_gid, v->i_gid, MAX_FDS);
    kprintf("Groups:\t");
    for (int i_at = 0; i_at < v->i_groups; i_at++) {
        kprintf((i_at == 0) ? "%d" : " %d", v->arr_groups[i_at]);
    }
    kprintf("\nNStgid:\t%d\nNSpid:\t%d\nNSpgid:\t%d\nNSsid:\t%d\n",
            v->i_pid, v->i_tid, v->i_pgid, v->i_sid);
    if (mem.b_any) {
        show_status_memory(&mem);
    }

    uint64_t u_pending = v->u_pending | v->u_shared;
    kprintf("Threads:\t%lu\nSigQ:\t%u/%lu\n", (uint64_t)v->sz_threads,
            bits_in(u_pending), v->arr_limits[RLIMIT_SIGPENDING].rlim_cur);
    kprintf("SigPnd:\t%016lx\nShdPnd:\t%016lx\nSigBlk:\t%016lx\n"
            "SigIgn:\t%016lx\nSigCgt:\t%016lx\n", v->u_pending, v->u_shared,
            v->u_blocked, v->u_ignored, v->u_caught);
    uint64_t u_caps = (v->i_uid == 0) ? CAPS_ALL : 0;
    kprintf("CapInh:\t%016lx\nCapPrm:\t%016lx\nCapEff:\t%016lx\n"
            "CapBnd:\t%016lx\nCapAmb:\t%016lx\n", 0ul, u_caps, u_caps,
            CAPS_ALL, 0ul);
    kprintf("NoNewPrivs:\t0\nSeccomp:\t0\nSeccomp_filters:\t0\n");

    uint64_t u_cores = sched_cores_mask();
    kprintf("Cpus_allowed:\t%lx\nCpus_allowed_list:\t", u_cores);
    show_core_list(u_cores);
    kprintf("Mems_allowed:\t1\nMems_allowed_list:\t0\n");
    struct cpu_usage none = { 0 };
    const struct cpu_usage *p_cpu = seen.b_info ? &seen.info.cpu : &none;
    kprintf("voluntary_ctxt_switches:\t%lu\nnonvoluntary_ctxt_switches:\t%lu\n",
            p_cpu->u_waited, p_cpu->u_preempted);
}

/* comm: the name, and a line's end */
static void show_comm(const struct proc_ref *p_ref)
{
    struct seen seen;
    char arr_comm[TASK_COMM_LEN];
    if (see(p_ref, &seen)) {
        comm_of(p_ref, &seen, arr_comm);
        kprintf("%s\n", arr_comm);
    }
}

/*
 * a stretch of a process's memory as it is now, read out of its own
 * pages: where exec laid out its arguments, or its environment
 */
static void show_strings(int i_pid, bool b_environment)
{
    struct addrspace *p_space = sched_space_of(i_pid);
    char *p_buf = (p_space != NULL) ? kmalloc(PAGE_SIZE) : NULL;
    if (p_buf != NULL) {
        uint64_t u_at = b_environment ? p_space->u_env_start
                                      : p_space->u_arg_start;
        uint64_t u_end = b_environment ? p_space->u_env_end
                                       : p_space->u_arg_end;
        while (u_at < u_end) {
            size_t sz_want = (u_end - u_at < PAGE_SIZE)
                             ? (size_t)(u_end - u_at) : PAGE_SIZE;
            size_t sz_got = addrspace_read(p_space, u_at, p_buf, sz_want);
            kwrite(p_buf, sz_got);
            u_at += sz_want;
            if (sz_got < sz_want) {
                break;
            }
        }
    }
    kfree(p_buf);
    addrspace_put(p_space);
}

/* cmdline: the arguments, each ended by a NUL, as the program has them now */
static void show_cmdline(const struct proc_ref *p_ref)
{
    show_strings(p_ref->i_pid, false);
}

/* environ: and the environment, NAME=value each, the same way */
static void show_environ(const struct proc_ref *p_ref)
{
    show_strings(p_ref->i_pid, true);
}

/* how many digits u_number is in base u_base, at least u_least */
static int digits(uint64_t u_number, unsigned u_base, int i_least)
{
    int i_count = 1;
    for (; u_number >= u_base; u_number /= u_base) {
        i_count++;
    }
    return (i_count > i_least) ? i_count : i_least;
}

/* the program's image, as maps names it and numbers the file */
struct image_file {
    char arr_name[PATH_MAX];
    uint64_t u_dev;
    uint64_t u_ino;
};

static void image_of(int i_pid, struct image_file *p_out)
{
    char arr_exe[PATH_MAX];
    struct vfs_file node;
    memset(p_out, 0, sizeof *p_out);
    if (!process_paths(i_pid, NULL, NULL, arr_exe) || arr_exe[0] == '\0') {
        return;
    }
    path_seen_from(process_root(current_pid()), arr_exe, p_out->arr_name);
    if (vfs_open(arr_exe, &node)) {
        p_out->u_dev = node_dev(&node);
        p_out->u_ino = (node.u_ino != 0) ? node.u_ino : path_ino(arr_exe);
    }
}

/* one line of maps: a region, what it allows, and what is in it */
static void show_region(const struct vma *v, const struct addrspace *p_space,
                        const struct image_file *p_image)
{
    bool b_file = v->kind == VMA_FILE;
    bool b_heap = !b_file && v->start <= p_space->u_brk
                  && v->end >= p_space->u_brk_start;
    const char *str_name = b_file ? p_image->arr_name
                         : (v->end == USER_STACK_TOP) ? "[stack]"
                         : b_heap ? "[heap]" : NULL;
    uint64_t u_offset = b_file ? v->image_offset : 0;
    uint64_t u_dev = b_file ? p_image->u_dev : 0;
    uint64_t u_ino = b_file ? p_image->u_ino : 0;
    bool b_read = (v->flags & PTE_USER) != 0;

    kprintf("%08lx-%08lx %c%c%c%c %08lx %02lx:%02lx %lu ", v->start, v->end,
            b_read ? 'r' : '-', (v->flags & PTE_WRITE) ? 'w' : '-',
            (b_read && !(v->flags & PTE_NX)) ? 'x' : '-', 'p', u_offset,
            u_dev >> 8, u_dev & 0xff, u_ino);
    if (str_name == NULL || str_name[0] == '\0') {
        kprintf("\n");
        return;
    }
    int i_used = digits(v->start, 16, 8) + 1 + digits(v->end, 16, 8) + 6
                 + digits(u_offset, 16, 8) + 1 + digits(u_dev >> 8, 16, 2)
                 + 1 + digits(u_dev & 0xff, 16, 2) + 1 + digits(u_ino, 10, 1)
                 + 1;
    int i_pad = (i_used < MAPS_NAME_COLUMN) ? MAPS_NAME_COLUMN - i_used : 0;
    kprintf("%*s%s\n", i_pad, "", str_name);
}

/*
 * maps: a line for each region of the space, lowest first, and what is
 * in it: the program's image, the heap, the stack, or nothing named
 */
static void show_maps(const struct proc_ref *p_ref)
{
    struct addrspace *p_space = sched_space_of(p_ref->i_pid);
    struct vma *p_regions = (p_space != NULL)
                            ? kmalloc(VMA_MAX * sizeof *p_regions) : NULL;
    struct image_file *p_image = (p_regions != NULL)
                                 ? kmalloc(sizeof *p_image) : NULL;
    if (p_image != NULL) {
        image_of(p_ref->i_pid, p_image);
        size_t sz_count = addrspace_regions(p_space, p_regions, VMA_MAX);
        for (size_t sz_at = 0; sz_at < sz_count && sz_at < VMA_MAX;
             sz_at++) {
            show_region(&p_regions[sz_at], p_space, p_image);
        }
    }
    kfree(p_image);
    kfree(p_regions);
    addrspace_put(p_space);
}

/* what each limit is called, in their order, and what it counts */
static const struct {
    const char *str_name;
    const char *str_unit;
} s_arr_limits[RLIM_NLIMITS] = {
    { "Max cpu time", "seconds" },
    { "Max file size", "bytes" },
    { "Max data size", "bytes" },
    { "Max stack size", "bytes" },
    { "Max core file size", "bytes" },
    { "Max resident set", "bytes" },
    { "Max processes", "processes" },
    { "Max open files", "files" },
    { "Max locked memory", "bytes" },
    { "Max address space", "bytes" },
    { "Max file locks", "locks" },
    { "Max pending signals", "signals" },
    { "Max msgqueue size", "bytes" },
    { "Max nice priority", NULL },
    { "Max realtime priority", NULL },
    { "Max realtime timeout", "us" },
};

static void show_limit(uint64_t u_limit)
{
    if (u_limit == RLIM_INFINITY) {
        kprintf("%-20s ", "unlimited");
    } else {
        kprintf("%-20lu ", u_limit);
    }
}

/* limits: a table of them, soft and hard, as getrlimit has them */
static void show_limits(const struct proc_ref *p_ref)
{
    struct process_view view;
    if (!process_view(p_ref->i_pid, p_ref->i_tid, &view)) {
        return;
    }
    kprintf("%-25s %-20s %-20s %-10s\n", "Limit", "Soft Limit", "Hard Limit",
            "Units");
    for (size_t sz_at = 0; sz_at < RLIM_NLIMITS; sz_at++) {
        kprintf("%-25s ", s_arr_limits[sz_at].str_name);
        show_limit(view.arr_limits[sz_at].rlim_cur);
        show_limit(view.arr_limits[sz_at].rlim_max);
        if (s_arr_limits[sz_at].str_unit != NULL) {
            kprintf("%-10s", s_arr_limits[sz_at].str_unit);
        }
        kprintf("\n");
    }
}

/*
 * a name the way mounts writes it, the characters that would end a field
 * or a line written as their octal escapes
 */
static void show_mangled(const char *str_name)
{
    for (const char *p_at = str_name; *p_at != '\0'; p_at++) {
        if (*p_at == ' ' || *p_at == '\t' || *p_at == '\n' || *p_at == '\\') {
            kprintf("\\%03o", (unsigned)(uint8_t)*p_at);
        } else {
            kprintf("%c", *p_at);
        }
    }
}

/* whether str_path is str_root or under it */
static bool is_under(const char *str_path, const char *str_root)
{
    size_t sz_root = strlen(str_root);
    if (strcmp(str_root, "/") == 0) {
        return true;
    }
    return strncmp(str_path, str_root, sz_root) == 0
           && (str_path[sz_root] == '\0' || str_path[sz_root] == '/');
}

/*
 * mounts: what is mounted where the process can see, from its own "/":
 * what it is, where, the kind of filesystem, and whether it may be
 * written, and the two numbers nobody has used since dump
 */
static void show_mounts(const struct proc_ref *p_ref)
{
    char *str_root = kmalloc(2 * PATH_MAX);
    struct vfs_mount *p_mount = (str_root != NULL)
                                ? kmalloc(sizeof *p_mount) : NULL;
    if (p_mount != NULL && process_paths(p_ref->i_pid, NULL, str_root, NULL)) {
        char *str_seen = str_root + PATH_MAX;
        for (size_t sz_at = 0; vfs_mount_at(sz_at, p_mount); sz_at++) {
            if (!p_mount->present || !is_under(p_mount->arr_at, str_root)) {
                continue;
            }
            path_seen_from(str_root, p_mount->arr_at, str_seen);
            show_mangled((p_mount->arr_source[0] != '\0')
                         ? p_mount->arr_source : p_mount->what);
            kprintf(" ");
            show_mangled(str_seen);
            kprintf(" %s %s 0 0\n", p_mount->what,
                    p_mount->writable ? "rw" : "ro");
        }
    }
    kfree(p_mount);
    kfree(str_root);
}

bool proc_may_follow(int i_pid)
{
    uint32_t u_uid;
    uint32_t u_gid;
    int i_me = current_uid();
    return i_me == 0 || (proc_owner(i_pid, &u_uid, &u_gid)
                         && u_uid == (uint32_t)i_me);
}

/* cwd, root and exe: where it stands, its "/", and the program it runs */
static bool link_cwd(int i_pid, char *str_out)
{
    return process_paths(i_pid, str_out, NULL, NULL) && str_out[0] != '\0';
}

static bool link_root(int i_pid, char *str_out)
{
    return process_paths(i_pid, NULL, str_out, NULL);
}

static bool link_exe(int i_pid, char *str_out)
{
    return process_paths(i_pid, NULL, NULL, str_out) && str_out[0] != '\0';
}

bool proc_fd_target(int i_pid, int i_fd, char *str_out)
{
    struct proc_text text = { str_out, PATH_MAX, 0 };
    struct file *p_file = process_fd_file(i_pid, i_fd);
    size_t sz_dev;
    bool b_name = false;
    str_out[0] = '\0';
    if (p_file == NULL) {
        return false;
    }
    if (p_file->e_type == FILE_TYPE_PIPE && p_file->node.kind == VFS_NOWHERE) {
        proc_text_add(&text, "pipe:[");
        proc_text_number(&text, ((struct pipe *)p_file->p_private)->u_ino);
        proc_text_add(&text, "]");
    } else if (p_file->e_type == FILE_TYPE_SOCKET) {
        proc_text_add(&text, "socket:[");
        proc_text_number(&text, socket_ino(p_file));
        proc_text_add(&text, "]");
    } else if (p_file->arr_path[0] != '\0') {
        proc_text_add(&text, p_file->arr_path);
        b_name = true;
    } else if (dev_find_rdev(p_file->u_rdev, &sz_dev)) {
        /* a terminal the kernel opened, by the name /dev gives it */
        proc_text_add(&text, "/dev/");
        proc_text_add(&text, dev_name(sz_dev));
        b_name = true;
    } else {
        proc_text_add(&text, "anon_inode:[file]");
    }
    file_put(p_file);
    return b_name;
}

/*
 * fdinfo/<fd>: where the descriptor is, the flags it was opened with as
 * open takes them, and the number of what it is open on
 */
void proc_show_fdinfo(const struct proc_ref *p_ref)
{
    int i_fd = (int)p_ref->sz_index;
    struct file *p_file = process_fd_file(p_ref->i_pid, i_fd);
    if (p_file == NULL) {
        return;
    }
    bool b_read = (p_file->u_mode & FILE_READ) != 0;
    bool b_write = (p_file->u_mode & FILE_WRITE) != 0;
    uint32_t u_flags = (b_read && b_write) ? O_RDWR : b_write ? O_WRONLY
                                                              : O_RDONLY;
    u_flags |= p_file->u_flags & (O_APPEND | O_NONBLOCK);
    u_flags |= (process_fd_cloexec(p_ref->i_pid, i_fd, -1) == 1) ? O_CLOEXEC
                                                                 : 0;
    /* a file opened by its name is opened for every size it may have */
    u_flags |= (p_file->arr_path[0] != '\0') ? O_LARGEFILE : 0;
    kprintf("pos:\t%lu\nflags:\t0%o\nmnt_id:\t0\nino:\t%lu\n", p_file->u_pos,
            u_flags, file_ino(p_file));
    file_put(p_file);
}

/* stack: the kernel's calls that led the thread to where it waits */
static void show_stack(const struct proc_ref *p_ref)
{
    struct process_view view;
    if (process_view(p_ref->i_pid, p_ref->i_tid, &view)
        && view.i_thread != 0) {
        sched_show_stack(view.i_thread);
    }
}

static const struct proc_entry s_arr_entries[] = {
    { "task",    S_IFDIR | 0555, false, true,  PROC_KIND_TASKS, NULL, NULL },
    /* what it has open is its owner's to look through */
    { "fd",      S_IFDIR | 0500, false, false, PROC_KIND_FDS, NULL, NULL },
    { "fdinfo",  S_IFDIR | 0555, false, false, PROC_KIND_FDINFOS, NULL,
      NULL },
    { "environ", S_IFREG | 0400, false, false, 0, show_environ, NULL },
    { "status",  S_IFREG | 0444, false, false, 0, show_status, NULL },
    { "limits",  S_IFREG | 0444, false, false, 0, show_limits, NULL },
    { "comm",    S_IFREG | 0444, false, false, 0, show_comm, NULL },
    { "cmdline", S_IFREG | 0444, false, false, 0, show_cmdline, NULL },
    { "stat",    S_IFREG | 0444, false, false, 0, show_stat, NULL },
    { "statm",   S_IFREG | 0444, false, false, 0, show_statm, NULL },
    { "maps",    S_IFREG | 0444, false, false, 0, show_maps, NULL },
    { "mounts",  S_IFREG | 0444, false, false, 0, show_mounts, NULL },
    { "cwd",     S_IFLNK | 0777, false, false, 0, NULL, link_cwd },
    { "root",    S_IFLNK | 0777, false, false, 0, NULL, link_root },
    { "exe",     S_IFLNK | 0777, false, false, 0, NULL, link_exe },
    /* where the kernel keeps things is root's to know */
    { "stack",   S_IFREG | 0400, true,  false, 0, show_stack, NULL },
};

#define ENTRIES (sizeof s_arr_entries / sizeof s_arr_entries[0])

const struct proc_entry *proc_entry_at(size_t sz_index)
{
    return (sz_index < ENTRIES) ? &s_arr_entries[sz_index] : NULL;
}

bool proc_entry_find(const char *str_name, bool b_thread, size_t *p_index)
{
    for (size_t sz_at = 0; sz_at < ENTRIES; sz_at++) {
        if (strcmp(s_arr_entries[sz_at].str_name, str_name) == 0
            && !(b_thread && s_arr_entries[sz_at].b_process)) {
            *p_index = sz_at;
            return true;
        }
    }
    return false;
}
