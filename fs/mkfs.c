// SPDX-License-Identifier: GPL-2.0-only
/*
 * fs/mkfs.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * ext2, laid out from nothing.
 */

#include "fs/mkfs.h"
#include "lib/string.h"

/* ext2, laid out from nothing. */

#define BLOCK_SIZE        1024
#define SECTORS_PER_BLOCK (BLOCK_SIZE / 512)
#define INODE_SIZE        128
#define INODES_PER_GROUP  512
#define BLOCKS_PER_GROUP  (BLOCK_SIZE * 8)      /* one bitmap block covers
                                                 * exactly this many */
#define FIRST_DATA_BLOCK  1                     /* block 0 is the boot block
                                                 * and belongs to no group */
#define FIRST_INO         11                    /* 1..10 are the format's */
#define ROOT_INO          2

#define EXT4_MAGIC          0xef53
#define EXT4_COMPAT_HAS_JOURNAL 0x0004
#define JOURNAL_INO            8
#define JBD_MAGIC              0xc03b3998u
#define JBD_SUPERBLOCK_V2      4

/* how big a log to lay down. */
#define JOURNAL_BLOCKS_MAX     1024
#define EXT4_INCOMPAT_FILETYPE 0x0002
#define EXT4_INCOMPAT_EXTENTS  0x0040
#define EXT4_EXTENTS_FL        0x00080000
#define EXT4_EXTENT_MAGIC      0xf30a

#define S_IFDIR 0x4000
#define EXT4_FT_DIR 2

#define GROUP_DESC_SIZE 32

/*
 * the descriptor table describes every group, so a filesystem that gains
 * groups eventually needs a longer one, and the table sits at the
 * front of every group with the bitmaps and the inode table directly
 * behind it. a table that grew by a block would push all of those along
 * by a block, in *every* group at once, which means moving every bitmap
 * and every inode table on the disk. nobody does that; it is not a
 * resize, it is a rewrite.
 *
 * so the format's answer, and this one: reserve the room up front. a
 * handful of blocks after the table that belong to it and are marked
 * used from the day the filesystem is made, so that growing the table
 * is filling in a block that was already there and nothing behind it
 * moves. `s_reserved_gdt_blocks` in the superblock says how many.
 *
 * the number is chosen for the disk in front of it rather than fixed:
 * enough to describe sixty-four times what is being formatted, which is
 * the difference between a 64 MiB disk and a 4 GiB one, capped so that a
 * small filesystem does not spend a quarter of itself on room it will
 * never use
 */
#define RESERVED_GDT_GROWTH  64
#define RESERVED_GDT_MAX     32

/*
 * the format is little endian whatever the machine is, so these write it
 * a byte at a time rather than casting a pointer at it. that also means
 * no alignment to worry about, which matters because these write into the
 * middle of a block buffer at whatever offset the layout says
 */

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

/* the journal, and only the journal, is big endian */
static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/*
 * worked out once, up front, because every part of the write depends on
 * it and a formatter that recomputed it in three places would be a
 * formatter with three chances to disagree with itself
 */

struct layout {
    uint64_t blocks;            /* the whole filesystem */
    uint32_t groups;
    uint32_t gdt_blocks;        /* how many blocks the descriptor table needs */
    uint32_t reserved_gdt;      /* and how many it may grow into */
    uint32_t inode_table_blocks;
    uint64_t inodes;

    /*
     * what every group spends on describing itself, in blocks: a
     * superblock copy, the descriptor table, two bitmaps, the inode
     * table. no sparse_super, so every group pays this and not just the
     * first, which is the older and simpler arrangement, and costs
     * about two percent of a small disk
     */
    uint32_t overhead;

    /* the log, which is laid down once and never moved. */
    uint32_t journal_blocks;
};

/* where group `g` begins, and where its pieces sit inside it */
static uint64_t group_base(uint32_t g)
{
    return (uint64_t)FIRST_DATA_BLOCK + (uint64_t)g * BLOCKS_PER_GROUP;
}

/*
 * the reserved blocks sit between the table and the bitmaps, which is
 * the whole point of them: the table grows into the gap rather than
 * through what is behind it, so `gdt_blocks + reserved_gdt` is a
 * constant for the life of the filesystem and nothing below ever moves
 */
static uint64_t group_block_bitmap(const struct layout *l, uint32_t g)
{
    return group_base(g) + 1 + l->gdt_blocks + l->reserved_gdt;
}

