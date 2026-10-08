// SPDX-License-Identifier: GPL-2.0-only
/*
 * fs/install.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * installing the machine onto a disk.
 */

#include "fs/install.h"
#include "fs/mkfs.h"
#include "drivers/part.h"
#include "lib/string.h"
#include "philemon.h"

/* how much room to leave between the end of the system area and the start of the partition. */
#define ALIGN_SECTORS 2048

/* how much of the disk the system gets. */
#define SYSTEM_FLOOR_SECTORS  (32ull * 1024 * 2)    /* 32 MiB */
#define WORK_FLOOR_SECTORS    (8ull * 1024 * 2)     /* 8 MiB */

static uint64_t round_up(uint64_t v, uint64_t to)
{
    return ((v + to - 1) / to) * to;
}

static void say(const struct install_io *io, const char *what)
{
    if (io->say != NULL) {
        io->say(io->ctx, what);
    }
}

/* read the table philemon left for himself, and say where the system ends. */
uint64_t install_system_end(bool (*read)(void *, uint64_t, uint32_t, void *),
                            void *ctx)
{
    uint8_t sector[PART_SECTOR];
    if (!read(ctx, PH_TABLE_LBA, 1, sector)) {
        return 0;
    }

    struct ph_table t;
    memcpy(&t, sector, sizeof t);

    if (t.magic != PHILEMON_MAGIC) {
        return 0;
    }

    /*
     * the source tree, when the image has one, sits past the ramdisk
     * and is part of the system for exactly one reason: it is on the
     * medium, and a copy of the medium that stopped at the ramdisk
     * would produce an installed machine that boots perfectly and can
     * no longer say what it is made of.
     *
     * it costs nothing to carry, these are sectors being copied
     * either way, and an image written before this says zero here and
     * ends where it always did
     */
    if (t.source_size > 0) {
        return t.source_lba + t.source_sectors;
    }
    return t.ramdisk_lba + t.ramdisk_sectors;
}

/*
 * mkfs writes from lba 0 of whatever it is handed, so it is handed a
 * view shifted into the partition, the same trick disk.c plays for the
 * mounted filesystem, and for the same reason: a filesystem that knew
 * where its partition started would be a filesystem that could write
 * outside it
 */
struct shifted {
    const struct install_io *io;
    uint64_t first;
    uint64_t sectors;
};

static bool shifted_write(void *ctx, uint64_t lba, uint32_t count,
                          const void *buf)
{
    struct shifted *s = ctx;
    if (lba + count > s->sectors) {
        return false;       /* past the end of the partition, which is not
                             * the same as past the end of the disk */
    }
    return s->io->dst_write(s->io->ctx, s->first + lba, count, buf);
}

bool install_system(const struct install_io *io, uint32_t now, bool split,
                    struct install_result *out, const char **error)
{
    *error = NULL;

    uint64_t system_end = install_system_end(io->src_read, io->ctx);
    if (system_end == 0) {
        *error = "there is no velvetOS boot medium to copy from";
        return false;
    }

    /* where the filesystem will go, and whether any of it fits */
    uint64_t first = round_up(system_end, ALIGN_SECTORS);
    if (first + MKFS_MIN_SECTORS > io->dst_sectors) {
        *error = "that disk is too small to hold this system and a "
                 "filesystem as well";
        return false;
    }
    uint64_t part_sectors = io->dst_sectors - first;

    /* and the second one, if there is room for both to be worth having */
    uint64_t work_first = 0, work_sectors = 0;
    if (split) {
        uint64_t want = part_sectors / 8;
        if (want < SYSTEM_FLOOR_SECTORS) {
            want = SYSTEM_FLOOR_SECTORS;
        }
        want = round_up(want, ALIGN_SECTORS);
        if (part_sectors > want + WORK_FLOOR_SECTORS) {
            work_first = first + want;
            work_sectors = part_sectors - want;
            part_sectors = want;
        }
    }

    /*
     * sector by sector rather than in one read, because the buffer for
     * "all of it at once" would be most of a megabyte of kernel stack.
     * it is slow and it is the part with a progress line
     */
    say(io, "copying the system");

    uint8_t buf[PART_SECTOR * 8];
    for (uint64_t lba = 0; lba < system_end; ) {
        uint32_t run = 8;
        if (lba + run > system_end) {
            run = (uint32_t)(system_end - lba);
        }
        if (!io->src_read(io->ctx, lba, run, buf)) {
            *error = "the boot medium stopped answering part way through";
            return false;
        }
        if (!io->dst_write(io->ctx, lba, run, buf)) {
            *error = "the target drive stopped taking writes";
            return false;
        }
        lba += run;
    }

    /* *after* the copy, and that ordering is not a preference. */
    say(io, "writing a partition table");

