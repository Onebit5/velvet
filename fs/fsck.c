// SPDX-License-Identifier: GPL-2.0-only
/*
 * fs/fsck.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * checking a filesystem, and mending it.
 */

#include <velvet/fsck.h>
#include <velvet/ext4.h>
#include <velvet/string.h>

#define SB_OFFSET       1024
#define SB_MAGIC        0xef53
#define ROOT_INO        2
#define GD_SIZE_BASE    32

/*
 * the same reason mkfs.c does it this way: the format is little endian
 * whatever the machine is, and these read out of the middle of a block
 * buffer at whatever offset the layout says, so there is no alignment
 * to rely on either
 */

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void wr32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++) {
        p[i] = (uint8_t)(v >> (i * 8));
    }
}

/*
 * gathered in a struct rather than passed around, because every check
 * below wants most of it and the alternative is a dozen arguments that
 * are the same dozen arguments every time
 */

struct check {
    fsck_io   read;
    fsck_out  write;
    void     *ctx;

    uint32_t block_size;
    uint32_t blocks_count, inodes_count;
    uint32_t first_data_block, blocks_per_group, inodes_per_group;
    uint32_t inode_size, first_ino, groups, gdt_block, desc_size;
    uint32_t reserved_gdt;      /* blocks the table may one day grow into */
    uint32_t incompat, ro_compat;

    /* every buffer this needs lives here rather than on the stack, and that is not style. */
    uint8_t  *block_seen;       /* a bit per block: somebody claims it */
    uint8_t  *inode_seen;       /* a bit per inode: a name points at it */
    uint16_t *links;            /* how many names, counted rather than read */

    uint32_t *queue;            /* directories still to walk, (ino, parent) */
    uint32_t  queued, walked;

    uint8_t  *dir;              /* the directory block being parsed */
    uint8_t  *node[5];          /* one per level of an extent tree */
    uint8_t  *table[3];         /* and per level of the older indirect map */

    uint8_t   buf[FSCK_MAX_BLOCK];      /* the block being looked at */
    uint8_t   side[FSCK_MAX_BLOCK];     /* and one for a bitmap beside it */

    struct fsck_report *report;
};

static void found(struct check *c, enum fsck_problem what)
{
    c->report->found[what]++;
    c->report->total_found++;
}

static void mended(struct check *c, enum fsck_problem what)
{
    c->report->mended[what]++;
    c->report->total_mended++;
}

static bool read_block(struct check *c, uint32_t block, void *into)
{
    uint32_t per = c->block_size / FSCK_SECTOR;
    return c->read(c->ctx, (uint64_t)block * per, per, into);
}

static bool write_block(struct check *c, uint32_t block, const void *from)
{
    if (c->write == NULL) {
        return false;
    }
    uint32_t per = c->block_size / FSCK_SECTOR;
    return c->write(c->ctx, (uint64_t)block * per, per, from);
}

static bool bit_get(const uint8_t *map, uint32_t bit)
{
    return (map[bit / 8] >> (bit % 8)) & 1;
}

static void bit_set(uint8_t *map, uint32_t bit)
{
    map[bit / 8] |= (uint8_t)(1u << (bit % 8));
}

static void bit_clear(uint8_t *map, uint32_t bit)
{
    map[bit / 8] &= (uint8_t)~(1u << (bit % 8));
}

/* the group descriptor for a group, read into `side` */
static bool read_gd(struct check *c, uint32_t group, uint32_t *offset)
{
    uint32_t per = c->block_size / c->desc_size;
    if (!read_block(c, c->gdt_block + group / per, c->side)) {
        return false;
    }
    *offset = (group % per) * c->desc_size;
    return true;
}



size_t fsck_workspace_bytes(uint32_t blocks_count, uint32_t inodes_count)
{
    size_t block_bits = ((size_t)blocks_count + 7) / 8;
    size_t inode_bits = ((size_t)inodes_count + 7) / 8;
    size_t links = (size_t)inodes_count * sizeof(uint16_t);

    /*
     * every directory can be queued once, since every inode is reached
     * once, so the worst case is every inode being a directory
     */
    size_t queue = (size_t)inodes_count * 2 * sizeof(uint32_t);

    /*
     * and the nine block buffers the walk needs at once: one for the
     * directory being parsed, five for the levels of an extent tree,
     * three for the levels of an indirect one
     */
    size_t buffers = 9 * (size_t)FSCK_MAX_BLOCK;

    return block_bits + inode_bits + links + queue + buffers + 64;
}

