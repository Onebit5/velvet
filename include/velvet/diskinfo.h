// SPDX-License-Identifier: GPL-2.0-only
/*
 * include/velvet/diskinfo.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the drives, the partitions and the mounts, to a person or in /proc.
 */

#ifndef VELVET_DISKINFO_H
#define VELVET_DISKINFO_H

/*
 * the drives, every filesystem on them, the block cache, and what each
 * partition table says: /proc/disk
 */
void disk_show(void);

/* every drive and partition, as block devices: /proc/partitions */
void disk_show_partitions(void);

/*
 * every type mount knows, and which need no drive: /proc/filesystems.
 * fs/namespace.c, whose table of them it is
 */
void mount_show_types(void);

/* /proc/disk, /proc/partitions and /proc/filesystems, those three */
void fs_proc_init(void);

#endif
