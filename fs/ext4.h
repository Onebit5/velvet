// SPDX-License-Identifier: GPL-2.0-only
/*
 * fs/ext4.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * ext4, read and written by hand.
 */

/* the design notes for this file are in docs/subsystems/fs.rst */

#ifndef FS_EXT4_H

#define FS_EXT4_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "fs/fat32.h"       /* for struct fat32_time, see below */
#include "fs/jbd2.h"

#define EXT4_SECTOR       512
#define EXT4_MAX_BLOCK    4096          /* the largest the kernel will mount */
#define EXT4_NAME_MAX     255
#define EXT4_ROOT_INO     2
#define EXT4_JOURNAL_INO  8         /* the journal is a file, and this is it */

/*
 * the rule about features: one this driver does not implement is a
 * filesystem the kernel would be guessing at, and guessing at a filesystem
 * is how you write over somebody's data. incompat means it cannot be read
 * without understanding it, ro_compat means it may be read and not
 * written. that distinction is the whole reason a real ext4 mounts at all,
 * since most of what modern mke2fs turns on is ro_compat
 */
#define EXT4_INCOMPAT_FILETYPE  0x0002
#define EXT4_INCOMPAT_RECOVER   0x0004  /* a journal wanting replay */
#define EXT4_INCOMPAT_EXTENTS   0x0040
#define EXT4_INCOMPAT_64BIT     0x0080
#define EXT4_INCOMPAT_FLEX_BG   0x0200

#define EXT4_INCOMPAT_KNOWN     (EXT4_INCOMPAT_FILETYPE \
                                 | EXT4_INCOMPAT_RECOVER \
                                 | EXT4_INCOMPAT_EXTENTS \
                                 | EXT4_INCOMPAT_64BIT \
                                 | EXT4_INCOMPAT_FLEX_BG)

#define EXT4_RO_SPARSE_SUPER    0x0001
#define EXT4_RO_LARGE_FILE      0x0002
#define EXT4_RO_HUGE_FILE       0x0008
#define EXT4_RO_GDT_CSUM        0x0010
#define EXT4_RO_DIR_NLINK       0x0020
#define EXT4_RO_EXTRA_ISIZE     0x0040
#define EXT4_RO_METADATA_CSUM   0x0400

#define EXT4_RO_COMPAT_WRITABLE (EXT4_RO_SPARSE_SUPER \
                                 | EXT4_RO_LARGE_FILE \
                                 | EXT4_RO_HUGE_FILE \
                                 | EXT4_RO_DIR_NLINK \
                                 | EXT4_RO_EXTRA_ISIZE)

#define EXT4_COMPAT_HAS_JOURNAL 0x0004

#define EXT4_EXTENTS_FL         0x00080000

#define EXT4_EXTENT_MAGIC       0xf30a

#define EXT4_S_IFMT   0xf000
#define EXT4_S_IFREG  0x8000
#define EXT4_S_IFDIR  0x4000
#define EXT4_S_IFLNK  0xa000

#define EXT4_DIRECT   12

typedef bool (*ext4_io)(void *ctx, uint64_t lba, uint32_t count, void *buf);
typedef bool (*ext4_out)(void *ctx, uint64_t lba, uint32_t count,
                         const void *buf);

typedef uint32_t (*ext4_clock)(void);

typedef bool (*ext4_sync)(void *ctx);

struct ext4 {
    ext4_io     read;
    ext4_out    write;          /* NULL for a filesystem the kernel may only read */
    ext4_out    pending_write;  /* held back until the journal is started */
    ext4_clock  clock;
    ext4_sync   sync;
    void       *ctx;

    uint32_t block_size;
    uint32_t inodes_count;
    uint32_t blocks_count;
    uint32_t free_blocks;
    uint32_t free_inodes;
    uint32_t first_data_block;
    uint32_t blocks_per_group;
    uint32_t inodes_per_group;
    uint32_t inode_size;
    uint32_t first_ino;
    uint32_t groups;
    uint32_t gdt_block;         /* where the descriptor table begins */

    uint32_t reserved_gdt;

    /*
     * what the feature fields said, kept because almost every decision
     * below asks about one of them
     */
    uint32_t compat, incompat, ro_compat;
    uint32_t desc_size;         /* 32, or 64 on a filesystem built 64-bit */
    bool     extents;           /* whether a new file gets an extent tree */

    /*
     * mounted, readable, and not to be written to, because something
     * in it is understood well enough to read and not well enough to
     * change. `write` being NULL is the other way to be read-only, and
     * means the caller asked for it rather than the disk
     */
    bool     read_only;

