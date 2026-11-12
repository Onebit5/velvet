// SPDX-License-Identifier: GPL-2.0-only
/*
 * fs/proc.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * /proc: files the kernel writes at the moment they are read.
 */

#include "proc_node.h"

#include <asm/cpu.h>
#include <asm/irq.h>
#include <velvet/epoch.h>
#include <velvet/file.h>
#include <velvet/kmalloc.h>
#include <velvet/kprintf.h>
#include <velvet/loadavg.h>
#include <velvet/namei.h>
#include <velvet/pipe.h>
#include <velvet/pit.h>
#include <velvet/process.h>
#include <velvet/sched.h>
#include <velvet/spinlock.h>
#include <velvet/string.h>
#include <velvet/vfs.h>
#include <uapi/velvet/errno.h>
#include <uapi/velvet/fcntl.h>
#include <uapi/velvet/stat.h>
#include <generated/version.h>

/*
 * a file here is a name and a function. the function writes the file's
 * text with kprintf, the same way the kernel writes anything for a
 * person, and a capture turns that into bytes for whoever reads it. so a
 * subsystem that can describe itself on the screen can describe itself
 * to a program, with the same words, by registering one name.
 *
 * nothing is stored but by an open file. a read from the start writes
 * the file again and the open keeps that text for the reads after it,
 * which is why a file here has no size until it is read, why two opens a
 * second apart may disagree, and why one open read in pieces never
 * does: the pieces are one answer, not the ends of several
 */
struct proc_file {
    char arr_name[PROC_NAME_MAX];
    uint32_t u_mode;
    void (*p_show)(void);
};

static struct proc_file s_arr_files[PROC_FILES_MAX];
static size_t s_sz_count;

bool proc_register(const char *str_name, uint32_t u_mode,
                   void (*p_show)(void))
{
    size_t sz_index;

    if (s_sz_count >= PROC_FILES_MAX || strlen(str_name) >= PROC_NAME_MAX
        || proc_find(str_name, &sz_index)) {
        return false;
    }
    strcpy(s_arr_files[s_sz_count].arr_name, str_name);
    s_arr_files[s_sz_count].u_mode = u_mode;
    s_arr_files[s_sz_count].p_show = p_show;
    s_sz_count++;
    return true;
}

bool proc_find(const char *str_name, size_t *p_index)
{
    for (size_t sz_at = 0; sz_at < s_sz_count; sz_at++) {
        if (strcmp(s_arr_files[sz_at].arr_name, str_name) == 0) {
            *p_index = sz_at;
            return true;
        }
    }
    return false;
}

const char *proc_name(size_t sz_index)
{
    return (sz_index < s_sz_count) ? s_arr_files[sz_index].arr_name : NULL;
}

uint32_t proc_mode(size_t sz_index)
{
    return (sz_index < s_sz_count) ? s_arr_files[sz_index].u_mode : 0;
}

/* whether p_ref is a file there is text for */
static bool is_file(const struct proc_ref *p_ref)
{
    return (p_ref->u_kind == PROC_KIND_FILE && p_ref->sz_index < s_sz_count)
           || (p_ref->u_kind == PROC_KIND_ENTRY
               && proc_entry_at(p_ref->sz_index) != NULL
               && proc_entry_at(p_ref->sz_index)->p_show != NULL)
           || p_ref->u_kind == PROC_KIND_FDINFO;
}

/* the text of a file as it is now, as proc_render tells it */
static int64_t render(const struct proc_ref *p_ref, char *str_buf,
                      size_t sz_size)
{
    if (!is_file(p_ref)) {
        return -1;
    }
    struct kcapture cap = { str_buf, sz_size, 0, false };

    sched_capture(&cap);
    if (p_ref->u_kind == PROC_KIND_FILE) {
        s_arr_files[p_ref->sz_index].p_show();
    } else if (p_ref->u_kind == PROC_KIND_FDINFO) {
        proc_show_fdinfo(p_ref);
    } else {
        proc_entry_at(p_ref->sz_index)->p_show(p_ref);
    }
    sched_capture(NULL);
    return (int64_t)cap.sz_len;
}

int64_t proc_render(size_t sz_index, char *str_buf, size_t sz_size)
{
    struct proc_ref ref = { .u_kind = PROC_KIND_FILE, .sz_index = sz_index };
    return render(&ref, str_buf, sz_size);
}

