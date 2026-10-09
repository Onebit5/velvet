// SPDX-License-Identifier: GPL-2.0-only
/*
 * fs/ext4.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * ext4, read and written by hand.
 */

#include <velvet/ext4.h>
#include <velvet/jbd2.h>
#include <velvet/string.h>

/*
 * by hand rather than as structs, for the same reason fat32 does it:
 * the layout is little-endian and packed, and a struct that happens to
 * match is a struct that stops matching the moment anybody changes a
 * compiler flag. offsets are the format; nothing else is.
 */

#define SB_OFFSET       1024
#define SB_MAGIC        0xef53

/*
 * the descriptors were this big before 64-bit made them bigger, and
 * `fs->desc_size` is what anything reading one goes by
 */
#define GD_SIZE_BASE    32
#define DIRENT_MIN      8

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}
static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

uint32_t ext4_block_bytes(const struct ext4 *fs)
{
    return fs->block_size;
}

/* ext2 keeps seconds since 1970 and everything above wants a date. */

void ext4_unix_to_time(uint32_t seconds, struct fat32_time *out)
{
    uint32_t days = seconds / 86400;
    uint32_t rest = seconds % 86400;

    out->hour = (uint8_t)(rest / 3600);
    out->minute = (uint8_t)((rest % 3600) / 60);
    out->second = (uint8_t)(rest % 60);

    /*
     * shift the epoch to 1st march 0000, which puts the leap day at the
     * end of the year and makes the month lengths a repeating pattern
     */
    int64_t z = (int64_t)days + 719468;
    int64_t era = z / 146097;
    uint64_t doe = (uint64_t)(z - era * 146097);
    uint64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = (int64_t)yoe + era * 400;
    uint64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    uint64_t mp = (5 * doy + 2) / 153;
    uint64_t d = doy - (153 * mp + 2) / 5 + 1;
    uint64_t m = mp + (mp < 10 ? 3 : -9);

    out->year = (uint16_t)(y + (m <= 2 ? 1 : 0));
    out->month = (uint8_t)m;
    out->day = (uint8_t)d;
}

uint32_t ext4_time_to_unix(const struct fat32_time *t)
{
    int64_t y = t->year;
    unsigned m = t->month, d = t->day;
    if (m <= 2) {
        y--;
    }
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    uint64_t yoe = (uint64_t)(y - era * 400);
    uint64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    uint64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t days = era * 146097 + (int64_t)doe - 719468;

    return (uint32_t)(days * 86400 + t->hour * 3600 + t->minute * 60
                      + t->second);
}



static bool read_block(struct ext4 *fs, uint32_t block, void *into)
{
    if (block == 0 || block >= fs->blocks_count) {
        return false;
    }
    /* the newest version of a block may be in the log rather than at home. */
    if (jbd_peek(&fs->journal, block, into)) {
        return true;
    }
    uint32_t per = fs->block_size / EXT4_SECTOR;
    return fs->read(fs->ctx, (uint64_t)block * per, per, into);
}

static bool raw_write(struct ext4 *fs, uint32_t block, const void *from)
{
    if (fs->write == NULL || block == 0 || block >= fs->blocks_count) {
        return false;
    }
    uint32_t per = fs->block_size / EXT4_SECTOR;
    return fs->write(fs->ctx, (uint64_t)block * per, per, from);
}

/*
 * every metadata write in this driver comes through here, which is what
 * makes journalling a change to one function rather than to fifty.
 *
 * with a transaction open the block goes into the log and *not* to its
 * home, it gets there when the transaction commits, and until then
 * the filesystem on the disk is exactly as it was. that is the whole
 * point: a change is either entirely absent or entirely recorded, and
 * never half-applied
 */
static bool write_block(struct ext4 *fs, uint32_t block, const void *from)
{
    if (jbd_open(&fs->journal)) {
        /* and no falling back if that fails. */
        return jbd_stage(&fs->journal, block, from);
    }
    return raw_write(fs, block, from);
}

/*
 * what the journal needs of the filesystem, and no more: read and write
 * a block without the log in the way, ask where a file's nth block is,
 * and make the disk hold what has been written to it
 */
bool ext4_raw_read(struct ext4 *fs, uint32_t block, void *into)
{
    return read_block(fs, block, into);
}

bool ext4_raw_write(struct ext4 *fs, uint32_t block, const void *from)
{
    return raw_write(fs, block, from);
}

bool ext4_flush(struct ext4 *fs)
{
    return fs->sync == NULL ? true : fs->sync(fs->ctx);
}



/* the descriptors live in a table of their own, and there is one per group. */
static bool read_gd(struct ext4 *fs, uint32_t group, uint8_t *block,
                    uint32_t *offset)
{
    uint32_t per_block = fs->block_size / fs->desc_size;
    uint32_t which = fs->gdt_block + group / per_block;
    if (!read_block(fs, which, block)) {
        return false;
    }
    *offset = (group % per_block) * fs->desc_size;
    return true;
}

static bool write_gd(struct ext4 *fs, uint32_t group, const uint8_t *block)
{
    uint32_t per_block = fs->block_size / fs->desc_size;
    return write_block(fs, fs->gdt_block + group / per_block, block);
}



static bool flush_super(struct ext4 *fs)
{
    if (fs->write == NULL) {
        return false;
    }
    /*
     * the superblock is not block-aligned when blocks are 1 KiB, it
     * is at byte 1024, which is block 1. read, change, write
     */
    /*
     * FIXME: on a filesystem with 2 KiB or 4 KiB blocks this is block 0,
     * and both paths to a block refuse block 0 as a matter of course, so
     * the free counts this function exists to write are never written and
     * nothing says so. the superblock is at byte 1024, which is inside
     * block 0 for any block size above 1 KiB, so this needs a way to say
     * "the first block" rather than a number that already means "none".
     * a 1 KiB filesystem hides the whole thing, and the one this kernel
     * is built on is 1 KiB.
     */
    uint32_t block = SB_OFFSET / fs->block_size;
    if (!read_block(fs, block, fs->scratch)) {
        return false;
    }
    uint8_t *sb = fs->scratch + (SB_OFFSET % fs->block_size);
    wr32(sb + 12, fs->free_blocks);
    wr32(sb + 16, fs->free_inodes);
    return write_block(fs, block, fs->scratch);
}

bool ext4_mount(struct ext4 *fs, ext4_io read, ext4_out write, void *ctx)
{
    memset(fs, 0, sizeof *fs);
    fs->read = read;
    fs->write = write;
    fs->ctx = ctx;

    uint8_t sector[EXT4_SECTOR * 2];
    if (!read(ctx, SB_OFFSET / EXT4_SECTOR, 2, sector)) {
        return false;
    }
    const uint8_t *sb = sector;

    if (rd16(sb + 56) != SB_MAGIC) {
        return false;
    }

    uint32_t log_block = rd32(sb + 24);
    if (log_block > 2) {
        return false;       /* blocks bigger than the kernel keeps scratch for */
    }
    fs->block_size = 1024u << log_block;

    fs->inodes_count = rd32(sb + 0);
    fs->blocks_count = rd32(sb + 4);
    fs->free_blocks = rd32(sb + 12);
    fs->free_inodes = rd32(sb + 16);
    fs->first_data_block = rd32(sb + 20);
    fs->blocks_per_group = rd32(sb + 32);
    fs->inodes_per_group = rd32(sb + 40);

    uint32_t rev = rd32(sb + 76);
    fs->desc_size = 32;
    if (rev >= 1) {
        fs->first_ino = rd32(sb + 84);
        fs->inode_size = rd16(sb + 88);

        fs->compat    = rd32(sb + 92);
        fs->incompat  = rd32(sb + 96);
        fs->ro_compat = rd32(sb + 100);
        fs->reserved_gdt = rd16(sb + 206);

        /*
         * a feature do not implement is a filesystem the kernel would be
         * guessing at, and guessing at a filesystem is how you write
         * over somebody's data. so: refuse, rather than try
         */
        if (fs->incompat & ~(uint32_t)EXT4_INCOMPAT_KNOWN) {
            return false;
        }

        /* and the ones the kernel can read without being able to maintain. */
        if (fs->ro_compat & ~(uint32_t)EXT4_RO_COMPAT_WRITABLE) {
            fs->read_only = true;
        }

        /* a journal with something in it. */
        if (fs->incompat & EXT4_INCOMPAT_RECOVER) {
            fs->read_only = true;
        }

        /*
         * a journal at all. read-only *for now*: the log has to be
         * replayed before anything else is believed, and this driver
         * has to write through it afterwards, both of which need a
         * promise about ordering that only the caller can make. so the
         * disk mounts unwritable and `ext4_start_journal` is what makes
         * it otherwise
         */
        if (fs->compat & EXT4_COMPAT_HAS_JOURNAL) {
            fs->journalled = true;
            fs->read_only = true;
        }

        fs->extents = (fs->incompat & EXT4_INCOMPAT_EXTENTS) != 0;

        /*
         * 64-bit means the group descriptors grew a second half, with
         * the high halves of every block number in it. the size is in
         * the superblock rather than implied, because it is allowed to
         * be larger still one day
         */
        if (fs->incompat & EXT4_INCOMPAT_64BIT) {
            fs->desc_size = rd16(sb + 254);
            if (fs->desc_size < 64 || fs->desc_size > fs->block_size
                || (fs->desc_size & (fs->desc_size - 1)) != 0) {
                return false;
            }
        }
    } else {
        fs->first_ino = 11;
        fs->inode_size = 128;
    }

    /*
     * FIXME: two numbers here size buffers further down and neither is
     * bounded to what those buffers hold. inode_size goes to read_inode,
     * which copies it into a buffer the caller owns, and every one of the
     * seventeen callers declares that buffer as 256 bytes while anything
     * up to block_size is accepted. blocks_per_group and inodes_per_group
     * are handed to the bitmap routines as a count of bits, and those
     * index a fixed EXT4_MAX_BLOCK array, while 8 * block_size is the
     * most a conforming filesystem promises. both are reachable from a
     * superblock the kernel did not write, and this file's own rule is
     * that such a filesystem is refused rather than guessed at. refuse
     * inode_size above 256, and both counts above eight times the block
     * size.
     */
    if (fs->inode_size == 0 || fs->inode_size > fs->block_size
        || fs->blocks_per_group == 0 || fs->inodes_per_group == 0
        || fs->desc_size < GD_SIZE_BASE) {
        return false;
    }

    /*
     * a block number here is thirty-two bits wide, so a filesystem
     * whose count does not fit in that is one where every descriptor's
     * high half would matter and every one of them is read as zero.
     * with 4 KiB blocks that ceiling is sixteen terabytes, which is
     * further than this machine is going, but saying so beats
     * truncating a number and writing where it points
     */
    if (rd32(sb + 336) != 0) {
        return false;
    }

    for (int i = 0; i < 16; i++) {
        fs->label[i] = (char)sb[120 + i];
    }
    fs->label[16] = '\0';

    fs->groups = (fs->blocks_count - fs->first_data_block
                  + fs->blocks_per_group - 1) / fs->blocks_per_group;
    fs->gdt_block = fs->first_data_block + 1;

    /*
     * a filesystem the kernel may read and must not change loses the way to
     * change it, rather than being trusted to remember not to. every
     * writing path already refuses when there is no write function, so
     * there is one rule here instead of fifteen, and `read_only` is
     * kept beside it so that `mount` can say which of the two reasons
     * it was: the caller asked, or the disk did
     */
    if (fs->read_only) {
        /*
         * held rather than dropped when the journal is the only reason,
         * since starting the journal is what gives it back
         */
        /*
         * a journal wanting recovery is the case this is *for*: the
         * replay is the thing that makes the disk trustworthy again,
         * and refusing it would refuse every disk unplugged carelessly
         */
        if (fs->journalled
            && !(fs->ro_compat & ~(uint32_t)EXT4_RO_COMPAT_WRITABLE)) {
            fs->pending_write = fs->write;
        }
        fs->write = NULL;
    }

    fs->mounted = true;
    return true;
}