static uint64_t group_inode_bitmap(const struct layout *l, uint32_t g)
{
    return group_block_bitmap(l, g) + 1;
}

static uint64_t group_inode_table(const struct layout *l, uint32_t g)
{
    return group_inode_bitmap(l, g) + 1;
}

/* how many blocks this group actually has. */
static uint32_t group_blocks(const struct layout *l, uint32_t g)
{
    uint64_t base = group_base(g);
    uint64_t left = l->blocks - base;
    return (left > BLOCKS_PER_GROUP) ? BLOCKS_PER_GROUP : (uint32_t)left;
}

static bool plan(uint64_t sectors, struct layout *l, const char **error)
{
    l->blocks = sectors / SECTORS_PER_BLOCK;
    if (l->blocks <= FIRST_DATA_BLOCK) {
        *error = "that is not enough disk to hold a superblock";
        return false;
    }

    uint64_t describable = l->blocks - FIRST_DATA_BLOCK;
    l->groups = (uint32_t)((describable + BLOCKS_PER_GROUP - 1)
                           / BLOCKS_PER_GROUP);
    if (l->groups == 0) {
        l->groups = 1;
    }

    l->inodes = (uint64_t)l->groups * INODES_PER_GROUP;
    l->inode_table_blocks = (INODES_PER_GROUP * INODE_SIZE) / BLOCK_SIZE;

    uint64_t gdt_bytes = (uint64_t)l->groups * GROUP_DESC_SIZE;
    l->gdt_blocks = (uint32_t)((gdt_bytes + BLOCK_SIZE - 1) / BLOCK_SIZE);

    /* and the room for it to grow into. */
    uint64_t far = l->blocks * RESERVED_GDT_GROWTH;
    uint64_t far_groups = (far + BLOCKS_PER_GROUP - 1) / BLOCKS_PER_GROUP;
    uint64_t far_gdt = (far_groups * GROUP_DESC_SIZE + BLOCK_SIZE - 1)
                       / BLOCK_SIZE;
    l->reserved_gdt = (uint32_t)(far_gdt > l->gdt_blocks
                                 ? far_gdt - l->gdt_blocks : 0);
    if (l->reserved_gdt > RESERVED_GDT_MAX) {
        l->reserved_gdt = RESERVED_GDT_MAX;
    }

    l->overhead = 1 + l->gdt_blocks + l->reserved_gdt + 2
                + l->inode_table_blocks;

    /*
     * a small disk pays for this proportionally and a very small one
     * would pay everything, so the reservation gives way rather than the
     * filesystem. room to grow is worth having and worth nothing at all
     * next to room to hold a file
     */
    uint32_t have = group_blocks(l, 0);
    while (l->reserved_gdt > 0 && l->overhead * 4 > have) {
        l->reserved_gdt--;
        l->overhead--;
    }

    /*
     * the first group has to have room for its own metadata *and* a
     * block left over for the root directory to live in. a disk that
     * cannot manage that cannot be an ext2 filesystem at all, and saying
     * so is better than producing one that mounts and then cannot hold
     * a file
     */
    if (group_blocks(l, 0) <= l->overhead) {
        *error = "too small: the metadata does not leave room for a root";
        return false;
    }
    return true;
}



struct writer {
    mkfs_io  io;
    void    *ctx;
    bool     failed;
};

static void write_block(struct writer *w, uint64_t block, const uint8_t *buf)
{
    if (w->failed) {
        return;
    }
    if (!w->io(w->ctx, block * SECTORS_PER_BLOCK, SECTORS_PER_BLOCK, buf)) {
        w->failed = true;
    }
}

/*
 * set the first `n` bits of a bitmap block, which is what "these are
 * taken" looks like when everything taken is at the front
 */
static void set_first_bits(uint8_t *block, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        block[i / 8] |= (uint8_t)(1u << (i % 8));
    }
}

/*
 * and the tail past the end of a short group, which must read as used,
 * a checker that believes in those blocks will hand one out
 */
static void set_bits_from(uint8_t *block, uint32_t from)
{
    for (uint32_t i = from; i < BLOCK_SIZE * 8; i++) {
        block[i / 8] |= (uint8_t)(1u << (i % 8));
    }
}