void proc_text_add(struct proc_text *p_text, const char *str_more)
{
    for (const char *p_at = str_more; *p_at != '\0'; p_at++) {
        if (p_text->sz_len + 1 < p_text->sz_size) {
            p_text->str_buf[p_text->sz_len++] = *p_at;
        }
    }
    if (p_text->sz_size > 0) {
        p_text->str_buf[p_text->sz_len] = '\0';
    }
}

void proc_text_number(struct proc_text *p_text, uint64_t u_number)
{
    char arr_digits[24];
    size_t sz_at = sizeof arr_digits - 1;
    arr_digits[sz_at] = '\0';
    do {
        arr_digits[--sz_at] = (char)('0' + u_number % 10);
        u_number /= 10;
    } while (u_number > 0);
    proc_text_add(p_text, &arr_digits[sz_at]);
}

/*
 * the node's name as the namespace has it, with self and thread-self
 * already followed, into sz_size of str_out: what its number is made of
 */
static void canonical(const struct proc_ref *p_ref, char *str_out,
                      size_t sz_size)
{
    struct proc_text text = { str_out, sz_size, 0 };
    bool b_thread = p_ref->b_thread;
    proc_text_add(&text, PROC_AT);
    switch (p_ref->u_kind) {
    case PROC_KIND_FILE:
        proc_text_add(&text, "/");
        proc_text_add(&text, proc_name(p_ref->sz_index));
        return;
    case PROC_KIND_SELF:
        proc_text_add(&text, "/self");
        return;
    case PROC_KIND_THREAD_SELF:
        proc_text_add(&text, "/thread-self");
        return;
    case PROC_KIND_ROOT:
        return;
    default:
        break;
    }
    proc_text_add(&text, "/");
    proc_text_number(&text, (uint64_t)(b_thread ? p_ref->i_pid
                                                : p_ref->i_tid));
    if (p_ref->u_kind == PROC_KIND_TASKS || b_thread) {
        proc_text_add(&text, "/task");
    }
    if (b_thread) {
        proc_text_add(&text, "/");
        proc_text_number(&text, (uint64_t)p_ref->i_tid);
    }
    switch (p_ref->u_kind) {
    case PROC_KIND_ENTRY:
        proc_text_add(&text, "/");
        proc_text_add(&text, proc_entry_at(p_ref->sz_index)->str_name);
        break;
    case PROC_KIND_FDS:
    case PROC_KIND_FDINFOS:
        proc_text_add(&text, (p_ref->u_kind == PROC_KIND_FDS) ? "/fd"
                                                             : "/fdinfo");
        break;
    case PROC_KIND_FD:
    case PROC_KIND_FD_FILE:
    case PROC_KIND_FDINFO:
        proc_text_add(&text, (p_ref->u_kind == PROC_KIND_FDINFO) ? "/fdinfo/"
                                                                : "/fd/");
        proc_text_number(&text, p_ref->sz_index);
        break;
    default:
        break;
    }
}

/*
 * a number for a node, the same every time it is asked: the hash a path
 * with no inode of its own is numbered by, of the node's canonical name
 */
static uint32_t ino_of(const struct proc_ref *p_ref)
{
    char arr_name[PATH_MAX];
    canonical(p_ref, arr_name, sizeof arr_name);
    uint64_t u_hash = 1469598103934665603ull;
    for (const char *p_at = arr_name; *p_at != '\0'; p_at++) {
        u_hash = (u_hash ^ (uint8_t)*p_at) * 1099511628211ull;
    }
    return (uint32_t)((u_hash & 0x7fffffffull) | 0x80000000ull);
}

/*
 * what a descriptor's node is: fdinfo's file, the link, readable or
 * writable as the descriptor is, or what the link leads to, followed,
 * which is the descriptor's own file and numbered as fstat numbers it
 */
static uint32_t descriptor_mode(const struct proc_ref *p_ref,
                                struct vfs_file *p_out)
{
    if (p_ref->u_kind == PROC_KIND_FDINFO) {
        return S_IFREG | 0400;
    }
    struct file *p_file = process_fd_file(p_ref->i_pid, (int)p_ref->sz_index);
    if (p_file == NULL) {
        return S_IFLNK;
    }
    uint32_t u_mode = ((p_file->u_mode & FILE_READ) ? 0500 : 0)
                      | ((p_file->u_mode & FILE_WRITE) ? 0300 : 0);
    if (p_ref->u_kind == PROC_KIND_FD_FILE) {
        p_out->u_type = (p_file->e_type == FILE_TYPE_SOCKET) ? S_IFSOCK
                                                             : S_IFIFO;
        p_out->u_ino = (uint32_t)file_ino(p_file);
        u_mode = p_out->u_type | 0600;
    } else {
        u_mode |= S_IFLNK;
    }
    file_put(p_file);
    return u_mode;
}

