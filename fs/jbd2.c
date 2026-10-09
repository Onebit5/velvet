// SPDX-License-Identifier: GPL-2.0-only
/*
 * fs/jbd2.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the ext4 journal: writing it, and replaying it.
 */

#include <velvet/jbd2.h>
#include <velvet/ext4.h>
#include <velvet/string.h>

/* everything else on this disk is little endian and the journal is not. */

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void wbe32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static void wbe16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

/* the twelve bytes at the top of every block the journal owns */
static void put_header(uint8_t *b, uint32_t type, uint32_t sequence)
{
    wbe32(b + 0, JBD_MAGIC);
    wbe32(b + 4, type);
    wbe32(b + 8, sequence);
}

static bool header_is(const uint8_t *b, uint32_t type, uint32_t *sequence)
{
    if (be32(b + 0) != JBD_MAGIC) {
        return false;
    }
    if (sequence != NULL) {
        *sequence = be32(b + 8);
    }
    return be32(b + 4) == type;
}

/* the journal is a file, so block N of it is wherever the filesystem says block N of inode 8 is. */

/* straight out of the map, which was built at mount. */
static bool journal_block(struct jbd *j, uint32_t index, uint32_t *out)
{
    if (index >= j->maxlen || index >= JBD_MAP_MAX || j->map[index] == 0) {
        return false;
    }
    *out = j->map[index];
    return true;
}

static bool read_log(struct jbd *j, uint32_t index, void *into)
{
    uint32_t block;
    return journal_block(j, index, &block) && block != 0
        && ext4_raw_read(j->fs, block, into);
}

static bool write_log(struct jbd *j, uint32_t index, const void *from)
{
    uint32_t block;
    return journal_block(j, index, &block) && block != 0
        && ext4_raw_write(j->fs, block, from);
}

/* where recovery should begin, written into the log's own superblock. */
static bool write_super(struct jbd *j)
{
    static uint8_t sb[EXT4_MAX_BLOCK];
    if (!read_log(j, 0, sb)) {
        return false;
    }
    wbe32(sb + 24, j->sequence);
    wbe32(sb + 28, j->start);
    return write_log(j, 0, sb);
}

/*
 * the descriptor is written *last* even though it comes first, because
 * its tags are not all known until the blocks have been staged. that is
 * safe for exactly one reason: nothing in a transaction means anything
 * until the commit block lands behind it, so a half-written descriptor
 * in an uncommitted transaction is not a state anybody can observe
 */

/*
 * the log is a ring, and block 0 of it is the superblock, so it turns
 * over between `first` and `maxlen` and never past either
 */
static uint32_t ring(struct jbd *j, uint32_t at)
{
    if (at >= j->maxlen) {
        at = j->first + (at - j->first) % (j->maxlen - j->first);
    }
    return at;
}

bool jbd_open(const struct jbd *j)
{
    return j != NULL && j->ready && j->depth > 0;
}

void jbd_begin(struct jbd *j)
{
    if (j == NULL || !j->ready) {
        return;
    }
    if (j->depth == 0) {
        j->staged = 0;
    }
    j->depth++;
}

static bool close_transaction(struct jbd *j);

bool jbd_stage(struct jbd *j, uint32_t block, const void *data)
{
    if (!jbd_open(j)) {
        return false;
    }
    if (j->staged >= JBD_MAX_BLOCKS) {
        /* more than one transaction holds. */
        if (!close_transaction(j)) {
            return false;
        }
    }

    /*
     * a block already staged in this transaction is written again in
     * place rather than twice: a bitmap touched by two steps of one
     * operation should reach the log once, with its final contents
     */
    uint32_t slot = j->staged;
    for (uint32_t i = 0; i < j->staged; i++) {
        if (j->home[i] == block) {
            slot = i;
            break;
        }
    }

    /* the block goes into the log now, at the slot after the descriptor. */
    static uint8_t staged[EXT4_MAX_BLOCK];
    memcpy(staged, data, j->block_size);
    bool escaped = be32(staged) == JBD_MAGIC;
    if (escaped) {
        wbe32(staged, 0);
    }

    if (!write_log(j, ring(j, j->start + 1 + slot), staged)) {
        return false;
    }

    j->home[slot] = block;
    j->escaped[slot] = escaped;
    if (slot == j->staged) {
        j->staged++;
    }
    return true;
}

