// SPDX-License-Identifier: GPL-2.0-only
/*
 * fs/ramdisk.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the ramdisk: a tar file the loader handed over, read where it lies and
 * unpacked into the filesystem in memory that is the root.
 */

#include <velvet/ramdisk.h>
#include "ustar.h"
#include <velvet/boot.h>
#include <velvet/ramfs.h>
#include <velvet/string.h>
#include <uapi/velvet/stat.h>

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
 * the header at a given byte offset, or NULL if the kernel has run off
 * the end, hit the zero blocks that mark the finish, or found one whose
 * bytes would run off it
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
    uint64_t size = ustar_octal(h->size, sizeof h->size);
    if (size > archive_size - offset - TAR_BLOCK) {
        return NULL;        /* a size that lies ends what can be trusted */
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
        file_count++;
        off = ustar_next(off, h);
    }
}

/* a field that is NUL-terminated only when it is short, copied as a string */
static size_t copy_field(char *str_out, const char *str_field, size_t sz_len)
{
    size_t sz_got = strnlen(str_field, sz_len);
    memcpy(str_out, str_field, sz_got);
    str_out[sz_got] = '\0';
    return sz_got;
}

bool ramdisk_stat(size_t index, struct ramdisk_file *out)
{
    size_t i = 0;
    for (uint64_t off = 0; ; i++) {
        const struct tar_header *h = header_at(off);
        if (h == NULL) {
            return false;
        }
        if (i != index) {
            off = ustar_next(off, h);
            continue;
        }
        memset(out, 0, sizeof *out);
        /* a long name is the prefix, a slash, and the name */
        size_t sz_at = copy_field(out->arr_name, h->prefix, sizeof h->prefix);
        if (sz_at > 0) {
            out->arr_name[sz_at++] = '/';
        }
        copy_field(out->arr_name + sz_at, h->name, sizeof h->name);
        if (out->arr_name[0] == '.' && out->arr_name[1] == '/') {
            memmove(out->arr_name, out->arr_name + 2,
                    strlen(out->arr_name + 2) + 1);
        }
        copy_field(out->arr_link, h->linkname, sizeof h->linkname);
        out->size = ustar_octal(h->size, sizeof h->size);
        out->data = archive + off + TAR_BLOCK;
        out->mode = (uint32_t)ustar_octal(h->mode, sizeof h->mode) & 07777;
        out->uid = (uint32_t)ustar_octal(h->uid, sizeof h->uid);
        out->gid = (uint32_t)ustar_octal(h->gid, sizeof h->gid);
        out->rdev = (uint32_t)(ustar_octal(h->devmajor, sizeof h->devmajor)
                               << 8
                               | ustar_octal(h->devminor, sizeof h->devminor));
        out->typeflag = h->typeflag;
        return true;
    }
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



/* the type a tar entry is, as a file's mode says it, or 0 for one not made */
static uint32_t type_of(char c_flag)
{
    switch (c_flag) {
    case '0':
    case '\0': return S_IFREG;
    case '2':  return S_IFLNK;
    case '3':  return S_IFCHR;
    case '4':  return S_IFBLK;
    case '5':  return S_IFDIR;
    case '6':  return S_IFIFO;
    default:   return 0;        /* a hard link, which nothing here has */
    }
}

/*
 * every directory a path needs on the way to its last name, made where
 * the archive did not name it first
 */
static void make_parents(size_t sz_fs, char *str_path)
{
    struct ramfs_entry e;

    for (char *p_at = str_path + 1; *p_at != '\0'; p_at++) {
        if (*p_at != '/') {
            continue;
        }
        *p_at = '\0';
        if (!ramfs_lookup(sz_fs, str_path, true, &e)) {
            ramfs_make(sz_fs, str_path, S_IFDIR | 0755, 0, 0, 0, NULL, NULL);
        }
        *p_at = '/';
    }
}

size_t ramdisk_unpack(size_t sz_fs)
{
    struct ramdisk_file f;
    struct ramfs_entry e;
    char arr_path[RAMDISK_NAME_MAX + 1];
    size_t sz_made = 0;

    for (size_t sz_at = 0; ramdisk_stat(sz_at, &f); sz_at++) {
        uint32_t u_type = type_of(f.typeflag);
        /* a directory is named with a slash on the end; "./" is the root */
        size_t sz_len = strlen(f.arr_name);
        while (sz_len > 0 && f.arr_name[sz_len - 1] == '/') {
            f.arr_name[--sz_len] = '\0';
        }
        if (u_type == 0 || sz_len == 0
            || (sz_len == 1 && f.arr_name[0] == '.')) {
            continue;
        }
        arr_path[0] = '/';
        memcpy(arr_path + 1, f.arr_name, sz_len + 1);
        make_parents(sz_fs, arr_path);

        uint32_t u_mode = u_type | f.mode;
        bool b_made = ramfs_make(sz_fs, arr_path, u_mode, f.uid, f.gid, f.rdev,
                                 (u_type == S_IFLNK) ? f.arr_link : NULL, &e);
        if (!b_made && u_type == S_IFDIR) {
            /* made already, on the way to something inside it */
            b_made = ramfs_chmod(sz_fs, arr_path, f.mode)
                     && ramfs_chown(sz_fs, arr_path, f.uid, f.gid);
        } else if (b_made && u_type == S_IFREG && f.size > 0) {
            b_made = ramfs_write(e.u_ino, 0, f.data, f.size)
                     == (int64_t)f.size;
        }
        sz_made += b_made ? 1 : 0;
    }
    return sz_made;
}

#ifndef VELVET_HOSTED

#include <velvet/kprintf.h>



void ramdisk_init(void)
{
    const struct boot_handoff *h = boot_handoff();
    if (h->ramdisk == 0 || h->ramdisk_size == 0) {
        kprintf("ramdisk    : none supplied, the shelves are bare\n");
        return;
    }

    /*
     * the bytes themselves are safe: the loader puts them in memory typed
     * "kernel + ramdisk", which is never reclaimed. the address arrives
     * already in the direct map, ready to read
     */
    ramdisk_mount((const void *)h->ramdisk, h->ramdisk_size);

    kprintf("ramdisk    : %lu KiB, %zu files\n",
            ramdisk_bytes() / 1024, ramdisk_count());
}

#endif