/* the node p_ref names, as the namespace is told it */
static void fill(const struct proc_ref *p_ref, struct vfs_file *p_out)
{
    memset(p_out, 0, sizeof *p_out);
    p_out->kind = VFS_PROC;
    p_out->proc = *p_ref;
    p_out->u_ino = ino_of(p_ref);
    struct proc_text name = { p_out->name, sizeof p_out->name, 0 };
    const struct proc_entry *p_entry = proc_entry_at(p_ref->sz_index);
    uint32_t u_mode = S_IFDIR | 0555;
    switch (p_ref->u_kind) {
    case PROC_KIND_ROOT:
        proc_text_add(&name, "proc");
        break;
    case PROC_KIND_FILE:
        /*
         * none has a size until it is read, so it says 0 and a reader
         * reads to the end like any other file. who may read it is the
         * file's own say
         */
        proc_text_add(&name, proc_name(p_ref->sz_index));
        u_mode = S_IFREG | proc_mode(p_ref->sz_index);
        break;
    case PROC_KIND_SELF:
    case PROC_KIND_THREAD_SELF:
        proc_text_add(&name, (p_ref->u_kind == PROC_KIND_SELF)
                             ? "self" : "thread-self");
        u_mode = S_IFLNK | 0777;
        break;
    case PROC_KIND_TASKS:
        proc_text_add(&name, "task");
        break;
    case PROC_KIND_ENTRY:
        proc_text_add(&name, p_entry->str_name);
        u_mode = p_entry->u_mode;
        break;
    case PROC_KIND_FDS:
        proc_text_add(&name, "fd");
        u_mode = S_IFDIR | 0500;
        break;
    case PROC_KIND_FDINFOS:
        proc_text_add(&name, "fdinfo");
        break;
    case PROC_KIND_FD:
    case PROC_KIND_FD_FILE:
    case PROC_KIND_FDINFO:
        proc_text_number(&name, p_ref->sz_index);
        u_mode = descriptor_mode(p_ref, p_out);
        break;
    default:
        proc_text_number(&name, (uint64_t)p_ref->i_tid);
        break;
    }
    p_out->is_dir = (u_mode & S_IFMT) == S_IFDIR;
    p_out->is_symlink = (u_mode & S_IFMT) == S_IFLNK;
    p_out->mode = u_mode & 07777;
    /* a process's are whoever it runs as, but for what is root's alone */
    bool b_process = p_ref->u_kind >= PROC_KIND_PROCESS;
    if (b_process && !(p_ref->u_kind == PROC_KIND_ENTRY && p_entry->b_root)) {
        proc_owner(p_ref->i_pid, &p_out->uid, &p_out->gid);
    }
}

/*
 * a name that is a number, as the number: 0 for one that is not, or that
 * starts with a nought, which no pid is written with
 */
static int number_in(const char *str_part)
{
    int i_number = 0;
    if (str_part[0] == '0') {
        return 0;
    }
    for (const char *p_at = str_part; *p_at != '\0'; p_at++) {
        if (*p_at < '0' || *p_at > '9' || i_number > 99999999) {
            return 0;
        }
        i_number = i_number * 10 + (*p_at - '0');
    }
    return i_number;
}

/* a descriptor's number, which may be 0: -1 for a name that is none */
static int fd_in(const char *str_part)
{
    int i_number = number_in(str_part);
    return (strcmp(str_part, "0") == 0) ? 0 : (i_number != 0) ? i_number
                                                               : -1;
}

/*
 * process i_pid's directory, seen through its thread i_tid, under task/
 * when b_thread
 */
static void see_process(struct proc_ref *p_ref, int i_pid, int i_tid,
                        bool b_thread)
{
    p_ref->u_kind = b_thread ? PROC_KIND_THREAD : PROC_KIND_PROCESS;
    p_ref->i_pid = i_pid;
    p_ref->i_tid = i_tid;
    p_ref->b_thread = b_thread;
}

