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

/*
 * `u_len` bytes of the file from `u_offset`, written afresh for this read:
 * fewer at the end, 0 past it, -1 for no such file or no memory to write
 * it in
 */
int64_t proc_read(size_t sz_index, uint64_t u_offset, void *p_buf,
                  uint64_t u_len);

/* the files /proc has of its own: the version and the uptime. */
void proc_init(void);

#endif
