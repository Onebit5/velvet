// SPDX-License-Identifier: GPL-2.0-only
/*
 * include/velvet/proc.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * /proc: files the kernel writes at the moment they are read.
 */

#ifndef VELVET_PROC_H
#define VELVET_PROC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PROC_AT        "/proc"
#define PROC_FILES_MAX 32
#define PROC_NAME_MAX  24

/* the most text one file may hold. the kernel's log is the largest */
#define PROC_TEXT_MAX  (40 * 1024)

/* who may read one: everybody, or root alone */
#define PROC_PUBLIC  0444
#define PROC_PRIVATE 0400

/*
 * which node of /proc a name led to: one of the files at the top, a
 * process's directory or one of its threads', or what is in one. which
 * fields mean anything is the kind's, as fs/proc_node.h has them
 */
struct proc_ref {
    uint32_t u_kind;
    int      i_pid;
    int      i_tid;             /* the thread the process is seen through */
    bool     b_thread;          /* under task/, rather than the process's */
    size_t   sz_index;          /* which file, of the top's or a process's */
};

/*
 * a file under /proc, written by `p_show` with kprintf whenever somebody
 * reads it, and readable by whoever `u_mode` says: PROC_PUBLIC, or
 * PROC_PRIVATE for what would tell a program where the kernel keeps
 * things. false when the name is taken or there is no room for another
 */
bool proc_register(const char *str_name, uint32_t u_mode,
                   void (*p_show)(void));

/* the mode a file was registered with, 0 for a file that does not exist. */
uint32_t proc_mode(size_t sz_index);

/* which registered file a name is, for an open. */
bool proc_find(const char *str_name, size_t *p_index);

/* the name of the file at `sz_index`, or NULL past the last, for a listing. */
const char *proc_name(size_t sz_index);

/*
 * the file's text as it is right now, written into `str_buf`. the length
 * written, or -1 for a file that does not exist. text past `sz_size` is
 * lost, which only a file that outgrew PROC_TEXT_MAX would notice
 */
int64_t proc_render(size_t sz_index, char *str_buf, size_t sz_size);

struct file;
struct vfs_file;

/*
 * what str_rest, a path inside /proc, names, following a link at the end
 * when b_follow: false for nothing there. self is the reader's process's
 * directory and thread-self its thread's, a number is a process's, and a
 * thread's number names its process too, seen through that thread, the
 * way it always has, though only the processes are listed
 */
bool proc_lookup(const char *str_rest, bool b_follow, struct vfs_file *p_out);

/* entry sz_index of the directory str_rest names, false past the last */
bool proc_readdir(const char *str_rest, size_t sz_index,
                  struct vfs_file *p_out);

/*
 * where the link str_rest names leads, as the reader is told it, into
 * sz_size of str_out: 0, -EINVAL for no link there, -EACCES for one into
 * a process the reader may not look into, or -ENOENT for one that leads
 * nowhere now
 */
int64_t proc_readlink(const char *str_rest, char *str_out, size_t sz_size);

/*
 * the first link on the way along str_rest that leads out of /proc,
 * where a process stands, its root, its program or what it has open,
 * the last part's too when b_last: 1 with the name in the namespace it
 * leads to in PATH_MAX of str_out, and in *p_used how much of str_rest
 * it took to reach it, 0 for none on the way, or -EACCES for one the
 * reader may not follow, -ENOENT for one that leads nowhere now
 */
int64_t proc_expand(const char *str_rest, bool b_last, char *str_out,
                    size_t *p_used);

/*
 * the open file a node is when it is a descriptor's link followed to a
 * file with no name, a pipe or a socket, with a reference the caller
 * puts: NULL for any other node
 */
struct file *proc_file_of(const struct vfs_file *p_node);

/*
 * the file p_ref names opened for reading, with one reference: NULL for
 * no such file or no memory. a read from the start writes its text and
 * the reads after it are of that text, so one open read in pieces is one
 * moment's answer; a read from the start again asks again. it seeks from
 * the start or from where it is, and never from the end
 */
struct file *proc_open(const struct proc_ref *p_ref, uint32_t u_mode);

/*
 * the files /proc has of its own: the version, the uptime, and cpuinfo,
 * which the architecture writes
 */
void proc_init(void);

#endif