/* what str_part is at the top: false for nothing */
static bool step_top(struct proc_ref *p_ref, const char *str_part,
                     bool b_follow)
{
    int i_number = number_in(str_part);
    int i_me = current_pid();
    if (proc_find(str_part, &p_ref->sz_index)) {
        p_ref->u_kind = PROC_KIND_FILE;
    } else if (strcmp(str_part, "self") == 0 && !b_follow) {
        p_ref->u_kind = PROC_KIND_SELF;
    } else if (strcmp(str_part, "thread-self") == 0 && !b_follow) {
        p_ref->u_kind = PROC_KIND_THREAD_SELF;
    } else if (strcmp(str_part, "self") == 0 && i_me != 0) {
        see_process(p_ref, i_me, i_me, false);
    } else if (strcmp(str_part, "thread-self") == 0 && i_me != 0) {
        see_process(p_ref, i_me, current_tid(), true);
    } else if (i_number != 0 && process_find(i_number) != NULL) {
        see_process(p_ref, i_number, i_number, false);
    } else if (i_number != 0 && process_pid_of(i_number) != 0) {
        see_process(p_ref, process_pid_of(i_number), i_number, false);
    } else {
        return false;
    }
    return true;
}

/*
 * descriptor i_fd in fd/ or fdinfo/, if it is open. a link in fd/ that
 * leads to a name is followed by the namespace, which looks that name
 * up; one that leads to a file with no name, a pipe's or a socket's, is
 * followed here, to that file
 */
static bool step_fd(struct proc_ref *p_ref, int i_fd, bool b_follow)
{
    char arr_to[PATH_MAX];
    if (i_fd < 0 || process_fd_from(p_ref->i_pid, i_fd) != i_fd) {
        return false;
    }
    bool b_named = proc_fd_target(p_ref->i_pid, i_fd, arr_to);
    p_ref->sz_index = (size_t)i_fd;
    p_ref->u_kind = (p_ref->u_kind == PROC_KIND_FDINFOS) ? PROC_KIND_FDINFO
                  : (b_follow && !b_named) ? PROC_KIND_FD_FILE
                  : PROC_KIND_FD;
    return true;
}

/* p_ref moved to str_part in it, a link there followed when b_follow */
static bool step(struct proc_ref *p_ref, const char *str_part, bool b_follow)
{
    size_t sz_index;
    int i_tid = number_in(str_part);
    switch (p_ref->u_kind) {
    case PROC_KIND_ROOT:
        return step_top(p_ref, str_part, b_follow);
    case PROC_KIND_PROCESS:
    case PROC_KIND_THREAD:
        if (!proc_entry_find(str_part, p_ref->b_thread, &sz_index)) {
            return false;
        }
        p_ref->sz_index = sz_index;
        p_ref->u_kind = (proc_entry_at(sz_index)->u_kind != 0)
                        ? proc_entry_at(sz_index)->u_kind : PROC_KIND_ENTRY;
        return true;
    case PROC_KIND_TASKS:
        if (i_tid == 0 || process_pid_of(i_tid) != p_ref->i_pid) {
            return false;
        }
        see_process(p_ref, p_ref->i_pid, i_tid, true);
        return true;
    case PROC_KIND_FDS:
    case PROC_KIND_FDINFOS:
        return step_fd(p_ref, fd_in(str_part), b_follow);
    default:
        return false;   /* a file, or a link not followed, holds nothing */
    }
}

/*
 * the node str_rest names, every link on the way followed and the last
 * one when b_follow: false for nothing there
 */
static bool walk(const char *str_rest, bool b_follow, struct proc_ref *p_ref)
{
    char arr_part[PROC_NAME_MAX];
    const char *p_at = str_rest;

    memset(p_ref, 0, sizeof *p_ref);
    p_ref->u_kind = PROC_KIND_ROOT;
    while (*p_at == '/') {
        p_at++;
    }
    while (*p_at != '\0') {
        size_t sz_len = 0;
        while (p_at[sz_len] != '\0' && p_at[sz_len] != '/') {
            sz_len++;
        }
        if (sz_len >= sizeof arr_part) {
            return false;
        }
        memcpy(arr_part, p_at, sz_len);
        arr_part[sz_len] = '\0';
        p_at += sz_len;
        while (*p_at == '/') {
            p_at++;
        }
        if (!step(p_ref, arr_part, b_follow || *p_at != '\0')) {
            return false;
        }
    }
    return true;
}

