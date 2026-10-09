// SPDX-License-Identifier: GPL-2.0-only
/*
 * include/velvet/install.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * copying this machine onto a disk it can then boot from on its own.
 */

/* the design notes for install.h are in docs/subsystems/mm.rst */

#ifndef VELVET_INSTALL_H
#define VELVET_INSTALL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* copying this machine onto a disk it can then boot from on its own. */

/*
 * everything the sequence touches, so that it can be pointed at a lump
 * of memory in a test instead of at two drives
 */
struct install_io {
    bool (*src_read)(void *ctx, uint64_t lba, uint32_t count, void *buf);

    bool (*dst_read)(void *ctx, uint64_t lba, uint32_t count, void *buf);
    bool (*dst_write)(void *ctx, uint64_t lba, uint32_t count,
                      const void *buf);

    void    *ctx;
    uint64_t dst_sectors;

    void (*say)(void *ctx, const char *what);
};

struct install_result {
    uint64_t system_sectors;    /* philemon, the table, the kernel, the ramdisk */
    uint64_t part_first_lba;
    uint64_t part_sectors;
    uint64_t fs_blocks;

    uint64_t work_first_lba;
    uint64_t work_sectors;
    uint64_t work_blocks;
};

bool install_system(const struct install_io *io, uint32_t now, bool split,
                    struct install_result *out, const char **error);

uint64_t install_system_end(bool (*read)(void *, uint64_t, uint32_t, void *),
                            void *ctx);

/*
 * and the half that needs a filesystem: copy everything under `from`
 * into `to`, recursively, through the vfs. this is what puts /bin and
 * /etc on the new disk, and it is deliberately the ordinary create-and-
 * write path rather than anything clever, that path being built and
 * tested, and a second copy of it would be a second thing to be
 * wrong.
 *
 * returns how many files were copied, and sets *error on the first
 * failure.
 *
 * declared here whether or not there is a vfs to do it with, the
 * definition is guarded in install.c, the same way <asm/machine.h>
 * declares things it does not always implement. a host test that wants
 * to stub this deliberately can, and a header that hid the name would
 * make that look like a missing function instead
 */
size_t install_copy_tree(const char *from, const char *to,
                         void (*report)(const char *what),
                         const char **error);

#endif
