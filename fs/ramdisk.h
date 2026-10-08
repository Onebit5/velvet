// SPDX-License-Identifier: GPL-2.0-only
/*
 * fs/ramdisk.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * a read-only filesystem that is just a tar file philemon handed the kernel at
 * boot.
 */

/* the design notes for ramdisk.h are in docs/subsystems/mm.rst */

#ifndef FS_RAMDISK_H
#define FS_RAMDISK_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

struct ramdisk_file {
    const char *name;
    const void *data;
    uint64_t    size;

    uint32_t    mode;
};

bool ramdisk_may_read(const struct ramdisk_file *f, int uid);

void ramdisk_init(void);

void ramdisk_mount(const void *base, uint64_t size);

bool     ramdisk_present(void);
uint64_t ramdisk_bytes(void);

bool ramdisk_stat(size_t index, struct ramdisk_file *out);

bool ramdisk_open(const char *name, struct ramdisk_file *out);

size_t ramdisk_count(void);

#endif