bool proc_lookup(const char *str_rest, bool b_follow, struct vfs_file *p_out)
{
    struct proc_ref ref;
    if (!walk(str_rest, b_follow, &ref)) {
        return false;
    }
    fill(&ref, p_out);
    return true;
}

/*
 * the entry sz_index of a process's or thread's directory, into p_ref,
 * skipping what is not in a thread's: false past the last
 */
static bool nth_entry(struct proc_ref *p_ref, size_t sz_index)
{
    for (size_t sz_at = 0; proc_entry_at(sz_at) != NULL; sz_at++) {
        const struct proc_entry *p_entry = proc_entry_at(sz_at);
        if (p_ref->b_thread && p_entry->b_process) {
            continue;
        }
        if (sz_index-- == 0) {
            p_ref->sz_index = sz_at;
            p_ref->u_kind = (p_entry->u_kind != 0) ? p_entry->u_kind
                                                   : PROC_KIND_ENTRY;
            return true;
        }
    }
    return false;
}

/*
 * what is at the top, in order: the files, self and thread-self, and a
 * directory for each process
 */
static bool nth_top(struct proc_ref *p_ref, size_t sz_index)
{
    int arr_pids[MAX_PROCESSES];
    if (sz_index < s_sz_count) {
        p_ref->u_kind = PROC_KIND_FILE;
        p_ref->sz_index = sz_index;
        return true;
    }
    sz_index -= s_sz_count;
    if (sz_index < 2) {
        p_ref->u_kind = (sz_index == 0) ? PROC_KIND_SELF
                                        : PROC_KIND_THREAD_SELF;
        return true;
    }
    sz_index -= 2;
    size_t sz_pids = process_pids(arr_pids, MAX_PROCESSES);
    if (sz_index >= sz_pids) {
        return false;
    }
    see_process(p_ref, arr_pids[sz_index], arr_pids[sz_index], false);
    return true;
}

/* the descriptor sz_index of those open, as a link or what fdinfo says */
static bool nth_fd(struct proc_ref *p_ref, size_t sz_index)
{
    int i_fd = process_fd_from(p_ref->i_pid, 0);
    for (; i_fd >= 0 && sz_index > 0; sz_index--) {
        i_fd = process_fd_from(p_ref->i_pid, i_fd + 1);
    }
    if (i_fd < 0) {
        return false;
    }
    p_ref->sz_index = (size_t)i_fd;
    p_ref->u_kind = (p_ref->u_kind == PROC_KIND_FDS) ? PROC_KIND_FD
                                                     : PROC_KIND_FDINFO;
    return true;
}

bool proc_readdir(const char *str_rest, size_t sz_index,
                  struct vfs_file *p_out)
{
    struct proc_ref ref;
    int arr_tids[MAX_THREADS];
    bool b_found = false;
    if (!walk(str_rest, true, &ref)) {
        return false;
    }
    switch (ref.u_kind) {
    case PROC_KIND_ROOT:
        b_found = nth_top(&ref, sz_index);
        break;
    case PROC_KIND_PROCESS:
    case PROC_KIND_THREAD:
        b_found = nth_entry(&ref, sz_index);
        break;
    case PROC_KIND_TASKS:
        b_found = sz_index < process_tids(ref.i_pid, arr_tids, MAX_THREADS);
        if (b_found) {
            see_process(&ref, ref.i_pid, arr_tids[sz_index], true);
        }
        break;
    case PROC_KIND_FDS:
    case PROC_KIND_FDINFOS:
        b_found = nth_fd(&ref, sz_index);
        break;
    default:
        break;
    }
    if (b_found) {
        fill(&ref, p_out);
    }
    return b_found;
}

/*
 * where a link that leads out of /proc leads, a name in the namespace,
 * into PATH_MAX of str_out: false for no such link, or a descriptor's
 * file with no name, which is then what str_out calls it
 */
static bool leads_to(const struct proc_ref *p_ref, char *str_out)
{
    const struct proc_entry *p_entry = proc_entry_at(p_ref->sz_index);
    str_out[0] = '\0';
    if (p_ref->u_kind == PROC_KIND_FD) {
        return proc_fd_target(p_ref->i_pid, (int)p_ref->sz_index, str_out);
    }
    return p_ref->u_kind == PROC_KIND_ENTRY && p_entry->p_link != NULL
           && p_entry->p_link(p_ref->i_pid, str_out);
}