bool jbd_peek(struct jbd *j, uint32_t block, void *into)
{
    if (!jbd_open(j)) {
        return false;
    }
    for (uint32_t i = 0; i < j->staged; i++) {
        if (j->home[i] != block) {
            continue;
        }
        if (!read_log(j, ring(j, j->start + 1 + i), into)) {
            return false;
        }
        /*
         * and undo the escaping on the way out, so the caller reads back
         * the bytes it wrote rather than the ones the log had to store
         */
        if (j->escaped[i]) {
            wbe32(into, JBD_MAGIC);
        }
        return true;
    }
    return false;
}

/* put a transaction's blocks where they actually live. */
static bool checkpoint(struct jbd *j, uint32_t at, uint32_t count)
{
    static uint8_t block[EXT4_MAX_BLOCK];
    uint8_t descriptor[EXT4_MAX_BLOCK];

    if (!read_log(j, at, descriptor)) {
        return false;
    }

    uint32_t off = 12;
    for (uint32_t i = 0; i < count; i++) {
        if (off + 8 > j->block_size) {
            return false;
        }
        uint32_t home = be32(descriptor + off);
        uint16_t flags = be16(descriptor + off + 6);
        off += 8;
        if (!(flags & JBD_FLAG_SAME_UUID)) {
            off += 16;
        }

        if (!read_log(j, ring(j, at + 1 + i), block)) {
            return false;
        }
        if (flags & JBD_FLAG_ESCAPE) {
            wbe32(block, JBD_MAGIC);
        }
        if (!ext4_raw_write(j->fs, home, block)) {
            return false;
        }
        if (flags & JBD_FLAG_LAST_TAG) {
            break;
        }
    }
    return true;
}

/*
 * write down everything staged so far and put it where it lives: the
 * descriptor at the head, the commit block behind it, the blocks to
 * their homes, and three waits holding those four apart. this is the
 * whole of a transaction's ending, and it is a function of its own
 * because it has two callers, the obvious one, and a transaction that
 * filled up before the operation making it had finished
 */
static bool close_transaction(struct jbd *j)
{
    if (j->staged == 0) {
        return true;
    }

    /* the descriptor, now that the tags are known */
    static uint8_t descriptor[EXT4_MAX_BLOCK];
    memset(descriptor, 0, j->block_size);
    put_header(descriptor, JBD_DESCRIPTOR, j->sequence);

    uint32_t off = 12;
    for (uint32_t i = 0; i < j->staged; i++) {
        uint16_t flags = j->escaped[i] ? JBD_FLAG_ESCAPE : 0;
        if (i > 0) {
            flags |= JBD_FLAG_SAME_UUID;
        }
        if (i == j->staged - 1) {
            flags |= JBD_FLAG_LAST_TAG;
        }
        wbe32(descriptor + off, j->home[i]);
        wbe16(descriptor + off + 4, 0);         /* no tag checksum */
        wbe16(descriptor + off + 6, flags);
        off += 8;
        if (i == 0) {
            memcpy(descriptor + off, j->uuid, 16);
            off += 16;
        }
    }

    uint32_t at = j->start;
    uint32_t count = j->staged;
    j->staged = 0;              /* however this ends, it is over */

    if (!write_log(j, at, descriptor)) {
        return false;
    }

    /* and now the first two of the three waits. */
    if (!ext4_flush(j->fs)) {
        return false;
    }

    static uint8_t commit[EXT4_MAX_BLOCK];
    memset(commit, 0, j->block_size);
    put_header(commit, JBD_COMMIT, j->sequence);
    if (!write_log(j, ring(j, at + 1 + count), commit)) {
        return false;
    }
    if (!ext4_flush(j->fs)) {
        return false;
    }

    /*
     * from here the change is safe however badly the machine stops: it
     * is written down, and recovery will finish it.
     *
     * so a checkpoint that fails leaves the tail exactly where it is,
     * rather than moving past a transaction that did not entirely
     * arrive. the next mount replays it, which is the whole reason it
     * was written down first
     */
    if (!checkpoint(j, at, count)) {
        return false;
    }

    /* and the third wait, which is the one that is easy to leave out. */
    if (!ext4_flush(j->fs)) {
        return false;
    }

    j->start = ring(j, at + 2 + count);
    j->sequence++;

    /*
     * the tail itself goes down last, and it alone is only a hint: if
     * the machine stops before it is written, recovery begins further
     * back and replays transactions that had already reached their
     * homes. that is harmless, they are replayed in the order they
     * were written, so the last word is still the newest one, and the
     * flush above is what makes "already at home" true rather than
     * hoped for
     */
    return write_super(j);
}

