// SPDX-License-Identifier: GPL-2.0-only
/*
 * include/velvet/pty.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * ptys: a terminal with a program at each end.
 */

#ifndef VELVET_PTY_H
#define VELVET_PTY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* how many there may be at once */
#define PTY_MAX 64

/*
 * how they are numbered as devices: /dev/ptmx is 5, 2, and opening it
 * makes a pty and is its master; the slave of pty N is /dev/pts/N, 136, N
 */
#define PTMX_RDEV  ((5u << 8) | 2u)
#define PTY_MAJOR  136

struct file;

/* a new pty, opened for u_mode as its master: or NULL, with none free */
struct file *pty_open_master(uint32_t u_mode);

/*
 * the slave of pty u_minor opened for u_mode: or NULL when there is no
 * such pty, or its master is closed
 */
struct file *pty_open_slave(uint32_t u_minor, uint32_t u_mode);

/* a name in /dev/pts, as a directory has it */
struct pty_node {
    char arr_name[8];
    uint32_t u_mode;            /* the permissions */
    uint32_t u_uid;
    uint32_t u_gid;
    uint32_t u_rdev;
};

/* the one called str_name: true, or false for none */
bool pty_node_named(const char *str_name, struct pty_node *p_out);

/* the sz_index'th, ptmx first and then each pty whose master is open */
bool pty_node_at(size_t sz_index, struct pty_node *p_out);

/*
 * what a mount of /dev/pts was given, in str_options: uid=, gid=, mode=
 * and ptmxmode=, which say whose and how open a slave and ptmx are, and
 * newinstance, which changes nothing, there being one /dev/pts. 0 and,
 * with b_apply, the settings taken from then on, or -EINVAL
 */
int64_t pty_options(const char *str_options, bool b_apply);

#endif