bool fsck_measure(fsck_io read, void *ctx, uint32_t *blocks_out,
                  uint32_t *inodes_out)
{
    uint8_t sector[FSCK_SECTOR * 2];
    if (!read(ctx, SB_OFFSET / FSCK_SECTOR, 2, sector)) {
        return false;
    }
    if (rd16(sector + 56) != SB_MAGIC) {
        return false;
    }
    *inodes_out = rd32(sector + 0);
    *blocks_out = rd32(sector + 4);
    return *blocks_out != 0 && *inodes_out != 0;
}



static bool read_super(struct check *c)
{
    uint8_t sector[FSCK_SECTOR * 2];
    if (!c->read(c->ctx, SB_OFFSET / FSCK_SECTOR, 2, sector)) {
        c->report->stopped = "the disk would not read";
        return false;
    }
    const uint8_t *sb = sector;

    if (rd16(sb + 56) != SB_MAGIC) {
        c->report->stopped = "no ext4 superblock here";
        return false;
    }

    uint32_t log_block = rd32(sb + 24);
    if (log_block > 2) {
        c->report->stopped = "blocks larger than this can hold";
        return false;
    }
    c->block_size = 1024u << log_block;

    c->inodes_count = rd32(sb + 0);
    c->blocks_count = rd32(sb + 4);
    c->first_data_block = rd32(sb + 20);
    c->blocks_per_group = rd32(sb + 32);
    c->inodes_per_group = rd32(sb + 40);
    c->inode_size = rd32(sb + 76) >= 1 ? rd16(sb + 88) : 128;
    c->first_ino = rd32(sb + 76) >= 1 ? rd32(sb + 84) : 11;
    c->incompat = rd32(sb + 76) >= 1 ? rd32(sb + 96) : 0;
    c->ro_compat = rd32(sb + 76) >= 1 ? rd32(sb + 100) : 0;

    c->desc_size = GD_SIZE_BASE;
    if (c->incompat & EXT4_INCOMPAT_64BIT) {
        c->desc_size = rd16(sb + 254);
    }
    c->reserved_gdt = rd32(sb + 76) >= 1 ? rd16(sb + 206) : 0;

    if (c->blocks_per_group == 0 || c->inodes_per_group == 0
        || c->inode_size == 0 || c->inode_size > c->block_size
        || c->desc_size < GD_SIZE_BASE || c->desc_size > c->block_size) {
        c->report->stopped = "the superblock does not describe a filesystem";
        return false;
    }

    c->groups = (c->blocks_count - c->first_data_block
                 + c->blocks_per_group - 1) / c->blocks_per_group;
    c->gdt_block = c->first_data_block + 1;

    struct fsck_report *r = c->report;
    r->blocks = c->blocks_count;
    r->inodes = c->inodes_count;
    r->groups = c->groups;
    r->block_size = c->block_size;

    /*
     * the counts in the superblock are not trusted, they are what is
     * being checked, but a count that is *impossible* means the
     * superblock itself is damaged, and everything below would then be
     * measured against a lie
     */
    if (rd32(sb + 12) > c->blocks_count || rd32(sb + 16) > c->inodes_count) {
        found(c, FSCK_SUPERBLOCK);
    }
    return true;
}



static bool read_inode(struct check *c, uint32_t ino, uint8_t *out)
{
    if (ino == 0 || ino > c->inodes_count) {
        return false;
    }
    uint32_t group = (ino - 1) / c->inodes_per_group;
    uint32_t index = (ino - 1) % c->inodes_per_group;

    uint32_t off;
    if (!read_gd(c, group, &off)) {
        return false;
    }
    uint32_t table = rd32(c->side + off + 8);

    uint32_t per = c->block_size / c->inode_size;
    if (!read_block(c, table + index / per, c->buf)) {
        return false;
    }
    memcpy(out, c->buf + (index % per) * c->inode_size, c->inode_size);
    return true;
}

