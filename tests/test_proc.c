// SPDX-License-Identifier: GPL-2.0-only
/*
 * tests/test_proc.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * host-side test for /proc: registering a file, reading what it says, and
 * a process's directory, against a process table made up for the purpose.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <sys/stat.h>      /* S_IFIFO, which is the same number here */

/* the lock complains through this when something is wrong with it */
void panic(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    exit(1);
}

#include "fs/proc_node.h"

#include <velvet/addrspace.h>
#include <velvet/file.h>
#include <velvet/kprintf.h>
#include <velvet/dev.h>
#include <velvet/namei.h>
#include <velvet/pipe.h>
#include <velvet/process.h>
#include <velvet/sched.h>
#include <velvet/slab.h>
#include <velvet/socket.h>
#include <velvet/tty.h>
#include <velvet/usermode.h>
#include <velvet/vfs.h>
#include <velvet/vmm.h>
#include <uapi/velvet/errno.h>
#include <uapi/velvet/fcntl.h>
#include <uapi/velvet/signal.h>

/* what kprintf.c and proc.c lean on, done the plainest way there is */
void serial_putchar(char c_ch)
{
    (void)c_ch;
}
bool console_ready(void)
{
    return false;
}
void console_putchar(char c_ch)
{
    (void)c_ch;
}
void *kmalloc(size_t sz_size)
{
    return malloc(sz_size);
}
void kfree(void *p_mem)
{
    free(p_mem);
}
uint64_t pit_uptime_ms(void)
{
    return 61234;
}

/* the slab an open file comes from, as plain malloc */
void slab_cache_init(struct slab_cache *cache, const char *name,
                     size_t sz_obj)
{
    (void)name;
    cache->obj_size = sz_obj;
}
void *slab_alloc(struct slab_cache *cache)
{
    return malloc(cache->obj_size);
}
void slab_free(void *object)
{
    free(object);
}

/* one thread, so the capture is a single pointer kprintf is told about. */
static struct kcapture *s_p_capture;

static struct kcapture *capturing(void)
{
    return s_p_capture;
}

void sched_capture(struct kcapture *p_cap)
{
    s_p_capture = p_cap;
}

/*
 * the process table /proc is reading, made up: init, which has ended and
 * not been waited for, and process 7, "worker", with a second thread, 9,
 * which is the reader unless a test says otherwise
 */
static int s_i_me = 7;
static int s_i_my_tid = 7;

#define ARG_START 0x6ffffffff000ull
#define ENV_START (ARG_START + 10)
#define ENV_END   (ENV_START + 7)
#define STACK_AT  0x6fffffffef80ull

static struct process s_init;
static struct process s_worker;
static struct addrspace s_space = {
    .u_brk_start = 0x402000, .u_brk = 0x404000,
    .u_arg_start = ARG_START, .u_arg_end = ENV_START,
    .u_env_start = ENV_START, .u_env_end = ENV_END,
    .u_stack_start = STACK_AT, .u_faults = 42,
};

int current_pid(void)
{
    return s_i_me;
}
int current_tid(void)
{
    return s_i_my_tid;
}
const struct process *process_find(int i_pid)
{
    return (i_pid == 1) ? &s_init : (i_pid == 7) ? &s_worker : NULL;
}
int process_pid_of(int i_tid)
{
    return (i_tid == 1) ? 1 : (i_tid == 7 || i_tid == 9) ? 7 : 0;
}
const char *process_root(int i_pid)
{
    (void)i_pid;
    return "/";
}
size_t process_pids(int *p_pids, size_t sz_max)
{
    (void)sz_max;
    p_pids[0] = 1;
    p_pids[1] = 7;
    return 2;
}
size_t process_tids(int i_pid, int *p_tids, size_t sz_max)
{
    (void)sz_max;
    p_tids[0] = i_pid;
    p_tids[1] = 9;
    return (i_pid == 7) ? 2 : 1;
}

bool process_view(int i_pid, int i_tid, struct process_view *p_out)
{
    memset(p_out, 0, sizeof *p_out);
    if (process_find(i_pid) == NULL || process_pid_of(i_tid) != i_pid) {
        return i_pid == 1 && i_tid == 1;
    }
    p_out->i_pid = i_pid;
    p_out->i_tid = i_tid;
    p_out->i_thread = i_tid * 10;
    for (size_t sz_at = 0; sz_at < RLIM_NLIMITS; sz_at++) {
        p_out->arr_limits[sz_at].rlim_cur = RLIM_INFINITY;
        p_out->arr_limits[sz_at].rlim_max = RLIM_INFINITY;
    }
    if (i_pid == 1) {
        strcpy(p_out->arr_name, "init");
        p_out->i_sid = 1;
        p_out->i_pgid = 1;
        p_out->b_exited = true;
        p_out->i_wait_status = 0x0100;
        p_out->i_thread = 0;
        return true;
    }
    strcpy(p_out->arr_name, "worker");
    p_out->i_ppid = 1;
    p_out->i_pgid = 7;
    p_out->i_sid = 1;
    p_out->i_uid = 1000;
    p_out->i_gid = 100;
    p_out->arr_groups[0] = 100;
    p_out->arr_groups[1] = 27;
    p_out->i_groups = 2;
    p_out->u_umask = 022;
    p_out->sz_threads = 2;
    p_out->sz_fds = 3;
    p_out->u_started_ms = 1500;
    p_out->u_pending = SIGNAL_BIT(SIGUSR1);
    p_out->u_shared = SIGNAL_BIT(SIGCHLD);
    p_out->u_blocked = SIGNAL_BIT(SIGINT);
    p_out->u_ignored = SIGNAL_BIT(SIGPIPE);
    p_out->u_caught = SIGNAL_BIT(SIGTERM);
    p_out->arr_limits[RLIMIT_NOFILE].rlim_cur = 64;
    p_out->arr_limits[RLIMIT_NOFILE].rlim_max = 64;
    p_out->arr_limits[RLIMIT_STACK].rlim_cur = 1048576;
    p_out->arr_limits[RLIMIT_CORE].rlim_cur = 0;
    return true;
}

