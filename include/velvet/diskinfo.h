// SPDX-License-Identifier: GPL-2.0-only
/*
 * include/velvet/diskinfo.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the drives, the partitions and the mounts, to a person or in /proc.
 */

#ifndef VELVET_DISKINFO_H
#define VELVET_DISKINFO_H

/* the drive, every filesystem on it, and the block cache: `disk`. */
void disk_show(void);

/* what each drive's partition table says it holds: `parts`. */
void disk_show_partitions(void);

/* which filesystem is where: `mount`, with nothing after it. */
void vfs_show_mounts(void);

/* /proc/disk, /proc/partitions and /proc/mounts, which are those three. */
void fs_proc_init(void);

#endif