void ext4_set_clock(struct ext4 *fs, ext4_clock clock)
{
    fs->clock = clock;
}

void ext4_set_sync(struct ext4 *fs, ext4_sync sync)
{
    fs->sync = sync;
}

static uint32_t map_block(struct ext4 *fs, uint8_t *inode, uint32_t index,
                          bool grow, bool *changed);
static bool read_inode(struct ext4 *fs, uint32_t ino, uint8_t *out);

/* the one flag on the filesystem's own superblock that the journal owns. */
static bool set_recover(struct ext4 *fs, bool needed)
{
    uint32_t want = needed ? (fs->incompat | (uint32_t)EXT4_INCOMPAT_RECOVER)
                           : (fs->incompat & ~(uint32_t)EXT4_INCOMPAT_RECOVER);
    if (want == fs->incompat) {
        return true;
    }
    uint8_t sector[EXT4_SECTOR * 2];
    if (!fs->read(fs->ctx, SB_OFFSET / EXT4_SECTOR, 2, sector)) {
        return false;
    }
    wr32(sector + 96, want);
    if (!fs->write(fs->ctx, SB_OFFSET / EXT4_SECTOR, 2, sector)
        || !ext4_flush(fs)) {
        return false;
    }
    fs->incompat = want;
    return true;
}

/* start journalling, once somebody has promised the ordering. */
bool ext4_start_journal(struct ext4 *fs)
{
    if (!fs->mounted || !fs->journalled || fs->sync == NULL
        || fs->pending_write == NULL) {
        return false;
    }

    /*
     * the log is replayed with the write function in place, because
     * replaying *is* writing, and if it fails, the filesystem stays
     * exactly as read-only as it was
     */
    fs->write = fs->pending_write;
    if (!jbd_mount(&fs->journal, fs)) {
        fs->write = NULL;
        return false;
    }

    /* and now say the disk needs recovering, which it is about to. */
    if (!set_recover(fs, true)) {
        fs->write = NULL;
        return false;
    }

    fs->read_only = false;
    fs->pending_write = NULL;
    return true;
}

/* and the other end of it: the disk is finished with. */
bool ext4_stop_journal(struct ext4 *fs)
{
    if (!fs->mounted || !fs->journal.ready || fs->write == NULL) {
        return false;
    }

    bool ok = ext4_flush(fs);
    ok = jbd_close(&fs->journal) && ok;
    ok = set_recover(fs, false) && ok;

    fs->journal.ready = false;
    fs->pending_write = fs->write;
    fs->write = NULL;
    fs->read_only = true;
    return ok;
}

/*
 * where the nth block of a file lives, for whoever needs to walk a file
 * without being the filesystem. the journal is the only caller and it
 * is a file like any other, which is what lets the log be laid out in
 * extents, anywhere on the disk, without fs/jbd2.c knowing either
 */
bool ext4_file_block(struct ext4 *fs, uint32_t ino, uint32_t index,
                     uint32_t *out)
{
    uint8_t inode[256];
    if (!fs->mounted || !read_inode(fs, ino, inode)) {
        return false;
    }
    bool changed = false;
    uint32_t block = map_block(fs, inode, index, false, &changed);
    if (block == 0) {
        return false;
    }
    *out = block;
    return true;
}



/* an inode's 128 bytes, copied out. */
static bool read_inode(struct ext4 *fs, uint32_t ino, uint8_t *out)
{
    if (ino == 0 || ino > fs->inodes_count) {
        return false;
    }
    uint32_t group = (ino - 1) / fs->inodes_per_group;
    uint32_t index = (ino - 1) % fs->inodes_per_group;

    uint32_t off;
    if (!read_gd(fs, group, fs->scratch, &off)) {
        return false;
    }
    uint32_t table = rd32(fs->scratch + off + 8);

    uint32_t per_block = fs->block_size / fs->inode_size;
    uint32_t block = table + index / per_block;
    uint32_t within = (index % per_block) * fs->inode_size;

    if (!read_block(fs, block, fs->scratch)) {
        return false;
    }
    memcpy(out, fs->scratch + within, fs->inode_size);
    return true;
}

static bool write_inode(struct ext4 *fs, uint32_t ino, const uint8_t *raw)
{
    if (fs->write == NULL || ino == 0 || ino > fs->inodes_count) {
        return false;
    }
    uint32_t group = (ino - 1) / fs->inodes_per_group;
    uint32_t index = (ino - 1) % fs->inodes_per_group;

    uint32_t off;
    if (!read_gd(fs, group, fs->scratch, &off)) {
        return false;
    }
    uint32_t table = rd32(fs->scratch + off + 8);

    uint32_t per_block = fs->block_size / fs->inode_size;
    uint32_t block = table + index / per_block;
    uint32_t within = (index % per_block) * fs->inode_size;

    if (!read_block(fs, block, fs->scratch)) {
        return false;
    }
    memcpy(fs->scratch + within, raw, fs->inode_size);
    return write_block(fs, block, fs->scratch);
}

static bool is_fast_symlink(const uint8_t *inode)
{
    /* the one place in ext2 where those fifteen words are not block numbers at all. */
    return (rd16(inode) & EXT4_S_IFMT) == EXT4_S_IFLNK && rd32(inode + 4) < 60;
}

static void fill_file(struct ext4 *fs, uint32_t ino, const uint8_t *raw,
                      struct ext4_file *out)
{
    (void)fs;
    memset(out, 0, sizeof *out);
    out->ino = ino;
    out->mode = rd16(raw);
    out->uid = rd16(raw + 2);
    out->size = rd32(raw + 4);
    out->gid = rd16(raw + 24);
    out->links = rd16(raw + 26);
    out->is_dir = (out->mode & EXT4_S_IFMT) == EXT4_S_IFDIR;
    out->is_symlink = (out->mode & EXT4_S_IFMT) == EXT4_S_IFLNK;

    ext4_unix_to_time(rd32(raw + 8), &out->accessed);
    ext4_unix_to_time(rd32(raw + 12), &out->created);
    ext4_unix_to_time(rd32(raw + 16), &out->modified);
}

static void stamp(struct ext4 *fs, uint8_t *raw, bool created)
{
    if (fs->clock == NULL) {
        return;
    }
    uint32_t now = fs->clock();
    wr32(raw + 8, now);         /* accessed */
    wr32(raw + 16, now);        /* modified */
    if (created) {
        wr32(raw + 12, now);
    }
}



static bool bitmap_take(struct ext4 *fs, uint32_t bitmap_block, uint32_t count,
                        uint32_t *found)
{
    if (!read_block(fs, bitmap_block, fs->indirect)) {
        return false;
    }
    for (uint32_t i = 0; i < count; i++) {
        if (fs->indirect[i / 8] & (1u << (i % 8))) {
            continue;
        }
        fs->indirect[i / 8] |= (uint8_t)(1u << (i % 8));
        if (!write_block(fs, bitmap_block, fs->indirect)) {
            return false;
        }
        *found = i;
        return true;
    }
    return false;
}

static bool bitmap_give(struct ext4 *fs, uint32_t bitmap_block, uint32_t bit)
{
    if (!read_block(fs, bitmap_block, fs->indirect)) {
        return false;
    }
    fs->indirect[bit / 8] &= (uint8_t)~(1u << (bit % 8));
    return write_block(fs, bitmap_block, fs->indirect);
}

/* a free block, from whichever group has one. */
static uint32_t alloc_block(struct ext4 *fs, bool journalled)
{
    if (fs->write == NULL) {
        return 0;
    }
    for (uint32_t g = 0; g < fs->groups; g++) {
        uint32_t off;
        if (!read_gd(fs, g, fs->scratch, &off)) {
            return 0;
        }
        if (rd16(fs->scratch + off + 12) == 0) {
            continue;
        }
        uint32_t bitmap = rd32(fs->scratch + off + 0);

        uint32_t bit;
        if (!bitmap_take(fs, bitmap, fs->blocks_per_group, &bit)) {
            continue;
        }

        /*
         * the descriptor was read into scratch and bitmap_take used
         * indirect, so scratch is still the descriptor block
         */
        if (!read_gd(fs, g, fs->scratch, &off)) {
            return 0;
        }
        wr16(fs->scratch + off + 12, (uint16_t)(rd16(fs->scratch + off + 12) - 1));
        if (!write_gd(fs, g, fs->scratch)) {
            return 0;
        }

        fs->free_blocks--;
        flush_super(fs);

        uint32_t block = fs->first_data_block + g * fs->blocks_per_group + bit;
        memset(fs->indirect, 0, fs->block_size);
        if (!(journalled ? write_block(fs, block, fs->indirect)
                         : raw_write(fs, block, fs->indirect))) {
            return 0;
        }
        return block;
    }
    return 0;
}

static bool free_block(struct ext4 *fs, uint32_t block)
{
    if (fs->write == NULL || block < fs->first_data_block
        || block >= fs->blocks_count) {
        return false;
    }
    uint32_t g = (block - fs->first_data_block) / fs->blocks_per_group;
    uint32_t bit = (block - fs->first_data_block) % fs->blocks_per_group;

    uint32_t off;
    if (!read_gd(fs, g, fs->scratch, &off)) {
        return false;
    }
    uint32_t bitmap = rd32(fs->scratch + off + 0);
    if (!bitmap_give(fs, bitmap, bit)) {
        return false;
    }

    if (!read_gd(fs, g, fs->scratch, &off)) {
        return false;
    }
    wr16(fs->scratch + off + 12, (uint16_t)(rd16(fs->scratch + off + 12) + 1));
    if (!write_gd(fs, g, fs->scratch)) {
        return false;
    }

    fs->free_blocks++;
    return flush_super(fs);
}

static uint32_t alloc_inode(struct ext4 *fs, bool directory)
{
    if (fs->write == NULL) {
        return 0;
    }
    for (uint32_t g = 0; g < fs->groups; g++) {
        uint32_t off;
        if (!read_gd(fs, g, fs->scratch, &off)) {
            return 0;
        }
        if (rd16(fs->scratch + off + 14) == 0) {
            continue;
        }
        uint32_t bitmap = rd32(fs->scratch + off + 4);

        uint32_t bit;
        if (!bitmap_take(fs, bitmap, fs->inodes_per_group, &bit)) {
            continue;
        }

        uint32_t ino = g * fs->inodes_per_group + bit + 1;
        if (ino < fs->first_ino && ino != EXT4_ROOT_INO) {
            /*
             * a reserved number. give it back and keep looking, rather
             * than handing out an inode the format has plans for
             */
            bitmap_give(fs, bitmap, bit);
            continue;
        }

        if (!read_gd(fs, g, fs->scratch, &off)) {
            return 0;
        }
        wr16(fs->scratch + off + 14,
             (uint16_t)(rd16(fs->scratch + off + 14) - 1));
        if (directory) {
            wr16(fs->scratch + off + 16,
                 (uint16_t)(rd16(fs->scratch + off + 16) + 1));
        }
        if (!write_gd(fs, g, fs->scratch)) {
            return 0;
        }

        fs->free_inodes--;
        flush_super(fs);
        return ino;
    }
    return 0;
}

static bool free_inode(struct ext4 *fs, uint32_t ino, bool directory)
{
    uint32_t g = (ino - 1) / fs->inodes_per_group;
    uint32_t bit = (ino - 1) % fs->inodes_per_group;

    uint32_t off;
    if (!read_gd(fs, g, fs->scratch, &off)) {
        return false;
    }
    uint32_t bitmap = rd32(fs->scratch + off + 4);
    if (!bitmap_give(fs, bitmap, bit)) {
        return false;
    }

    if (!read_gd(fs, g, fs->scratch, &off)) {
        return false;
    }
    wr16(fs->scratch + off + 14, (uint16_t)(rd16(fs->scratch + off + 14) + 1));
    if (directory && rd16(fs->scratch + off + 16) > 0) {
        wr16(fs->scratch + off + 16,
             (uint16_t)(rd16(fs->scratch + off + 16) - 1));
    }
    if (!write_gd(fs, g, fs->scratch)) {
        return false;
    }

    fs->free_inodes++;
    return flush_super(fs);
}