bool process_paths(int i_pid, char *str_cwd, char *str_root, char *str_exe)
{
    if (str_cwd != NULL) {
        strcpy(str_cwd, "/home");
    }
    if (str_root != NULL) {
        strcpy(str_root, "/");
    }
    if (str_exe != NULL) {
        strcpy(str_exe, "/bin/worker");
    }
    return process_find(i_pid) != NULL;
}

bool sched_info(int i_id, struct sched_info *p_out)
{
    memset(p_out, 0, sizeof *p_out);
    p_out->e_state = (i_id == 70) ? THREAD_RUNNING : THREAD_SLEEPING;
    p_out->cpu = (i_id == 70) ? (struct cpu_usage){ 200, 40, 5, 3 }
                              : (struct cpu_usage){ 50, 10, 8, 1 };
    p_out->i_cpu = (i_id == 70) ? 1 : -1;
    strcpy(p_out->arr_name, (i_id == 70) ? "worker" : "helper");
    return i_id == 70 || i_id == 90;
}

struct cpu_usage sched_process_usage(int i_pid, struct cpu_usage *p_children)
{
    if (p_children != NULL) {
        *p_children = (struct cpu_usage){ 30, 20, 0, 0 };
    }
    return (i_pid == 7) ? (struct cpu_usage){ 250, 50, 13, 4 }
                        : (struct cpu_usage){ 0 };
}

/* the cores taking work: both, unless a check says otherwise */
static uint64_t s_u_cores = 3;

uint64_t sched_cores_mask(void)
{
    return s_u_cores;
}

/* two cores, their ticks by what each found, and the load */
bool sched_cpu_stat(unsigned u_cpu, struct sched_cpu_stat *p_out)
{
    if (u_cpu > 1) {
        return false;
    }
    p_out->cpu = (u_cpu == 0) ? (struct cpu_usage){ 120, 30, 0, 0 }
                              : (struct cpu_usage){ 80, 20, 0, 0 };
    p_out->u_idle = (u_cpu == 0) ? 5973 : 6023;
    p_out->u_switches = (u_cpu == 0) ? 1000 : 2345;
    return true;
}
void sched_load(uint64_t arr_load[3])
{
    arr_load[0] = 2048 + 1024;          /* 1.50 */
    arr_load[1] = 41;                   /* 0.02 */
    arr_load[2] = 0;
}
size_t sched_runnable(size_t *p_threads)
{
    if (p_threads != NULL) {
        *p_threads = 12;
    }
    return 2;
}
uint64_t process_made(void)
{
    return 314;
}
int process_last_id(void)
{
    return 77;
}
bool epoch_known(void)
{
    return true;
}
uint64_t epoch_now(void)
{
    return 1800000061;
}

void sched_show_stack(int i_id)
{
    kprintf("call trace of %d\n", i_id);
}

/* a space, as the reader sees one: init's is gone with its threads */
struct addrspace *sched_space_of(int i_pid)
{
    return (i_pid == 7) ? &s_space : NULL;
}
void addrspace_put(struct addrspace *p_as)
{
    (void)p_as;
}
void addrspace_sizes(struct addrspace *p_as, struct addrspace_sizes *p_out)
{
    (void)p_as;
    *p_out = (struct addrspace_sizes){
        .u_size = 300, .u_resident = 6, .u_file = 2, .u_exe = 1,
        .u_data = 3, .u_stack = 256, .u_tables = 4,
    };
}

/* code, data, a heap and the stack */
static const struct vma s_arr_regions[] = {
    { VMA_FILE, 0x400000, 0x401000, PTE_USER, NULL, 0, 0 },
    { VMA_FILE, 0x401000, 0x402000, PTE_USER | PTE_WRITE | PTE_NX, NULL,
      0x1000, 0 },
    { VMA_ANON, 0x402000, 0x404000, PTE_USER | PTE_WRITE | PTE_NX, NULL,
      0, 0 },
    { VMA_ANON, USER_STACK_TOP - 0x100000, USER_STACK_TOP,
      PTE_USER | PTE_WRITE | PTE_NX, NULL, 0, 0 },
};

size_t addrspace_regions(struct addrspace *p_as, struct vma *p_out,
                         size_t sz_max)
{
    (void)p_as;
    size_t sz_count = sizeof s_arr_regions / sizeof s_arr_regions[0];
    memcpy(p_out, s_arr_regions,
           ((sz_count < sz_max) ? sz_count : sz_max) * sizeof *p_out);
    return sz_count;
}