static bool write_inode(struct check *c, uint32_t ino, const uint8_t *raw)
{
    uint32_t group = (ino - 1) / c->inodes_per_group;
    uint32_t index = (ino - 1) % c->inodes_per_group;

    uint32_t off;
    if (!read_gd(c, group, &off)) {
        return false;
    }
    uint32_t table = rd32(c->side + off + 8);

    uint32_t per = c->block_size / c->inode_size;
    uint32_t block = table + index / per;
    if (!read_block(c, block, c->buf)) {
        return false;
    }
    memcpy(c->buf + (index % per) * c->inode_size, raw, c->inode_size);
    return write_block(c, block, c->buf);
}

static bool inode_in_use(struct check *c, uint32_t ino)
{
    uint32_t group = (ino - 1) / c->inodes_per_group;
    uint32_t index = (ino - 1) % c->inodes_per_group;

    uint32_t off;
    if (!read_gd(c, group, &off)) {
        return false;
    }
    uint32_t bitmap = rd32(c->side + off + 4);
    if (!read_block(c, bitmap, c->buf)) {
        return false;
    }
    return bit_get(c->buf, index);
}

static bool is_fast_symlink(const uint8_t *inode)
{
    return (rd16(inode) & EXT4_S_IFMT) == EXT4_S_IFLNK && rd32(inode + 4) < 60;
}

/* does this group carry a copy of the superblock and the descriptor table? */
static bool group_has_super(const struct check *c, uint32_t g)
{
    if (!(c->ro_compat & EXT4_RO_SPARSE_SUPER)) {
        return true;
    }
    if (g <= 1) {
        return true;
    }
    for (uint32_t base = 3; base <= 7; base += 2) {
        uint32_t n = base;
        while (n < g) {
            n *= base;
        }
        if (n == g) {
            return true;
        }
    }
    return false;
}

/*
 * every block a file owns is marked here, and a block that is already
 * marked is one two files both claim. that second case is the reason
 * this is a bitmap of its own rather than a comparison against the
 * filesystem's: the filesystem's bitmap cannot tell you a block was
 * claimed *twice*, only that it was claimed.
 */

static void claim(struct check *c, uint32_t block)
{
    if (block < c->first_data_block || block >= c->blocks_count) {
        found(c, FSCK_EXTENT_BAD);      /* a number that is not a block */
        return;
    }
    if (bit_get(c->block_seen, block)) {
        found(c, FSCK_BLOCK_CROSSED);
        return;
    }
    bit_set(c->block_seen, block);
}

/*
 * the extent tree of one inode, claiming every block under it, the
 * runs it names and the blocks holding the nodes alike, since both are
 * space the file is using.
 *
 * `level` says which of the workspace's node buffers this may read
 * into, so a tree five deep costs five buffers that were counted for
 * rather than five stack frames that were not
 */
static bool claim_extents(struct check *c, const uint8_t *node,
                          uint32_t max_here, uint32_t level)
{
    if (rd16(node + 0) != EXT4_EXTENT_MAGIC) {
        return false;
    }
    uint32_t entries = rd16(node + 2);
    uint32_t max = rd16(node + 4);
    uint32_t depth = rd16(node + 6);
    if (entries > max || max > max_here || depth > 5 || level >= 5) {
        return false;
    }

    for (uint32_t i = 0; i < entries; i++) {
        const uint8_t *rec = node + 12 + i * 12;
        if (depth == 0) {
            uint32_t len = rd16(rec + 4);
            if (len > 32768) {
                len -= 32768;           /* allocated and never written */
            }
            uint64_t start = ((uint64_t)rd16(rec + 6) << 32) | rd32(rec + 8);
            for (uint32_t k = 0; k < len; k++) {
                claim(c, (uint32_t)(start + k));
            }
        } else {
            uint64_t child = ((uint64_t)rd16(rec + 8) << 32) | rd32(rec + 4);
            claim(c, (uint32_t)child);
            if (child < c->first_data_block || child >= c->blocks_count) {
                return false;
            }

            /*
             * into a buffer of this level's own, because the one the
             * node above is in is still being walked
             */
            uint8_t *below = c->node[level];
            if (!read_block(c, (uint32_t)child, below)) {
                return false;
            }
            if (!claim_extents(c, below, (c->block_size - 12) / 12,
                               level + 1)) {
                return false;
            }
        }
    }
    return true;
}