/*
 * ext2's answer to "where is block N of this file" is a list of block
 * numbers, one per block, with indirection once the list outgrows the
 * inode. it is exact, and it is enormous: a gigabyte in 1 KiB blocks is
 * a million numbers, four megabytes of pointers, and reading the file
 * means reading them.
 *
 * an extent says the other thing: *the next N logical blocks live at
 * physical block P*. a file written in one go is one extent whatever
 * its size, and the twelve bytes that say so sit in the inode. that is
 * the difference ext4 is actually about, not a bigger ext2, a
 * different sentence about where a file is.
 *
 * the shape is a tree, and the header at the top of every node says how
 * deep it is. depth 0 means the twelve-byte records after the header
 * are extents. deeper means they are indices, each naming a block with
 * another header at the top of it. the inode's sixty bytes hold a
 * header and four records, so a file in four pieces or fewer costs no
 * blocks at all to describe.
 *
 *
 * reading walks any depth up to five, which is deeper than ext4 itself
 * goes. writing builds depth 0 and depth 1, which with these blocks is
 * four leaves of eighty-four extents apiece, three hundred and
 * thirty-six runs, any of them up to thirty-two thousand blocks. a file
 * this machine cannot describe that way is a file it has no room for.
 * deeper than that is refused rather than half-built.
 */

#define EH_MAGIC        0       /* the offsets inside a node's header */
#define EH_ENTRIES      2
#define EH_MAX          4
#define EH_DEPTH        6
#define EH_SIZE         12
#define EXTENT_SIZE     12

/* the sixty bytes an inode keeps its block map in, whichever kind it is */
static uint8_t *inode_map(uint8_t *inode)
{
    return inode + 40;
}

static uint32_t root_max(void)
{
    return (60 - EH_SIZE) / EXTENT_SIZE;
}
static uint32_t leaf_max(const struct ext4 *fs)
{
    return (fs->block_size - EH_SIZE) / EXTENT_SIZE;
}

static bool has_extents(const uint8_t *inode)
{
    return (rd32(inode + 32) & EXT4_EXTENTS_FL) != 0;
}

/* a node the kernel is willing to walk. */
static bool node_ok(const uint8_t *node, uint32_t max_here)
{
    if (rd16(node + EH_MAGIC) != EXT4_EXTENT_MAGIC) {
        return false;
    }
    uint32_t entries = rd16(node + EH_ENTRIES);
    uint32_t max = rd16(node + EH_MAX);
    return entries <= max && max <= max_here && rd16(node + EH_DEPTH) <= 5;
}

static void node_init(uint8_t *node, uint32_t max, uint32_t depth)
{
    memset(node, 0, EH_SIZE);
    wr16(node + EH_MAGIC, EXT4_EXTENT_MAGIC);
    wr16(node + EH_ENTRIES, 0);
    wr16(node + EH_MAX, (uint16_t)max);
    wr16(node + EH_DEPTH, (uint16_t)depth);
}

/*
 * a leaf record. the length's top bit means "allocated and never
 * written", which a reader is meant to see as zeroes, and does here,
 * since nothing this driver writes ever sets it
 */
static uint32_t ee_block(const uint8_t *e)
{
    return rd32(e + 0);
}
static uint32_t ee_len(const uint8_t *e)
{
    uint32_t len = rd16(e + 4);
    return len > 32768 ? len - 32768 : len;
}
static bool ee_written(const uint8_t *e)
{
    return rd16(e + 4) <= 32768;
}
static uint64_t ee_start(const uint8_t *e)
{
    return ((uint64_t)rd16(e + 6) << 32) | rd32(e + 8);
}
static uint64_t ei_leaf(const uint8_t *e)
{
    return ((uint64_t)rd16(e + 8) << 32) | rd32(e + 4);
}

/*
 * the index whose subtree holds `index`: the last one that does not
 * start after it
 */
static uint32_t descend(const uint8_t *node, uint32_t entries, uint32_t index)
{
    uint32_t which = entries;           /* entries means "none of them" */
    for (uint32_t i = 0; i < entries; i++) {
        if (rd32(node + EH_SIZE + i * EXTENT_SIZE) > index) {
            break;
        }
        which = i;
    }
    return which;
}

/*
 * the physical block holding logical block `index`, or 0 for a hole,
 * which a file is allowed to have, and which reads as zeroes
 */
static uint32_t extent_lookup(struct ext4 *fs, uint8_t *inode,
                              uint32_t index)
{
    uint8_t *node = inode_map(inode);
    uint32_t max_here = root_max();

    /* a tree is at most five deep, so five descents is the whole of it. */
    for (uint32_t descents = 0; descents <= 5; descents++) {
        if (!node_ok(node, max_here)) {
            return 0;
        }
        uint32_t entries = rd16(node + EH_ENTRIES);
        uint32_t depth = rd16(node + EH_DEPTH);
        uint8_t *rec = node + EH_SIZE;

        if (depth == 0) {
            for (uint32_t i = 0; i < entries; i++) {
                const uint8_t *e = rec + i * EXTENT_SIZE;
                uint32_t first = ee_block(e);
                if (index >= first && index < first + ee_len(e)) {
                    uint64_t at = ee_start(e) + (index - first);
                    return at > 0xffffffffu ? 0 : (uint32_t)at;
                }
            }
            return 0;
        }

        uint32_t which = descend(node, entries, index);
        if (which == entries) {
            return 0;
        }
        uint64_t child = ei_leaf(rec + which * EXTENT_SIZE);
        if (child == 0 || child > 0xffffffffu
            || !read_block(fs, (uint32_t)child, fs->indirect)) {
            return 0;
        }
        node = fs->indirect;
        max_here = leaf_max(fs);
    }
    return 0;               /* deeper than a tree can be: a cycle */
}

/* one more block into a leaf held in memory. */
static bool leaf_put(uint8_t *leaf, uint32_t max, uint32_t index,
                     uint32_t block)
{
    /*
     * NOTE: entries is the kernel's own. a leaf reaches this function only
     * after being built or reinitialised here, and one read from the disk
     * goes through node_ok() first, so the read below is bounded by max
     * without a check of its own.
     */
    uint32_t entries = rd16(leaf + EH_ENTRIES);

    if (entries > 0) {
        uint8_t *last = leaf + EH_SIZE + (entries - 1) * EXTENT_SIZE;
        uint32_t first = ee_block(last);
        uint32_t len = ee_len(last);
        if (ee_written(last) && len < 32767
            && index == first + len && ee_start(last) + len == block) {
            wr16(last + 4, (uint16_t)(len + 1));
            return true;
        }
    }

    if (entries >= max) {
        return false;
    }
    uint8_t *e = leaf + EH_SIZE + entries * EXTENT_SIZE;
    wr32(e + 0, index);
    wr16(e + 4, 1);
    wr16(e + 6, 0);             /* the high half of a block number the kernel has already refused to exceed */
    wr32(e + 8, block);
    wr16(leaf + EH_ENTRIES, (uint16_t)(entries + 1));
    return true;
}

/*
 * the inode's four records into a leaf block of their own, leaving the
 * inode an index one entry deep. this is the only place the tree gets
 * taller, and it happens exactly once per file
 */
static bool spill_root(struct ext4 *fs, uint8_t *root)
{
    uint32_t leaf = alloc_block(fs, true);
    if (leaf == 0) {
        return false;
    }
    uint32_t entries = rd16(root + EH_ENTRIES);

    /*
     * alloc_block zeroed the new block through `indirect`, so it is
     * free to build the leaf in
     */
    memset(fs->indirect, 0, fs->block_size);
    node_init(fs->indirect, leaf_max(fs), 0);
    wr16(fs->indirect + EH_ENTRIES, (uint16_t)entries);
    memcpy(fs->indirect + EH_SIZE, root + EH_SIZE, entries * EXTENT_SIZE);
    if (!write_block(fs, leaf, fs->indirect)) {
        free_block(fs, leaf);
        return false;
    }

    uint32_t first = rd32(root + EH_SIZE);
    memset(root, 0, 60);
    node_init(root, root_max(), 1);
    wr16(root + EH_ENTRIES, 1);
    wr32(root + EH_SIZE + 0, first);
    wr32(root + EH_SIZE + 4, leaf);
    wr16(root + EH_SIZE + 8, 0);
    return true;
}

/* make a block for logical `index` and record where it went. */
static uint32_t extent_grow(struct ext4 *fs, uint8_t *inode, uint32_t index,
                            bool journalled, bool *changed)
{
    uint8_t *root = inode_map(inode);
    if (!node_ok(root, root_max())) {
        return 0;
    }
    if (rd16(root + EH_DEPTH) > 1) {
        return 0;       /* deeper than this writes, and said so above */
    }

    uint32_t block = alloc_block(fs, journalled);
    if (block == 0) {
        return 0;
    }

    if (rd16(root + EH_DEPTH) == 0) {
        if (leaf_put(root, root_max(), index, block)) {
            *changed = true;
            return block;
        }
        if (!spill_root(fs, root)) {
            free_block(fs, block);
            return 0;
        }
        *changed = true;
    }

    /*
     * depth one: the leaf that ends nearest the block being added, and
     * a new one beside it when that leaf is full
     */
    uint32_t entries = rd16(root + EH_ENTRIES);
    uint32_t which = descend(root, entries, index);
    if (which == entries) {
        which = entries - 1;    /* before everything: still the first leaf */
    }
    uint8_t *slot = root + EH_SIZE + which * EXTENT_SIZE;
    uint64_t leaf = ei_leaf(slot);

    if (leaf == 0 || leaf > 0xffffffffu
        || !read_block(fs, (uint32_t)leaf, fs->indirect)
        || !node_ok(fs->indirect, leaf_max(fs))
        || rd16(fs->indirect + EH_DEPTH) != 0) {
        free_block(fs, block);
        return 0;
    }

    if (leaf_put(fs->indirect, leaf_max(fs), index, block)) {
        if (!write_block(fs, (uint32_t)leaf, fs->indirect)) {
            free_block(fs, block);
            return 0;
        }
        return block;
    }

    /* that leaf is full. another one, if the inode has room to name it */
    if (entries >= root_max()) {
        free_block(fs, block);
        return 0;
    }
    uint32_t fresh = alloc_block(fs, true);
    if (fresh == 0) {
        free_block(fs, block);
        return 0;
    }
    memset(fs->indirect, 0, fs->block_size);
    node_init(fs->indirect, leaf_max(fs), 0);
    if (!leaf_put(fs->indirect, leaf_max(fs), index, block)
        || !write_block(fs, fresh, fs->indirect)) {
        free_block(fs, fresh);
        free_block(fs, block);
        return 0;
    }

    uint8_t *e = root + EH_SIZE + entries * EXTENT_SIZE;
    wr32(e + 0, index);
    wr32(e + 4, fresh);
    wr16(e + 8, 0);
    wr16(root + EH_ENTRIES, (uint16_t)(entries + 1));
    *changed = true;
    return block;
}