/* the arguments and the environment, where exec put them */
size_t addrspace_read(struct addrspace *p_as, uint64_t u_virt, void *p_out,
                      size_t sz_len)
{
    static const char s_arr_strings[] = "worker\0-v\0HOME=/";
    (void)p_as;
    if (u_virt < ARG_START || u_virt - ARG_START >= sizeof s_arr_strings) {
        return 0;
    }
    size_t sz_left = sizeof s_arr_strings - (size_t)(u_virt - ARG_START);
    size_t sz_got = (sz_len < sz_left) ? sz_len : sz_left;
    memcpy(p_out, s_arr_strings + (u_virt - ARG_START), sz_got);
    return sz_got;
}

bool tty_of_session(int i_sid, uint32_t *p_rdev, int *p_front)
{
    *p_rdev = (4u << 8) | 1u;
    *p_front = 7;
    return i_sid == 1;
}

/* the root, /proc, and a disk mounted where a name has a space in it */
bool vfs_mount_at(size_t sz_index, struct vfs_mount *p_out)
{
    static const char *const s_arr_at[] = { "/", "/proc", "/mnt/a disk" };
    static const char *const s_arr_from[] = { "", "proc", "/dev/sda1" };
    static const char *const s_arr_kind[] = { "ramfs", "proc", "ext4" };
    if (sz_index >= 3) {
        return false;
    }
    memset(p_out, 0, sizeof *p_out);
    strcpy(p_out->arr_at, s_arr_at[sz_index]);
    strcpy(p_out->arr_source, s_arr_from[sz_index]);
    p_out->what = s_arr_kind[sz_index];
    p_out->writable = sz_index != 1;
    p_out->present = true;
    return true;
}

bool vfs_open(const char *str_path, struct vfs_file *p_out)
{
    memset(p_out, 0, sizeof *p_out);
    p_out->u_ino = 1234;
    return strcmp(str_path, "/bin/worker") == 0;
}
uint64_t node_dev(const struct vfs_file *p_node)
{
    (void)p_node;
    return 0x10;
}
uint64_t path_ino(const char *str_path)
{
    (void)str_path;
    return 99;
}
void path_seen_from(const char *str_root, const char *str_path,
                    char *str_out)
{
    (void)str_root;
    strcpy(str_out, str_path);
}

/* who the reader is: worker's owner, unless a test says otherwise */
static int s_i_my_uid = 1000;
int current_uid(void)
{
    return s_i_my_uid;
}

/*
 * what worker has open: the terminal the kernel gave it, a pipe's
 * writing end, a file by its name, and a socket. each with references
 * enough that a test's puts never free it
 */
static struct pipe s_pipe = { .u_ino = 77 };
static struct file s_arr_fds[4] = {
    { .u_refs = 1000, .u_mode = FILE_READ | FILE_WRITE,
      .e_type = FILE_TYPE_TTY, .u_rdev = (4u << 8) | 1u },
    { .u_refs = 1000, .u_mode = FILE_WRITE, .e_type = FILE_TYPE_PIPE,
      .p_private = &s_pipe },
    { .u_refs = 1000, .u_mode = FILE_READ | FILE_WRITE,
      .e_type = FILE_TYPE_REGULAR, .u_pos = 12, .u_flags = O_APPEND,
      .arr_path = "/home/notes" },
    { .u_refs = 1000, .u_mode = FILE_READ | FILE_WRITE,
      .e_type = FILE_TYPE_SOCKET },
};
static const int s_arr_fd_numbers[4] = { 0, 1, 2, 5 };

struct file *process_fd_file(int i_pid, int i_fd)
{
    for (size_t sz_at = 0; i_pid == 7 && sz_at < 4; sz_at++) {
        if (s_arr_fd_numbers[sz_at] == i_fd) {
            s_arr_fds[sz_at].u_refs++;
            return &s_arr_fds[sz_at];
        }
    }
    return NULL;
}
int process_fd_from(int i_pid, int i_from)
{
    for (size_t sz_at = 0; i_pid == 7 && sz_at < 4; sz_at++) {
        if (s_arr_fd_numbers[sz_at] >= i_from) {
            return s_arr_fd_numbers[sz_at];
        }
    }
    return -1;
}
int process_fd_cloexec(int i_pid, int i_fd, int i_set)
{
    (void)i_set;
    return (i_pid == 7 && i_fd == 5) ? 1 : 0;
}
uint64_t file_ino(const struct file *p_file)
{
    return (p_file == &s_arr_fds[1]) ? 77 : 4321;
}
uint32_t socket_ino(const struct file *p_file)
{
    (void)p_file;
    return 1048577;
}
bool dev_find_rdev(uint32_t u_rdev, size_t *p_index)
{
    *p_index = 0;
    return u_rdev == ((4u << 8) | 1u);
}
const char *dev_name(size_t sz_index)
{
    return (sz_index == 0) ? "tty1" : NULL;
}

/* an end reopened: which it was another of, and how */
static const struct file *s_p_reopened;
static uint32_t s_u_reopened_mode;
static struct file s_new_end = { .u_refs = 1000 };
struct file *pipe_reopen(const struct file *p_end, uint32_t u_mode)
{
    s_p_reopened = p_end;
    s_u_reopened_mode = u_mode;
    return &s_new_end;
}

static int s_i_failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); s_i_failures++; } \
} while (0)

static void show_greeting(void)
{
    kprintf("hello from %s, %d lines\n", "the kernel", 1);
}

static void show_nothing(void)
{
}

/* a file that says something different every time it is asked */
static int s_i_asked;

static void show_count(void)
{
    s_i_asked++;
    kprintf("asked %d times, and this is the line after\n", s_i_asked);
}