/* whether p_ref is a link out of /proc */
static bool is_link_out(const struct proc_ref *p_ref)
{
    return p_ref->u_kind == PROC_KIND_FD
           || (p_ref->u_kind == PROC_KIND_ENTRY
               && proc_entry_at(p_ref->sz_index)->p_link != NULL);
}

int64_t proc_readlink(const char *str_rest, char *str_out, size_t sz_size)
{
    struct proc_ref ref;
    struct proc_text text = { str_out, sz_size, 0 };
    char arr_to[PATH_MAX];
    if (!walk(str_rest, false, &ref) || current_pid() == 0) {
        return -EINVAL;
    }
    /* relative, so a process under a root of its own reads it the same */
    switch (ref.u_kind) {
    case PROC_KIND_SELF:
        proc_text_number(&text, (uint64_t)current_pid());
        return 0;
    case PROC_KIND_THREAD_SELF:
        proc_text_number(&text, (uint64_t)current_pid());
        proc_text_add(&text, "/task/");
        proc_text_number(&text, (uint64_t)current_tid());
        return 0;
    default:
        break;
    }
    if (!is_link_out(&ref)) {
        return -EINVAL;
    }
    if (!proc_may_follow(ref.i_pid)) {
        return -EACCES;
    }
    /* a name as the reader would give it, under its own root */
    char arr_seen[PATH_MAX];
    if (leads_to(&ref, arr_to)) {
        path_seen_from(process_root(current_pid()), arr_to, arr_seen);
        proc_text_add(&text, arr_seen);
    } else if (arr_to[0] != '\0') {
        proc_text_add(&text, arr_to);
    } else {
        return -ENOENT;
    }
    return 0;
}

int64_t proc_expand(const char *str_rest, bool b_last, char *str_out,
                    size_t *p_used)
{
    struct proc_ref ref;
    char arr_part[PROC_NAME_MAX];
    const char *p_at = str_rest;

    memset(&ref, 0, sizeof ref);
    ref.u_kind = PROC_KIND_ROOT;
    while (*p_at == '/') {
        p_at++;
    }
    while (*p_at != '\0') {
        size_t sz_len = 0;
        while (p_at[sz_len] != '\0' && p_at[sz_len] != '/') {
            sz_len++;
        }
        if (sz_len >= sizeof arr_part) {
            return 0;
        }
        memcpy(arr_part, p_at, sz_len);
        arr_part[sz_len] = '\0';
        p_at += sz_len;
        size_t sz_used = (size_t)(p_at - str_rest);
        while (*p_at == '/') {
            p_at++;
        }
        bool b_more = *p_at != '\0';
        /* what is not there is the lookup's to say, after this */
        if (!step(&ref, arr_part, true)) {
            return 0;
        }
        if (is_link_out(&ref) && (b_more || b_last)) {
            if (!proc_may_follow(ref.i_pid)) {
                return -EACCES;
            }
            if (!leads_to(&ref, str_out)) {
                /* a pipe's, followed in /proc, or nothing there */
                return (ref.u_kind == PROC_KIND_FD && str_out[0] != '\0')
                       ? 0 : -ENOENT;
            }
            *p_used = sz_used;
            return 1;
        }
    }
    return 0;
}

/*
 * an open file of /proc: which node, and the text it was last written
 * with, NULL before the first read and for a file that said nothing.
 * the text is swapped under the lock and read under it, so a thread
 * reading from the start while another reads further on hands the
 * second either the old text or the new, never one being freed
 */
struct proc_open {
    struct spinlock lock;
    struct proc_ref ref;
    bool b_written;
    char *p_text;
    size_t sz_len;
};

/*
 * the file's text as it is now, in memory of its own length, into
 * *pp_text: false with no memory to write it in
 */
static bool written(const struct proc_ref *p_ref, char **pp_text,
                    size_t *p_len)
{
    char *str_whole = kmalloc(PROC_TEXT_MAX);
    if (str_whole == NULL) {
        return false;
    }
    int64_t i_len = render(p_ref, str_whole, PROC_TEXT_MAX);
    char *p_text = (i_len > 0) ? kmalloc((size_t)i_len) : NULL;
    bool b_ok = i_len == 0 || p_text != NULL;
    if (p_text != NULL) {
        memcpy(p_text, str_whole, (size_t)i_len);
    }
    kfree(str_whole);
    *pp_text = p_text;
    *p_len = (i_len > 0) ? (size_t)i_len : 0;
    return b_ok;
}