bool jbd_commit(struct jbd *j)
{
    if (j == NULL || !j->ready || j->depth == 0) {
        return false;
    }
    if (--j->depth > 0) {
        return true;            /* an inner call: not finished yet */
    }
    return close_transaction(j);
}

/*
 * the log has nothing in it and nothing is coming: a start of zero,
 * which is what a cleanly unmounted journal says. the sequence goes
 * down with it and is one ahead of anything the log still physically
 * holds, so the next mount reads those old descriptors, finds a number
 * it is not expecting, and stops, which is why an empty log needs no
 * erasing
 */
bool jbd_close(struct jbd *j)
{
    if (j == NULL || !j->ready) {
        return false;
    }
    if (j->depth > 0 && !close_transaction(j)) {
        return false;
    }
    j->depth = 0;
    j->start = 0;
    if (!write_super(j)) {
        return false;
    }
    j->start = j->first;
    return ext4_flush(j->fs);
}

/*
 * walk forward from where the superblock says the log begins, following
 * descriptors and their blocks, and apply every transaction that has a
 * commit block behind it with the sequence number it should have.
 *
 * the walk stops at the first thing that is not what it should be,
 * a block with no magic, a sequence out of step, a missing commit,
 * and that is not an error. it is the end of the log: whatever the
 * machine was in the middle of when it stopped
 */

static bool replay(struct jbd *j)
{
    uint32_t at = j->start;
    uint32_t sequence = j->sequence;
    uint32_t applied = 0;

    static uint8_t descriptor[EXT4_MAX_BLOCK];

    for (uint32_t guard = 0; guard < j->maxlen; guard++) {
        uint32_t seen;
        if (!read_log(j, at, descriptor)) {
            break;
        }
        if (!header_is(descriptor, JBD_DESCRIPTOR, &seen) || seen != sequence) {
            break;
        }

        /* how many blocks this transaction has, by walking the tags */
        uint32_t count = 0;
        uint32_t off = 12;
        bool last = false;
        while (!last && off + 8 <= j->block_size && count < j->maxlen) {
            uint16_t flags = be16(descriptor + off + 6);
            off += 8;
            if (!(flags & JBD_FLAG_SAME_UUID)) {
                off += 16;
            }
            count++;
            last = (flags & JBD_FLAG_LAST_TAG) != 0;
        }
        if (!last || count == 0) {
            break;              /* a descriptor that was never finished */
        }

        /* and the commit behind them, which is what makes it real */
        static uint8_t commit[EXT4_MAX_BLOCK];
        if (!read_log(j, ring(j, at + 1 + count), commit)) {
            break;
        }
        if (!header_is(commit, JBD_COMMIT, &seen) || seen != sequence) {
            break;              /* it never finished. nothing after this
                                 * happened either */
        }

        if (!checkpoint(j, at, count)) {
            return false;
        }
        applied++;

        at = ring(j, at + 2 + count);
        sequence++;
    }

    if (applied > 0 && !ext4_flush(j->fs)) {
        return false;
    }

    j->start = at;
    j->sequence = sequence;

    /*
     * and say so, so that a second recovery does not replay what this
     * one finished, and, on a log with nothing in it, so that the
     * *first* transaction written after this is one recovery can find.
     * that is why this is not skipped when there was nothing to apply,
     * and why it is waited on: until the start is on the disk, a log
     * whose superblock still says zero is a log nobody will read
     */
    if (!write_super(j)) {
        return false;
    }
    if (!ext4_flush(j->fs)) {
        return false;
    }
    j->recovered = true;
    return true;
}