/* the whole of an open file from where it is, `sz_step` bytes a read */
static size_t read_rest(struct file *p_file, char *str_out, size_t sz_step)
{
    size_t sz_at = 0;
    for (;;) {
        int64_t i_got = p_file->p_ops->read(p_file, str_out + sz_at,
                                            sz_step);
        if (i_got <= 0) {
            break;
        }
        sz_at += (size_t)i_got;
    }
    str_out[sz_at] = '\0';
    return sz_at;
}

/* registered file sz_index, opened */
static struct file *open_file(size_t sz_index)
{
    struct proc_ref ref = { .u_kind = PROC_KIND_FILE, .sz_index = sz_index };
    return proc_open(&ref, FILE_READ);
}

/* the whole of what str_rest names, read, and how long it is: -1 for none */
static int64_t read_node(const char *str_rest, char *str_out)
{
    struct vfs_file node;
    if (!proc_lookup(str_rest, true, &node)) {
        return -1;
    }
    struct file *p_file = proc_open(&node.proc, FILE_READ);
    if (p_file == NULL) {
        return -1;
    }
    size_t sz_len = read_rest(p_file, str_out, 512);
    file_put(p_file);
    return (int64_t)sz_len;
}

/* what entry sz_index of the directory str_rest is called, or "" */
static const char *listed(const char *str_rest, size_t sz_index)
{
    static struct vfs_file s_node;
    return proc_readdir(str_rest, sz_index, &s_node) ? s_node.name : "";
}

/* how many fields a line has, split at single spaces */
static int fields_in(const char *str_line)
{
    int i_count = 1;
    for (const char *p_at = str_line; *p_at != '\0' && *p_at != '\n'; p_at++) {
        i_count += *p_at == ' ';
    }
    return i_count;
}

static void check_tree(void)
{
    struct vfs_file node;
    struct vfs_file other;
    char arr_to[64];

    CHECK(proc_lookup("/7", true, &node) && node.is_dir && node.mode == 0555
          && node.uid == 1000 && node.gid == 100
          && strcmp(node.name, "7") == 0,
          "a process has a directory, its owner's");
    CHECK(proc_lookup("/9", true, &node) && node.is_dir
          && node.proc.i_pid == 7 && node.proc.i_tid == 9,
          "and a thread's number is its process, seen through it");
    CHECK(!proc_lookup("/8", true, &node) && !proc_lookup("/07", true, &node)
          && !proc_lookup("/0", true, &node),
          "a number nobody has is nothing, and so is one with a nought");

    size_t sz_files = 0;
    while (proc_name(sz_files) != NULL) {
        sz_files++;
    }
    CHECK(strcmp(listed("/", sz_files - 1), proc_name(sz_files - 1)) == 0
          && strcmp(listed("/", sz_files), "self") == 0
          && strcmp(listed("/", sz_files + 1), "thread-self") == 0
          && strcmp(listed("/", sz_files + 2), "1") == 0
          && strcmp(listed("/", sz_files + 3), "7") == 0
          && listed("/", sz_files + 4)[0] == '\0',
          "the top lists its files, self and thread-self, and the processes");

    CHECK(proc_lookup("/self", false, &node) && node.is_symlink
          && node.mode == 0777, "self is a link");
    CHECK(proc_readlink("/self", arr_to, sizeof arr_to) == 0
          && strcmp(arr_to, "7") == 0, "to the reader's process");
    CHECK(proc_lookup("/self", true, &node) && node.is_dir
          && node.proc.i_pid == 7, "and followed, is its directory");
    s_i_my_tid = 9;
    CHECK(proc_readlink("/thread-self", arr_to, sizeof arr_to) == 0
          && strcmp(arr_to, "7/task/9") == 0,
          "thread-self is the reader's thread's");
    CHECK(proc_lookup("/thread-self/comm", true, &node)
          && node.proc.b_thread && node.proc.i_tid == 9,
          "and what is in it is that thread's");
    s_i_my_tid = 7;
    CHECK(proc_readlink("/7", arr_to, sizeof arr_to) == -EINVAL,
          "a directory is no link");

    CHECK(strcmp(listed("/7/task", 0), "7") == 0
          && strcmp(listed("/7/task", 1), "9") == 0
          && listed("/7/task", 2)[0] == '\0', "task lists the threads");
    CHECK(proc_lookup("/7/task/9/stat", true, &node)
          && !proc_lookup("/7/task/9/task", true, &node)
          && !proc_lookup("/1/task/9", true, &node),
          "each a directory of its own, with no task in it, and only its own"
          " process's");
    CHECK(strcmp(listed("/7", 0), "task") == 0
          && strcmp(listed("/7/task/9", 0), "fd") == 0,
          "which a thread's listing leaves out");

    CHECK(proc_lookup("/7/stack", true, &node) && node.mode == 0400
          && node.uid == 0 && proc_lookup("/7/environ", true, &other)
          && other.mode == 0400 && other.uid == 1000,
          "the stack is root's alone, the environment its owner's");
    CHECK(proc_lookup("/self/stat", true, &node)
          && proc_lookup("/7/stat", true, &other) && node.u_ino == other.u_ino
          && proc_lookup("/7/task/7/stat", true, &other)
          && node.u_ino != other.u_ino && other.u_ino != 0,
          "a node's number is its own, by whichever name it is reached");
    CHECK(!proc_lookup("/7/nothing", true, &node)
          && !proc_lookup("/version/x", true, &node),
          "and what is not there is not");
}