    struct part_plan plan[2] = {
        {
            .first_lba = first,
            .sectors   = part_sectors,
            .type      = 0x83,  /* linux, which is what ext2 is labelled
                                 * as everywhere */
            .bootable  = true,
        },
        {
            .first_lba = work_first,
            .sectors   = work_sectors,
            .type      = 0x83,
            .bootable  = false, /* nothing boots from it and saying so
                                 * costs one byte */
        },
    };
    size_t count = work_sectors > 0 ? 2 : 1;
    if (!part_write_mbr(io->dst_read, io->dst_write, io->ctx,
                        io->dst_sectors, plan, count, error)) {
        return false;
    }


    say(io, "formatting");

    struct shifted shift = { io, first, part_sectors };

    struct mkfs_result r;
    if (!mkfs_ext4(shifted_write, &shift, part_sectors, "velvetos",
                   now, &r, error)) {
        return false;
    }

    struct mkfs_result w;
    memset(&w, 0, sizeof w);
    if (work_sectors > 0) {
        say(io, "formatting somewhere to work");
        struct shifted work_shift = { io, work_first, work_sectors };
        if (!mkfs_ext4(shifted_write, &work_shift, work_sectors, "work",
                       now, &w, error)) {
            return false;
        }
    }

    if (out != NULL) {
        out->system_sectors = system_end;
        out->part_first_lba = first;
        out->part_sectors   = part_sectors;
        out->fs_blocks      = r.blocks;
        out->work_first_lba = work_first;
        out->work_sectors   = work_sectors;
        out->work_blocks    = w.blocks;
    }
    return true;
}

/* everything above works on sectors and can be pointed at a lump of memory. */

#ifndef VELVETOS_HOSTED

#include "fs/vfs.h"
#include "fs/path.h"
#include "lib/kprintf.h"

/* one buffer, reused down the recursion. */
static uint8_t copy_buf[4096];

static void join(char *out, size_t max, const char *dir, const char *name)
{
    size_t n = 0;
    while (dir[n] != '\0' && n < max - 2) {
        out[n] = dir[n];
        n++;
    }
    if (n > 0 && out[n - 1] != '/') {
        out[n++] = '/';
    }
    for (size_t i = 0; name[i] != '\0' && n < max - 1; i++) {
        out[n++] = name[i];
    }
    out[n] = '\0';
}

static bool copy_file(const char *from, const char *to, const char **error)
{
    struct vfs_file in;
    if (!vfs_open(from, &in)) {
        *error = "could not read a file that was there a moment ago";
        return false;
    }

    struct vfs_file out;
    if (!vfs_create(to, &out)) {
        *error = "could not create a file on the new disk";
        return false;
    }

    for (uint64_t at = 0; at < in.size; ) {
        uint64_t want = in.size - at;
        if (want > sizeof copy_buf) {
            want = sizeof copy_buf;
        }
        int64_t got = vfs_read(&in, at, copy_buf, want);
        if (got <= 0) {
            *error = "a read stopped short part way through a file";
            return false;
        }
        int64_t put = vfs_write(&out, at, copy_buf, (uint64_t)got);
        if (put != got) {
            *error = "a write stopped short, the new disk may be full";
            return false;
        }
        at += (uint64_t)got;
    }
    return true;
}

/* make sure every directory in a path exists, up to but not including the last component. */
static bool make_parents(char *path, const char **error)
{
    for (size_t i = 1; path[i] != '\0'; i++) {
        if (path[i] != '/') {
            continue;
        }
        path[i] = '\0';

        struct vfs_file existing;
        bool there = vfs_open(path, &existing);
        if (!there && !vfs_mkdir(path)) {
            *error = "could not make a directory on the new disk";
            path[i] = '/';
            return false;
        }
        path[i] = '/';
    }
    return true;
}

size_t install_copy_tree(const char *from, const char *to,
                         void (*report)(const char *what),
                         const char **error)
{
    size_t copied = 0;

    for (size_t i = 0; ; i++) {
        struct vfs_file e;
        if (!vfs_readdir(from, i, &e)) {
            break;
        }
        if (e.is_dir) {
            continue;       /* the ramdisk never reports one, and a source
                             * that did would have its files listed anyway */
        }

        char src_path[PATH_MAX], dst_path[PATH_MAX];
        join(src_path, sizeof src_path, from, e.name);
        join(dst_path, sizeof dst_path, to, e.name);

        if (!make_parents(dst_path, error)) {
            return copied;
        }
        if (!copy_file(src_path, dst_path, error)) {
            return copied;
        }
        if (report != NULL) {
            report(dst_path);
        }
        copied++;
    }
    return copied;
}

#endif /* VELVETOS_HOSTED */
