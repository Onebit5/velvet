// SPDX-License-Identifier: GPL-2.0-only
/*
 * include/velvet/shared.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * pages more than one space maps.
 */

/* the design notes for shared.h are in docs/subsystems/mm.rst */

#ifndef VELVET_SHARED_H
#define VELVET_SHARED_H

#include <stdint.h>
#include <stdbool.h>

struct file;
struct vfs_file;

/*
 * the pages of a file mapped shared, or of memory mapped shared with no
 * file behind it: a frame for each page touched, which every space
 * mapping it maps, so a store through one mapping is in all of them at
 * once, and in what a read of the file says. what is stored is carried
 * back to the file by msync, and when the last mapping goes
 */
struct shared_pages;

/*
 * the set of p_file, the one every mapping of that file has, however it
 * was opened, held for the caller. NULL when there is no memory, or for
 * a file with no inode to know it by, which no set can be kept of
 */
struct shared_pages *shared_of_file(struct file *p_file);

/* a set of no file, whose pages start as zeroes, held for the caller */
struct shared_pages *shared_anon(void);

void shared_get(struct shared_pages *p_set);

/*
 * one hold fewer. the last writes back what was stored and lets the
 * frames go, so never with a lock held
 */
void shared_put(struct shared_pages *p_set);

/*
 * page u_index of the set into *p_frame, read in or zeroed the first
 * time, with a reference to the frame taken for the caller to map: and
 * stored to from now on when b_write, which a page of a writable mapping
 * is as soon as it is there. false when there is no memory, or the file
 * would not be read
 */
bool shared_frame(struct shared_pages *p_set, uint64_t u_index, bool b_write,
                  uint64_t *p_frame);

/* page u_index, if it is there, stored to from now on */
void shared_dirty(struct shared_pages *p_set, uint64_t u_index);

/*
 * the pages from u_first to u_last of the set that were stored to,
 * written to the file, as far as the file goes: 0, or -EIO
 */
int64_t shared_sync(struct shared_pages *p_set, uint64_t u_first,
                    uint64_t u_last);

/* the same for every file's set, for sync and fsync */
int64_t shared_sync_all(void);

/*
 * the lock a read or a write of a file holds, for the pages of it a set
 * has to be the file's newest bytes, and for no page to be read in from
 * the file while it is half written. the three below are called with it
 * held
 */
uint64_t shared_lock(void);
void shared_unlock(uint64_t u_flags);

/* u_len bytes written to the file at u_offset: the set's pages take them */
void shared_wrote(const struct vfs_file *p_node, uint64_t u_offset,
                  const void *p_buf, uint64_t u_len);

/* u_len bytes read from the file at u_offset: the set's pages over them */
void shared_overlay(const struct vfs_file *p_node, uint64_t u_offset,
                    void *p_buf, uint64_t u_len);

/* the file is p_node->size long now: the set's bytes past that are zeroes */
void shared_resized(const struct vfs_file *p_node);

#endif