/* where the link str_rest names leads, or why not, as a string */
static const char *link_to(const char *str_rest)
{
    static char s_arr_to[PATH_MAX];
    int64_t i_why = proc_readlink(str_rest, s_arr_to, sizeof s_arr_to);
    return (i_why == -EACCES) ? "EACCES" : (i_why < 0) ? "ERROR" : s_arr_to;
}

static void check_links(void)
{
    struct vfs_file node;
    char arr_text[512];
    char arr_to[PATH_MAX];
    size_t sz_used = 0;

    CHECK(strcmp(listed("/7/fd", 0), "0") == 0
          && strcmp(listed("/7/fd", 1), "1") == 0
          && strcmp(listed("/7/fd", 3), "5") == 0 && listed("/7/fd", 4)[0] == 0
          && strcmp(listed("/7/fdinfo", 2), "2") == 0,
          "fd and fdinfo list what is open");
    CHECK(proc_lookup("/7/fd", true, &node) && node.is_dir && node.mode == 0500
          && node.uid == 1000, "fd is its owner's alone to look through");
    CHECK(proc_lookup("/7/fd/1", false, &node) && node.is_symlink
          && node.mode == 0300 && proc_lookup("/7/fd/2", false, &node)
          && node.mode == 0700, "a descriptor is a link, as open as it is");
    CHECK(!proc_lookup("/7/fd/3", false, &node)
          && !proc_lookup("/7/fd/00", false, &node),
          "one that is not open is nothing");

    CHECK(strcmp(link_to("/7/fd/0"), "/dev/tty1") == 0,
          "a terminal the kernel opened leads to its name in /dev");
    CHECK(strcmp(link_to("/7/fd/1"), "pipe:[77]") == 0
          && strcmp(link_to("/7/fd/5"), "socket:[1048577]") == 0,
          "a pipe and a socket to what they are called");
    CHECK(strcmp(link_to("/7/fd/2"), "/home/notes") == 0
          && strcmp(link_to("/7/cwd"), "/home") == 0
          && strcmp(link_to("/7/root"), "/") == 0
          && strcmp(link_to("/self/exe"), "/bin/worker") == 0,
          "and a file, where it stands, its root and its program, to names");
    s_i_my_uid = 1001;
    CHECK(strcmp(link_to("/7/cwd"), "EACCES") == 0
          && strcmp(link_to("/7/fd/2"), "EACCES") == 0
          && strcmp(link_to("/self"), "7") == 0,
          "somebody else may not follow them");
    CHECK(proc_expand("/7/cwd/notes", false, arr_to, &sz_used) == -EACCES,
          "nor through them");
    s_i_my_uid = 0;
    CHECK(strcmp(link_to("/7/cwd"), "/home") == 0, "and root may");
    s_i_my_uid = 1000;
    CHECK(strcmp(link_to("/7/status"), "ERROR") == 0, "a file is no link");

    CHECK(proc_lookup("/7/fd/1", true, &node) && !node.is_symlink
          && node.u_type == S_IFIFO && node.u_ino == 77 && node.mode == 0600,
          "followed, a pipe's is the pipe");
    struct file *p_of = proc_file_of(&node);
    CHECK(p_of == &s_arr_fds[1], "the very file the descriptor is");
    if (p_of != NULL) {
        file_put(p_of);
    }
    CHECK(proc_open(&node.proc, FILE_READ) == &s_new_end
          && s_p_reopened == &s_arr_fds[1] && s_u_reopened_mode == FILE_READ,
          "and opening it opens another end of that pipe, either way");
    CHECK(proc_lookup("/7/fd/2", true, &node) && node.is_symlink,
          "a name's is left for the namespace to follow");

    CHECK(proc_expand("/7/cwd/notes", false, arr_to, &sz_used) == 1
          && strcmp(arr_to, "/home") == 0 && sz_used == 6,
          "a link out of /proc on the way is where it leads");
    CHECK(proc_expand("/7/cwd", false, arr_to, &sz_used) == 0
          && proc_expand("/7/cwd", true, arr_to, &sz_used) == 1
          && sz_used == 6, "and the last one only when asked");
    CHECK(proc_expand("/self/fd/2", true, arr_to, &sz_used) == 1
          && strcmp(arr_to, "/home/notes") == 0 && sz_used == 10,
          "a descriptor's, by whichever name it is reached");
    CHECK(proc_expand("/7/fd/1/x", false, arr_to, &sz_used) == 0
          && proc_expand("/7/status", true, arr_to, &sz_used) == 0
          && proc_expand("/self/task/9", true, arr_to, &sz_used) == 0,
          "and a pipe's, a file or a directory is no way out");

    CHECK(read_node("/7/fdinfo/2", arr_text) > 0
          && strcmp(arr_text, "pos:\t12\nflags:\t0102002\nmnt_id:\t0\n"
                              "ino:\t4321\n") == 0,
          "fdinfo is where it is and the flags it was opened with");
    CHECK(read_node("/7/fdinfo/5", arr_text) > 0
          && strncmp(arr_text, "pos:\t0\nflags:\t02000002\n", 23) == 0,
          "close on exec among them");
    struct vfs_file other;
    CHECK(proc_lookup("/self/fd/1", false, &node)
          && proc_lookup("/7/fd/1", false, &other) && node.u_ino == other.u_ino
          && proc_lookup("/7/fdinfo/1", false, &other)
          && node.u_ino != other.u_ino,
          "a descriptor's link is numbered by its name too");
}