static void fill_superblock(uint8_t *sb, const struct layout *l,
                            uint64_t free_blocks, uint64_t free_inodes,
                            const char *label, uint32_t now,
                            uint32_t group_nr)
{
    memset(sb, 0, BLOCK_SIZE);

    put32(sb + 0,  (uint32_t)l->inodes);
    put32(sb + 4,  (uint32_t)l->blocks);
    put32(sb + 8,  0);                      /* none reserved for root. */
    put32(sb + 12, (uint32_t)free_blocks);
    put32(sb + 16, (uint32_t)free_inodes);
    put32(sb + 20, FIRST_DATA_BLOCK);
    put32(sb + 24, 0);                      /* log2(1024/1024) */
    put32(sb + 28, 0);
    put32(sb + 32, BLOCKS_PER_GROUP);
    put32(sb + 36, BLOCKS_PER_GROUP);
    put32(sb + 40, INODES_PER_GROUP);
    put32(sb + 44, now);                    /* mount time */
    put32(sb + 48, now);                    /* write time */
    put16(sb + 52, 0);                      /* mounts since the last check */
    put16(sb + 54, 0xffff);                 /* and no limit on them */
    put16(sb + 56, EXT4_MAGIC);
    put16(sb + 58, 1);                      /* clean */
    put16(sb + 60, 1);                      /* on error: carry on */
    put16(sb + 62, 0);
    put32(sb + 64, now);                    /* last checked */
    put32(sb + 68, 0);                      /* and never mind again */
    put32(sb + 72, 0);                      /* creator os: linux, which is
                                             * what every reader expects */
    put32(sb + 76, 1);                      /* revision 1, which is what
                                             * makes the fields below mean
                                             * anything */
    put16(sb + 80, 0);
    put16(sb + 82, 0);

    put32(sb + 84, FIRST_INO);
    put16(sb + 88, INODE_SIZE);
    put16(sb + 90, (uint16_t)group_nr);     /* which copy this is */

    /* what this filesystem claims to be. */
    put32(sb + 92,  l->journal_blocks > 0 ? EXT4_COMPAT_HAS_JOURNAL : 0);
    put32(sb + 96,  EXT4_INCOMPAT_FILETYPE | EXT4_INCOMPAT_EXTENTS);
    put32(sb + 100, 0);

    put32(sb + 224, JOURNAL_INO);           /* where the log's inode is */
    put16(sb + 206, (uint16_t)l->reserved_gdt);     /* room to grow into */

    if (label != NULL) {
        for (size_t i = 0; i < 16 && label[i] != '\0'; i++) {
            sb[120 + i] = (uint8_t)label[i];
        }
    }
}

bool mkfs_ext4(mkfs_io io, void *ctx, uint64_t sectors,
               const char *label, uint32_t now,
               struct mkfs_result *out, const char **error)
{
    *error = NULL;

    if (sectors < MKFS_MIN_SECTORS) {
        *error = "too small to be a filesystem";
        return false;
    }

    struct layout l;
    if (!plan(sectors, &l, error)) {
        return false;
    }

    struct writer w = { .io = io, .ctx = ctx, .failed = false };
    uint8_t block[BLOCK_SIZE];

    /* the root directory's one block, taken from the first group right after its metadata. */
    uint64_t root_block = group_base(0) + l.overhead;

    /* and the journal, immediately after the root directory. */
    /*
     * FIXME: this size is chosen by a rule of this file's own and never
     * held against the smallest log the journal will agree to mount. jbd2
     * refuses a log whose maxlen - first is under 2 * (JBD_MAX_BLOCKS +
     * 2), which is 36 blocks after the superblock; with first at 1 and a
     * log of blocks/16, any disk under 592 blocks gets one jbd2 will not
     * take. so the filesystem formats cleanly, reports its free space,
     * and comes up read-only, because a journal it cannot start means the
     * filesystem is mounted read-only. mkfs's own minimum is 512 sectors,
     * well inside that. size the log to what jbd2 accepts, or refuse the
     * disk the way the root check above does.
     */
    uint32_t journal_blocks = JOURNAL_BLOCKS_MAX;
    if (journal_blocks > l.blocks / 16) {
        journal_blocks = (uint32_t)(l.blocks / 16);
    }
    uint32_t have_group0 = group_blocks(&l, 0);
    if (l.overhead + 1 + journal_blocks > have_group0) {
        journal_blocks = 0;         /* too small to hold one */
    }
    l.journal_blocks = journal_blocks;
    uint64_t journal_block = root_block + 1;