/* and the older map: twelve direct, then the three levels of indirect */
static void claim_indirect(struct check *c, uint32_t block, uint32_t depth)
{
    if (block == 0 || depth == 0 || depth > 3) {
        return;
    }
    claim(c, block);
    if (block < c->first_data_block || block >= c->blocks_count) {
        return;
    }

    uint8_t *table = c->table[depth - 1];
    if (!read_block(c, block, table)) {
        return;
    }
    uint32_t per = c->block_size / 4;
    for (uint32_t i = 0; i < per; i++) {
        uint32_t n = rd32(table + i * 4);
        if (n == 0) {
            continue;
        }
        if (depth == 1) {
            claim(c, n);
        } else {
            claim_indirect(c, n, depth - 1);
        }
    }
}

static void claim_blocks_of(struct check *c, uint32_t ino,
                            const uint8_t *inode)
{
    (void)ino;
    if (is_fast_symlink(inode)) {
        return;                 /* those sixty bytes are the target */
    }

    if (rd32(inode + 32) & EXT4_EXTENTS_FL) {
        if (!claim_extents(c, inode + 40, (60 - 12) / 12, 0)) {
            found(c, FSCK_EXTENT_BAD);
        }
        return;
    }

    for (uint32_t i = 0; i < 12; i++) {
        uint32_t b = rd32(inode + 40 + i * 4);
        if (b != 0) {
            claim(c, b);
        }
    }
    claim_indirect(c, rd32(inode + 40 + 12 * 4), 1);
    claim_indirect(c, rd32(inode + 40 + 13 * 4), 2);
    claim_indirect(c, rd32(inode + 40 + 14 * 4), 3);
}

/*
 * walked here rather than through the driver, records and all: a
 * directory is a file whose contents are (inode, length, name) and the
 * length is what a reader follows. a length of zero is the end of the
 * world, the walk would never advance, and a length that runs past
 * the block is a record claiming space that is not there
 */

/* a directory found, to be walked when this one is finished with. */
static void enqueue(struct check *c, uint32_t ino, uint32_t parent)
{
    if (c->queued >= c->inodes_count) {
        return;                 /* every inode is queued at most once */
    }
    c->queue[c->queued * 2 + 0] = ino;
    c->queue[c->queued * 2 + 1] = parent;
    c->queued++;
}

static void walk_dir_block(struct check *c, const uint8_t *block,
                           uint32_t ino, uint32_t parent,
                           bool *saw_dot, bool *saw_dotdot)
{
    uint32_t off = 0;
    while (off + 8 <= c->block_size) {
        uint32_t child = rd32(block + off + 0);
        uint32_t len = rd16(block + off + 4);
        uint32_t name_len = block[off + 6];

        if (len < 8 || (len & 3) != 0 || off + len > c->block_size) {
            found(c, FSCK_DIR_BAD_RECORD);
            return;
        }
        if (child == 0) {
            off += len;                 /* a hole, which is allowed */
            continue;
        }
        if (name_len + 8u > len) {
            found(c, FSCK_DIR_BAD_RECORD);
            return;
        }

        const char *name = (const char *)(block + off + 8);
        bool dot = (name_len == 1 && name[0] == '.');
        bool dotdot = (name_len == 2 && name[0] == '.' && name[1] == '.');

        if (dot) {
            *saw_dot = true;
            if (child != ino) {
                found(c, FSCK_DIR_BAD_PARENT);
            }
        } else if (dotdot) {
            *saw_dotdot = true;
            if (child != parent) {
                found(c, FSCK_DIR_BAD_PARENT);
            }
        }

        if (child > c->inodes_count) {
            found(c, FSCK_DIR_BAD_RECORD);
            off += len;
            continue;
        }

        /*
         * a name is a link, whatever it points at, and dot and dotdot
         * are links too, which is why a directory's count is two
         * before anything is in it
         */
        c->links[child - 1]++;

        if (!dot && !dotdot) {
            if (!bit_get(c->inode_seen, child - 1)) {
                bit_set(c->inode_seen, child - 1);
                if (!inode_in_use(c, child)) {
                    found(c, FSCK_INODE_UNMARKED);
                }

                uint8_t inode[256];
                if (read_inode(c, child, inode)) {
                    claim_blocks_of(c, child, inode);
                    if ((rd16(inode) & EXT4_S_IFMT) == EXT4_S_IFDIR) {
                        enqueue(c, child, ino);
                    }
                }
            }
        }
        off += len;
    }
}

