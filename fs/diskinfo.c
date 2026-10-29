// SPDX-License-Identifier: GPL-2.0-only
/*
 * fs/diskinfo.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the drives, the partitions and the mounts, to a person or in /proc.
 */

#include <velvet/diskinfo.h>
#include <velvet/ahci.h>
#include <velvet/bcache.h>
#include <velvet/disk.h>
#include <velvet/kprintf.h>
#include <velvet/part.h>
#include <velvet/proc.h>
#include <velvet/vfs.h>

/* one filesystem: where, on which partition, of what kind, how full. */
static void say_mount(size_t which)
{
    char arr_at[PATH_MAX];

    vfs_where(VFS_DISK, which, arr_at, sizeof arr_at);
    kprintf("\n%-10s %s\n", "mounted at", arr_at);

    /*
     * which partition, rather than which drive answered first,
     * which is the difference a partition table made
     */
    struct disk_part e;
    int part = disk_mounted_part(which);
    if (part >= 0 && disk_part_at((size_t)part, &e)) {
        if (e.scheme == PART_NONE) {
            kprintf("partition  none, a filesystem written straight "
                    "to sector zero\n");
        } else {
            kprintf("partition  %d of %lu, %s, starting at sector %lu\n",
                    part, (uint64_t)disk_part_count(),
                    e.scheme == PART_GPT ? "gpt" : "mbr",
                    e.p.first_lba);
        }
    }
    kprintf("filesystem %s, labelled \"%s\"\n",
            disk_kind_name(which), disk_label(which));
    if (disk_which(which) == DISK_EXT4) {
        kprintf("journal    %s\n", disk_journalled(which)
                ? "yes, metadata is written to a log first, so a crash "
                  "costs a replay"
                : "none. a crash costs a walk of the whole disk, which "
                  "is what `fsck` is");
    }
    if (disk_which(which) == DISK_FAT32) {
        kprintf("           (fat records no owners and no permissions, so "
                "everything on it\n");
        kprintf("            is 0644 owned by root by decree. an ext2 disk "
                "answers for itself)\n");
    }
    kprintf("%s   %lu bytes each\n",
            disk_which(which) == DISK_EXT4 ? "blocks  " : "clusters",
            (uint64_t)disk_cluster_bytes(which));

    uint64_t used = 0, total = 0;
    if (disk_usage(which, &used, &total)) {
        kprintf("used       %lu KiB of %lu MiB\n",
                used / 1024, total / (1024 * 1024));
    }
}

void disk_show(void)
{
    if (disk_block_count() == 0) {
        kprintf("no disk. this machine has only what the loader handed the "
                "kernel,\nkept in memory, which forgets everything on "
                "reboot.\n");
        kprintf("give qemu a drive and there will be somewhere to write.\n");
        return;
    }

    /* every drive, a block device with no partition number */
    const struct disk_block *p_block;
    for (size_t sz_at = 0; (p_block = disk_block_at(sz_at)) != NULL;
         sz_at++) {
        if ((p_block->u_rdev & 0xf) != 0) {
            continue;
        }
        kprintf("drive      %s, %s, %lu MiB (%lu sectors)\n",
                p_block->arr_name, ahci_model(p_block->u_drive),
                p_block->u_sectors * AHCI_SECTOR / (1024 * 1024),
                p_block->u_sectors);
    }

    /* every filesystem hanging off the drives, rather than the one. */
    for (size_t i = 0; i < DISK_MOUNTS; i++) {
        if (disk_ready(i)) {
            say_mount(i);
        }
    }

    struct bcache_stats c;
    disk_cache_stats(&c);
    uint64_t asked = c.hits + c.misses;
    kprintf("\ncache      %lu of %d blocks held, %lu dirty\n",
            (uint64_t)c.held, BCACHE_BLOCKS, (uint64_t)c.dirty);
    if (asked > 0) {
        /* the hit rate is the only honest measure of whether the cache was worth writing. */
        kprintf("           %lu of %lu reads answered without the drive "
                "(%lu%%)\n", c.hits, asked, (c.hits * 100) / asked);
    }
    if (c.writes > 0) {
        kprintf("           %lu writes became %lu trips to the drive\n",
                c.writes, c.writebacks);
    }
}

void disk_show_partitions(void)
{
    size_t n = disk_part_count();
    if (n == 0) {
        kprintf("no drives, or none the kernel could read a sector from\n");
        return;
    }

    kprintf("  #  drive  scheme  start        sectors      kind\n");
    for (size_t i = 0; i < n; i++) {
        struct disk_part e;
        if (!disk_part_at(i, &e)) {
            continue;
        }
        const char *scheme = (e.scheme == PART_GPT) ? "gpt"
                           : (e.scheme == PART_MBR) ? "mbr" : "-";

        kprintf("  %2lu  %5u  %-6s  %-11lu  %-11lu  %s",
                (uint64_t)i, e.p.drive, scheme,
                e.p.first_lba, e.p.sectors, e.p.kind);
        if (e.p.name[0] != '\0') {
            kprintf("  \"%s\"", e.p.name);
        }
        if (e.fs[0] != '\0') {
            kprintf("  [%s]", e.fs);
        }
        /* and where it is mounted, if it is */
        for (size_t sz_slot = 0; sz_slot < DISK_MOUNTS; sz_slot++) {
            char arr_at[PATH_MAX];
            if (disk_ready(sz_slot) && disk_mounted_part(sz_slot) == (int)i
                && vfs_where(VFS_DISK, sz_slot, arr_at, sizeof arr_at)) {
                kprintf("  at %s", arr_at);
            }
        }
        kprintf("\n");
    }
}

void vfs_show_mounts(void)
{
    kprintf("%-10s %-7s %-5s %s\n", "at", "kind", "write", "on");

    struct vfs_mount m;
    for (size_t i = 0; vfs_mount_at(i, &m); i++) {
        kprintf("%-10s %-7s %-5s %s%s\n", m.arr_at, m.what,
                m.writable ? "yes" : "no", m.arr_where,
                m.present ? "" : "   (absent)");
    }
}

void fs_proc_init(void)
{
    proc_register("disk", PROC_PUBLIC, disk_show);
    proc_register("partitions", PROC_PUBLIC, disk_show_partitions);
    proc_register("mounts", PROC_PUBLIC, vfs_show_mounts);
}
