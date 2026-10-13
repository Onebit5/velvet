// SPDX-License-Identifier: GPL-2.0-only
/*
 * fs/disk.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the disk, mounted.
 */

#include <velvet/disk.h>
#include <velvet/fat32.h>
#include <velvet/ahci.h>
#include <velvet/string.h>
#include <asm/irq.h>
#include <velvet/spinlock.h>
#include <velvet/rtc.h>
#include <velvet/epoch.h>
#include <velvet/bcache.h>
#include <velvet/fsck.h>
#include <velvet/kmalloc.h>
#include <velvet/part.h>
#include <velvet/philemon.h>

/*
 * the filesystem keeps one sector of scratch and every path through
 * it assumes nobody else is halfway through another. that was a
 * promise of a race rather than a race, and this is it being kept
 */
static struct spinlock disk_lock = SPINLOCK("disk", LOCK_RANK_DEVICE);

/* a disk is not a filesystem, and this is the same sentence one level up. */

struct mount {
    struct fat32   fat;
    struct ext4    ext;
    enum disk_kind kind;
    bool           ready;

    /* the partition it looks through. */
    uint64_t first, sectors;

    int          part;          /* which entry of `table`, or -1 */
    const char  *at;            /* where the vfs hangs it */
};

static struct mount mounts[DISK_MOUNTS] = {
    [DISK_ROOT] = { .part = -1, .at = "/" },
    [DISK_WORK] = { .part = -1, .at = DISK_WORK_AT },
};

/* the one every call that did not say which one means */
static struct mount *at(size_t which)
{
    return which < DISK_MOUNTS ? &mounts[which] : &mounts[DISK_ROOT];
}

/* ext2 keeps seconds since 1970; fat keeps a broken-down date. */
static uint32_t ext4_now(void)
{
    /* the kept clock rather than the chip. */
    return (uint32_t)epoch_now();
}

/*
 * the filesystem keeps one sector of scratch and every path through it
 * assumes nobody else is halfway through another. two threads reading
 * at once would hand each other the wrong sector, so they do not
 */
/*
 * XXX: this is a spinlock taken with interrupts off, and it is held for
 * as long as the operation takes rather than for as long as the scratch
 * sector needs protecting. the note above is about the sector; what the
 * duration costs is the clock. disk_write_at holds this across an entire
 * write, however many blocks that is, and disk_fsck holds it across the
 * whole filesystem scan, so the timer does not fire for the duration:
 * ticks stop, a sleep_ms in another thread overshoots, and the machine
 * is deaf and blind until the call returns. a sleeping lock would suit
 * a hold this long, or the hold could be narrowed to the sector copies
 * themselves instead of the whole call.
 */
static uint64_t enter(void)
{
    return spin_lock_irq(&disk_lock);
}
static void leave(uint64_t flags)
{
    spin_unlock_irq(&disk_lock, flags);
}

/* what the filesystem asks when it needs to stamp something. */
static void disk_now(struct fat32_time *out)
{
    /*
     * fat wants it broken down, so this is the other direction, but
     * still from the kept clock, not from the chip
     */
    struct rtc_time t;
    uint64_t now = epoch_now();
    if (now != 0) {
        epoch_to_date(now, &t);
    } else {
        rtc_read(&t);       /* before the clock was seeded */
    }
    out->year = t.year;
    out->month = t.month;
    out->day = t.day;
    out->hour = t.hour;
    out->minute = t.minute;
    out->second = t.second;
}

bool disk_ready(size_t which)
{
    return at(which)->ready;
}

const char *disk_mount_point(size_t which)
{
    return at(which)->at;
}

enum disk_kind disk_which(size_t which)
{
    struct mount *m = at(which);
    return m->ready ? m->kind : DISK_NONE;
}

const char *disk_kind_name(size_t which)
{
    struct mount *m = at(which);
    switch (disk_which(which)) {
    /* one driver, two filesystems, and the honest answer is whichever the disk actually is. */
    case DISK_EXT4:
        return (m->ext.incompat & EXT4_INCOMPAT_EXTENTS) ? "ext4" : "ext2";
    case DISK_FAT32: return "fat32";
    default:         return "none";
    }
}