/* every block of a directory, whichever map it uses. */
static void walk_dir(struct check *c, uint32_t ino, uint32_t parent)
{
    uint8_t inode[256];
    if (!read_inode(c, ino, inode)) {
        return;
    }

    bool saw_dot = false, saw_dotdot = false;
    uint32_t size = rd32(inode + 4);
    uint32_t blocks = (size + c->block_size - 1) / c->block_size;

    for (uint32_t i = 0; i < blocks; i++) {
        uint32_t block = 0;

        if (rd32(inode + 32) & EXT4_EXTENTS_FL) {
            /*
             * the same walk as the driver's, kept short: a directory
             * deep enough to need an index node is one this machine
             * will not make
             */
            const uint8_t *node = inode + 40;
            if (rd16(node + 0) != EXT4_EXTENT_MAGIC || rd16(node + 6) != 0) {
                found(c, FSCK_EXTENT_BAD);
                return;
            }
            uint32_t entries = rd16(node + 2);
            for (uint32_t e = 0; e < entries; e++) {
                const uint8_t *rec = node + 12 + e * 12;
                uint32_t first = rd32(rec + 0);
                uint32_t len = rd16(rec + 4);
                if (len > 32768) {
                    len -= 32768;
                }
                if (i >= first && i < first + len) {
                    block = (uint32_t)((((uint64_t)rd16(rec + 6) << 32)
                                        | rd32(rec + 8)) + (i - first));
                    break;
                }
            }
        } else if (i < 12) {
            block = rd32(inode + 40 + i * 4);
        } else {
            uint32_t ind = rd32(inode + 40 + 12 * 4);
            if (ind == 0 || !read_block(c, ind, c->buf)) {
                continue;
            }
            block = rd32(c->buf + (i - 12) * 4);
        }

        if (block == 0 || block >= c->blocks_count) {
            continue;
        }
        if (!read_block(c, block, c->dir)) {
            continue;
        }
        walk_dir_block(c, c->dir, ino, parent, &saw_dot, &saw_dotdot);
    }

    if (!saw_dot || !saw_dotdot) {
        found(c, FSCK_DIR_NO_DOTS);
    }
}