/* every block an extent-mapped file owns, given back. */
static bool free_extents(struct ext4 *fs, uint8_t *inode)
{
    uint8_t *root = inode_map(inode);
    if (!node_ok(root, root_max())) {
        return false;
    }
    uint32_t depth = rd16(root + EH_DEPTH);
    uint32_t entries = rd16(root + EH_ENTRIES);
    /*
     * FIXME: the read path walks an extent tree five deep and this
     * refuses anything past one, so unlinking a file whose tree is deeper
     * frees nothing and has no way to say so: free_all_blocks returns
     * void and its only caller looks at free_inode. the name goes, the
     * inode goes, and every block the file owned is stranded, which is
     * space nothing reaches and nothing will ever reclaim. walk the
     * leaves the way extent_lookup already does, or report the refusal so
     * the caller can leave the name alone rather than delete one it
     * cannot pay for.
     */
    if (depth > 1) {
        return false;
    }

    /*
     * the runs of one leaf, copied out before any of them is freed,
     * freeing reads a bitmap into the same buffer the leaf is in
     */
    /*
     * XXX: static, so one set for the whole machine. two cores unlinking
     * files at the same instant fill the same arrays and each frees a
     * mixture of the two, which is a bitmap given back for blocks that
     * are still in use. the note in free_all_blocks explains why a copy
     * is needed at all and is right; what it does not say is that these
     * are shared. make them locals: the frames are shallow, and this is
     * a path a program can reach from any core. the same goes for the
     * three in free_all_blocks below.
     */
    static uint32_t starts[EXT4_MAX_BLOCK / EXTENT_SIZE];
    static uint32_t lengths[EXT4_MAX_BLOCK / EXTENT_SIZE];

    uint32_t leaves[(60 - EH_SIZE) / EXTENT_SIZE];
    uint32_t leaf_count = 0;

    if (depth == 1) {
        for (uint32_t i = 0; i < entries && i < root_max(); i++) {
            leaves[leaf_count++] = (uint32_t)ei_leaf(root + EH_SIZE
                                                     + i * EXTENT_SIZE);
        }
    }

    for (uint32_t l = 0; l == 0 || l < leaf_count; l++) {
        uint8_t *node = root;
        if (depth == 1) {
            if (leaf_count == 0) {
                break;
            }
            if (!read_block(fs, leaves[l], fs->indirect)
                || !node_ok(fs->indirect, leaf_max(fs))) {
                continue;
            }
            node = fs->indirect;
        }

        uint32_t n = rd16(node + EH_ENTRIES);
        for (uint32_t i = 0; i < n; i++) {
            const uint8_t *e = node + EH_SIZE + i * EXTENT_SIZE;
            starts[i] = (uint32_t)ee_start(e);
            lengths[i] = ee_len(e);
        }
        for (uint32_t i = 0; i < n; i++) {
            for (uint32_t k = 0; k < lengths[i]; k++) {
                free_block(fs, starts[i] + k);
            }
        }
        if (depth == 1) {
            free_block(fs, leaves[l]);
        }
        if (depth == 0) {
            break;
        }
    }

    memset(root, 0, 60);
    node_init(root, root_max(), 0);
    return true;
}

/*
 * the older way, and still the one an ext2 inode on an ext4 filesystem
 * uses: twelve direct, then one indirect, then double, then triple. the
 * arithmetic is the same at each level and the temptation is to write
 * it once and recurse, but a recursive walk needs a block of scratch
 * per level, and there are two. so it is spelled out
 */

static uint32_t per_block(const struct ext4 *fs)
{
    return fs->block_size / 4;
}

/* the block holding the `index`th block of a file. */
static uint32_t map_block(struct ext4 *fs, uint8_t *inode, uint32_t index,
                          bool grow, bool *changed)
{
    uint32_t n = per_block(fs);

    /*
     * a directory's blocks are metadata and a file's are not, and that
     * one line decides how a new one is zeroed. it is the same rule the
     * writes themselves follow, `dir_add` goes through `write_block`
     * and `write_bytes` goes through `raw_write`, and it has to be,
     * because a block zeroed one way and filled the other is a block
     * the commit overwrites
     */
    bool journalled = (rd16(inode) & EXT4_S_IFMT) == EXT4_S_IFDIR;

    /*
     * which of the two maps this file uses is a bit in the inode, not a
     * property of the filesystem, an ext4 disk holds both, since a
     * file made before the feature was turned on keeps the map it was
     * born with, and rewriting it would be rewriting every file on the
     * disk to change one flag
     */
    if (has_extents(inode)) {
        uint32_t block = extent_lookup(fs, inode, index);
        if (block != 0 || !grow) {
            return block;
        }
        return extent_grow(fs, inode, index, journalled, changed);
    }

    if (index < EXT4_DIRECT) {
        uint32_t b = rd32(inode + 40 + index * 4);
        if (b == 0 && grow) {
            b = alloc_block(fs, journalled);
            if (b == 0) {
                return 0;
            }
            wr32(inode + 40 + index * 4, b);
            *changed = true;
        }
        return b;
    }
    index -= EXT4_DIRECT;

    if (index < n) {
        uint32_t ind = rd32(inode + 40 + 12 * 4);
        if (ind == 0) {
            if (!grow) {
                return 0;
            }
            ind = alloc_block(fs, true);
            if (ind == 0) {
                return 0;
            }
            wr32(inode + 40 + 12 * 4, ind);
            *changed = true;
        }
        if (!read_block(fs, ind, fs->indirect)) {
            return 0;
        }
        uint32_t b = rd32(fs->indirect + index * 4);
        if (b == 0 && grow) {
            b = alloc_block(fs, journalled);
            if (b == 0) {
                return 0;
            }
            /*
             * alloc_block used `indirect` to zero the new block, so it
             * has to be read again before being changed
             */
            if (!read_block(fs, ind, fs->indirect)) {
                return 0;
            }
            wr32(fs->indirect + index * 4, b);
            if (!write_block(fs, ind, fs->indirect)) {
                return 0;
            }
        }
        return b;
    }
    index -= n;

    if (index < n * n) {
        uint32_t dind = rd32(inode + 40 + 13 * 4);
        if (dind == 0) {
            if (!grow) {
                return 0;
            }
            dind = alloc_block(fs, true);
            if (dind == 0) {
                return 0;
            }
            wr32(inode + 40 + 13 * 4, dind);
            *changed = true;
        }
        if (!read_block(fs, dind, fs->indirect)) {
            return 0;
        }
        uint32_t which = index / n;
        uint32_t ind = rd32(fs->indirect + which * 4);
        if (ind == 0) {
            if (!grow) {
                return 0;
            }
            ind = alloc_block(fs, true);
            if (ind == 0) {
                return 0;
            }
            if (!read_block(fs, dind, fs->indirect)) {
                return 0;
            }
            wr32(fs->indirect + which * 4, ind);
            if (!write_block(fs, dind, fs->indirect)) {
                return 0;
            }
        }
        if (!read_block(fs, ind, fs->indirect)) {
            return 0;
        }
        uint32_t at = index % n;
        uint32_t b = rd32(fs->indirect + at * 4);
        if (b == 0 && grow) {
            b = alloc_block(fs, journalled);
            if (b == 0) {
                return 0;
            }
            if (!read_block(fs, ind, fs->indirect)) {
                return 0;
            }
            wr32(fs->indirect + at * 4, b);
            if (!write_block(fs, ind, fs->indirect)) {
                return 0;
            }
        }
        return b;
    }

    /*
     * triple indirection reaches sixteen gigabytes with these blocks,
     * and nothing here is going to. saying so beats pretending
     */
    return 0;
}

/* every block of a file, given back one at a time. */
static void free_all_blocks(struct ext4 *fs, uint8_t *inode)
{
    if (is_fast_symlink(inode)) {
        return;
    }
    if (has_extents(inode)) {
        free_extents(fs, inode);
        return;
    }
    uint32_t n = per_block(fs);

    for (uint32_t i = 0; i < EXT4_DIRECT; i++) {
        uint32_t b = rd32(inode + 40 + i * 4);
        if (b) {
            free_block(fs, b);
            wr32(inode + 40 + i * 4, 0);
        }
    }

    uint32_t ind = rd32(inode + 40 + 12 * 4);
    if (ind) {
        if (read_block(fs, ind, fs->indirect)) {
            /*
             * copied out, because freeing each one reads the bitmap
             * into the same buffer
             */
            static uint32_t list[EXT4_MAX_BLOCK / 4];
            for (uint32_t i = 0; i < n; i++) {
                list[i] = rd32(fs->indirect + i * 4);
            }
            for (uint32_t i = 0; i < n; i++) {
                if (list[i]) {
                    free_block(fs, list[i]);
                }
            }
        }
        free_block(fs, ind);
        wr32(inode + 40 + 12 * 4, 0);
    }

    uint32_t dind = rd32(inode + 40 + 13 * 4);
    if (dind) {
        static uint32_t outer[EXT4_MAX_BLOCK / 4];
        if (read_block(fs, dind, fs->indirect)) {
            for (uint32_t i = 0; i < n; i++) {
                outer[i] = rd32(fs->indirect + i * 4);
            }
            for (uint32_t i = 0; i < n; i++) {
                if (!outer[i]) {
                    continue;
                }
                if (read_block(fs, outer[i], fs->indirect)) {
                    static uint32_t inner[EXT4_MAX_BLOCK / 4];
                    for (uint32_t j = 0; j < n; j++) {
                        inner[j] = rd32(fs->indirect + j * 4);
                    }
                    for (uint32_t j = 0; j < n; j++) {
                        if (inner[j]) {
                            free_block(fs, inner[j]);
                        }
                    }
                }
                free_block(fs, outer[i]);
            }
        }
        free_block(fs, dind);
        wr32(inode + 40 + 13 * 4, 0);
    }
}



static int64_t read_bytes(struct ext4 *fs, uint8_t *inode, uint64_t offset,
                          void *buf, uint64_t len)
{
    uint64_t size = rd32(inode + 4);

    if (is_fast_symlink(inode)) {
        if (offset >= size) {
            return 0;
        }
        if (offset + len > size) {
            len = size - offset;
        }
        memcpy(buf, inode + 40 + offset, len);
        return (int64_t)len;
    }

    if (offset >= size) {
        return 0;
    }
    if (offset + len > size) {
        len = size - offset;
    }

    uint8_t *out = buf;
    uint64_t done = 0;
    bool changed = false;

    while (done < len) {
        uint64_t at = offset + done;
        uint32_t index = (uint32_t)(at / fs->block_size);
        uint32_t within = (uint32_t)(at % fs->block_size);

        uint32_t take = fs->block_size - within;
        if (take > len - done) {
            take = (uint32_t)(len - done);
        }

        uint32_t block = map_block(fs, inode, index, false, &changed);
        if (block == 0) {
            /*
             * a hole. ext2 allows them and they read as zeroes, which
             * is not the same as an error and must not be treated as
             * one
             */
            memset(out + done, 0, take);
        } else {
            if (!read_block(fs, block, fs->scratch)) {
                return done > 0 ? (int64_t)done : -1;
            }
            memcpy(out + done, fs->scratch + within, take);
        }
        done += take;
    }
    return (int64_t)done;
}

static int64_t write_bytes(struct ext4 *fs, uint32_t ino, uint8_t *inode,
                           uint64_t offset, const void *buf, uint64_t len)
{
    if (fs->write == NULL) {
        return -1;
    }
    if ((rd16(inode) & EXT4_S_IFMT) == EXT4_S_IFDIR) {
        return -1;
    }

    const uint8_t *in = buf;
    uint64_t done = 0;
    bool changed = false;

    while (done < len) {
        uint64_t at = offset + done;
        uint32_t index = (uint32_t)(at / fs->block_size);
        uint32_t within = (uint32_t)(at % fs->block_size);

        uint32_t take = fs->block_size - within;
        if (take > len - done) {
            take = (uint32_t)(len - done);
        }

        uint32_t block = map_block(fs, inode, index, true, &changed);
        if (block == 0) {
            break;      /* out of room. keep what went */
        }

        if (take == fs->block_size) {
            memcpy(fs->scratch, in + done, take);
        } else {
            if (!read_block(fs, block, fs->scratch)) {
                break;
            }
            memcpy(fs->scratch + within, in + done, take);
        }
        /* a file's *contents* go to the disk rather than through the log. */
        if (!raw_write(fs, block, fs->scratch)) {
            break;
        }
        done += take;
    }

    uint64_t size = rd32(inode + 4);
    if (offset + done > size) {
        wr32(inode + 4, (uint32_t)(offset + done));
    }
    stamp(fs, inode, false);
    if (!write_inode(fs, ino, inode)) {
        return -1;
    }
    return (int64_t)done;
}

/*
 * a directory is a file whose contents are entries, and every entry's
 * rec_len says where the next one starts. the last entry in each block
 * stretches to the end of it, which is what makes deleting one a
 * matter of widening the entry before it rather than moving anything
 */

