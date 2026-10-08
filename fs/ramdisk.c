// SPDX-License-Identifier: GPL-2.0-only
/*
 * fs/ramdisk.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the ramdisk: a tar file philemon handed over.
 */

#include "fs/ramdisk.h"
#include "fs/ustar.h"
#include "boot.h"
#include "lib/string.h"

/*
 * the format itself lives in fs/ustar.h, because there are
 * two readers of it, this one, and the source tree out on the boot
 * medium, which is a tar for exactly the reason that this file already
 * knew how to read one
 */
#define TAR_BLOCK USTAR_BLOCK

static const uint8_t *archive;
static uint64_t archive_size;
static size_t file_count;

/*
 * the header at a given byte offset, or NULL if the kernel has run off the end
 * or hit the zero blocks that mark the finish
 */
static const struct tar_header *header_at(uint64_t offset)
{
    if (archive == NULL || offset + TAR_BLOCK > archive_size) {
        return NULL;
    }
    const struct tar_header *h = (const struct tar_header *)(archive + offset);
    if (h->name[0] == '\0') {
        return NULL;        /* end of archive */
    }
    if (!ustar_valid(h)) {
        return NULL;        /* not something the kernel understands, stop rather
                             * than wander off into the bytes */
    }
    return h;
}

void ramdisk_mount(const void *base, uint64_t size)
{
    archive = base;
    archive_size = size;
    file_count = 0;

    for (uint64_t off = 0; ; ) {
        const struct tar_header *h = header_at(off);
        if (h == NULL) {
            break;
        }
        /* typeflag '0' and '\0' both mean a normal file. */
        file_count++;
        off = ustar_next(off, h);
    }
}

bool ramdisk_stat(size_t index, struct ramdisk_file *out)
{
    size_t i = 0;
    for (uint64_t off = 0; ; i++) {
        const struct tar_header *h = header_at(off);
        if (h == NULL) {
            return false;
        }
        if (i == index) {
            out->name = h->name;
            out->size = ustar_octal(h->size, sizeof h->size);
            out->data = archive + off + TAR_BLOCK;
            out->mode = (uint32_t)ustar_octal(h->mode, sizeof h->mode);
            return true;
        }
        off = ustar_next(off, h);
    }
}

bool ramdisk_open(const char *name, struct ramdisk_file *out)
{
    /*
     * tar keeps "./foo" and "foo" as different names depending on how
     * it was made, so let a leading ./ be optional on both sides
     */
    if (name[0] == '.' && name[1] == '/') {
        name += 2;
    }

    struct ramdisk_file f;
    for (size_t i = 0; ramdisk_stat(i, &f); i++) {
        const char *have = f.name;
        if (have[0] == '.' && have[1] == '/') {
            have += 2;
        }
        if (strcmp(have, name) == 0) {
            *out = f;
            return true;
        }
    }
    return false;
}

bool ramdisk_may_read(const struct ramdisk_file *f, int uid)
{
    if (uid == 0) {
        return true;        /* the master of the velvet room reads all */
    }
    return (f->mode & 0004) != 0;   /* everyone else needs other-read */
}

bool     ramdisk_present(void)
{
    return archive != NULL && file_count > 0;
}
uint64_t ramdisk_bytes(void)
{
    return archive_size;
}
size_t   ramdisk_count(void)
{
    return file_count;
}

#ifndef VELVETOS_HOSTED

#include "lib/kprintf.h"



void ramdisk_init(void)
{
    const struct ph_handoff *h = boot_handoff();
    if (h->ramdisk == 0 || h->ramdisk_size == 0) {
        kprintf("ramdisk    : none supplied, the shelves are bare\n");
        return;
    }

    /*
     * the bytes themselves are safe: philemon puts them in memory typed
     * "kernel + ramdisk", which is never reclaimed. the address arrives
     * already in the direct map, ready to read
     */
    ramdisk_mount((const void *)h->ramdisk, h->ramdisk_size);

    kprintf("ramdisk    : %lu KiB, %zu files\n",
            ramdisk_bytes() / 1024, ramdisk_count());
}

#endif
