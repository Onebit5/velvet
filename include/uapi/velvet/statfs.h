// SPDX-License-Identifier: GPL-2.0-only
/*
 * include/uapi/velvet/statfs.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * what statfs says about a filesystem, and the numbers it says it is.
 */

#ifndef UAPI_VELVET_STATFS_H
#define UAPI_VELVET_STATFS_H

#include <stdint.h>

struct statfs {
    int64_t  f_type;            /* which kind, as one of the numbers below */
    int64_t  f_bsize;
    uint64_t f_blocks;
    uint64_t f_bfree;
    uint64_t f_bavail;
    uint64_t f_files;
    uint64_t f_ffree;
    int32_t  f_fsid[2];
    int64_t  f_namelen;
    int64_t  f_frsize;
    int64_t  f_flags;
    int64_t  f_spare[4];
};

#define EXT4_SUPER_MAGIC  0xef53        /* ext2 and ext4 alike */
#define MSDOS_SUPER_MAGIC 0x4d44        /* fat */
#define PROC_SUPER_MAGIC  0x9fa0
#define RAMFS_MAGIC       0x858458f6
#define TMPFS_MAGIC       0x01021994    /* and devtmpfs, which is one */
#define PIPEFS_MAGIC      0x50495045
#define SOCKFS_MAGIC      0x534f434b

#define ST_RDONLY 1                     /* in f_flags */

#endif