struct dirent_at {
    uint32_t block_index;       /* which block of the directory */
    uint32_t offset;            /* where in it */
    uint32_t prev_offset;       /* the entry before, or the same if first */
    uint32_t ino;
    uint16_t rec_len;
    uint8_t  name_len;
    uint8_t  file_type;
};

static uint32_t entry_size(uint8_t name_len)
{
    return ((uint32_t)DIRENT_MIN + name_len + 3) & ~3u;
}

/*
 * walk every entry of a directory, calling nothing, the caller drives
 * it by index so that the block stays in scratch between calls
 */
static bool dir_scan(struct ext4 *fs, uint8_t *dir_inode, const char *want,
                     size_t want_index, struct dirent_at *out, char *name_out)
{
    uint64_t size = rd32(dir_inode + 4);
    uint32_t blocks = (uint32_t)((size + fs->block_size - 1) / fs->block_size);
    bool changed = false;
    size_t seen = 0;

    for (uint32_t bi = 0; bi < blocks; bi++) {
        uint32_t block = map_block(fs, dir_inode, bi, false, &changed);
        if (block == 0) {
            continue;
        }
        if (!read_block(fs, block, fs->scratch)) {
            return false;
        }

        uint32_t at = 0;
        uint32_t prev = 0;
        while (at + DIRENT_MIN <= fs->block_size) {
            uint32_t ino = rd32(fs->scratch + at);
            uint16_t rec = rd16(fs->scratch + at + 4);
            uint8_t nlen = fs->scratch[at + 6];

            /*
             * FIXME: nlen is never held against rec, so an entry
             * that says rec is 8 and nlen is 255 makes the reads below run
             * up to 247 bytes past scratch, and the over read bytes are
             * handed back to the caller as the entry's name. refuse the
             * entry unless DIRENT_MIN + nlen fits inside rec.
             */
            if (rec < DIRENT_MIN || at + rec > fs->block_size) {
                break;      /* a directory do not believe */
            }

            if (ino != 0) {
                bool dot = (nlen == 1 && fs->scratch[at + 8] == '.')
                        || (nlen == 2 && fs->scratch[at + 8] == '.'
                            && fs->scratch[at + 9] == '.');

                bool match;
                if (want != NULL) {
                    match = true;
                    for (uint8_t i = 0; i < nlen && match; i++) {
                        if (want[i] == '\0' || want[i] != (char)fs->scratch[at + 8 + i]) {
                            match = false;
                        }
                    }
                    if (match && want[nlen] != '\0') {
                        match = false;
                    }
                } else {
                    match = !dot && (seen++ == want_index);
                }

                if (match) {
                    out->block_index = bi;
                    out->offset = at;
                    out->prev_offset = prev;
                    out->ino = ino;
                    out->rec_len = rec;
                    out->name_len = nlen;
                    out->file_type = fs->scratch[at + 7];
                    if (name_out != NULL) {
                        memcpy(name_out, fs->scratch + at + 8, nlen);
                        name_out[nlen] = '\0';
                    }
                    return true;
                }
            }

            prev = at;
            at += rec;
        }
    }
    return false;
}

static bool dir_find(struct ext4 *fs, uint8_t *dir_inode, const char *name,
                     struct dirent_at *out)
{
    return dir_scan(fs, dir_inode, name, 0, out, NULL);
}

/* put a name in a directory. */
static bool dir_add(struct ext4 *fs, uint32_t dir_ino, uint8_t *dir_inode,
                    const char *name, uint32_t ino, uint8_t file_type)
{
    uint32_t nlen = 0;
    while (name[nlen] != '\0') {
        nlen++;
    }
    if (nlen == 0 || nlen > EXT4_NAME_MAX) {
        return false;
    }
    uint32_t need = entry_size((uint8_t)nlen);

    uint64_t size = rd32(dir_inode + 4);
    uint32_t blocks = (uint32_t)((size + fs->block_size - 1) / fs->block_size);
    bool changed = false;

    for (uint32_t bi = 0; bi < blocks; bi++) {
        uint32_t block = map_block(fs, dir_inode, bi, false, &changed);
        if (block == 0 || !read_block(fs, block, fs->scratch)) {
            continue;
        }

        uint32_t at = 0;
        while (at + DIRENT_MIN <= fs->block_size) {
            uint32_t e_ino = rd32(fs->scratch + at);
            uint16_t rec = rd16(fs->scratch + at + 4);
            uint8_t e_nlen = fs->scratch[at + 6];
            if (rec < DIRENT_MIN || at + rec > fs->block_size) {
                break;
            }

            /*
             * FIXME: e_nlen comes off the disk and is never held against
             * rec. this is the write-side twin of the note in dir_scan,
             * and worse than it: used can be bigger than rec, `rec -
             * used` is unsigned and wraps to something enormous, the
             * test passes, and the new entry is written at at + used,
             * which is past the end of the block and into whatever the
             * struct holds next. the block is then written back carrying
             * an entry length that swallowed it. refuse the entry unless
             * DIRENT_MIN + e_nlen fits inside rec, before subtracting.
             */
            uint32_t used = (e_ino == 0) ? 0 : entry_size(e_nlen);
            if (rec - used >= need) {
                uint32_t put = at + used;
                if (used > 0) {
                    wr16(fs->scratch + at + 4, (uint16_t)used);
                }
                wr32(fs->scratch + put, ino);
                wr16(fs->scratch + put + 4, (uint16_t)(rec - used));
                fs->scratch[put + 6] = (uint8_t)nlen;
                fs->scratch[put + 7] = file_type;
                memcpy(fs->scratch + put + 8, name, nlen);
                return write_block(fs, block, fs->scratch);
            }
            at += rec;
        }
    }

    /* nowhere it fits: another block, entirely this entry */
    uint32_t block = map_block(fs, dir_inode, blocks, true, &changed);
    if (block == 0) {
        return false;
    }
    memset(fs->scratch, 0, fs->block_size);
    wr32(fs->scratch + 0, ino);
    wr16(fs->scratch + 4, (uint16_t)fs->block_size);
    fs->scratch[6] = (uint8_t)nlen;
    fs->scratch[7] = file_type;
    memcpy(fs->scratch + 8, name, nlen);
    if (!write_block(fs, block, fs->scratch)) {
        return false;
    }

    wr32(dir_inode + 4, (uint32_t)((blocks + 1) * fs->block_size));
    return write_inode(fs, dir_ino, dir_inode);
}

static bool dir_remove(struct ext4 *fs, uint8_t *dir_inode,
                       const struct dirent_at *where)
{
    bool changed = false;
    uint32_t block = map_block(fs, dir_inode, where->block_index, false,
                               &changed);
    if (block == 0 || !read_block(fs, block, fs->scratch)) {
        return false;
    }

    if (where->offset == where->prev_offset) {
        /* the first entry in its block. */
        wr32(fs->scratch + where->offset, 0);
    } else {
        uint16_t prev_rec = rd16(fs->scratch + where->prev_offset + 4);
        wr16(fs->scratch + where->prev_offset + 4,
             (uint16_t)(prev_rec + where->rec_len));
    }
    return write_block(fs, block, fs->scratch);
}

static bool dir_is_empty(struct ext4 *fs, uint32_t ino)
{
    uint8_t inode[256];
    if (!read_inode(fs, ino, inode)) {
        return false;
    }
    struct dirent_at at;
    return !dir_scan(fs, inode, NULL, 0, &at, NULL);
}



#define SYMLINK_MAX 8

static bool resolve(struct ext4 *fs, const char *path, bool follow_last,
                    uint32_t *out_ino, uint8_t *out_inode, char *out_name);

static bool read_link_target(struct ext4 *fs, uint8_t *inode, char *out,
                             size_t size)
{
    uint64_t len = rd32(inode + 4);
    if (len >= size) {
        return false;
    }
    if (read_bytes(fs, inode, 0, out, len) != (int64_t)len) {
        return false;
    }
    out[len] = '\0';
    return true;
}

/* walk a path from the root. */
static bool resolve(struct ext4 *fs, const char *path, bool follow_last,
                    uint32_t *out_ino, uint8_t *out_inode, char *out_name)
{
    uint32_t ino = EXT4_ROOT_INO;
    uint8_t inode[256];
    if (!read_inode(fs, ino, inode)) {
        return false;
    }
    if (out_name != NULL) {
        out_name[0] = '\0';
    }

    /*
     * FIXME: this counter is per call, and resolve calls itself for a
     * symlink, so a chain is bounded and a loop is not. `a -> a`, or two
     * links pointing at each other, or a relative target that walks back
     * into its own directory, recurses with a fresh counter every time
     * until the kernel stack is gone: about ten frames, since each one
     * holds a name, a target and an inode. symlink loops are legal on
     * unix and no filesystem checker removes them. carry the count
     * through the recursion, or walk a link chain in a loop instead of
     * in a second frame.
     */
    int followed = 0;
    const char *p = path;

    while (*p != '\0') {
        while (*p == '/') {
            p++;
        }
        if (*p == '\0') {
            break;
        }

        char part[EXT4_NAME_MAX + 1];
        size_t n = 0;
        while (p[n] != '\0' && p[n] != '/') {
            if (n >= EXT4_NAME_MAX) {
                return false;
            }
            part[n] = p[n];
            n++;
        }
        part[n] = '\0';
        const char *rest = p + n;
        bool last = true;
        for (const char *q = rest; *q != '\0'; q++) {
            if (*q != '/') {
                last = false;
                break;
            }
        }

        if ((rd16(inode) & EXT4_S_IFMT) != EXT4_S_IFDIR) {
            return false;
        }

        struct dirent_at at;
        if (!dir_find(fs, inode, part, &at)) {
            return false;
        }
        ino = at.ino;
        if (!read_inode(fs, ino, inode)) {
            return false;
        }
        if (out_name != NULL) {
            memcpy(out_name, part, n + 1);
        }

        /* a symlink in the middle is always followed; one at the end only if asked. */
        if ((rd16(inode) & EXT4_S_IFMT) == EXT4_S_IFLNK
            && (!last || follow_last)) {
            if (++followed > SYMLINK_MAX) {
                return false;
            }
            char target[EXT4_NAME_MAX + 1];
            if (!read_link_target(fs, inode, target, sizeof target)) {
                return false;
            }
            if (target[0] == '/') {
                /* an absolute target starts again from the root */
                ino = EXT4_ROOT_INO;
                if (!read_inode(fs, ino, inode)) {
                    return false;
                }
                if (!resolve(fs, target, true, &ino, inode, out_name)) {
                    return false;
                }
            } else {
                /*
                 * relative: resolve it against where the link lives,
                 * which is the directory this walk was standing in.
                 * rebuilding that path is more bookkeeping than this
                 * needs, so a relative target is resolved from the
                 * parent by walking it fresh
                 */
                char here[EXT4_NAME_MAX * 2 + 2];
                size_t upto = (size_t)(p - path);
                if (upto + n + 4 >= sizeof here) {
                    return false;
                }
                memcpy(here, path, upto);
                here[upto] = '\0';
                size_t at_end = upto;
                const char *t = target;
                while (*t != '\0' && at_end + 1 < sizeof here) {
                    here[at_end++] = *t++;
                }
                here[at_end] = '\0';
                if (!resolve(fs, here, true, &ino, inode, out_name)) {
                    return false;
                }
            }
        }

        p = rest;
    }

    *out_ino = ino;
    memcpy(out_inode, inode, fs->inode_size);
    return true;
}

bool ext4_lookup(struct ext4 *fs, const char *path, struct ext4_file *out)
{
    if (!fs->mounted) {
        return false;
    }
    uint32_t ino;
    uint8_t inode[256];
    char name[EXT4_NAME_MAX + 1];
    if (!resolve(fs, path, true, &ino, inode, name)) {
        return false;
    }
    fill_file(fs, ino, inode, out);
    memcpy(out->name, name, sizeof name > sizeof out->name
                            ? sizeof out->name : sizeof name);
    return true;
}