/* the block bitmaps, against what the walk actually found claimed. */
static void check_block_bitmaps(struct check *c)
{
    for (uint32_t g = 0; g < c->groups; g++) {
        uint32_t off;
        if (!read_gd(c, g, &off)) {
            return;
        }
        uint32_t bitmap = rd32(c->side + off + 0);
        if (!read_block(c, bitmap, c->buf)) {
            continue;
        }

        uint32_t first = c->first_data_block + g * c->blocks_per_group;
        uint32_t count = c->blocks_per_group;
        if (first + count > c->blocks_count) {
            count = c->blocks_count - first;
        }

        bool changed = false;
        uint32_t free_here = 0;

        for (uint32_t i = 0; i < count; i++) {
            bool marked = bit_get(c->buf, i);
            bool claimed = bit_get(c->block_seen, first + i);

            if (claimed && !marked) {
                found(c, FSCK_BLOCK_UNMARKED);
                if (c->write != NULL) {
                    bit_set(c->buf, i);
                    marked = true;
                    changed = true;
                    mended(c, FSCK_BLOCK_UNMARKED);
                }
            } else if (!claimed && marked) {
                found(c, FSCK_BLOCK_LEAKED);
                if (c->write != NULL) {
                    bit_clear(c->buf, i);
                    marked = false;
                    changed = true;
                    mended(c, FSCK_BLOCK_LEAKED);
                }
            }
            if (!marked) {
                free_here++;
            }
        }

        if (changed && !write_block(c, bitmap, c->buf)) {
            return;
        }
        c->report->free_blocks += free_here;

        /*
         * and the count in the descriptor, which is a cache of what was
         * just counted and is allowed to be wrong in exactly no way
         */
        if (!read_gd(c, g, &off)) {
            return;
        }
        if (rd16(c->side + off + 12) != (uint16_t)free_here) {
            found(c, FSCK_COUNTS_WRONG);
            if (c->write != NULL) {
                wr16(c->side + off + 12, (uint16_t)free_here);
                uint32_t per = c->block_size / c->desc_size;
                if (write_block(c, c->gdt_block + g / per, c->side)) {
                    mended(c, FSCK_COUNTS_WRONG);
                }
            }
        }
    }
}

/*
 * the inode bitmaps and the link counts together, since both are about
 * a name pointing at an inode and the walk counted both at once
 */
static void check_inodes(struct check *c)
{
    for (uint32_t g = 0; g < c->groups; g++) {
        uint32_t off;
        if (!read_gd(c, g, &off)) {
            return;
        }
        /*
         * into `dir` rather than `buf`, because the loop below reads
         * inodes and reading an inode reads a block, and the bitmap
         * being iterated over would be the block it landed on. that is
         * a bug this had: the first inode read turned the rest of the
         * bitmap into whatever the inode table held there
         */
        uint32_t bitmap = rd32(c->side + off + 4);
        if (!read_block(c, bitmap, c->dir)) {
            continue;
        }

        /*
         * FIXME: when a group's inode range begins past inodes_count this
         * subtracts the other way round and wraps, so count becomes four
         * billion: the loop then walks off the end of the bitmap buffer
         * and links[ino - 1] walks off the end of the workspace. groups
         * is worked out from the *block* count and nothing holds the two
         * counts together, which is exactly the inconsistency a checker
         * gets handed. clamp first against inodes_count first and let a
         * group with no inodes in it have a count of zero. the block
         * equivalent above is safe only because groups comes from the
         * same number it is clamped against.
         */
        uint32_t first = g * c->inodes_per_group;
        uint32_t count = c->inodes_per_group;
        if (first + count > c->inodes_count) {
            count = c->inodes_count - first;
        }

        uint32_t free_here = 0;
        for (uint32_t i = 0; i < count; i++) {
            uint32_t ino = first + i + 1;
            bool marked = bit_get(c->dir, i);
            bool reached = bit_get(c->inode_seen, ino - 1) || ino == ROOT_INO;

            if (!marked) {
                free_here++;
                continue;
            }
            if (ino < c->first_ino) {
                continue;       /* the format's own, which no name names */
            }
            if (!reached) {
                found(c, FSCK_INODE_ORPHANED);

                /* an orphan whose link count is zero is not a judgement call. */
                uint8_t inode[256];
                if (!read_inode(c, ino, inode) || rd16(inode + 26) != 0) {
                    continue;
                }
                if (c->write == NULL) {
                    continue;
                }
                /*
                 * its blocks are already being given back: nothing
                 * reached this inode, so nothing claimed them, so the
                 * bitmap pass above saw them as leaked
                 */
                memset(inode, 0, c->inode_size);
                if (!write_inode(c, ino, inode)) {
                    continue;
                }
                bit_clear(c->dir, i);
                free_here++;
                mended(c, FSCK_INODE_ORPHANED);
                if (!write_block(c, bitmap, c->dir)) {
                    continue;
                }
                continue;
            }

            uint8_t inode[256];
            if (!read_inode(c, ino, inode)) {
                continue;
            }
            uint16_t claimed = rd16(inode + 26);
            uint16_t real = c->links[ino - 1];
            if (claimed != real) {
                found(c, FSCK_LINKS_WRONG);
                if (c->write != NULL) {
                    wr16(inode + 26, real);
                    if (write_inode(c, ino, inode)) {
                        mended(c, FSCK_LINKS_WRONG);
                    }
                }
            }
        }

        if (!read_gd(c, g, &off)) {
            return;
        }
        if (rd16(c->side + off + 14) != (uint16_t)free_here) {
            found(c, FSCK_COUNTS_WRONG);
            if (c->write != NULL) {
                wr16(c->side + off + 14, (uint16_t)free_here);
                uint32_t per = c->block_size / c->desc_size;
                if (write_block(c, c->gdt_block + g / per, c->side)) {
                    mended(c, FSCK_COUNTS_WRONG);
                }
            }
        }
        c->report->free_inodes += free_here;
    }
}

