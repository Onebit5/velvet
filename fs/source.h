// SPDX-License-Identifier: GPL-2.0-only
/*
 * fs/source.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the source this machine was built from, still on the medium it booted
 * from.
 */

/* the design notes for source.h are in docs/subsystems/mm.rst */

#ifndef FS_SOURCE_H
#define FS_SOURCE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define SOURCE_NAME_MAX 128

struct source_file {
    char     name[SOURCE_NAME_MAX];
    uint64_t size;

    uint64_t at;

    uint32_t mode;
};

void source_init(void);

/*
 * take the medium. `read` is handed sector numbers on the drive that
 * carries this system, which the caller supplies because knowing which
 * drive that is belongs to whoever scanned them.
 *
 * this reads philemon's table, and an image built with no source tree,
 * or one built before this, leaves it saying there is nothing
 * here, which is not an error.
 */
void source_mount(bool (*read)(void *, uint64_t, uint32_t, void *),
                  void *ctx);

bool     source_present(void);
uint64_t source_lba(void);      /* where it starts, for anybody curious */
uint64_t source_bytes(void);    /* how big the archive is */

/*
 * how many files. the first call walks the whole archive and the rest
 * are free, because the answer cannot change
 */
size_t source_count(void);

/*
 * the nth file, in the order the archive holds them, which is sorted,
 * because the build sorts it. walking forwards costs one read per file;
 * jumping about costs a walk from the start
 */
bool source_stat(size_t index, struct source_file *out);

bool source_open(const char *name, struct source_file *out);

int64_t source_read_at(uint64_t at, uint64_t size, uint64_t offset,
                       void *buf, uint64_t len);

bool source_digest(uint8_t out[20]);

extern const char source_stamp[41];
extern const uint64_t source_stamp_bytes;

#endif /* FS_SOURCE_H */