static void check_files(void)
{
    char arr_text[8192];
    char arr_want[1024];

    /*
     * stat, in its order: pid, comm, state, ppid, pgrp, session, tty_nr,
     * tpgid, flags, minflt, cminflt, majflt, cmajflt, utime, stime,
     * cutime, cstime, priority, nice, num_threads, itrealvalue,
     * starttime, vsize, rss, rsslim, startcode, endcode, startstack,
     * kstkesp, kstkeip, signal, blocked, sigignore, sigcatch, wchan,
     * nswap, cnswap, exit_signal, processor, rt_priority, policy,
     * delayacct_blkio_ticks, guest_time, cguest_time, start_data,
     * end_data, start_brk, arg_start, arg_end, env_start, env_end,
     * exit_code
     */
    snprintf(arr_want, sizeof arr_want,
             "7 (worker) R 1 7 1 1025 7 0 42 0 0 0 250 50 30 20 20 0 2 0 150 "
             "%llu 6 18446744073709551615 4194304 4198400 %llu 0 0 "
             "512 2 4096 16384 0 0 0 17 1 0 0 0 0 0 4198400 4202496 4202496 "
             "%llu %llu %llu %llu 0\n",
             300ull * 4096, STACK_AT, ARG_START, ENV_START, ENV_START,
             ENV_END);
    int64_t i_len = read_node("/7/stat", arr_text);
    CHECK(i_len > 0 && strcmp(arr_text, arr_want) == 0,
          "stat is a process's, field for field");
    if (i_len > 0 && strcmp(arr_text, arr_want) != 0) {
        printf("  got:  %s  want: %s", arr_text, arr_want);
    }
    CHECK(fields_in(arr_text) == 52, "fifty-two of them");
    i_len = read_node("/7/task/9/stat", arr_text);
    CHECK(i_len > 0 && strncmp(arr_text, "9 (helper) S 1 7 1 1025 7 0 0 0 0 0 "
                                         "50 10 30 20 ", 48) == 0
          && strstr(arr_text, " -1 0 0 0 0 0 0 ") != NULL,
          "a thread's is its own: its number, name, state and time, and no "
          "exit signal");
    i_len = read_node("/1/stat", arr_text);
    CHECK(i_len > 0 && strncmp(arr_text, "1 (init) Z 0 1 1 1025 7 ", 24) == 0
          && strcmp(arr_text + i_len - 5, " 256\n") == 0,
          "an ended process is a zombie, with the status it ended with");

    i_len = read_node("/7/statm", arr_text);
    CHECK(i_len > 0 && strcmp(arr_text, "300 6 2 1 0 259 0\n") == 0,
          "statm is the space in pages");

    i_len = read_node("/7/status", arr_text);
    CHECK(i_len > 0 && strncmp(arr_text, "Name:\tworker\nUmask:\t0022\n"
                                         "State:\tR (running)\nTgid:\t7\n"
                                         "Ngid:\t0\nPid:\t7\nPPid:\t1\n", 75)
                       == 0,
          "status names it, and says what it is doing and whose it is");
    CHECK(strstr(arr_text, "\nUid:\t1000\t1000\t1000\t1000\n"
                           "Gid:\t100\t100\t100\t100\nFDSize:\t64\n"
                           "Groups:\t100 27\n") != NULL,
          "who it runs as, four times over, and its groups");
    CHECK(strstr(arr_text, "\nVmSize:\t    1200 kB\n") != NULL
          && strstr(arr_text, "\nVmRSS:\t      24 kB\nRssAnon:\t      16 kB\n"
                              "RssFile:\t       8 kB\n") != NULL
          && strstr(arr_text, "\nVmStk:\t    1024 kB\nVmExe:\t       4 kB\n")
             != NULL && strstr(arr_text, "\nVmPTE:\t      16 kB\n") != NULL,
          "its memory in kilobytes, eight wide");
    CHECK(strstr(arr_text, "\nThreads:\t2\nSigQ:\t2/18446744073709551615\n"
                           "SigPnd:\t0000000000000200\n"
                           "ShdPnd:\t0000000000010000\n"
                           "SigBlk:\t0000000000000002\n"
                           "SigIgn:\t0000000000001000\n"
                           "SigCgt:\t0000000000004000\n") != NULL,
          "its threads, and its signals as sets");
    CHECK(strstr(arr_text, "\nCapEff:\t0000000000000000\n") != NULL
          && strstr(arr_text, "\nCpus_allowed:\t3\nCpus_allowed_list:\t0-1\n")
             != NULL,
          "no capabilities for a guest, and either core");
    s_u_cores = 0x2d;
    i_len = read_node("/7/status", arr_text);
    CHECK(strstr(arr_text, "\nCpus_allowed:\t2d\n"
                           "Cpus_allowed_list:\t0,2-3,5\n") != NULL,
          "and when a core between has not joined, the ones that have");
    s_u_cores = 3;
    i_len = read_node("/7/status", arr_text);
    const char *str_last = "voluntary_ctxt_switches:\t5\n"
                           "nonvoluntary_ctxt_switches:\t3\n";
    CHECK(i_len > (int64_t)strlen(str_last)
          && strcmp(arr_text + i_len - strlen(str_last), str_last) == 0,
          "and how often it gave its core up, last");
    i_len = read_node("/7/task/9/status", arr_text);
    CHECK(i_len > 0 && strncmp(arr_text, "Name:\thelper\n", 13) == 0
          && strstr(arr_text, "\nState:\tS (sleeping)\nTgid:\t7\nNgid:\t0\n"
                              "Pid:\t9\n") != NULL,
          "a thread's status is the thread's");
    i_len = read_node("/1/status", arr_text);
    CHECK(i_len > 0 && strstr(arr_text, "State:\tZ (zombie)\n") != NULL
          && strstr(arr_text, "VmSize") == NULL,
          "and a zombie's has no memory");

    CHECK(read_node("/7/comm", arr_text) == 7
          && strcmp(arr_text, "worker\n") == 0
          && read_node("/7/task/9/comm", arr_text) == 7
          && strcmp(arr_text, "helper\n") == 0,
          "comm is the name, a thread's its own");
    CHECK(read_node("/7/cmdline", arr_text) == 10
          && memcmp(arr_text, "worker\0-v\0", 10) == 0,
          "cmdline is the arguments, each ended by a NUL");
    CHECK(read_node("/7/environ", arr_text) == 7
          && memcmp(arr_text, "HOME=/\0", 7) == 0, "environ the environment");
    CHECK(read_node("/1/cmdline", arr_text) == 0,
          "and an ended process has neither");

    i_len = read_node("/7/maps", arr_text);
    snprintf(arr_want, sizeof arr_want,
             "00400000-00401000 r-xp 00000000 00:10 1234 %30s/bin/worker\n"
             "00401000-00402000 rw-p 00001000 00:10 1234 %30s/bin/worker\n"
             "00402000-00404000 rw-p 00000000 00:00 0 %33s[heap]\n"
             "6ffffff00000-700000000000 rw-p 00000000 00:00 0 %25s[stack]\n",
             "", "", "", "");
    CHECK(i_len > 0 && strcmp(arr_text, arr_want) == 0,
          "maps is the regions, and what is in each, from column 73");
    if (i_len > 0 && strcmp(arr_text, arr_want) != 0) {
        printf("  got:\n%s  want:\n%s", arr_text, arr_want);
    }

    i_len = read_node("/7/limits", arr_text);
    CHECK(i_len > 0 && strncmp(arr_text,
          "Limit                     Soft Limit           Hard Limit"
          "           Units     \n"
          "Max cpu time              unlimited            unlimited"
          "            seconds   \n", 158) == 0,
          "limits is a table, a line a limit");
    CHECK(strstr(arr_text, "\nMax stack size            1048576"
                           "              unlimited            bytes     \n")
          != NULL
          && strstr(arr_text, "\nMax core file size        0"
                              "                    unlimited            bytes"
                              "     \n") != NULL
          && strstr(arr_text, "\nMax open files            64"
                              "                   64                   files"
                              "     \n") != NULL
          && strstr(arr_text, "\nMax nice priority         unlimited"
                              "            unlimited            \n") != NULL,
          "each soft and hard, unlimited or a number, and what it counts");

    i_len = read_node("/7/mounts", arr_text);
    CHECK(i_len > 0 && strcmp(arr_text, "ramfs / ramfs rw 0 0\n"
                                        "proc /proc proc ro 0 0\n"
                                        "/dev/sda1 /mnt/a\\040disk ext4 rw"
                                        " 0 0\n") == 0,
          "mounts is the table, a space in a name written as its escape");
    CHECK(read_node("/7/stack", arr_text) > 0
          && strcmp(arr_text, "call trace of 70\n") == 0,
          "and stack the thread's calls");
}