/* what a drive said it holds, for every drive, gathered at boot. */

#define DISK_PARTS_MAX 16

static struct disk_part table[DISK_PARTS_MAX];
static size_t table_count;

/* which drive carries philemon's table, found during the scan above. */
static int system_drive = -1;

/*
 * the view a filesystem has of its own partition: every read and write
 * it makes is shifted by where that partition starts and bounded by how
 * long it is, which is the whole of what a partition is.
 *
 * which partition comes through `ctx`, and that is the change two
 * mounts needed. it used to be a pair of file-scope numbers, which is
 * exactly the arrangement that works until there are two of something
 */
static bool view_read(void *ctx, uint64_t lba, uint32_t count, void *buf)
{
    const struct mount *m = ctx;
    if (lba + count > m->sectors) {
        /*
         * past the end of the partition, which is *not* the same as past
         * the end of the drive, and letting one become the other is
         * how a filesystem writes over its neighbour
         */
        return false;
    }
    return bcache_read(NULL, m->first + lba, count, buf);
}

static bool view_write(void *ctx, uint64_t lba, uint32_t count,
                       const void *buf)
{
    const struct mount *m = ctx;
    if (lba + count > m->sectors) {
        return false;
    }
    return bcache_write(NULL, m->first + lba, count, buf);
}

/*
 * what the journal asks of everything under it: that when it says so,
 * what has been written is *on the disk*. between this driver and the
 * platters sits the block cache, which is free to hold a write for as
 * long as it likes, exactly the freedom a journal cannot allow, so
 * emptying it is the promise, and this is where the promise is made.
 *
 * no lock is taken and none may be: every call to this comes from
 * inside a filesystem operation that is holding the disk lock already,
 * which is why `disk_sync` is not the function handed over
 */
static bool ext_sync(void *ctx)
{
    (void)ctx;
    return bcache_sync();
}

/* try to put a filesystem on one of them, at one of the two places a filesystem can hang. */
static bool try_mount(struct mount *m, size_t index)
{
    struct disk_part *e = &table[index];

    m->first = e->p.first_lba;
    m->sectors = e->p.sectors;

    /* the filesystem is handed the view rather than the drive. */
    if (ext4_mount(&m->ext, view_read, view_write, m)) {
        ext4_set_clock(&m->ext, ext4_now);

        /* and if it has a journal, replay it and start writing through it. */
        ext4_set_sync(&m->ext, ext_sync);
        ext4_start_journal(&m->ext);

        m->kind = DISK_EXT4;
        m->ready = true;
        m->part = (int)index;
        e->mountable = true;
        e->fs = (m->ext.incompat & EXT4_INCOMPAT_EXTENTS) ? "ext4" : "ext2";
        return true;
    }
    if (fat32_mount(&m->fat, view_read, view_write, m)) {
        fat32_set_clock(&m->fat, disk_now);
        m->kind = DISK_FAT32;
        m->ready = true;
        m->part = (int)index;
        e->mountable = true;
        e->fs = "fat32";
        return true;
    }
    m->ready = false;
    m->part = -1;
    return false;
}

/* the drive the mounts live on, selected once and left selected. */
static bool select_drive(unsigned drive)
{
    if (!ahci_use_disk(drive)) {
        return false;
    }
    bcache_sync();
    bcache_init(ahci_read, ahci_write, NULL);
    return true;
}