bool ext4_lookup_nofollow(struct ext4 *fs, const char *path,
                          struct ext4_file *out)
{
    if (!fs->mounted) {
        return false;
    }
    uint32_t ino;
    uint8_t inode[256];
    char name[EXT4_NAME_MAX + 1];
    if (!resolve(fs, path, false, &ino, inode, name)) {
        return false;
    }
    fill_file(fs, ino, inode, out);
    memcpy(out->name, name, sizeof name > sizeof out->name
                            ? sizeof out->name : sizeof name);
    return true;
}

bool ext4_readlink(struct ext4 *fs, const struct ext4_file *f, char *out,
                   size_t size)
{
    if (!f->is_symlink) {
        return false;
    }
    uint8_t inode[256];
    if (!read_inode(fs, f->ino, inode)) {
        return false;
    }
    return read_link_target(fs, inode, out, size);
}

bool ext4_readdir(struct ext4 *fs, uint32_t dir_ino, size_t index,
                  struct ext4_file *out)
{
    if (!fs->mounted) {
        return false;
    }
    if (dir_ino == 0) {
        dir_ino = EXT4_ROOT_INO;
    }

    uint8_t dir_inode[256];
    if (!read_inode(fs, dir_ino, dir_inode)) {
        return false;
    }

    struct dirent_at at;
    char name[EXT4_NAME_MAX + 1];
    if (!dir_scan(fs, dir_inode, NULL, index, &at, name)) {
        return false;
    }

    uint8_t inode[256];
    if (!read_inode(fs, at.ino, inode)) {
        return false;
    }
    fill_file(fs, at.ino, inode, out);

    size_t n = 0;
    while (name[n] != '\0' && n < EXT4_NAME_MAX) {
        out->name[n] = name[n];
        n++;
    }
    out->name[n] = '\0';
    return true;
}

int64_t ext4_read(struct ext4 *fs, const struct ext4_file *f, uint64_t offset,
                  void *buf, uint64_t len)
{
    if (!fs->mounted) {
        return -1;
    }
    uint8_t inode[256];
    if (!read_inode(fs, f->ino, inode)) {
        return -1;
    }
    return read_bytes(fs, inode, offset, buf, len);
}

static int64_t ext4_write_locked(struct ext4 *fs, struct ext4_file *f,
                                 uint64_t offset, const void *buf,
                                 uint64_t len)
{
    uint8_t inode[256];
    if (!read_inode(fs, f->ino, inode)) {
        return -1;
    }
    int64_t n = write_bytes(fs, f->ino, inode, offset, buf, len);
    if (n > 0) {
        f->size = rd32(inode + 4);
    }
    return n;
}

/*
 * a write is a transaction too, and the odd one out: the bytes go
 * straight to the disk and everything that *describes* them goes
 * through the log. blocks taken out of a bitmap, an extent tree grown,
 * a size and an mtime, that is metadata like any other, and leaving
 * it outside a transaction left the busiest allocator on the disk as
 * the one thing the journal did not cover.
 *
 * this is also the one operation with no bound on its size, so it is
 * the one that makes `jbd_stage` restart rather than refuse: a write of
 * a hundred megabytes is many transactions, each of them whole
 */
int64_t ext4_write(struct ext4 *fs, struct ext4_file *f, uint64_t offset,
                   const void *buf, uint64_t len)
{
    if (!fs->mounted) {
        return -1;
    }
    jbd_begin(&fs->journal);
    int64_t n = ext4_write_locked(fs, f, offset, buf, len);
    jbd_commit(&fs->journal);
    return n;
}



/* split a path into the directory holding it and the last name */
static bool split_parent(struct ext4 *fs, const char *path, uint32_t *dir_ino,
                         uint8_t *dir_inode, char *leaf)
{
    const char *name = path;
    for (const char *p = path; *p != '\0'; p++) {
        if (*p == '/') {
            name = p + 1;
        }
    }
    if (*name == '\0') {
        return false;
    }

    size_t n = 0;
    while (name[n] != '\0') {
        if (n >= EXT4_NAME_MAX) {
            return false;
        }
        leaf[n] = name[n];
        n++;
    }
    leaf[n] = '\0';

    char parent[EXT4_NAME_MAX * 2 + 2];
    size_t plen = (size_t)(name - path);
    if (plen >= sizeof parent) {
        return false;
    }
    memcpy(parent, path, plen);
    parent[plen] = '\0';

    char ignored[EXT4_NAME_MAX + 1];
    if (!resolve(fs, parent, true, dir_ino, dir_inode, ignored)) {
        return false;
    }
    return (rd16(dir_inode) & EXT4_S_IFMT) == EXT4_S_IFDIR;
}

static bool make(struct ext4 *fs, const char *path, uint32_t mode,
                 uint32_t uid, uint32_t gid, const char *link_target,
                 uint32_t *out_ino)
{
    if (!fs->mounted || fs->write == NULL) {
        return false;
    }

    uint32_t dir_ino;
    uint8_t dir_inode[256];
    char leaf[EXT4_NAME_MAX + 1];
    if (!split_parent(fs, path, &dir_ino, dir_inode, leaf)) {
        return false;
    }

    struct dirent_at at;
    if (dir_find(fs, dir_inode, leaf, &at)) {
        return false;       /* already there */
    }

    bool directory = (mode & EXT4_S_IFMT) == EXT4_S_IFDIR;
    uint32_t ino = alloc_inode(fs, directory);
    if (ino == 0) {
        return false;
    }

    uint8_t inode[256];
    memset(inode, 0, sizeof inode);
    wr16(inode + 0, (uint16_t)mode);
    wr16(inode + 2, (uint16_t)uid);
    wr16(inode + 24, (uint16_t)gid);

    /* no links yet, and that is the whole of what makes this survivable. */
    wr16(inode + 26, 0);
    stamp(fs, inode, true);

    /*
     * born with an extent tree if the filesystem has them, and an empty
     * one is a header and nothing else. a fast symlink is the exception
     * and gets neither: those sixty bytes are the target, and a header
     * written over the front of it would be the target
     */
    if (fs->extents && !(link_target != NULL && strlen(link_target) < 60)) {
        wr32(inode + 32, rd32(inode + 32) | EXT4_EXTENTS_FL);
        node_init(inode_map(inode), root_max(), 0);
    }

    if (link_target != NULL) {
        size_t len = 0;
        while (link_target[len] != '\0') {
            len++;
        }
        wr32(inode + 4, (uint32_t)len);
        if (len < 60) {
            memcpy(inode + 40, link_target, len);
        } else {
            if (!write_inode(fs, ino, inode)) {
                return false;
            }
            if (write_bytes(fs, ino, inode, 0, link_target, len)
                != (int64_t)len) {
                return false;
            }
            wr32(inode + 4, (uint32_t)len);
        }
    }

    /*
     * a directory is born with the two entries every directory has, and
     * they go down *before* the name that reaches it.
     *
     * the other order is the obvious one and it is wrong: it leaves a
     * window where the parent has an entry pointing at a directory with
     * no `.` and no `..` in it. that is not a leak, something reaches
     * it, and no fsck can invent what should have been there, because
     * nothing on the disk says what it was going to be.
     *
     * this way the window holds an allocated inode and an allocated
     * block that nothing points at, which is a leak and has one answer.
     *
     * `.` is one of its links and `..` is one of its parent's, which is
     * why a directory's count starts at two rather than one
     */
    if (directory) {
        bool changed = false;
        uint32_t block = map_block(fs, inode, 0, true, &changed);
        if (block == 0) {
            return false;
        }
        memset(fs->scratch, 0, fs->block_size);
        uint32_t dot = entry_size(1);
        wr32(fs->scratch + 0, ino);
        wr16(fs->scratch + 4, (uint16_t)dot);
        fs->scratch[6] = 1;
        fs->scratch[7] = 2;
        fs->scratch[8] = '.';
        wr32(fs->scratch + dot, dir_ino);
        wr16(fs->scratch + dot + 4, (uint16_t)(fs->block_size - dot));
        fs->scratch[dot + 6] = 2;
        fs->scratch[dot + 7] = 2;
        fs->scratch[dot + 8] = '.';
        fs->scratch[dot + 9] = '.';
        if (!write_block(fs, block, fs->scratch)) {
            return false;
        }
        wr32(inode + 4, fs->block_size);
    }

    if (!write_inode(fs, ino, inode)) {
        return false;
    }

    uint8_t file_type = directory ? 2 : (link_target != NULL ? 7 : 1);
    if (!dir_add(fs, dir_ino, dir_inode, leaf, ino, file_type)) {
        return false;
    }

    if (directory) {
        /* the parent gained a child, and a child's `..` is a link to it */
        wr16(dir_inode + 26, (uint16_t)(rd16(dir_inode + 26) + 1));
        if (!write_inode(fs, dir_ino, dir_inode)) {
            return false;
        }
    }

    /* and last of all, the link count, now that there is a name to count. */
    wr16(inode + 26, directory ? 2 : 1);
    if (!write_inode(fs, ino, inode)) {
        return false;
    }

    if (out_ino != NULL) {
        *out_ino = ino;
    }
    return true;
}

static bool ext4_create_locked(struct ext4 *fs, const char *path, uint32_t mode,
                 uint32_t uid, uint32_t gid, struct ext4_file *out)
{
    /*
     * already there? then this is an open, which is what create has
     * always meant when the thing exists
     */
    if (ext4_lookup(fs, path, out) && !out->is_dir) {
        return true;
    }

    uint32_t ino;
    if (!make(fs, path, EXT4_S_IFREG | (mode & 0xfff), uid, gid, NULL, &ino)) {
        return false;
    }
    return ext4_lookup(fs, path, out);
}

/* one thing a caller asked for is one transaction, however many blocks it turns into. */
bool ext4_create(struct ext4 *fs, const char *path, uint32_t mode,
                 uint32_t uid, uint32_t gid, struct ext4_file *out)
{
    jbd_begin(&fs->journal);
    bool ok = ext4_create_locked(fs, path, mode, uid, gid, out);
    jbd_commit(&fs->journal);
    return ok;
}

static bool ext4_mkdir_locked(struct ext4 *fs, const char *path, uint32_t mode,
                uint32_t uid, uint32_t gid)
{
    return make(fs, path, EXT4_S_IFDIR | (mode & 0xfff), uid, gid, NULL, NULL);
}

/* one thing a caller asked for is one transaction, however many blocks it turns into. */
bool ext4_mkdir(struct ext4 *fs, const char *path, uint32_t mode,
                uint32_t uid, uint32_t gid)
{
    jbd_begin(&fs->journal);
    bool ok = ext4_mkdir_locked(fs, path, mode, uid, gid);
    jbd_commit(&fs->journal);
    return ok;
}

static bool ext4_symlink_locked(struct ext4 *fs, const char *path, const char *target,
                  uint32_t uid, uint32_t gid)
{
    return make(fs, path, EXT4_S_IFLNK | 0777, uid, gid, target, NULL);
}

/* one thing a caller asked for is one transaction, however many blocks it turns into. */
bool ext4_symlink(struct ext4 *fs, const char *path, const char *target,
                  uint32_t uid, uint32_t gid)
{
    jbd_begin(&fs->journal);
    bool ok = ext4_symlink_locked(fs, path, target, uid, gid);
    jbd_commit(&fs->journal);
    return ok;
}