/*
 * and the two counts in the superblock, which are a cache of the sum of
 * every group's
 */
static void check_super_counts(struct check *c)
{
    uint8_t sector[FSCK_SECTOR * 2];
    if (!c->read(c->ctx, SB_OFFSET / FSCK_SECTOR, 2, sector)) {
        return;
    }
    bool wrong = rd32(sector + 12) != c->report->free_blocks
              || rd32(sector + 16) != c->report->free_inodes;
    if (!wrong) {
        return;
    }
    found(c, FSCK_COUNTS_WRONG);
    if (c->write == NULL) {
        return;
    }
    wr32(sector + 12, c->report->free_blocks);
    wr32(sector + 16, c->report->free_inodes);
    if (c->write(c->ctx, SB_OFFSET / FSCK_SECTOR, 2, sector)) {
        mended(c, FSCK_COUNTS_WRONG);
    }
}



bool fsck_run(fsck_io read, fsck_out write, void *ctx,
              void *workspace, size_t workspace_size,
              struct fsck_report *out)
{
    /*
     * XXX: static, so one check exists in the machine at a time. the
     * reason given is right, two blocks are not a stack's worth, but
     * they are a workspace's worth: the caller already hands over a
     * buffer sized for exactly the nine block buffers this struct holds
     * pointers to, and buf and side could be carved from it beside them.
     * then c is a local, and two cores checking two filesystems cannot
     * share one queue, one links table and one report pointer.
     */
    static struct check c;      /* two blocks of buffer: not a stack's worth */

    memset(&c, 0, sizeof c);
    memset(out, 0, sizeof *out);
    c.read = read;
    c.write = write;
    c.ctx = ctx;
    c.report = out;

    if (!read_super(&c)) {
        return false;
    }

    size_t want = fsck_workspace_bytes(c.blocks_count, c.inodes_count);
    if (workspace == NULL || workspace_size < want) {
        out->stopped = "not enough room to check a filesystem this size";
        return false;
    }
    memset(workspace, 0, want);

    uint8_t *at = (uint8_t *)workspace;
    c.block_seen = at;
    at += (c.blocks_count + 7) / 8;
    c.inode_seen = at;
    at += (c.inodes_count + 7) / 8;

    /*
     * the two that are read as something wider than a byte get aligned
     * on the way past, since the bitmaps above are any length at all
     */
    at += (8 - ((uintptr_t)at & 7)) & 7;
    c.links = (uint16_t *)(void *)at;
    at += (size_t)c.inodes_count * sizeof(uint16_t);
    at += (8 - ((uintptr_t)at & 7)) & 7;
    c.queue = (uint32_t *)(void *)at;
    at += (size_t)c.inodes_count * 2 * sizeof(uint32_t);

    c.dir = at;
    at += FSCK_MAX_BLOCK;
    for (int i = 0; i < 5; i++) {
        c.node[i] = at;
        at += FSCK_MAX_BLOCK;
    }
    for (int i = 0; i < 3; i++) {
        c.table[i] = at;
        at += FSCK_MAX_BLOCK;
    }