    uint64_t free_blocks = 0;
    for (uint32_t g = 0; g < l.groups; g++) {
        uint32_t have = group_blocks(&l, g);
        uint32_t used = l.overhead
                      + (g == 0 ? 1 + journal_blocks : 0);  /* root, log */
        free_blocks += (have > used) ? (have - used) : 0;
    }
    uint64_t free_inodes = l.inodes - (FIRST_INO - 1);


    memset(block, 0, BLOCK_SIZE);
    write_block(&w, 0, block);


    for (uint32_t g = 0; g < l.groups; g++) {
        uint32_t have = group_blocks(&l, g);
        uint32_t used = l.overhead + (g == 0 ? 1 + journal_blocks : 0);

        /* the superblock, and a copy of it in every group. */
        fill_superblock(block, &l, free_blocks, free_inodes, label, now, g);
        write_block(&w, group_base(g), block);

        /*
         * the descriptor table, which describes *every* group and is
         * therefore identical in each copy
         */
        for (uint32_t part = 0; part < l.gdt_blocks; part++) {
            memset(block, 0, BLOCK_SIZE);

            uint32_t first = part * (BLOCK_SIZE / GROUP_DESC_SIZE);
            for (uint32_t i = 0; i < BLOCK_SIZE / GROUP_DESC_SIZE; i++) {
                uint32_t d = first + i;
                if (d >= l.groups) {
                    break;
                }
                uint32_t d_have = group_blocks(&l, d);
                uint32_t d_used = l.overhead
                                + (d == 0 ? 1 + journal_blocks : 0);
                uint8_t *desc = block + i * GROUP_DESC_SIZE;

                put32(desc + 0, (uint32_t)group_block_bitmap(&l, d));
                put32(desc + 4, (uint32_t)group_inode_bitmap(&l, d));
                put32(desc + 8, (uint32_t)group_inode_table(&l, d));
                put16(desc + 12, (uint16_t)(d_have > d_used
                                            ? d_have - d_used : 0));
                put16(desc + 14, (uint16_t)(d == 0
                                            ? INODES_PER_GROUP - (FIRST_INO - 1)
                                            : INODES_PER_GROUP));
                put16(desc + 16, (uint16_t)(d == 0 ? 1 : 0));   /* the root */
            }
            write_block(&w, group_base(g) + 1 + part, block);
        }

        /*
         * and the room the table may grow into, written as zeroes
         * rather than left as whatever was on the disk. a reserved
         * block is a descriptor block that has not been filled in yet,
         * and the day it is filled in only part of it will be, so the
         * rest of it had better already read as "no such group"
         */
        memset(block, 0, BLOCK_SIZE);
        for (uint32_t part = 0; part < l.reserved_gdt; part++) {
            write_block(&w, group_base(g) + 1 + l.gdt_blocks + part, block);
        }

        /*
         * the block bitmap. everything this group spends on itself is at
         * the front of it, so the used blocks are a prefix, and in
         * group 0 the root's block comes directly after them
         */
        memset(block, 0, BLOCK_SIZE);
        set_first_bits(block, used);        /* metadata, root, then the log */
        if (have < BLOCK_SIZE * 8) {
            set_bits_from(block, have);
        }
        write_block(&w, group_block_bitmap(&l, g), block);

        /*
         * the inode bitmap. only group 0 has anything taken: the ten
         * inodes the format reserves, root among them
         */
        memset(block, 0, BLOCK_SIZE);
        if (g == 0) {
            set_first_bits(block, FIRST_INO - 1);
        }
        set_bits_from(block, INODES_PER_GROUP);
        write_block(&w, group_inode_bitmap(&l, g), block);

        /* and the inode table, empty except for root */
        for (uint32_t t = 0; t < l.inode_table_blocks; t++) {
            memset(block, 0, BLOCK_SIZE);

            if (g == 0 && t == 0) {
                /*
                 * root is inode 2, so the second slot in the first block
                 * of the first group's table
                 */
                uint8_t *ino = block + (ROOT_INO - 1) * INODE_SIZE;
                put16(ino + 0,  S_IFDIR | 0755);
                put16(ino + 2,  0);             /* owned by the master */
                put32(ino + 4,  BLOCK_SIZE);    /* one block of entries */
                put32(ino + 8,  now);
                put32(ino + 12, now);
                put32(ino + 16, now);
                put32(ino + 20, 0);             /* not deleted */
                put16(ino + 24, 0);
                put16(ino + 26, 2);             /* `.` and its own name in
                                                 * itself, a directory
                                                 * with one link is one
                                                 * nothing points at */
                put32(ino + 28, SECTORS_PER_BLOCK);  /* i_blocks counts 512s */

                /*
                 * and where its one block is, said the ext4 way: a flag
                 * that these sixty bytes are a tree rather than a list,
                 * a header saying the tree is one node deep and holds
                 * one record, and the record, logical block 0, one
                 * block long, at root_block.
                 *
                 * the same thing ext2 said in four bytes takes
                 * twenty-four here, and that is the trade: it costs
                 * more to say where one block is and nothing more to
                 * say where a million contiguous ones are
                 */
                put32(ino + 32, EXT4_EXTENTS_FL);
                put16(ino + 40, EXT4_EXTENT_MAGIC);
                put16(ino + 42, 1);             /* one record in use */
                put16(ino + 44, 4);             /* and room for four */
                put16(ino + 46, 0);             /* a leaf: no depth */
                put32(ino + 48, 0);             /* generation */
                put32(ino + 52, 0);             /* starting at block 0 */
                put16(ino + 56, 1);             /* one block long */
                put16(ino + 58, 0);             /* the high half of where */
                put32(ino + 60, (uint32_t)root_block);
            }
            /*
             * inode 8 is the journal's, and it is a file like any other
             *, one extent, the run laid down after the root. every
             * other reader of this disk finds the log by reading this
             * inode, which is why it is written rather than the log's
             * position being implied
             */
            if (g == 0 && t == 0 && journal_blocks > 0) {
                uint8_t *jn = block + (JOURNAL_INO - 1) * INODE_SIZE;
                put16(jn + 0,  0x8000 | 0600);      /* a regular file */
                put32(jn + 4,  journal_blocks * BLOCK_SIZE);
                put32(jn + 8,  now);
                put32(jn + 12, now);
                put32(jn + 16, now);
                put16(jn + 26, 1);
                put32(jn + 28, journal_blocks * SECTORS_PER_BLOCK);
                put32(jn + 32, EXT4_EXTENTS_FL);
                put16(jn + 40, EXT4_EXTENT_MAGIC);
                put16(jn + 42, 1);
                put16(jn + 44, 4);
                put16(jn + 46, 0);
                put32(jn + 48, 0);
                put32(jn + 52, 0);
                put16(jn + 56, (uint16_t)journal_blocks);
                put16(jn + 58, 0);
                put32(jn + 60, (uint32_t)journal_block);
            }
            write_block(&w, group_inode_table(&l, g) + t, block);
        }
    }