static int64_t open_read(struct file *p_file, void *p_buf, size_t sz_len)
{
    struct proc_open *p_open = p_file->p_private;

    if (p_file->u_pos == 0 || !p_open->b_written) {
        char *p_text;
        size_t sz_text;
        if (!written(&p_open->ref, &p_text, &sz_text)) {
            return -ENOMEM;
        }
        uint64_t u_flags = spin_lock_irq(&p_open->lock);
        char *p_old = p_open->p_text;
        p_open->p_text = p_text;
        p_open->sz_len = sz_text;
        p_open->b_written = true;
        spin_unlock_irq(&p_open->lock, u_flags);
        kfree(p_old);
    }
    uint64_t u_flags = spin_lock_irq(&p_open->lock);
    uint64_t u_left = (p_file->u_pos < p_open->sz_len)
                      ? p_open->sz_len - p_file->u_pos : 0;
    size_t sz_got = (sz_len < u_left) ? sz_len : (size_t)u_left;
    if (sz_got > 0) {
        memcpy(p_buf, p_open->p_text + p_file->u_pos, sz_got);
    }
    spin_unlock_irq(&p_open->lock, u_flags);
    p_file->u_pos += sz_got;
    return (int64_t)sz_got;
}

/*
 * from the start or from where it is. the end is wherever the next
 * read from the start says, so there is none to seek from
 */
static int64_t open_seek(struct file *p_file, int64_t i_offset, int i_whence)
{
    int64_t i_from = (i_whence == SEEK_SET) ? 0
                   : (i_whence == SEEK_CUR) ? (int64_t)p_file->u_pos
                   : -1;
    if (i_from < 0 || i_from + i_offset < 0) {
        return -EINVAL;
    }
    p_file->u_pos = (uint64_t)(i_from + i_offset);
    return (int64_t)p_file->u_pos;
}

static void open_release(struct file *p_file)
{
    struct proc_open *p_open = p_file->p_private;

    kfree(p_open->p_text);
    kfree(p_open);
}

static const struct file_operations s_open_ops = {
    .read = open_read,
    .seek = open_seek,
    .release = open_release,
};

struct file *proc_file_of(const struct vfs_file *p_node)
{
    return (p_node->kind == VFS_PROC
            && p_node->proc.u_kind == PROC_KIND_FD_FILE)
           ? process_fd_file(p_node->proc.i_pid, (int)p_node->proc.sz_index)
           : NULL;
}

/*
 * a descriptor's file with no name opened again, the way a pipe is
 * opened by the link to it: another end of the same pipe, either way. a
 * socket is not opened by a name, and is not by this one
 */
static struct file *reopen(const struct proc_ref *p_ref, uint32_t u_mode)
{
    struct file *p_end = process_fd_file(p_ref->i_pid, (int)p_ref->sz_index);
    struct file *p_file = (p_end != NULL && p_end->e_type == FILE_TYPE_PIPE)
                          ? pipe_reopen(p_end, u_mode) : NULL;
    if (p_end != NULL) {
        file_put(p_end);
    }
    return p_file;
}

struct file *proc_open(const struct proc_ref *p_ref, uint32_t u_mode)
{
    if (p_ref->u_kind == PROC_KIND_FD_FILE) {
        return reopen(p_ref, u_mode);
    }
    if (!is_file(p_ref)) {
        return NULL;
    }
    struct proc_open *p_open = kmalloc(sizeof *p_open);
    struct file *p_file = (p_open != NULL)
                          ? file_alloc(&s_open_ops, u_mode,
                                       FILE_TYPE_REGULAR)
                          : NULL;
    if (p_file == NULL) {
        kfree(p_open);
        return NULL;
    }
    memset(p_open, 0, sizeof *p_open);
    spin_init_unlisted(&p_open->lock, "proc file", LOCK_RANK_DEVICE);
    p_open->ref = *p_ref;
    p_file->p_private = p_open;
    return p_file;
}

/*
 * what is running, the compiler that made it and when: version, then
 * the compiler in brackets, then the date. a program wanting one of
 * them splits the line rather than opening three files
 */