int main(void)
{
    char arr_text[256];
    size_t sz_index = 99;

    kprintf_capture_source(capturing);
    proc_init();

    CHECK(proc_find("version", &sz_index) && sz_index == 0,
          "version is the first file /proc has");
    CHECK(proc_find("uptime", &sz_index) && sz_index == 1,
          "and uptime the second");
    CHECK(proc_register("greeting", PROC_PUBLIC, show_greeting),
          "a file registers");
    CHECK(!proc_register("greeting", PROC_PUBLIC, show_nothing),
          "but not twice under one name");
    CHECK(!proc_register("a name far too long for any proc file",
                         PROC_PUBLIC, show_nothing),
          "nor under a name that does not fit");
    CHECK(proc_register("empty", PROC_PRIVATE, show_nothing),
          "an empty one is fine, and so is root's own");
    CHECK(!proc_find("nothing", &sz_index), "a name nobody registered");

    CHECK(proc_find("cpuinfo", &sz_index) && sz_index == 2,
          "and cpuinfo, which the architecture writes, the third");

    /* the ones registered here come after the ones /proc has of its own */
    size_t sz_greeting = 0;
    size_t sz_empty = 0;
    proc_find("greeting", &sz_greeting);
    proc_find("empty", &sz_empty);
    const size_t sz_none = PROC_FILES_MAX + 1;
    CHECK(strcmp(proc_name(sz_greeting), "greeting") == 0,
          "listed by its name");
    CHECK(proc_name(sz_empty + 1) == NULL, "and the listing ends");
    CHECK(proc_mode(sz_greeting) == 0444 && proc_mode(sz_empty) == 0400,
          "each keeps the mode it was registered with");
    CHECK(proc_mode(sz_none) == 0, "and one that does not exist has none");

    int64_t i_len = proc_render(sz_greeting, arr_text, sizeof arr_text);
    CHECK(i_len == 31 && memcmp(arr_text, "hello from the kernel, 1 lines\n",
                                31) == 0, "its text is what it printed");
    CHECK(s_p_capture == NULL, "and printing goes back to normal after");
    CHECK(proc_render(sz_empty, arr_text, sizeof arr_text) == 0,
          "a file that says nothing is empty");
    CHECK(proc_render(sz_none, arr_text, sizeof arr_text) == -1,
          "a file that does not exist cannot be written");

    i_len = proc_render(0, arr_text, sizeof arr_text);
    CHECK(i_len > 15 && memcmp(arr_text, "velvet version ", 15) == 0,
          "version says which kernel");
    arr_text[i_len] = '\0';
    CHECK(strstr(arr_text, " (gcc ") != NULL && arr_text[i_len - 1] == '\n',
          "and which compiler made it, on one line");

    /* read in pieces, it is the same file */
    char arr_whole[128];
    struct file *p_file = open_file(sz_greeting);
    CHECK(p_file != NULL && p_file->u_refs == 1 && p_file->p_ops->seek
          && p_file->p_ops->write == NULL, "a file opens, for reading");
    CHECK(read_rest(p_file, arr_whole, 5) == 31
          && strcmp(arr_whole, "hello from the kernel, 1 lines\n") == 0,
          "five bytes at a time adds up");
    CHECK(p_file->p_ops->read(p_file, arr_whole, 5) == 0,
          "and the end is a read of nothing");
    file_put(p_file);
    CHECK(open_file(sz_none) == NULL, "no such file, no open");

    /* one open is one moment, and a read from the start is the next */
    size_t sz_count = 0;
    CHECK(proc_register("count", PROC_PUBLIC, show_count)
          && proc_find("count", &sz_count),
          "a file that changes registers");
    p_file = open_file(sz_count);
    CHECK(read_rest(p_file, arr_whole, 7) == 42
          && strcmp(arr_whole, "asked 1 times, and this is the line after\n")
             == 0 && s_i_asked == 1,
          "read seven bytes at a time, it was asked once");
    CHECK(p_file->p_ops->seek(p_file, 6, SEEK_SET) == 6
          && read_rest(p_file, arr_whole, 64) == 36
          && strncmp(arr_whole, "1 times", 7) == 0 && s_i_asked == 1,
          "a read from further on reads what was written then");
    CHECK(p_file->p_ops->seek(p_file, -3, SEEK_CUR) == 39
          && read_rest(p_file, arr_whole, 64) == 3
          && strcmp(arr_whole, "er\n") == 0,
          "and from where it is");
    CHECK(p_file->p_ops->seek(p_file, 0, SEEK_END) == -EINVAL
          && p_file->p_ops->seek(p_file, -50, SEEK_CUR) == -EINVAL
          && p_file->p_ops->seek(p_file, -1, SEEK_SET) == -EINVAL,
          "but not from the end, nor to before the start");
    CHECK(p_file->p_ops->seek(p_file, 0, SEEK_SET) == 0
          && read_rest(p_file, arr_whole, 64) == 42
          && strncmp(arr_whole, "asked 2 times", 13) == 0 && s_i_asked == 2,
          "a read from the start asks again");
    struct file *p_other = open_file(sz_count);
    CHECK(p_other->p_ops->seek(p_other, 10, SEEK_SET) == 10
          && read_rest(p_other, arr_whole, 64) == 32 && s_i_asked == 3,
          "an open first read further on is asked then");
    CHECK(p_file->p_ops->seek(p_file, 4, SEEK_SET) == 4
          && read_rest(p_file, arr_whole, 64) == 38
          && strncmp(arr_whole, "d 2 ", 4) == 0 && s_i_asked == 3,
          "and leaves the other open's moment alone");
    file_put(p_other);
    file_put(p_file);

    p_file = open_file(sz_empty);
    CHECK(read_rest(p_file, arr_whole, 64) == 0,
          "a file that says nothing reads as nothing");
    file_put(p_file);

    check_tree();
    check_files();
    check_links();

    /* what the machine says of itself, a line or a few */
    char arr_machine[1024];
    CHECK(proc_find("uptime", &sz_index)
          && proc_render(sz_index, arr_machine, sizeof arr_machine) == 13
          && memcmp(arr_machine, "61.23 119.96\n", 13) == 0,
          "uptime is seconds up and every core's seconds idle");
    CHECK(proc_find("loadavg", &sz_index)
          && (i_len = proc_render(sz_index, arr_machine,
                                  sizeof arr_machine)) > 0
          && memcmp(arr_machine, "1.50 0.02 0.00 2/12 77\n", 23) == 0
          && i_len == 23,
          "loadavg is the three averages, the threads and the last pid");
    CHECK(proc_find("stat", &sz_index)
          && (i_len = proc_render(sz_index, arr_machine,
                                  sizeof arr_machine)) > 0,
          "stat writes");
    arr_machine[(i_len > 0) ? i_len : 0] = '\0';
    CHECK(strcmp(arr_machine,
                 "cpu  200 0 50 11996 0 0 0 0 0 0\n"
                 "cpu0 120 0 30 5973 0 0 0 0 0 0\n"
                 "cpu1 80 0 20 6023 0 0 0 0 0 0\n"
                 "intr 0\nctxt 3345\nbtime 1800000000\nprocesses 314\n"
                 "procs_running 2\nprocs_blocked 0\n"
                 "softirq 0 0 0 0 0 0 0 0 0 0 0\n") == 0,
          "and is the cores' time, all and each, and the machine's counts");

    if (s_i_failures == 0) {
        printf("all good\n");
    }
    return s_i_failures;
}
