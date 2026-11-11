// SPDX-License-Identifier: GPL-2.0-only
/*
 * fs/proc_node.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * what /proc's nodes are, between the two files that make it up.
 */

#ifndef FS_PROC_NODE_H
#define FS_PROC_NODE_H

#include <velvet/proc.h>

/*
 * the kinds of node, and what each uses of a proc_ref. fs/proc.c walks
 * names to them and fs/proc_pid.c writes what is in a process's, which
 * is every kind from PROC_KIND_PROCESS on
 */
enum proc_kind {
    PROC_KIND_ROOT,             /* /proc itself */
    PROC_KIND_FILE,             /* sz_index of the files at the top */
    PROC_KIND_SELF,             /* the link to the reader's process */
    PROC_KIND_THREAD_SELF,      /* and to the reader's thread */
    PROC_KIND_PROCESS,          /* /proc/<i_tid>: process i_pid */
    PROC_KIND_TASKS,            /* its task directory */
    PROC_KIND_THREAD,           /* task/<i_tid> in that */
    PROC_KIND_ENTRY,            /* sz_index of what either has in it */
};

/* what a process's directory, and a thread's, have in them */
struct proc_entry {
    const char *str_name;
    uint32_t u_mode;            /* the type with the permissions */
    bool b_root;                /* root's, whoever's process it is */
    bool b_process;             /* in a process's, and not in a thread's */
    uint32_t u_kind;            /* where a directory leads */
    void (*p_show)(const struct proc_ref *p_ref);
};

/* entry sz_index, or NULL past the last */
const struct proc_entry *proc_entry_at(size_t sz_index);

/*
 * which entry str_name is in a process's directory, or in a thread's
 * when b_thread: false for none there
 */
bool proc_entry_find(const char *str_name, bool b_thread, size_t *p_index);

/*
 * who the files of process i_pid belong to, which is who it runs as:
 * false for a process that is not there any more
 */
bool proc_owner(int i_pid, uint32_t *p_uid, uint32_t *p_gid);

/* a string made a piece at a time in sz_size of str_buf, cut to fit */
struct proc_text {
    char  *str_buf;
    size_t sz_size;
    size_t sz_len;
};

void proc_text_add(struct proc_text *p_text, const char *str_more);
void proc_text_number(struct proc_text *p_text, uint64_t u_number);

#endif
