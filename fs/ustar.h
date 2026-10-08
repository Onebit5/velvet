// SPDX-License-Identifier: GPL-2.0-only
/*
 * fs/ustar.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * ustar, which two things in this kernel now read.
 */

/* the design notes for ustar.h are in docs/subsystems/mm.rst */

#ifndef FS_USTAR_H
#define FS_USTAR_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define USTAR_BLOCK 512

struct tar_header {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char checksum[8];
    char typeflag;
    char linkname[100];
    char magic[6];      /* "ustar" */
    char version[2];
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];

    /*
     * the answer to names longer than a hundred bytes, and the reason
     * it is not simply a longer name field: the format had to stay
     * readable by tars that predate it. a reader that ignores this
     * reads a deep file under the wrong name and never says so
     */
    char prefix[155];
    char padding[12];
};

_Static_assert(sizeof(struct tar_header) == USTAR_BLOCK,
               "a tar header is one block, no more and no less");

static inline uint64_t ustar_octal(const char *field, size_t len)
{
    uint64_t v = 0;
    for (size_t i = 0; i < len; i++) {
        char c = field[i];
        if (c == '\0' || c == ' ') {
            if (v != 0) {
                break;      /* trailing padding after the digits */
            }
            continue;       /* leading padding before them */
        }
        if (c < '0' || c > '7') {
            break;
        }
        v = v * 8 + (uint64_t)(c - '0');
    }
    return v;
}

static inline bool ustar_valid(const struct tar_header *h)
{
    return h->magic[0] == 'u' && h->magic[1] == 's' && h->magic[2] == 't'
        && h->magic[3] == 'a' && h->magic[4] == 'r';
}

static inline uint64_t ustar_next(uint64_t offset, const struct tar_header *h)
{
    uint64_t size = ustar_octal(h->size, sizeof h->size);
    uint64_t blocks = (size + USTAR_BLOCK - 1) / USTAR_BLOCK;
    return offset + USTAR_BLOCK + blocks * USTAR_BLOCK;
}

#endif /* FS_USTAR_H */