static bool drop_name(struct ext4 *fs, const char *path, bool want_dir)
{
    if (!fs->mounted || fs->write == NULL) {
        return false;
    }

    uint32_t dir_ino;
    uint8_t dir_inode[256];
    char leaf[EXT4_NAME_MAX + 1];
    if (!split_parent(fs, path, &dir_ino, dir_inode, leaf)) {
        return false;
    }

    struct dirent_at at;
    if (!dir_find(fs, dir_inode, leaf, &at)) {
        return false;
    }

    uint8_t inode[256];
    if (!read_inode(fs, at.ino, inode)) {
        return false;
    }
    bool is_dir = (rd16(inode) & EXT4_S_IFMT) == EXT4_S_IFDIR;
    if (is_dir != want_dir) {
        return false;
    }
    if (is_dir && !dir_is_empty(fs, at.ino)) {
        return false;
    }

    /*
     * the count comes down *before* the name goes, which is the reverse
     * of the obvious order and the reason this survives a crash.
     *
     * take the name away first and a machine that stops right there
     * leaves an inode nothing reaches that still claims a link, an
     * orphan insisting somebody wants it, which is lost+found's problem
     * everywhere else and unanswerable here.
     *
     * this way the window holds the opposite: a name pointing at an
     * inode whose count is too low. fsck counts the names and puts the
     * number back, and the file is simply still there, the delete did
     * not happen, which is a perfectly good outcome for a delete that
     * was interrupted
     */
    uint16_t links = rd16(inode + 26);
    if (is_dir) {
        links = 0;      /* its own `.` and its parent's entry both went */
    } else if (links > 0) {
        links--;
    }
    wr16(inode + 26, links);
    if (!write_inode(fs, at.ino, inode)) {
        return false;
    }

    if (!dir_remove(fs, dir_inode, &at)) {
        return false;
    }

    if (is_dir) {
        wr16(dir_inode + 26, (uint16_t)(rd16(dir_inode + 26) - 1));
        write_inode(fs, dir_ino, dir_inode);
    }

    if (links == 0) {
        free_all_blocks(fs, inode);
        if (fs->clock != NULL) {
            wr32(inode + 20, fs->clock());     /* deleted at */
        }
        wr32(inode + 4, 0);
        write_inode(fs, at.ino, inode);
        return free_inode(fs, at.ino, is_dir);
    }
    return write_inode(fs, at.ino, inode);
}

static bool ext4_unlink_locked(struct ext4 *fs, const char *path)
{
    return drop_name(fs, path, false);
}

/* one thing a caller asked for is one transaction, however many blocks it turns into. */
bool ext4_unlink(struct ext4 *fs, const char *path)
{
    jbd_begin(&fs->journal);
    bool ok = ext4_unlink_locked(fs, path);
    jbd_commit(&fs->journal);
    return ok;
}

static bool ext4_rmdir_locked(struct ext4 *fs, const char *path)
{
    return drop_name(fs, path, true);
}

/* one thing a caller asked for is one transaction, however many blocks it turns into. */
bool ext4_rmdir(struct ext4 *fs, const char *path)
{
    jbd_begin(&fs->journal);
    bool ok = ext4_rmdir_locked(fs, path);
    jbd_commit(&fs->journal);
    return ok;
}

static bool ext4_rename_locked(struct ext4 *fs, const char *from, const char *to)
{
    if (!fs->mounted || fs->write == NULL) {
        return false;
    }

    uint32_t from_dir;
    uint8_t from_inode[256];
    char from_leaf[EXT4_NAME_MAX + 1];
    if (!split_parent(fs, from, &from_dir, from_inode, from_leaf)) {
        return false;
    }

    struct dirent_at at;
    if (!dir_find(fs, from_inode, from_leaf, &at)) {
        return false;
    }
    uint32_t ino = at.ino;
    uint8_t file_type = at.file_type;

    uint32_t to_dir;
    uint8_t to_inode[256];
    char to_leaf[EXT4_NAME_MAX + 1];
    if (!split_parent(fs, to, &to_dir, to_inode, to_leaf)) {
        return false;
    }

    struct dirent_at exists;
    if (dir_find(fs, to_inode, to_leaf, &exists)) {
        return false;       /* the new name is taken */
    }

    /* the new name first, then the old one struck out. */
    if (!dir_add(fs, to_dir, to_inode, to_leaf, ino, file_type)) {
        return false;
    }

    /*
     * the parent's inode may have changed under dir_add, so read it
     * again before removing from it
     */
    if (!read_inode(fs, from_dir, from_inode)) {
        return false;
    }
    if (!dir_find(fs, from_inode, from_leaf, &at)) {
        return false;
    }
    return dir_remove(fs, from_inode, &at);
}

/* one thing a caller asked for is one transaction, however many blocks it turns into. */
bool ext4_rename(struct ext4 *fs, const char *from, const char *to)
{
    jbd_begin(&fs->journal);
    bool ok = ext4_rename_locked(fs, from, to);
    jbd_commit(&fs->journal);
    return ok;
}



static bool ext4_chmod_locked(struct ext4 *fs, const char *path, uint32_t mode)
{
    if (!fs->mounted || fs->write == NULL) {
        return false;
    }
    uint32_t ino;
    uint8_t inode[256];
    char name[EXT4_NAME_MAX + 1];
    if (!resolve(fs, path, true, &ino, inode, name)) {
        return false;
    }
    wr16(inode + 0, (uint16_t)((rd16(inode) & EXT4_S_IFMT) | (mode & 0xfff)));
    if (fs->clock != NULL) {
        wr32(inode + 12, fs->clock());      /* changed, not modified */
    }
    return write_inode(fs, ino, inode);
}

/* one thing a caller asked for is one transaction, however many blocks it turns into. */
bool ext4_chmod(struct ext4 *fs, const char *path, uint32_t mode)
{
    jbd_begin(&fs->journal);
    bool ok = ext4_chmod_locked(fs, path, mode);
    jbd_commit(&fs->journal);
    return ok;
}

static bool ext4_chown_locked(struct ext4 *fs, const char *path, uint32_t uid,
                uint32_t gid)
{
    if (!fs->mounted || fs->write == NULL) {
        return false;
    }
    uint32_t ino;
    uint8_t inode[256];
    char name[EXT4_NAME_MAX + 1];
    if (!resolve(fs, path, true, &ino, inode, name)) {
        return false;
    }
    wr16(inode + 2, (uint16_t)uid);
    wr16(inode + 24, (uint16_t)gid);
    if (fs->clock != NULL) {
        wr32(inode + 12, fs->clock());
    }
    return write_inode(fs, ino, inode);
}

/* one thing a caller asked for is one transaction, however many blocks it turns into. */
bool ext4_chown(struct ext4 *fs, const char *path, uint32_t uid,
                uint32_t gid)
{
    jbd_begin(&fs->journal);
    bool ok = ext4_chown_locked(fs, path, uid, gid);
    jbd_commit(&fs->journal);
    return ok;
}

/* ext4 was written to be read and written, not to be *managed*. */

/* how many blocks group `g` holds if the filesystem were `count` blocks long. */
static uint32_t group_span(const struct ext4 *fs, uint32_t g,
                           uint32_t count)
{
    uint64_t base = (uint64_t)fs->first_data_block
                  + (uint64_t)g * fs->blocks_per_group;
    if (base >= count) {
        return 0;
    }
    uint64_t left = count - base;
    return left > fs->blocks_per_group ? fs->blocks_per_group
                                       : (uint32_t)left;
}

static uint64_t group_base_of(const struct ext4 *fs, uint32_t g)
{
    return (uint64_t)fs->first_data_block
         + (uint64_t)g * fs->blocks_per_group;
}

static uint32_t groups_for(const struct ext4 *fs, uint32_t count)
{
    return (count - fs->first_data_block + fs->blocks_per_group - 1)
           / fs->blocks_per_group;
}

static uint32_t gdt_blocks_for(const struct ext4 *fs, uint32_t groups)
{
    uint32_t per = fs->block_size / fs->desc_size;
    return (groups + per - 1) / per;
}

static uint32_t inode_table_blocks(const struct ext4 *fs)
{
    return (fs->inodes_per_group * fs->inode_size + fs->block_size - 1)
           / fs->block_size;
}

static void bits_set(uint8_t *bm, uint32_t from, uint32_t to)
{
    for (uint32_t i = from; i < to; i++) {
        bm[i / 8] |= (uint8_t)(1u << (i % 8));
    }
}

static void bits_clear(uint8_t *bm, uint32_t from, uint32_t to)
{
    for (uint32_t i = from; i < to; i++) {
        bm[i / 8] &= (uint8_t)~(1u << (i % 8));
    }
}

static bool bit_at(const uint8_t *bm, uint32_t i)
{
    return ((bm[i / 8] >> (i % 8)) & 1) != 0;
}

/*
 * the superblock fields a resize changes, which are the ones nothing
 * else ever touches: how big this filesystem says it is. `flush_super`
 * keeps the two counts up to date and knows nothing about the rest
 */
static bool write_geometry(struct ext4 *fs)
{
    /*
     * FIXME: the same block-0 arithmetic as flush_super, and here it does
     * more damage. on a 2 KiB or 4 KiB filesystem this is block 0, the
     * read below refuses it, and a resize that has already written the
     * new groups' bitmaps and tables then fails to write the size that
     * makes them real: it reports a failure over a half-changed
     * filesystem. refresh_backups below computes the same number and
     * would fail the same way. one helper meaning "the block holding the
     * superblock", used by all three.
     */
    uint32_t block = SB_OFFSET / fs->block_size;
    if (!read_block(fs, block, fs->scratch)) {
        return false;
    }
    uint8_t *sb = fs->scratch + (SB_OFFSET % fs->block_size);
    wr32(sb + 0,  fs->inodes_count);
    wr32(sb + 4,  fs->blocks_count);
    wr32(sb + 12, fs->free_blocks);
    wr32(sb + 16, fs->free_inodes);
    wr16(sb + 206, (uint16_t)fs->reserved_gdt);
    return write_block(fs, block, fs->scratch);
}

/*
 * a block past the end of the filesystem, which `raw_write` refuses,
 * correctly, because for every other caller writing out there is a bug.
 * this is the one place where past the end is the whole point, so the
 * bound it checks against is the size the filesystem is *becoming*
 */
static bool write_beyond(struct ext4 *fs, uint32_t block, uint32_t count,
                         const void *from)
{
    if (fs->write == NULL || block == 0 || block >= count) {
        return false;
    }
    uint32_t per = fs->block_size / EXT4_SECTOR;
    return fs->write(fs->ctx, (uint64_t)block * per, per, from);
}

/* the metadata of a group nothing believes in yet. */
static bool lay_down_group(struct ext4 *fs, uint32_t g, uint32_t count,
                           uint32_t span)
{
    uint32_t bbm = (uint32_t)(group_base_of(fs, g) + 1 + span);
    uint32_t have = group_span(fs, g, count);
    uint32_t table = inode_table_blocks(fs);
    uint32_t bits = fs->block_size * 8;

    /*
     * the block bitmap: this group's own metadata at the front, and
     * everything past the end of a short group marked used, a checker
     * that believes in those blocks would hand one out
     */
    memset(fs->scratch, 0, fs->block_size);
    bits_set(fs->scratch, 0, 1 + span + 2 + table);
    if (have < bits) {
        bits_set(fs->scratch, have, bits);
    }
    if (!write_beyond(fs, bbm, count, fs->scratch)) {
        return false;
    }

    /*
     * the inode bitmap: nothing in use, and the tail past this group's
     * share of the inodes marked used for the same reason
     */
    memset(fs->scratch, 0, fs->block_size);
    bits_set(fs->scratch, fs->inodes_per_group, bits);
    if (!write_beyond(fs, bbm + 1, count, fs->scratch)) {
        return false;
    }

    memset(fs->scratch, 0, fs->block_size);
    for (uint32_t t = 0; t < table; t++) {
        if (!write_beyond(fs, bbm + 2 + t, count, fs->scratch)) {
            return false;
        }
    }
    return true;
}

/*
 * and its descriptor. the positions are the formatter's rule, and they
 * need agree with nothing else on the disk: a descriptor says where its
 * own group's pieces are, which is what lets a group laid down today
 * sit beside groups laid down by somebody else's mke2fs
 */