bool disk_mount(void)
{
    for (size_t i = 0; i < DISK_MOUNTS; i++) {
        const char *where = mounts[i].at;
        memset(&mounts[i], 0, sizeof mounts[i]);
        mounts[i].part = -1;
        mounts[i].at = where;
    }
    table_count = 0;
    system_drive = -1;

    if (!ahci_init()) {
        return false;
    }

    /* every drive, and everything each of them says it holds. */
    for (size_t i = 0; i < ahci_disk_count() && table_count < DISK_PARTS_MAX;
         i++) {
        if (!ahci_use_disk(i)) {
            continue;
        }

        /* while this drive is selected anyway: is it a boot medium? */
        if (system_drive < 0) {
            uint8_t sector[PART_SECTOR];
            if (ahci_read(NULL, PH_TABLE_LBA, 1, sector)) {
                struct ph_table t;
                memcpy(&t, sector, sizeof t);
                if (t.magic == PHILEMON_MAGIC) {
                    system_drive = (int)i;
                }
            }
        }

        struct partition found[PART_MAX];
        enum part_scheme scheme;
        size_t n = part_scan(ahci_read, NULL, found, PART_MAX, &scheme);

        if (n == 0) {
            /*
             * no table. an image written straight to sector zero is a
             * perfectly ordinary thing, so it gets one entry covering
             * the whole drive rather than a special case everywhere
             * above this
             */
            struct disk_part *e = &table[table_count++];
            memset(e, 0, sizeof *e);
            e->p.drive = (unsigned)i;
            e->p.first_lba = 0;
            e->p.sectors = ahci_sectors();
            e->p.kind = "whole drive";
            e->scheme = PART_NONE;
            e->fs = "";
            continue;
        }

        for (size_t k = 0; k < n && table_count < DISK_PARTS_MAX; k++) {
            struct disk_part *e = &table[table_count++];
            memset(e, 0, sizeof *e);
            e->p = found[k];
            e->p.drive = (unsigned)i;
            e->scheme = scheme;
            e->fs = "";
        }
    }

    /* and now the first one with something on it. */
    struct mount *root = at(DISK_ROOT);
    for (size_t i = 0; i < table_count; i++) {
        if (!select_drive(table[i].p.drive) || !try_mount(root, i)) {
            continue;
        }

        /* and then somewhere to work: the next partition on the *same drive* holding a filesystem. */
        struct mount *work = at(DISK_WORK);
        for (size_t k = i + 1; k < table_count; k++) {
            if (table[k].p.drive != table[i].p.drive) {
                continue;
            }
            if (try_mount(work, k)) {
                break;
            }
        }
        return true;
    }
    return false;
}



size_t disk_part_count(void)
{
    return table_count;
}

bool disk_part_at(size_t index, struct disk_part *out)
{
    if (index >= table_count) {
        return false;
    }
    *out = table[index];
    return true;
}

int disk_mounted_part(size_t which)
{
    return at(which)->part;
}

bool disk_mount_part(size_t which, size_t index)
{
    if (index >= table_count || which >= DISK_MOUNTS) {
        return false;
    }
    /*
     * the same drive as everything else, for the reason the cache gives
     * at the top of this file. a partition on another drive is a
     * partition this cannot reach without a second cache
     */
    struct mount *root = at(DISK_ROOT);
    if (root->ready && root->part >= 0
        && table[index].p.drive != table[root->part].p.drive) {
        return false;
    }
    /*
     * and not the one that is already somewhere else, because two
     * mounts of one filesystem are two block caches' worth of state
     * disagreeing about the same disk
     */
    for (size_t i = 0; i < DISK_MOUNTS; i++) {
        if (i != which && mounts[i].ready && mounts[i].part == (int)index) {
            return false;
        }
    }

    uint64_t flags = enter();
    struct mount *m = at(which);
    struct mount was = *m;
    m->ready = false;           /* nothing may reach the old one now */
    bool ok = try_mount(m, index);
    if (!ok) {
        *m = was;               /* put it back; nothing was disturbed */
    }
    leave(flags);
    return ok;
}



/*
 * the disk is the root now, so a path arrives already rooted at it and
 * all that is left is the leading slash fat32 has no use for
 */
static const char *below(const char *path)
{
    while (*path == '/') {
        path++;
    }
    return path;
}