    /*
     * the metadata claims its own space first: without this every block
     * of every bitmap and every inode table reads as a leak, which is
     * true of nothing and would be "mended" into a filesystem that
     * hands out its own superblock.
     *
     * and the *copies* count too. the superblock and the descriptor
     * table are repeated at the front of every group, that is what
     * makes a filesystem with a damaged block 1 recoverable at all,
     * and a checker that only knew about the originals would call every
     * copy a leak and free it. which it did, on the first image it was
     * ever pointed at: two blocks, group 1's pair, and the free counts
     * wrong by exactly two behind them
     */
    uint32_t gdt_blocks = (c.groups * c.desc_size + c.block_size - 1)
                          / c.block_size;
    for (uint32_t g = 0; g < c.groups; g++) {
        if (!group_has_super(&c, g)) {
            continue;
        }
        uint32_t at = c.first_data_block + g * c.blocks_per_group;
        claim(&c, at);
        /* the table, and the room it may grow into. */
        for (uint32_t i = 0; i < gdt_blocks + c.reserved_gdt; i++) {
            claim(&c, at + 1 + i);
        }
    }

    for (uint32_t g = 0; g < c.groups; g++) {
        uint32_t off;
        if (!read_gd(&c, g, &off)) {
            out->stopped = "the group descriptors would not read";
            return false;
        }
        uint32_t bbm = rd32(c.side + off + 0);
        uint32_t ibm = rd32(c.side + off + 4);
        uint32_t tbl = rd32(c.side + off + 8);
        claim(&c, bbm);
        claim(&c, ibm);
        uint32_t table_blocks = (c.inodes_per_group * c.inode_size
                                 + c.block_size - 1) / c.block_size;
        for (uint32_t i = 0; i < table_blocks; i++) {
            claim(&c, tbl + i);
        }
    }

    /* the reserved inodes claim their blocks too. */
    for (uint32_t ino = 1; ino < c.first_ino; ino++) {
        uint8_t reserved[256];
        if (!inode_in_use(&c, ino) || !read_inode(&c, ino, reserved)) {
            continue;
        }
        if (ino == ROOT_INO) {
            continue;           /* claimed below, with the tree */
        }
        if (rd16(reserved) == 0 && rd32(reserved + 4) == 0) {
            continue;           /* in the bitmap and never written */
        }
        claim_blocks_of(&c, ino, reserved);
    }

    /* then the tree, from the root down. */
    uint8_t root[256];
    if (!read_inode(&c, ROOT_INO, root)) {
        out->stopped = "the root inode would not read";
        return false;
    }
    bit_set(c.inode_seen, ROOT_INO - 1);
    claim_blocks_of(&c, ROOT_INO, root);

    /* and then the queue, until it runs dry. */
    enqueue(&c, ROOT_INO, ROOT_INO);
    while (c.walked < c.queued) {
        uint32_t ino = c.queue[c.walked * 2 + 0];
        uint32_t parent = c.queue[c.walked * 2 + 1];
        c.walked++;
        walk_dir(&c, ino, parent);
    }

    check_block_bitmaps(&c);
    check_inodes(&c);
    check_super_counts(&c);
    return true;
}

const char *fsck_problem_name(enum fsck_problem which)
{
    switch (which) {
    case FSCK_SUPERBLOCK:     return "the superblock contradicts itself";
    case FSCK_BLOCK_UNMARKED: return "a block in use that the bitmap calls free";
    case FSCK_BLOCK_CROSSED:  return "a block two files both claim";
    case FSCK_BLOCK_LEAKED:   return "a block marked used that nothing claims";
    case FSCK_INODE_UNMARKED: return "a name pointing at a free inode";
    case FSCK_INODE_ORPHANED: return "an inode in use that no name reaches";
    case FSCK_LINKS_WRONG:    return "a link count that is not the number of names";
    case FSCK_DIR_NO_DOTS:    return "a directory without . or ..";
    case FSCK_DIR_BAD_PARENT: return ".. that is not the parent";
    case FSCK_DIR_BAD_RECORD: return "a directory record that walks off its block";
    case FSCK_EXTENT_BAD:     return "an extent tree that does not parse";
    case FSCK_COUNTS_WRONG:   return "a free count that is not the true one";
    default:                  return NULL;
    }
}
