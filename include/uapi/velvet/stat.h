// SPDX-License-Identifier: GPL-2.0-only
/*
 * include/uapi/velvet/stat.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * what a file is, as stat and its relatives say it.
 */

#ifndef UAPI_VELVET_STAT_H
#define UAPI_VELVET_STAT_H

#include <stdint.h>

#include "time.h"

#define S_IFMT   0170000
#define S_IFSOCK 0140000
#define S_IFLNK  0120000
#define S_IFREG  0100000
#define S_IFBLK  0060000
#define S_IFDIR  0040000
#define S_IFCHR  0020000
#define S_IFIFO  0010000
#define S_ISUID  0004000
#define S_ISGID  0002000
#define S_ISVTX  0001000

/* 144 bytes, laid out the way an x86_64 libc reads it */
struct stat {
    uint64_t st_dev;
    uint64_t st_ino;
    uint64_t st_nlink;
    uint32_t st_mode;
    uint32_t st_uid;
    uint32_t st_gid;
    uint32_t pad0;
    uint64_t st_rdev;
    int64_t  st_size;
    int64_t  st_blksize;
    int64_t  st_blocks;         /* in units of 512 bytes, whatever the disk's */
    struct timespec st_atim;
    struct timespec st_mtim;
    struct timespec st_ctim;
    int64_t  unused[3];
};

#endif