static void copy_name(char *dst, const char *src)
{
    size_t i = 0;
    while (src[i] != '\0' && i < DISK_NAME_MAX - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static void fill(struct disk_entry *out, const struct fat32_file *f)
{
    memset(out, 0, sizeof *out);
    copy_name(out->name, f->name);
    out->size = f->size;
    out->cluster = f->first_cluster;
    out->is_dir = f->is_dir;
    out->entry_sector = f->entry_sector;
    out->entry_offset = f->entry_offset;
    out->written = f->written;

    /* fat records no ownership and no permissions. */
    out->mode = 0644;
    out->uid = 0;
    out->gid = 0;
}

static void fill_ext4(struct disk_entry *out, const struct ext4_file *f)
{
    memset(out, 0, sizeof *out);
    copy_name(out->name, f->name);
    out->size = f->size;
    out->is_dir = f->is_dir;
    out->is_symlink = f->is_symlink;
    out->written = f->modified;
    out->mode = f->mode & 0xfff;
    out->uid = f->uid;
    out->gid = f->gid;
    out->ino = f->ino;

    /*
     * an ext2 file is named by its inode rather than by where its
     * directory entry happens to sit, so `cluster` carries the inode
     * and the two `entry_` fields have nothing to say
     */
    out->cluster = f->ino;
}




bool disk_lookup(size_t which, const char *path, struct disk_entry *out)
{
    struct mount *m = at(which);
    if (!m->ready) {
        return false;
    }
    uint64_t flags = enter();
    bool ok;
    if (m->kind == DISK_EXT4) {
        struct ext4_file f;
        ok = ext4_lookup(&m->ext, below(path), &f);
        if (ok) fill_ext4(out, &f);
    } else {
        struct fat32_file f;
        ok = fat32_lookup(&m->fat, below(path), &f);
        if (ok) fill(out, &f);
    }
    leave(flags);
    return ok;
}

bool disk_lookup_nofollow(size_t which, const char *path,
                          struct disk_entry *out)
{
    struct mount *m = at(which);
    if (!m->ready) {
        return false;
    }
    uint64_t flags = enter();
    bool ok;
    if (m->kind == DISK_EXT4) {
        struct ext4_file f;
        ok = ext4_lookup_nofollow(&m->ext, below(path), &f);
        if (ok) fill_ext4(out, &f);
    } else {
        /*
         * fat has no symlinks, so there is nothing a lookup could
         * follow and this is the same question
         */
        struct fat32_file f;
        ok = fat32_lookup(&m->fat, below(path), &f);
        if (ok) fill(out, &f);
    }
    leave(flags);
    return ok;
}

bool disk_readlink(size_t which, const char *path, char *out, size_t size)
{
    struct mount *m = at(which);
    if (!m->ready || m->kind != DISK_EXT4) {
        return false;
    }
    uint64_t flags = enter();
    struct ext4_file f;
    bool ok = ext4_lookup_nofollow(&m->ext, below(path), &f)
           && ext4_readlink(&m->ext, &f, out, size);
    leave(flags);
    return ok;
}

bool disk_readdir(size_t which, const char *path, size_t index,
                  struct disk_entry *out)
{
    struct mount *m = at(which);
    if (!m->ready) {
        return false;
    }
    uint64_t flags = enter();

    bool ok;
    if (m->kind == DISK_EXT4) {
        struct ext4_file dir;
        ok = ext4_lookup(&m->ext, below(path), &dir) && dir.is_dir;
        if (ok) {
            struct ext4_file f;
            ok = ext4_readdir(&m->ext, dir.ino, index, &f);
            if (ok) fill_ext4(out, &f);
        }
    } else {
        struct fat32_file dir;
        ok = fat32_lookup(&m->fat, below(path), &dir) && dir.is_dir;
        if (ok) {
            struct fat32_file f;
            ok = fat32_readdir(&m->fat, dir.first_cluster, index, &f);
            if (ok) fill(out, &f);
        }
    }

    leave(flags);
    return ok;
}

int64_t disk_read(size_t which, uint32_t cluster, uint64_t size,
                  uint64_t offset, void *buf, uint64_t len)
{
    struct mount *m = at(which);
    if (!m->ready) {
        return -1;
    }
    uint64_t flags = enter();

    int64_t n;
    if (m->kind == DISK_EXT4) {
        /*
         * on ext2 a descriptor remembers the inode number, which is
         * the file's identity rather than a place on the disk
         */
        struct ext4_file f;
        memset(&f, 0, sizeof f);
        f.ino = cluster;
        f.size = size;
        n = ext4_read(&m->ext, &f, offset, buf, len);
    } else {
        /*
         * a fat descriptor remembers where the file starts and how big
         * it is, which is all fat32_read needs to find any byte of it
         */
        struct fat32_file f;
        memset(&f, 0, sizeof f);
        f.first_cluster = cluster;
        f.size = (uint32_t)size;
        f.is_dir = false;
        n = fat32_read(&m->fat, &f, offset, buf, len);
    }
    leave(flags);
    return n;
}

bool disk_create(size_t which, const char *path, struct disk_entry *out)
{
    struct mount *m = at(which);
    if (!m->ready) {
        return false;
    }
    uint64_t flags = enter();
    bool ok;
    if (m->kind == DISK_EXT4) {
        struct ext4_file f;
        ok = ext4_create(&m->ext, below(path), 0644, 0, 0, &f);
        if (ok) fill_ext4(out, &f);
    } else {
        struct fat32_file f;
        ok = fat32_create(&m->fat, below(path), &f);
        if (ok) fill(out, &f);
    }
    leave(flags);
    return ok;
}

int64_t disk_write_at(size_t which, struct disk_entry *e, uint64_t offset,
                      const void *buf, uint64_t len)
{
    struct mount *m = at(which);
    if (!m->ready || e->is_dir) {
        return -1;
    }
    if (m->kind == DISK_EXT4) {
        uint64_t flags = enter();
        struct ext4_file f;
        memset(&f, 0, sizeof f);
        f.ino = e->cluster;
        f.size = e->size;
        int64_t n = ext4_write(&m->ext, &f, offset, buf, len);
        if (n > 0) {
            e->size = f.size;
        }
        leave(flags);
        return n;
    }
    if (e->entry_sector == 0) {
        return -1;
    }
    uint64_t flags = enter();

    /* rebuild what the filesystem wants out of what the descriptor remembered. */
    struct fat32_file f;
    memset(&f, 0, sizeof f);
    f.first_cluster = e->cluster;
    f.size = (uint32_t)e->size;
    f.is_dir = false;
    f.attr = FAT32_ATTR_ARCHIVE;
    f.entry_sector = e->entry_sector;
    f.entry_offset = e->entry_offset;

    int64_t n = fat32_write(&m->fat, &f, offset, buf, len);
    if (n > 0) {
        e->size = f.size;
        e->cluster = f.first_cluster;
    }

    leave(flags);
    return n;
}

bool disk_mkdir(size_t which, const char *path)
{
    struct mount *m = at(which);
    if (!m->ready) {
        return false;
    }
    uint64_t flags = enter();
    bool ok = (m->kind == DISK_EXT4) ? ext4_mkdir(&m->ext, below(path), 0755, 0, 0)
                                  : fat32_mkdir(&m->fat, below(path));
    leave(flags);
    return ok;
}

bool disk_rmdir(size_t which, const char *path)
{
    struct mount *m = at(which);
    if (!m->ready) {
        return false;
    }
    uint64_t flags = enter();
    bool ok = (m->kind == DISK_EXT4) ? ext4_rmdir(&m->ext, below(path))
                                  : fat32_rmdir(&m->fat, below(path));
    leave(flags);
    return ok;
}

bool disk_unlink(size_t which, const char *path)
{
    struct mount *m = at(which);
    if (!m->ready) {
        return false;
    }
    uint64_t flags = enter();
    bool ok = (m->kind == DISK_EXT4) ? ext4_unlink(&m->ext, below(path))
                                  : fat32_unlink(&m->fat, below(path));
    leave(flags);
    return ok;
}

bool disk_rename(size_t which, const char *from, const char *to)
{
    struct mount *m = at(which);
    if (!m->ready) {
        return false;
    }
    uint64_t flags = enter();
    bool ok = (m->kind == DISK_EXT4)
            ? ext4_rename(&m->ext, below(from), below(to))
            : fat32_rename(&m->fat, below(from), below(to));
    leave(flags);
    return ok;
}

/* all three answer false on a fat disk, and say so rather than pretending. */
bool disk_chmod(size_t which, const char *path, uint32_t mode)
{
    struct mount *m = at(which);
    if (!m->ready || m->kind != DISK_EXT4) {
        return false;
    }
    uint64_t flags = enter();
    bool ok = ext4_chmod(&m->ext, below(path), mode);
    leave(flags);
    return ok;
}

bool disk_chown(size_t which, const char *path, uint32_t uid, uint32_t gid)
{
    struct mount *m = at(which);
    if (!m->ready || m->kind != DISK_EXT4) {
        return false;
    }
    uint64_t flags = enter();
    bool ok = ext4_chown(&m->ext, below(path), uid, gid);
    leave(flags);
    return ok;
}

bool disk_symlink(size_t which, const char *path, const char *target)
{
    struct mount *m = at(which);
    if (!m->ready || m->kind != DISK_EXT4) {
        return false;
    }
    uint64_t flags = enter();
    bool ok = ext4_symlink(&m->ext, below(path), target, 0, 0);
    leave(flags);
    return ok;
}

/* everything the cache is holding, onto the drive. */
bool disk_sync(void)
{
    /*
     * one cache under both mounts, so this is one question however many
     * filesystems are hanging off it
     */
    if (!mounts[DISK_ROOT].ready) {
        return true;    /* nothing to lose */
    }
    uint64_t flags = enter();
    bool ok = bcache_sync();
    leave(flags);
    return ok;
}

/*
 * the disk is finished with: the last of what is in memory goes out,
 * and the log is closed behind it.
 *
 * closing the log is what makes the difference between a filesystem
 * that was *unmounted* and one that merely stopped being written to.
 * the first replays nothing next time and the second replays whatever
 * the log still holds, harmless, but a walk the machine did not need
 * to make, and a `recover` flag left set that tells every other system
 * this disk was left in a state nobody knows.
 *
 * after this the filesystem is read-only, which is the point: there is
 * no longer a log to write through, and the only thing that may happen
 * next is the machine stopping
 */
bool disk_unmount(void)
{
    if (!mounts[DISK_ROOT].ready) {
        return true;
    }
    uint64_t flags = enter();
    bool ok = true;
    /*
     * every mount, and the work one first: it is the one somebody was
     * using, and the root is the one the machine needs last
     */
    for (size_t i = DISK_MOUNTS; i-- > 0;) {
        struct mount *m = &mounts[i];
        if (m->ready && m->kind == DISK_EXT4 && m->ext.journal.ready) {
            ok = ext4_stop_journal(&m->ext) && ok;
        }
    }
    ok = bcache_sync() && ok;
    leave(flags);
    return ok;
}

/*
 * the filesystem describes the disk it was made for, and the partition
 * it sits in is usually longer than that, an image written to a
 * larger drive, a partition somebody made room in. this is the one
 * question the driver cannot answer for itself, because how long the
 * partition is belongs to the layer that knows about partitions.
 *
 * the sync first is the same argument `disk_fsck` makes: what a
 * write-back cache is holding is newer than the disk, and a resize
 * reads bitmaps to decide what is safe. the sync after is the other
 * end of it
 */
bool disk_resize(size_t which, uint64_t blocks, struct ext4_resize *out,
                 const char **error)
{
    *error = NULL;
    /*
     * cleared here rather than only in the driver, because the two
     * refusals below never reach the driver and the caller reads this
     * to find out what was in the way
     */
    memset(out, 0, sizeof *out);

    struct mount *m = at(which);
    if (!m->ready || m->kind != DISK_EXT4) {
        *error = "there is no ext4 filesystem mounted there to resize";
        return false;
    }

    /*
     * the filesystem was handed a view that ends where the partition
     * does and cannot see past it, so only this knows how far it may
     * grow. a filesystem longer than its partition writes over whatever
     * is next
     */
    if (blocks > disk_room(which)) {
        *error = "longer than the partition under it, which would write "
                 "over its neighbour";
        return false;
    }

    if (!disk_sync()) {
        *error = "the drive would not take what was waiting for it";
        return false;
    }

    uint64_t flags = enter();
    bool ok = ext4_resize(&m->ext, blocks, out, error);
    leave(flags);

    if (!disk_sync() && ok) {
        *error = "resized, and the drive would not take it";
        return false;
    }
    return ok;
}

/*
 * how long the partition under a mount is, in the filesystem's own
 * blocks, which is what a resize is usually asked to fill. the
 * filesystem cannot work this out: it has been handed a view that
 * begins at zero and ends where the partition does, and knowing more
 * than that is exactly what it must not do
 */
uint64_t disk_room(size_t which)
{
    struct mount *m = at(which);
    if (!m->ready || m->kind != DISK_EXT4) {
        return 0;
    }
    uint32_t per = ext4_block_bytes(&m->ext) / AHCI_SECTOR;
    return per > 0 ? m->sectors / per : 0;
}

uint64_t disk_ceiling(size_t which)
{
    struct mount *m = at(which);
    if (!m->ready || m->kind != DISK_EXT4) {
        return 0;
    }
    return ext4_resize_ceiling(&m->ext);
}

/* check the mounted filesystem, and mend what has exactly one right answer. */
bool disk_fsck(size_t which, bool mend, struct fsck_report *out,
               const char **error)
{
    *error = NULL;

    struct mount *m = at(which);
    if (!m->ready || m->kind != DISK_EXT4) {
        *error = "there is no ext4 filesystem mounted there to check";
        return false;
    }
    if (!disk_sync()) {
        *error = "the drive would not take what was waiting for it";
        return false;
    }

    uint32_t blocks, inodes;
    uint64_t flags = enter();
    bool measured = fsck_measure(view_read, m, &blocks, &inodes);
    leave(flags);
    if (!measured) {
        *error = "no filesystem where one was mounted";
        return false;
    }

    size_t need = fsck_workspace_bytes(blocks, inodes);
    void *work = kmalloc(need);
    if (work == NULL) {
        *error = "not enough memory to check a filesystem this size";
        return false;
    }

    flags = enter();
    bool ok = fsck_run(view_read, mend ? view_write : NULL, m,
                       work, need, out);
    leave(flags);
    kfree(work);

    if (!ok) {
        *error = out->stopped != NULL ? out->stopped : "cannot check it";
        return false;
    }
    if (mend && out->total_mended > 0 && !disk_sync()) {
        *error = "mended, and the drive would not take it";
        return false;
    }
    return true;
}

bool disk_dirty(void)
{
    if (!mounts[DISK_ROOT].ready) {
        return false;
    }
    uint64_t flags = enter();
    bool any = bcache_dirty();
    leave(flags);
    return any;
}

void disk_cache_stats(struct bcache_stats *out)
{
    memset(out, 0, sizeof *out);
    if (!mounts[DISK_ROOT].ready) {
        return;
    }
    uint64_t flags = enter();
    bcache_get_stats(out);
    leave(flags);
}



const char *disk_label(size_t which)
{
    struct mount *m = at(which);
    if (!m->ready) {
        return "";
    }
    return (m->kind == DISK_EXT4) ? m->ext.label : m->fat.label;
}

const char *disk_model(void)
{
    return ahci_model();
}

/* is the mounted filesystem writing through a log? */
bool disk_journalled(size_t which)
{
    struct mount *m = at(which);
    return m->ready && m->kind == DISK_EXT4 && m->ext.journal.ready;
}

uint64_t disk_bytes(void)
{
    return ahci_sectors() * AHCI_SECTOR;
}

/* ext2 calls them blocks and fat calls them clusters. */
uint32_t disk_cluster_bytes(size_t which)
{
    struct mount *m = at(which);
    if (!m->ready) {
        return 0;
    }
    return (m->kind == DISK_EXT4) ? ext4_block_bytes(&m->ext)
                                  : fat32_cluster_bytes(&m->fat);
}

bool disk_usage(size_t which, uint64_t *used_bytes, uint64_t *total_bytes)
{
    struct mount *m = at(which);
    if (!m->ready) {
        return false;
    }
    uint64_t flags = enter();
    bool ok;

    if (m->kind == DISK_EXT4) {
        ok = ext4_usage(&m->ext, used_bytes, total_bytes);
    } else {
        uint32_t used, total;
        ok = fat32_usage(&m->fat, &used, &total);
        if (ok) {
            uint64_t per = fat32_cluster_bytes(&m->fat);
            *used_bytes = (uint64_t)used * per;
            *total_bytes = (uint64_t)total * per;
        }
    }

    leave(flags);
    return ok;
}


/* the cache is deliberately not involved. */

size_t disk_drive_count(void)
{
    return ahci_disk_count();
}

static bool with_drive(unsigned drive, bool writing, uint64_t lba,
                       uint32_t count, void *buf)
{
    if (drive >= ahci_disk_count()) {
        return false;
    }

    uint64_t flags = enter();

    /*
     * where to go back to. the mounted filesystem is still pointing at
     * its own drive and has no idea any of this is happening
     */
    int root = mounts[DISK_ROOT].part;
    int back = (root >= 0) ? (int)table[root].p.drive : -1;

    /* and the drive that is already selected is not selected again. */
    bool same = (back >= 0 && back == (int)drive);

    bool ok = false;
    if (same || ahci_use_disk(drive)) {
        ok = writing ? ahci_write(NULL, lba, count, buf)
                     : ahci_read(NULL, lba, count, buf);
    }

    /* put it back whatever happened. */
    if (back >= 0 && !same) {
        ahci_use_disk((size_t)back);
    }

    leave(flags);
    return ok;
}

bool disk_raw_read(unsigned drive, uint64_t lba, uint32_t count, void *buf)
{
    return with_drive(drive, false, lba, count, buf);
}

bool disk_raw_write(unsigned drive, uint64_t lba, uint32_t count,
                    const void *buf)
{
    /* the cast is the one place in this file where const is given up. */
    return with_drive(drive, true, lba, count, (void *)(uintptr_t)buf);
}

/* the boot medium, in the shape whoever reads sectors off it wants. */
bool disk_system_read(void *ctx, uint64_t lba, uint32_t count, void *buf)
{
    (void)ctx;
    if (system_drive < 0) {
        return false;
    }
    return disk_raw_read((unsigned)system_drive, lba, count, buf);
}

uint64_t disk_drive_sectors(unsigned drive)
{
    if (drive >= ahci_disk_count()) {
        return 0;
    }
    /* the same selection, and therefore the same lock */
    uint64_t flags = enter();
    int root = mounts[DISK_ROOT].part;
    int back = (root >= 0) ? (int)table[root].p.drive : -1;
    uint64_t n = 0;
    if (ahci_use_disk(drive)) {
        n = ahci_sectors();
    }
    if (back >= 0) {
        ahci_use_disk((size_t)back);
    }
    leave(flags);
    return n;
}

/*
 * FIXME: this is the one function in the file that picks a disk without
 * taking disk_lock, and it is the one that must not. the switch below
 * stops the port, starts it again and re-reads the identify page, and
 * every sibling that does the same thing wraps that in enter() and
 * leave(); this one can therefore land in the middle of another thread's
 * transfer. the pointer it returns is the worse half: ahci_model() hands
 * back a static buffer inside the driver, and the switch back on the way
 * out rewrites exactly that buffer, so the caller can print a string
 * that changes under it. take the lock, and copy the name out instead of
 * returning a pointer into a controller that is about to be
 * reconfigured.
 */
const char *disk_drive_model(unsigned drive)
{
    if (drive >= ahci_disk_count()) {
        return "?";
    }
    int root = mounts[DISK_ROOT].part;
    int back = (root >= 0) ? (int)table[root].p.drive : -1;
    const char *m = "?";
    if (ahci_use_disk(drive)) {
        m = ahci_model();
    }
    if (back >= 0) {
        ahci_use_disk((size_t)back);
    }
    return m;
}

int disk_system_drive(void)
{
    return system_drive;
}

int disk_mounted_drive(void)
{
    int root = mounts[DISK_ROOT].part;
    return (root >= 0) ? (int)table[root].p.drive : -1;
}