    /* two entries, and the second one's length runs to the end of the block. */
    memset(block, 0, BLOCK_SIZE);

    put32(block + 0, ROOT_INO);
    put16(block + 4, 12);                   /* 8 + a one-byte name, rounded */
    block[6] = 1;                           /* name_len */
    block[7] = EXT4_FT_DIR;
    block[8] = '.';

    put32(block + 12, ROOT_INO);
    put16(block + 16, BLOCK_SIZE - 12);     /* all the way to the end */
    block[18] = 2;
    block[19] = EXT4_FT_DIR;
    block[20] = '.';
    block[21] = '.';

    write_block(&w, root_block, block);

    /* one block of superblock and the rest zeroes. */
    if (journal_blocks > 0) {
        memset(block, 0, BLOCK_SIZE);
        put_be32(block + 0,  JBD_MAGIC);
        put_be32(block + 4,  JBD_SUPERBLOCK_V2);
        put_be32(block + 8,  0);                    /* no sequence yet */
        put_be32(block + 12, BLOCK_SIZE);
        put_be32(block + 16, journal_blocks);
        put_be32(block + 20, 1);                    /* block 0 is this */
        put_be32(block + 24, 1);                    /* the first commit id */
        put_be32(block + 28, 0);                    /* empty */
        put_be32(block + 64, 1);                    /* one filesystem uses it */
        write_block(&w, journal_block, block);

        memset(block, 0, BLOCK_SIZE);
        for (uint32_t i = 1; i < journal_blocks; i++) {
            write_block(&w, journal_block + i, block);
        }
    }

    if (w.failed) {
        *error = "the drive stopped taking writes part way through";
        return false;
    }

    if (out != NULL) {
        out->blocks      = l.blocks;
        out->inodes      = l.inodes;
        out->free_blocks = free_blocks;
        out->groups      = l.groups;
    }
    return true;
}