static void show_version(void)
{
    kprintf("velvet version %s (gcc %s) %s %s\n", VELVET_VERSION,
            __VERSION__, __DATE__, __TIME__);
}

/* ticks as the hundredths of a second a clock tick is to a program */
static uint64_t hundredths(uint64_t u_ticks)
{
    return u_ticks * 100 / CLOCK_TICK_HZ;
}

/*
 * how long it has been up, in seconds, to the hundredth, and how long
 * the cores have spent with nothing to do, every core's added up
 */
static void show_uptime(void)
{
    uint64_t u_ms = pit_uptime_ms();
    uint64_t u_idle = 0;
    struct sched_cpu_stat stat;

    for (unsigned u_cpu = 0; u_cpu < CPU_MAX; u_cpu++) {
        u_idle += sched_cpu_stat(u_cpu, &stat) ? stat.u_idle : 0;
    }
    u_idle = hundredths(u_idle);
    kprintf("%lu.%02lu %lu.%02lu\n", u_ms / 1000, (u_ms % 1000) / 10,
            u_idle / 100, u_idle % 100);
}

/* one line of stat's cpus: user, nice, system, idle, and six never here */
static void show_cpu(const struct sched_cpu_stat *p_stat)
{
    kprintf(" %lu 0 %lu %lu 0 0 0 0 0 0\n", hundredths(p_stat->cpu.u_user),
            hundredths(p_stat->cpu.u_system), hundredths(p_stat->u_idle));
}

/*
 * stat: the time every core has spent, all of them and then each, in
 * clock ticks, the interrupts taken, the switches between threads, when
 * the machine came up, how many processes and threads were ever made and
 * how many want a core now
 */
static void show_stat(void)
{
    struct sched_cpu_stat all;
    struct sched_cpu_stat one;
    memset(&all, 0, sizeof all);
    for (unsigned u_cpu = 0; u_cpu < CPU_MAX; u_cpu++) {
        if (sched_cpu_stat(u_cpu, &one)) {
            cpu_usage_add(&all.cpu, &one.cpu);
            all.u_idle += one.u_idle;
            all.u_switches += one.u_switches;
        }
    }
    kprintf("cpu ");
    show_cpu(&all);
    for (unsigned u_cpu = 0; u_cpu < CPU_MAX; u_cpu++) {
        if (sched_cpu_stat(u_cpu, &one)) {
            kprintf("cpu%u", u_cpu);
            show_cpu(&one);
        }
    }
    uint64_t u_up = pit_uptime_ms() / 1000;
    uint64_t u_booted = epoch_known() ? epoch_now() - u_up : 0;
    kprintf("intr %lu\nctxt %lu\nbtime %lu\nprocesses %lu\n", irq_count(),
            all.u_switches, u_booted, process_made());
    kprintf("procs_running %lu\nprocs_blocked 0\n",
            (uint64_t)sched_runnable(NULL));
    kprintf("softirq 0 0 0 0 0 0 0 0 0 0 0\n");
}

/*
 * loadavg: the three averages, the threads that want a core now of all
 * there are, and the pid handed out last
 */
static void show_loadavg(void)
{
    uint64_t arr_load[LOAD_SPANS];
    uint64_t arr_whole[LOAD_SPANS];
    uint64_t arr_hundredths[LOAD_SPANS];
    size_t sz_threads = 0;

    sched_load(arr_load);
    for (size_t sz_at = 0; sz_at < LOAD_SPANS; sz_at++) {
        load_digits(arr_load[sz_at], &arr_whole[sz_at], &arr_hundredths[sz_at]);
    }
    size_t sz_runnable = sched_runnable(&sz_threads);
    kprintf("%lu.%02lu %lu.%02lu %lu.%02lu %lu/%lu %d\n", arr_whole[0],
            arr_hundredths[0], arr_whole[1], arr_hundredths[1], arr_whole[2],
            arr_hundredths[2], (uint64_t)sz_runnable, (uint64_t)sz_threads,
            process_last_id());
}

void proc_init(void)
{
    proc_register("version", PROC_PUBLIC, show_version);
    proc_register("uptime", PROC_PUBLIC, show_uptime);
    proc_register("cpuinfo", PROC_PUBLIC, cpu_show);
    proc_register("stat", PROC_PUBLIC, show_stat);
    proc_register("loadavg", PROC_PUBLIC, show_loadavg);
}