    struct jbd journal;
    bool     journalled;

    char     label[17];
    bool     mounted;

    /*
     * somewhere to put a block while the kernel looks at it, and a second for
     * walking indirect blocks, which means holding two at once, and
     * is the whole reason there are two
     */
    uint8_t  scratch[EXT4_MAX_BLOCK];
    uint8_t  indirect[EXT4_MAX_BLOCK];
};

struct ext4_file {
    uint32_t ino;
    uint32_t mode;              /* type and permissions together */
    uint32_t uid, gid;
    uint64_t size;
    uint32_t links;

    bool     is_dir;
    bool     is_symlink;

    /*
     * the three ext2 keeps, broken down so that everything above sees
     * one shape of timestamp whichever filesystem it came from
     */
    struct fat32_time accessed, modified, created;

    char     name[EXT4_NAME_MAX + 1];
};

bool ext4_mount(struct ext4 *fs, ext4_io read, ext4_out write, void *ctx);

void ext4_set_clock(struct ext4 *fs, ext4_clock clock);

bool ext4_lookup(struct ext4 *fs, const char *path, struct ext4_file *out);

bool ext4_lookup_nofollow(struct ext4 *fs, const char *path,
                          struct ext4_file *out);

/* where a symlink points. false if it is not one */
bool ext4_readlink(struct ext4 *fs, const struct ext4_file *f, char *out,
                   size_t size);

bool ext4_readdir(struct ext4 *fs, uint32_t dir_ino, size_t index,
                  struct ext4_file *out);

int64_t ext4_read(struct ext4 *fs, const struct ext4_file *f,
                  uint64_t offset, void *buf, uint64_t len);

int64_t ext4_write(struct ext4 *fs, struct ext4_file *f,
                   uint64_t offset, const void *buf, uint64_t len);

bool ext4_create(struct ext4 *fs, const char *path, uint32_t mode,
                 uint32_t uid, uint32_t gid, struct ext4_file *out);
bool ext4_mkdir(struct ext4 *fs, const char *path, uint32_t mode,
                uint32_t uid, uint32_t gid);
bool ext4_symlink(struct ext4 *fs, const char *path, const char *target,
                  uint32_t uid, uint32_t gid);

bool ext4_unlink(struct ext4 *fs, const char *path);
bool ext4_rmdir(struct ext4 *fs, const char *path);

/* a name moves; the file does not. */
bool ext4_rename(struct ext4 *fs, const char *from, const char *to);

bool ext4_chmod(struct ext4 *fs, const char *path, uint32_t mode);
bool ext4_chown(struct ext4 *fs, const char *path, uint32_t uid, uint32_t gid);

/* how much of it is spoken for */
/*
 * growing adds groups to the end and moves nothing, so it is safe in
 * the way a journal is safe: everything new is written where nothing
 * can see it, and one transaction says it is there.
 *
 * shrinking moves nothing either, and that is a limit rather than a
 * design: a tail with anything in it would have to be emptied first,
 * which means finding everything that points at every block out there.
 * so a shrink whose tail is already empty is arithmetic, and one whose
 * tail is not is refused with a count of what is in the way
 */
struct ext4_resize {
    uint32_t blocks_before, blocks_after;
    uint32_t groups_before, groups_after;
    uint32_t inodes_before, inodes_after;

    uint64_t blocks_in_way;
    uint64_t inodes_in_way;
};

bool ext4_resize(struct ext4 *fs, uint64_t blocks, struct ext4_resize *out,
                 const char **error);

uint64_t ext4_resize_ceiling(const struct ext4 *fs);

bool ext4_usage(struct ext4 *fs, uint64_t *used_bytes, uint64_t *total_bytes);

uint32_t ext4_block_bytes(const struct ext4 *fs);

bool ext4_raw_read(struct ext4 *fs, uint32_t block, void *into);
bool ext4_raw_write(struct ext4 *fs, uint32_t block, const void *from);
bool ext4_file_block(struct ext4 *fs, uint32_t ino, uint32_t index,
                     uint32_t *out);
bool ext4_flush(struct ext4 *fs);

void ext4_set_sync(struct ext4 *fs, ext4_sync sync);

bool ext4_start_journal(struct ext4 *fs);

/*
 * and finish with it: the log is marked empty, the superblock stops
 * asking to be recovered, and the filesystem goes read-only because
 * there is no longer a log to write through. what a clean unmount is
 */
bool ext4_stop_journal(struct ext4 *fs);

void ext4_unix_to_time(uint32_t seconds, struct fat32_time *out);
uint32_t ext4_time_to_unix(const struct fat32_time *t);

#endif
