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

/*
 * the file at `sz_index` opened for reading, with one reference: NULL for
 * no such file or no memory. a read from the start writes its text and
 * the reads after it are of that text, so one open read in pieces is one
 * moment's answer; a read from the start again asks again. it seeks from
 * the start or from where it is, and never from the end
 */
struct file *proc_open(size_t sz_index, uint32_t u_mode);

/*
 * the files /proc has of its own: the version, the uptime, and cpuinfo,
 * which the architecture writes
 */
void proc_init(void);

#endif
