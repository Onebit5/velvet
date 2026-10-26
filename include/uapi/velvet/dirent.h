// SPDX-License-Identifier: GPL-2.0-only
/*
 * include/uapi/velvet/dirent.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * a directory, as getdents64 reads it out.
 */

#ifndef UAPI_VELVET_DIRENT_H
#define UAPI_VELVET_DIRENT_H

#include <stdint.h>

/* one entry, d_reclen long and 8 byte aligned, the name NUL ended */
struct dirent64 {
    uint64_t d_ino;
    int64_t  d_off;             /* where the next one is, to seek back to */
    uint16_t d_reclen;
    uint8_t  d_type;
    char     d_name[];
};

#define DT_UNKNOWN 0
#define DT_FIFO    1
#define DT_CHR     2
#define DT_DIR     4
#define DT_BLK     6
#define DT_REG     8
#define DT_LNK     10
#define DT_SOCK    12

#endif
