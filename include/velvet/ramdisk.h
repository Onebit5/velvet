// SPDX-License-Identifier: GPL-2.0-only
/*
 * include/velvet/ramdisk.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the tar file the loader hands the kernel at boot, read where it lies
 * and unpacked into the root.
 */

/* the design notes for ramdisk.h are in docs/subsystems/fs.rst */

#ifndef VELVET_RAMDISK_H
#define VELVET_RAMDISK_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* a ustar name is a prefix of 155, a slash and a name of 100 */
#define RAMDISK_NAME_MAX 257

/*
 * one entry of the archive. the name is the whole of it, prefix and all,
 * with no "./" in front; the data is where it lies in the archive
 */
struct ramdisk_file {
    char        arr_name[RAMDISK_NAME_MAX];
    char        arr_link[101];      /* a link's target */
    const void *data;
    uint64_t    size;
    uint32_t    mode;               /* the permissions, without the type */
    uint32_t    uid, gid;
    uint32_t    rdev;               /* a device's number, major << 8 | minor */
    char        typeflag;           /* tar's: '0' a file, '5' a directory */
};

void ramdisk_init(void);

void ramdisk_mount(const void *base, uint64_t size);

bool     ramdisk_present(void);
uint64_t ramdisk_bytes(void);
size_t   ramdisk_count(void);

/*
 * entry `index` of the archive, in the order it holds them. false past
 * the end, at a header that is not ustar's, and at one whose size runs
 * off the end of the archive, which is where the walk stops
 */
bool ramdisk_stat(size_t index, struct ramdisk_file *out);

/*
 * every entry of the archive made in the filesystem in memory numbered
 * sz_fs, with the directories each needs on the way: how many it made.
 * one the filesystem refuses is left out and the rest carried on with,
 * since a root short of one file is a machine that still boots
 */
size_t ramdisk_unpack(size_t sz_fs);

#endif
