// SPDX-License-Identifier: GPL-2.0-only
/*
 * include/velvet/part.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * a disk is not a filesystem.
 */

#ifndef VELVET_PART_H
#define VELVET_PART_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* a disk is not a filesystem. */

#define PART_SECTOR   512
#define PART_MAX      16
#define PART_NAME_MAX 36

enum part_scheme {
    PART_NONE = 0,      /* no table, the whole drive, or nothing */
    PART_MBR,
    PART_GPT,
};

struct partition {
    uint64_t first_lba;
    uint64_t sectors;

    unsigned drive;         /* which drive it was found on */
    unsigned index;         /* 1-based, the way everybody numbers them */

    uint8_t  mbr_type;      /* 0 on a gpt disk */
    bool     bootable;      /* the mbr's active flag */

    /* gpt gives partitions names. */
    char     name[PART_NAME_MAX + 1];

    /* what the type says it holds, as far as the kernel recognises it. */
    const char *kind;
};

/* how to read the drive being scanned */
typedef bool (*part_io)(void *ctx, uint64_t lba, uint32_t count, void *buf);

/* read whichever table this drive has. */
size_t part_scan(part_io read, void *ctx, struct partition *out, size_t max,
                 enum part_scheme *scheme);

/* the crc32 this needs lives in <velvet/hash.h> now, with the rest of them. */

/* what an mbr type byte is usually used for. */
const char *part_mbr_kind(uint8_t type);

/* everything above reads. */

/* how to write to the drive being partitioned */
typedef bool (*part_out)(void *ctx, uint64_t lba, uint32_t count,
                         const void *buf);

/* one entry to lay down. */
struct part_plan {
    uint64_t first_lba;
    uint64_t sectors;
    uint8_t  type;
    bool     bootable;
};

/*
 * write an mbr describing `count` partitions, keeping whatever is in the
 * first 446 bytes, because on this machine that is philemon, and the
 * table lives *inside* the boot sector it shares with him.
 *
 * that sharing is the whole awkwardness of the mbr and the reason this
 * takes a reader as well as a writer: the sector has to be read, have
 * its last 66 bytes replaced, and be written back. a formatter that
 * simply wrote a fresh sector would erase the bootloader it is
 * installing.
 *
 * false with *error set if a partition runs past the end of the disk,
 * overlaps another, or if the disk already carries a gpt
 */
bool part_write_mbr(part_io read, part_out write, void *ctx,
                    uint64_t disk_sectors,
                    const struct part_plan *plan, size_t count,
                    const char **error);

#endif