bool jbd_mount(struct jbd *j, struct ext4 *fs)
{
    memset(j, 0, sizeof *j);
    j->fs = fs;

    static uint8_t sb[EXT4_MAX_BLOCK];
    j->block_size = ext4_block_bytes(fs);
    if (j->block_size > EXT4_MAX_BLOCK) {
        return false;
    }
    uint32_t sb_block;
    if (!ext4_file_block(fs, EXT4_JOURNAL_INO, 0, &sb_block)
        || !ext4_raw_read(fs, sb_block, sb)) {
        return false;
    }

    uint32_t type;
    if (be32(sb + 0) != JBD_MAGIC) {
        return false;
    }
    type = be32(sb + 4);
    if (type != JBD_SUPERBLOCK_V1 && type != JBD_SUPERBLOCK_V2) {
        return false;
    }

    /*
     * a journal with features do not implement is one the kernel must not
     * write into and must not pretend to have recovered.
     *
     * every one of them changes what the blocks *mean*: revoke adds
     * records that say which blocks not to replay, the checksum ones
     * make a commit block without a correct crc32c a commit block that
     * did not happen, and async commit removes the very ordering this
     * depends on. reading such a log with none of that understood does
     * not give a partly-recovered filesystem, it gives a confidently
     * wrong one, so it is refused, and the disk stays read-only,
     * which is a filesystem nobody can damage.
     *
     * so the set understood here is empty, and the test is that the
     * journal claims nothing at all. the flags are named in jbd2.h
     * anyway, because a refusal is worth being able to read
     */
    if (be32(sb + 36) != 0 || be32(sb + 40) != 0) {
        return false;
    }
    if (be32(sb + 12) != j->block_size) {
        return false;
    }

    j->maxlen = be32(sb + 16);
    if (j->maxlen > JBD_MAP_MAX) {
        return false;           /* a log longer than the kernel keeps a map for */
    }
    j->first = be32(sb + 20);
    j->sequence = be32(sb + 24);
    j->start = be32(sb + 28);
    memcpy(j->uuid, sb + 48, 16);

    /*
     * room for a whole transaction and then some, since a log too short
     * to hold one is a log every commit would fail against. refusing it
     * leaves the filesystem read-only, which is the honest answer
     */
    if (j->first == 0 || j->first >= j->maxlen
        || j->maxlen - j->first < 2 * (JBD_MAX_BLOCKS + 2)) {
        return false;
    }
    if (j->sequence == 0) {
        j->sequence = 1;
    }

    /* and the map, before anything else reads a log block */
    for (uint32_t i = 0; i < j->maxlen; i++) {
        if (!ext4_file_block(j->fs, EXT4_JOURNAL_INO, i, &j->map[i])) {
            return false;
        }
    }

    /*
     * start of zero is what a cleanly unmounted journal says: there is
     * nothing in it, and the next transaction goes at the beginning.
     *
     * it is *not* a reason to skip recovery, and skipping it was the
     * second thing journalling broke. a freshly made log says zero, so
     * the first mount of a new filesystem would take that road, write
     * transactions, and never write a start anywhere until one of them
     * finished, leaving the whole of the first transaction outside
     * recovery's reach. a machine stopped between its commit block and
     * its blocks reaching home came back to an inode allocated, named
     * nowhere, and claiming a link: exactly the damage the journal was
     * added to prevent.
     *
     * so an empty log is replayed too. it costs one read, because the
     * first thing recovery looks at is a block of zeroes with no magic
     * in it and it stops there, and the walk ends by writing the
     * start down, which is what closes the window. what keeps a *stale*
     * transaction from being replayed is the sequence number, which is
     * one ahead of anything the log holds, and never the start
     */
    if (j->start == 0) {
        j->start = j->first;
    }

    j->ready = true;
    return replay(j);
}