static bool describe_group(struct ext4 *fs, uint32_t g, uint32_t count,
                           uint32_t span)
{
    uint32_t per = fs->block_size / fs->desc_size;
    if (!read_block(fs, fs->gdt_block + g / per, fs->scratch)) {
        return false;
    }
    uint8_t *desc = fs->scratch + (g % per) * fs->desc_size;
    uint32_t bbm = (uint32_t)(group_base_of(fs, g) + 1 + span);
    uint32_t have = group_span(fs, g, count);
    uint32_t used = 1 + span + 2 + inode_table_blocks(fs);

    memset(desc, 0, fs->desc_size);
    wr32(desc + 0, bbm);
    wr32(desc + 4, bbm + 1);
    wr32(desc + 8, bbm + 2);
    wr16(desc + 12, (uint16_t)(have > used ? have - used : 0));
    wr16(desc + 14, (uint16_t)fs->inodes_per_group);
    wr16(desc + 16, 0);                 /* no directories in it yet */
    return write_block(fs, fs->gdt_block + g / per, fs->scratch);
}

/*
 * every group carries a copy of the superblock and of the whole
 * descriptor table, which is what makes a filesystem with a damaged
 * block 1 recoverable at all. a resize that moved the real ones and
 * left the copies saying the old size has left every backup lying.
 *
 * they are refreshed after the change rather than as part of it, and a
 * failure here is reported without undoing anything: a filesystem with
 * stale backups is still a filesystem, and one whose resize was rolled
 * back half way through would not be
 */
static bool refresh_backups(struct ext4 *fs)
{
    uint32_t gdt = gdt_blocks_for(fs, fs->groups);
    uint32_t sb_block = SB_OFFSET / fs->block_size;
    uint32_t within = SB_OFFSET % fs->block_size;

    for (uint32_t g = 1; g < fs->groups; g++) {
        uint64_t base = group_base_of(fs, g);

        /*
         * the superblock is 1024 bytes wherever the blocks are bigger,
         * so this replaces the first kilobyte of the group's first
         * block rather than the whole of it, which on a 1 KiB
         * filesystem is the whole of it anyway
         */
        if (!read_block(fs, sb_block, fs->scratch)
            || !read_block(fs, (uint32_t)base, fs->indirect)) {
            return false;
        }
        memcpy(fs->indirect, fs->scratch + within, 1024);
        wr16(fs->indirect + 90, (uint16_t)g);       /* which copy this is */
        if (!raw_write(fs, (uint32_t)base, fs->indirect)) {
            return false;
        }

        for (uint32_t part = 0; part < gdt; part++) {
            if (!read_block(fs, fs->gdt_block + part, fs->scratch)
                || !raw_write(fs, (uint32_t)(base + 1 + part), fs->scratch)) {
                return false;
            }
        }
    }
    return ext4_flush(fs);
}

/* is there anything out past `count` that somebody would lose? */
static bool tail_is_empty(struct ext4 *fs, uint32_t count, uint32_t span,
                          uint64_t *blocks_in_way, uint64_t *inodes_in_way)
{
    uint32_t keep = groups_for(fs, count);
    uint32_t table = inode_table_blocks(fs);
    *blocks_in_way = 0;
    *inodes_in_way = 0;

    for (uint32_t g = keep - 1; g < fs->groups; g++) {
        uint32_t off;
        if (!read_gd(fs, g, fs->indirect, &off)) {
            return false;
        }
        uint32_t bbm = rd32(fs->indirect + off + 0);
        uint32_t ibm = rd32(fs->indirect + off + 4);
        uint32_t have = group_span(fs, g, fs->blocks_count);

        /*
         * the last group that survives keeps its front matter and
         * everything below the new end; the ones after it keep nothing
         */
        uint32_t from = 0;
        if (g == keep - 1) {
            uint64_t base = group_base_of(fs, g);
            from = (uint32_t)(count - base);
        } else {
            from = 1 + span + 2 + table;    /* past its own metadata */
        }

        if (!read_block(fs, bbm, fs->scratch)) {
            return false;
        }
        for (uint32_t i = from; i < have; i++) {
            if (bit_at(fs->scratch, i)) {
                (*blocks_in_way)++;
            }
        }

        if (g >= keep) {
            if (!read_block(fs, ibm, fs->scratch)) {
                return false;
            }
            for (uint32_t i = 0; i < fs->inodes_per_group; i++) {
                if (bit_at(fs->scratch, i)) {
                    (*inodes_in_way)++;
                }
            }
        }
    }
    return true;
}

/*
 * the last group that survives a change of size, whichever direction it
 * went: its bitmap has to say the truth about how long it now is, and
 * its descriptor has to agree
 */
static bool retrim_group(struct ext4 *fs, uint32_t g, uint32_t was,
                         uint32_t now, int64_t *free_delta)
{
    if (was == now) {
        return true;
    }
    uint32_t off;
    if (!read_gd(fs, g, fs->indirect, &off)) {
        return false;
    }
    uint32_t bbm = rd32(fs->indirect + off + 0);
    if (!read_block(fs, bbm, fs->scratch)) {
        return false;
    }
    if (now > was) {
        bits_clear(fs->scratch, was, now);
    } else {
        bits_set(fs->scratch, now, was);
    }
    if (!write_block(fs, bbm, fs->scratch)) {
        return false;
    }

    if (!read_gd(fs, g, fs->indirect, &off)) {
        return false;
    }
    int32_t change = (int32_t)now - (int32_t)was;
    wr16(fs->indirect + off + 12,
         (uint16_t)((int32_t)rd16(fs->indirect + off + 12) + change));
    *free_delta += change;
    return write_gd(fs, g, fs->indirect);
}

uint64_t ext4_resize_ceiling(const struct ext4 *fs)
{
    if (!fs->mounted) {
        return 0;
    }
    uint32_t span = gdt_blocks_for(fs, fs->groups) + fs->reserved_gdt;
    uint64_t most = (uint64_t)span * (fs->block_size / fs->desc_size);
    uint64_t blocks = (uint64_t)fs->first_data_block
                    + most * fs->blocks_per_group;
    return blocks > 0xffffffffu ? 0xffffffffu : blocks;
}

bool ext4_resize(struct ext4 *fs, uint64_t want, struct ext4_resize *out,
                 const char **error)
{
    *error = NULL;
    memset(out, 0, sizeof *out);

    if (!fs->mounted || fs->write == NULL) {
        *error = "the filesystem is not open for writing";
        return false;
    }
    if (fs->ro_compat & (uint32_t)EXT4_RO_SPARSE_SUPER) {
        /*
         * with sparse_super only some groups carry a superblock and a
         * table, so a group's front matter is a different size
         * depending on which group it is. that is a perfectly good
         * filesystem and it is not the one this lays out, and guessing
         * at the difference is how a resize writes over a bitmap
         */
        *error = "this filesystem uses sparse superblocks, which this "
                 "does not lay out";
        return false;
    }

    out->blocks_before = fs->blocks_count;
    out->groups_before = fs->groups;
    out->inodes_before = fs->inodes_count;
    out->blocks_after = fs->blocks_count;
    out->groups_after = fs->groups;
    out->inodes_after = fs->inodes_count;

    if (want == fs->blocks_count) {
        return true;            /* already that size, which is not a failure */
    }

    uint32_t span = gdt_blocks_for(fs, fs->groups) + fs->reserved_gdt;
    uint32_t table = inode_table_blocks(fs);
    uint32_t overhead = 1 + span + 2 + table;

    if (want > ext4_resize_ceiling(fs)) {
        *error = "bigger than the descriptor table can describe, the "
                 "table may only grow into the room reserved for it";
        return false;
    }
    if (want <= (uint64_t)fs->first_data_block + overhead + 1) {
        *error = "smaller than a filesystem can be";
        return false;
    }

    uint32_t count = (uint32_t)want;
    uint32_t keep = groups_for(fs, count);
    uint32_t last = keep - 1;               /* the group that survives */
    int64_t free_delta = 0;

    if (count > fs->blocks_count) {
        /*
         * everything new first, where nothing can see it, and then one
         * transaction that says so
         */
        for (uint32_t g = fs->groups; g < keep; g++) {
            if (!lay_down_group(fs, g, count, span)) {
                *error = "the drive stopped taking writes";
                return false;
            }
        }
        if (!ext4_flush(fs)) {
            *error = "the new groups would not reach the disk";
            return false;
        }

        jbd_begin(&fs->journal);
        bool ok = true;

        /* the descriptors before the count that makes them real. */
        for (uint32_t g = fs->groups; ok && g < keep; g++) {
            ok = describe_group(fs, g, count, span);
            if (ok) {
                uint32_t have = group_span(fs, g, count);
                free_delta += (have > overhead) ? (int64_t)(have - overhead)
                                                : 0;
            }
        }

        /* and the group that was last, which may have got longer */
        if (ok && fs->groups > 0) {
            uint32_t was = group_span(fs, fs->groups - 1, fs->blocks_count);
            uint32_t now = group_span(fs, fs->groups - 1, count);
            ok = retrim_group(fs, fs->groups - 1, was, now, &free_delta);
        }

        if (ok) {
            uint32_t gained = keep - fs->groups;
            fs->blocks_count = count;
            fs->groups = keep;
            fs->inodes_count += gained * fs->inodes_per_group;
            fs->free_inodes += gained * fs->inodes_per_group;
            fs->free_blocks = (uint32_t)((int64_t)fs->free_blocks
                                         + free_delta);
            fs->reserved_gdt = span - gdt_blocks_for(fs, fs->groups);
            ok = write_geometry(fs);
        }
        jbd_commit(&fs->journal);

        if (!ok) {
            *error = "the drive stopped taking writes part way through";
            return false;
        }
    } else {

        uint64_t blocks_in_way = 0, inodes_in_way = 0;
        if (!tail_is_empty(fs, count, span, &blocks_in_way, &inodes_in_way)) {
            *error = "the bitmaps would not read";
            return false;
        }
        out->blocks_in_way = blocks_in_way;
        out->inodes_in_way = inodes_in_way;
        if (blocks_in_way > 0 || inodes_in_way > 0) {
            *error = "there is something out past the new end. nothing is "
                     "moved here, so this refuses rather than losing it";
            return false;
        }

        jbd_begin(&fs->journal);

        uint32_t was = group_span(fs, last, fs->blocks_count);
        uint32_t now = group_span(fs, last, count);
        bool ok = retrim_group(fs, last, was, now, &free_delta);

        /*
         * the groups that go away take their free counts with them, and
         * their descriptors are wiped rather than left: a descriptor
         * past the end describes a group that is not there, and the
         * next thing to grow this filesystem would find one already
         * written and believe it
         */
        uint32_t per = fs->block_size / fs->desc_size;
        for (uint32_t g = keep; ok && g < fs->groups; g++) {
            uint32_t off;
            if (!read_gd(fs, g, fs->indirect, &off)) {
                ok = false;
                break;
            }
            free_delta -= rd16(fs->indirect + off + 12);
            memset(fs->indirect + off, 0, fs->desc_size);
            ok = write_block(fs, fs->gdt_block + g / per, fs->indirect);
        }

        if (ok) {
            uint32_t lost = fs->groups - keep;
            uint32_t free_ino = lost * fs->inodes_per_group;
            fs->blocks_count = count;
            fs->groups = keep;
            fs->inodes_count -= free_ino;
            fs->free_inodes -= free_ino;
            fs->free_blocks = (uint32_t)((int64_t)fs->free_blocks
                                         + free_delta);
            fs->reserved_gdt = span - gdt_blocks_for(fs, fs->groups);
            ok = write_geometry(fs);
        }
        jbd_commit(&fs->journal);

        if (!ok) {
            *error = "the drive stopped taking writes part way through";
            return false;
        }
    }

    out->blocks_after = fs->blocks_count;
    out->groups_after = fs->groups;
    out->inodes_after = fs->inodes_count;

    if (!refresh_backups(fs)) {
        *error = "the filesystem is the new size, but its backup "
                 "superblocks could not be brought up to date";
        return false;
    }
    return true;
}

bool ext4_usage(struct ext4 *fs, uint64_t *used_bytes, uint64_t *total_bytes)
{
    if (!fs->mounted) {
        return false;
    }
    uint64_t total = (uint64_t)fs->blocks_count * fs->block_size;
    uint64_t free_now = (uint64_t)fs->free_blocks * fs->block_size;
    *total_bytes = total;
    *used_bytes = total - free_now;
    return true;
}
