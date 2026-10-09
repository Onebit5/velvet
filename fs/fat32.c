// SPDX-License-Identifier: GPL-2.0-only
/*
 * fs/fat32.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * fat32, read and written by hand.
 */

#include <velvet/fat32.h>
#include <velvet/string.h>

#define BAD_CLUSTER  0x0ffffff7u
#define EOC          0x0ffffff8u     /* anything at or above ends a chain */
#define CLUSTER_MASK 0x0fffffffu     /* the top four bits are not the kernel's */

/*
 * squeezed into sixteen bits each, which is why seconds go in twos and
 * why the year cannot be earlier than 1980: there was no room for a
 * century, so the epoch was simply declared
 */

static uint16_t pack_date(const struct fat32_time *t)
{
    if (t->year < 1980) {
        return 0;
    }
    return (uint16_t)(((t->year - 1980) << 9) | ((t->month & 0xf) << 5)
                      | (t->day & 0x1f));
}

static uint16_t pack_time(const struct fat32_time *t)
{
    return (uint16_t)(((t->hour & 0x1f) << 11) | ((t->minute & 0x3f) << 5)
                      | ((t->second / 2) & 0x1f));
}

static void unpack(uint16_t date, uint16_t time, struct fat32_time *out)
{
    out->year   = (uint16_t)(1980 + (date >> 9));
    out->month  = (uint8_t)((date >> 5) & 0xf);
    out->day    = (uint8_t)(date & 0x1f);
    out->hour   = (uint8_t)(time >> 11);
    out->minute = (uint8_t)((time >> 5) & 0x3f);
    out->second = (uint8_t)((time & 0x1f) * 2);
}



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
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;         p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}



static bool read_sector(struct fat32 *fs, uint64_t lba, void *buf)
{
    return fs->read(fs->ctx, lba, 1, buf);
}

static bool write_sector(struct fat32 *fs, uint64_t lba, const void *buf)
{
    return fs->write != NULL && fs->write(fs->ctx, lba, 1, buf);
}

uint32_t fat32_cluster_bytes(const struct fat32 *fs)
{
    return fs->sectors_per_cluster * FAT32_SECTOR;
}

static uint64_t cluster_lba(const struct fat32 *fs, uint32_t cluster)
{
    return fs->first_data_sector
         + (uint64_t)(cluster - 2) * fs->sectors_per_cluster;
}

static bool cluster_ok(const struct fat32 *fs, uint32_t cluster)
{
    return cluster >= 2 && cluster < fs->cluster_count + 2;
}



/* entry N says which cluster follows N. that is the entire idea */
static bool fat_get(struct fat32 *fs, uint32_t cluster, uint32_t *out)
{
    if (!cluster_ok(fs, cluster)) {
        return false;
    }
    uint64_t offset = (uint64_t)cluster * 4;
    uint64_t lba = fs->reserved_sectors + offset / FAT32_SECTOR;

    if (!read_sector(fs, lba, fs->scratch)) {
        return false;
    }
    *out = rd32(&fs->scratch[offset % FAT32_SECTOR]) & CLUSTER_MASK;
    return true;
}

/* written to every copy of the table. */
static bool fat_set(struct fat32 *fs, uint32_t cluster, uint32_t value)
{
    if (!cluster_ok(fs, cluster) || fs->write == NULL) {
        return false;
    }
    uint64_t offset = (uint64_t)cluster * 4;
    uint64_t within = offset / FAT32_SECTOR;

    for (uint32_t copy = 0; copy < fs->num_fats; copy++) {
        uint64_t lba = fs->reserved_sectors + copy * fs->fat_sectors + within;
        if (!read_sector(fs, lba, fs->scratch)) {
            return false;
        }
        uint8_t *slot = &fs->scratch[offset % FAT32_SECTOR];
        /* the top four bits belong to whoever formatted the disk */
        wr32(slot, (rd32(slot) & ~CLUSTER_MASK) | (value & CLUSTER_MASK));
        if (!write_sector(fs, lba, fs->scratch)) {
            return false;
        }
    }
    return true;
}

static uint32_t alloc_cluster(struct fat32 *fs)
{
    if (fs->write == NULL) {
        return 0;
    }
    /* a linear search for a zero entry. */
    for (uint32_t c = 2; c < fs->cluster_count + 2; c++) {
        uint32_t value;
        if (!fat_get(fs, c, &value)) {
            return 0;
        }
        if (value == 0) {
            if (!fat_set(fs, c, EOC | 0x7)) {   /* 0x0fffffff, end of chain */
                return 0;
            }
            return c;
        }
    }
    return 0;       /* the disk is full */
}



bool fat32_mount(struct fat32 *fs, fat32_io read, fat32_out write, void *ctx)
{
    memset(fs, 0, sizeof *fs);
    fs->read = read;
    fs->write = write;
    fs->ctx = ctx;

    uint8_t boot[FAT32_SECTOR];
    if (read == NULL || !read(ctx, 0, 1, boot)) {
        return false;
    }

    /* the signature every boot sector ends with. */
    if (boot[510] != 0x55 || boot[511] != 0xaa) {
        return false;
    }
    if (rd16(&boot[11]) != FAT32_SECTOR) {
        return false;       /* 512-byte sectors only, which is everything */
    }

    fs->sectors_per_cluster = boot[13];
    fs->reserved_sectors    = rd16(&boot[14]);
    fs->num_fats            = boot[16];
    fs->fat_sectors         = rd32(&boot[36]);
    fs->root_cluster        = rd32(&boot[44]);

    fs->total_sectors = rd16(&boot[19]);
    if (fs->total_sectors == 0) {
        fs->total_sectors = rd32(&boot[32]);
    }

    /*
     * fat12 and fat16 put a number here and fat32 puts zero, which is
     * the cleanest way to tell them apart
     */
    if (rd16(&boot[22]) != 0 || fs->fat_sectors == 0) {
        return false;
    }
    if (fs->sectors_per_cluster == 0 || fs->num_fats == 0
        || fs->reserved_sectors == 0 || fs->root_cluster < 2) {
        return false;
    }

    fs->first_data_sector = fs->reserved_sectors
                          + (uint64_t)fs->num_fats * fs->fat_sectors;
    if (fs->total_sectors <= fs->first_data_sector) {
        return false;
    }
    fs->cluster_count = (uint32_t)((fs->total_sectors - fs->first_data_sector)
                                   / fs->sectors_per_cluster);

    /* the geometry may imply more clusters than the table has room to describe. */
    uint64_t addressable = (uint64_t)fs->fat_sectors * FAT32_SECTOR / 4;
    if (addressable < 2 || fs->cluster_count > addressable - 2) {
        fs->cluster_count = (uint32_t)(addressable > 2 ? addressable - 2 : 0);
    }
    if (fs->cluster_count < 2) {
        return false;
    }

    /*
     * the label, which lives in two places; the boot sector's copy is
     * the one that is always there
     */
    for (int i = 0; i < 11; i++) {
        fs->label[i] = (char)boot[71 + i];
    }
    fs->label[11] = '\0';
    for (int i = 10; i >= 0 && fs->label[i] == ' '; i--) {
        fs->label[i] = '\0';
    }

    fs->mounted = true;
    return true;
}

void fat32_set_clock(struct fat32 *fs, fat32_clock clock)
{
    fs->clock = clock;
}

/* stamp an entry with now. */
static void stamp(struct fat32 *fs, uint8_t *entry, bool created)
{
    if (fs->clock == NULL) {
        return;
    }
    struct fat32_time now;
    fs->clock(&now);

    uint16_t d = pack_date(&now);
    uint16_t t = pack_time(&now);

    if (created) {
        wr16(&entry[14], t);
        wr16(&entry[16], d);
    }
    wr16(&entry[18], d);        /* last access, which has no time field */
    wr16(&entry[22], t);
    wr16(&entry[24], d);
}



/* "HELLO   TXT" -> "hello.txt". */
static void short_name(const uint8_t *entry, char *out)
{
    bool lower_base = (entry[12] & 0x08) != 0;
    bool lower_ext  = (entry[12] & 0x10) != 0;
    size_t n = 0;

    for (int i = 0; i < 8 && entry[i] != ' '; i++) {
        char c = (char)entry[i];
        out[n++] = (lower_base && c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    }
    if (entry[8] != ' ') {
        out[n++] = '.';
        for (int i = 8; i < 11 && entry[i] != ' '; i++) {
            char c = (char)entry[i];
            out[n++] = (lower_ext && c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
        }
    }
    out[n] = '\0';
}

/*
 * the kernel's string lib has no strncpy and does not need one for the
 * sake of two call sites
 */
static void copy_name(char *dst, const char *src)
{
    size_t i = 0;
    while (src[i] != '\0' && i < FAT32_NAME_MAX - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static uint8_t short_checksum(const uint8_t *name11)
{
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++) {
        sum = (uint8_t)((((sum & 1) << 7) | (sum >> 1)) + name11[i]);
    }
    return sum;
}

/*
 * a long name is spread over the entries *before* the short one, in
 * reverse, thirteen utf-16 characters at a time, in three runs at odd
 * offsets because those were the only bytes left unused
 */
static const int lfn_at[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };

static void lfn_chars(const uint8_t *entry, uint16_t *out)
{
    for (int i = 0; i < 13; i++) {
        out[i] = rd16(&entry[lfn_at[i]]);
    }
}

/* the state of a long name being collected across several entries */
struct lfn_state {
    char     name[FAT32_NAME_MAX];
    bool     valid;
    uint8_t  checksum;
    uint32_t want;      /* how many pieces the last entry said there were */
    uint32_t got;       /* one bit per piece that actually turned up */
};

static void lfn_reset(struct lfn_state *l)
{
    l->valid = false;
    l->want = 0;
    l->got = 0;
    l->name[0] = '\0';
}

static void lfn_take(struct lfn_state *l, const uint8_t *entry)
{
    uint8_t sequence = entry[0];
    bool last = (sequence & 0x40) != 0;
    uint32_t index = (uint32_t)(sequence & 0x1f);

    if (index == 0 || index > 10) {
        lfn_reset(l);
        return;
    }
    if (last) {
        lfn_reset(l);
        l->checksum = entry[13];
        l->valid = true;
        l->want = index;
        memset(l->name, 0, sizeof l->name);
    } else if (!l->valid || entry[13] != l->checksum) {
        lfn_reset(l);       /* a piece of some other name, or of nothing */
        return;
    }

    l->got |= 1u << (index - 1);

    uint16_t chars[13];
    lfn_chars(entry, chars);

    size_t base = (index - 1) * 13;
    for (int i = 0; i < 13; i++) {
        size_t at = base + (size_t)i;
        if (at >= FAT32_NAME_MAX - 1) {
            break;
        }
        uint16_t c = chars[i];
        if (c == 0x0000 || c == 0xffff) {
            continue;       /* padding past the end of the name */
        }
        /* anything outside ascii becomes a question mark rather than half a character. */
        l->name[at] = (c < 0x80) ? (char)c : '?';
    }
}

/* did every piece arrive, and does it belong to this entry? */
static bool lfn_finish(struct lfn_state *l, const uint8_t *entry, char *out)
{
    if (!l->valid || l->checksum != short_checksum(entry)) {
        return false;
    }
    /* every piece, not just some of them. */
    if (l->want == 0 || l->got != (1u << l->want) - 1) {
        return false;
    }
    if (l->name[0] == '\0') {
        return false;
    }
    copy_name(out, l->name);
    return true;
}



/* directories are files. */
struct dir_walk {
    uint32_t cluster;
    uint32_t sector_in_cluster;
    uint32_t offset_in_sector;
    uint64_t lba;
    bool     done;
    struct lfn_state lfn;

    /* where the run of long-name entries started, so that removing a file can remove them too. */
    uint64_t lfn_lba;
    uint32_t lfn_off;
};

static void walk_start(struct dir_walk *w, uint32_t cluster)
{
    w->cluster = cluster;
    w->sector_in_cluster = 0;
    w->offset_in_sector = 0;
    w->done = false;
    w->lfn_lba = 0;
    w->lfn_off = 0;
    lfn_reset(&w->lfn);
}

/* the next real entry, with its long name assembled if it had one. */
static bool walk_next(struct fat32 *fs, struct dir_walk *w,
                      struct fat32_file *out)
{
    while (!w->done) {
        if (!cluster_ok(fs, w->cluster)) {
            return false;
        }

        w->lba = cluster_lba(fs, w->cluster) + w->sector_in_cluster;
        if (!read_sector(fs, w->lba, fs->scratch)) {
            return false;
        }

        while (w->offset_in_sector < FAT32_SECTOR) {
            uint8_t *entry = &fs->scratch[w->offset_in_sector];
            uint64_t entry_lba = w->lba;
            uint32_t entry_off = w->offset_in_sector;
            w->offset_in_sector += 32;

            if (entry[0] == 0x00) {
                w->done = true;         /* nothing beyond here, ever */
                return false;
            }
            if (entry[0] == 0xe5) {
                lfn_reset(&w->lfn);     /* deleted */
                w->lfn_lba = 0;
                continue;
            }

            uint8_t attr = entry[11];
            if ((attr & FAT32_ATTR_LFN) == FAT32_ATTR_LFN) {
                if (w->lfn_lba == 0) {
                    w->lfn_lba = entry_lba;
                    w->lfn_off = entry_off;
                }
                lfn_take(&w->lfn, entry);
                continue;
            }
            if (attr & FAT32_ATTR_VOLUME_ID) {
                lfn_reset(&w->lfn);     /* the label, not a file */
                w->lfn_lba = 0;
                continue;
            }

            memset(out, 0, sizeof *out);
            if (!lfn_finish(&w->lfn, entry, out->name)) {
                short_name(entry, out->name);
            }
            lfn_reset(&w->lfn);

            out->attr = attr;
            out->is_dir = (attr & FAT32_ATTR_DIRECTORY) != 0;
            out->size = rd32(&entry[28]);
            out->first_cluster = ((uint32_t)rd16(&entry[20]) << 16)
                               | rd16(&entry[26]);
            out->entry_sector = entry_lba;
            out->entry_offset = entry_off;
            out->lfn_sector = w->lfn_lba;
            out->lfn_offset = w->lfn_off;
            unpack(rd16(&entry[24]), rd16(&entry[22]), &out->written);

            w->lfn_lba = 0;
            w->lfn_off = 0;
            return true;
        }

        /* on to the next sector, and then the next cluster */
        w->offset_in_sector = 0;
        w->sector_in_cluster++;
        if (w->sector_in_cluster >= fs->sectors_per_cluster) {
            w->sector_in_cluster = 0;
            /*
             * FIXME: nothing remembers where the walk has been, so a chain
             * that loops makes this run forever: reading a directory, or
             * looking up one name in it, never returns, and the machine
             * stops with no fault to show for it. short_taken and free_run
             * walk the same chain the same way and have the same hole. a
             * loop is the one chain shape that is always a mistake, and it
             * takes only a corrupt table or a crafted one. count the
             * clusters visited and stop at the count the superblock
             * implies.
             */
            uint32_t next;
            if (!fat_get(fs, w->cluster, &next) || next >= EOC) {
                w->done = true;
                return false;
            }
            w->cluster = next;
        }
    }
    return false;
}

static bool is_dot(const char *name)
{
    return name[0] == '.' && (name[1] == '\0'
                              || (name[1] == '.' && name[2] == '\0'));
}

bool fat32_readdir(struct fat32 *fs, uint32_t dir_cluster, size_t index,
                   struct fat32_file *out)
{
    if (!fs->mounted) {
        return false;
    }
    if (dir_cluster == 0) {
        dir_cluster = fs->root_cluster;
    }

    struct dir_walk w;
    walk_start(&w, dir_cluster);

    size_t seen = 0;
    struct fat32_file f;
    while (walk_next(fs, &w, &f)) {
        if (is_dot(f.name)) {
            continue;       /* nothing above the kernel has any use for these */
        }
        if (seen == index) {
            *out = f;
            return true;
        }
        seen++;
    }
    return false;
}



static bool name_eq(const char *a, const char *b)
{
    /* fat has never cared about case and neither does the kernel */
    while (*a != '\0' && *b != '\0') {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
        if (x != y) {
            return false;
        }
        a++; b++;
    }
    return *a == *b;
}

static bool find_in(struct fat32 *fs, uint32_t dir_cluster, const char *name,
                    struct fat32_file *out)
{
    struct dir_walk w;
    walk_start(&w, dir_cluster);

    struct fat32_file f;
    while (walk_next(fs, &w, &f)) {
        if (name_eq(f.name, name)) {
            *out = f;
            return true;
        }
    }
    return false;
}

bool fat32_lookup(struct fat32 *fs, const char *path, struct fat32_file *out)
{
    if (!fs->mounted) {
        return false;
    }

    while (*path == '/') {
        path++;
    }

    /* the root is not a directory entry anywhere, so it has to be made up. */
    memset(out, 0, sizeof *out);
    out->first_cluster = fs->root_cluster;
    out->is_dir = true;
    out->attr = FAT32_ATTR_DIRECTORY;
    copy_name(out->name, "/");

    while (*path != '\0') {
        char component[FAT32_NAME_MAX];
        size_t n = 0;
        while (*path != '\0' && *path != '/' && n < FAT32_NAME_MAX - 1) {
            component[n++] = *path++;
        }
        component[n] = '\0';
        while (*path == '/') {
            path++;
        }
        if (n == 0) {
            continue;
        }

        if (!out->is_dir) {
            return false;       /* a file cannot have anything inside it */
        }

        struct fat32_file found;
        if (!find_in(fs, out->first_cluster, component, &found)) {
            return false;
        }
        *out = found;

        /*
         * a subdirectory's dotdot points at the root as cluster 0, which
         * is a convention rather than a real cluster number
         */
        if (out->is_dir && out->first_cluster == 0) {
            out->first_cluster = fs->root_cluster;
        }
    }
    return true;
}



/* step along the chain to the cluster holding byte `offset` */
static bool seek_cluster(struct fat32 *fs, uint32_t start, uint64_t offset,
                         uint32_t *out)
{
    uint32_t cluster = start;
    uint64_t skip = offset / fat32_cluster_bytes(fs);

    while (skip-- > 0) {
        uint32_t next;
        if (!fat_get(fs, cluster, &next) || next >= EOC) {
            return false;
        }
        cluster = next;
    }
    *out = cluster;
    return true;
}

int64_t fat32_read(struct fat32 *fs, const struct fat32_file *f,
                   uint64_t offset, void *buf, uint64_t len)
{
    if (!fs->mounted || f->is_dir) {
        return -1;
    }
    if (offset >= f->size) {
        return 0;                       /* the end, which is not an error */
    }
    if (offset + len > f->size) {
        len = f->size - offset;         /* a short read, as usual */
    }

    uint32_t cluster;
    if (!cluster_ok(fs, f->first_cluster)
        || !seek_cluster(fs, f->first_cluster, offset, &cluster)) {
        return -1;
    }

    uint8_t *dst = buf;
    uint64_t done = 0;
    uint32_t within = (uint32_t)(offset % fat32_cluster_bytes(fs));

    while (done < len) {
        if (!cluster_ok(fs, cluster)) {
            return -1;
        }

        uint32_t sector = within / FAT32_SECTOR;
        uint32_t in_sector = within % FAT32_SECTOR;

        if (!read_sector(fs, cluster_lba(fs, cluster) + sector, fs->scratch)) {
            return -1;
        }

        uint64_t chunk = FAT32_SECTOR - in_sector;
        if (chunk > len - done) {
            chunk = len - done;
        }
        memcpy(dst + done, &fs->scratch[in_sector], chunk);
        done += chunk;
        within += (uint32_t)chunk;

        if (within >= fat32_cluster_bytes(fs) && done < len) {
            uint32_t next;
            if (!fat_get(fs, cluster, &next) || next >= EOC) {
                break;      /* the chain ended before the size said it would */
            }
            cluster = next;
            within = 0;
        }
    }
    return (int64_t)done;
}



/* go back to the record this file came from and correct it */
static bool update_entry(struct fat32 *fs, const struct fat32_file *f)
{
    if (f->entry_sector == 0 || fs->write == NULL) {
        return false;
    }
    if (!read_sector(fs, f->entry_sector, fs->scratch)) {
        return false;
    }
    uint8_t *entry = &fs->scratch[f->entry_offset];
    stamp(fs, entry, false);        /* written just now, whenever now is */
    wr32(&entry[28], f->size);
    wr16(&entry[20], (uint16_t)(f->first_cluster >> 16));
    wr16(&entry[26], (uint16_t)(f->first_cluster & 0xffff));
    return write_sector(fs, f->entry_sector, fs->scratch);
}

/*
 * the cluster holding `offset`, adding one to the end of the chain if
 * the file does not reach that far yet
 */
static bool cluster_for_write(struct fat32 *fs, struct fat32_file *f,
                              uint64_t offset, uint32_t *out)
{
    uint32_t per = fat32_cluster_bytes(fs);

    if (f->first_cluster == 0) {
        uint32_t fresh = alloc_cluster(fs);
        if (fresh == 0) {
            return false;
        }
        f->first_cluster = fresh;
    }

    uint32_t cluster = f->first_cluster;
    uint64_t steps = offset / per;

    while (steps-- > 0) {
        uint32_t next;
        if (!fat_get(fs, cluster, &next)) {
            return false;
        }
        if (next >= EOC) {
            uint32_t fresh = alloc_cluster(fs);
            if (fresh == 0 || !fat_set(fs, cluster, fresh)) {
                return false;
            }
            next = fresh;
        }
        cluster = next;
    }
    *out = cluster;
    return true;
}

int64_t fat32_write(struct fat32 *fs, struct fat32_file *f,
                    uint64_t offset, const void *buf, uint64_t len)
{
    if (!fs->mounted || fs->write == NULL || f->is_dir) {
        return -1;
    }
    if (f->attr & FAT32_ATTR_READ_ONLY) {
        return -1;
    }
    /* a write of nothing still says the file was written. */
    if (len == 0) {
        return update_entry(fs, f) ? 0 : -1;
    }

    const uint8_t *src = buf;
    uint64_t done = 0;

    while (done < len) {
        uint64_t at = offset + done;
        uint32_t cluster;
        if (!cluster_for_write(fs, f, at, &cluster)) {
            break;      /* the disk is full. keep what the kernel managed */
        }

        uint32_t within = (uint32_t)(at % fat32_cluster_bytes(fs));
        uint64_t lba = cluster_lba(fs, cluster) + within / FAT32_SECTOR;
        uint32_t in_sector = within % FAT32_SECTOR;

        /*
         * a partial sector has to be read before it is written, or the
         * bytes either side of the kernel's would be replaced with nothing
         */
        uint64_t chunk = FAT32_SECTOR - in_sector;
        if (chunk > len - done) {
            chunk = len - done;
        }
        if (chunk != FAT32_SECTOR) {
            if (!read_sector(fs, lba, fs->scratch)) {
                break;
            }
        }
        memcpy(&fs->scratch[in_sector], src + done, chunk);
        if (!write_sector(fs, lba, fs->scratch)) {
            break;
        }
        done += chunk;
    }

    if (offset + done > f->size) {
        f->size = (uint32_t)(offset + done);
    }
    if (!update_entry(fs, f)) {
        return -1;
    }
    return (int64_t)done;
}



/* the characters 8.3 will take, once they are uppercase. */
static bool short_char(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
        || c == '_' || c == '-' || c == '~';
}

static char upper(char c)
{
    return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
}

/* "notes.txt" -> "NOTES   TXT", plus the two bits recording that it was lowercase. */
static bool to_short(const char *name, uint8_t *out11, uint8_t *case_bits)
{
    const char *dot = NULL;
    for (const char *p = name; *p != '\0'; p++) {
        if (*p == '.') {
            dot = p;
        }
    }

    size_t base_len = dot ? (size_t)(dot - name) : strlen(name);
    size_t ext_len  = dot ? strlen(dot + 1) : 0;
    if (base_len == 0 || base_len > 8 || ext_len > 3) {
        return false;
    }

    bool base_lower = false, base_upper = false;
    bool ext_lower = false, ext_upper = false;

    for (int i = 0; i < 11; i++) {
        out11[i] = ' ';
    }
    for (size_t i = 0; i < base_len; i++) {
        char c = name[i];
        if (c >= 'a' && c <= 'z') { base_lower = true; c = (char)(c - 32); }
        else if (c >= 'A' && c <= 'Z') { base_upper = true; }
        if (!short_char(c)) {
            return false;
        }
        out11[i] = (uint8_t)c;
    }
    for (size_t i = 0; i < ext_len; i++) {
        char c = dot[1 + i];
        if (c >= 'a' && c <= 'z') { ext_lower = true; c = (char)(c - 32); }
        else if (c >= 'A' && c <= 'Z') { ext_upper = true; }
        if (!short_char(c)) {
            return false;
        }
        out11[8 + i] = (uint8_t)c;
    }

    /*
     * mixed case in one part cannot be recorded by a single bit, so
     * such a name genuinely does need a long entry
     */
    if ((base_lower && base_upper) || (ext_lower && ext_upper)) {
        return false;
    }

    *case_bits = (uint8_t)((base_lower ? 0x08 : 0) | (ext_lower ? 0x10 : 0));
    return true;
}



/* what a long name may hold. */
static bool long_name_ok(const char *name)
{
    size_t len = strlen(name);
    if (len == 0 || len >= FAT32_NAME_MAX || is_dot(name)) {
        return false;
    }
    if (name[len - 1] == ' ' || name[len - 1] == '.') {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c < 0x20 || c >= 0x7f) {
            return false;       /* the kernel has no business pretending to unicode */
        }
        if (strchr("\"*/:<>?\\|", (char)c) != NULL) {
            return false;
        }
    }
    return true;
}

/*
 * the eleven bytes for a name they cannot hold as itself: as much of it
 * as is usable, cut short to make room for a ~N that makes it unique.
 * windows starts hashing the name instead once it reaches ~4, which is
 * faster on a directory of ten thousand files and no more correct
 */
static void mangle(const char *name, uint32_t n, uint8_t *out11)
{
    const char *dot = NULL;
    for (const char *p = name; *p != '\0'; p++) {
        if (*p == '.') {
            dot = p;
        }
    }
    size_t base_len = dot ? (size_t)(dot - name) : strlen(name);

    char tail[4];
    size_t tail_len = 0;
    tail[tail_len++] = '~';
    char digits[3];
    size_t d = 0;
    for (uint32_t v = n; v > 0 && d < sizeof digits; v /= 10) {
        digits[d++] = (char)('0' + v % 10);
    }
    while (d > 0) {
        tail[tail_len++] = digits[--d];
    }

    for (int i = 0; i < 11; i++) {
        out11[i] = ' ';
    }

    /*
     * unusable characters are dropped rather than turned into
     * underscores: this name is not the file's name, it is a handle for
     * tools that cannot read the real one, and shortening it is honest
     * in a way that spelling it wrong is not
     */
    size_t at = 0;
    for (size_t i = 0; i < base_len && at + tail_len < 8; i++) {
        char c = upper(name[i]);
        if (short_char(c)) {
            out11[at++] = (uint8_t)c;
        }
    }
    if (at == 0) {
        memcpy(out11, "FILE", 4);   /* four, so ~999 still fits behind it */
        at = 4;
    }
    for (size_t i = 0; i < tail_len; i++) {
        out11[at + i] = (uint8_t)tail[i];
    }

    if (dot != NULL) {
        size_t k = 0;
        for (const char *p = dot + 1; *p != '\0' && k < 3; p++) {
            char c = upper(*p);
            if (short_char(c)) {
                out11[8 + k++] = (uint8_t)c;
            }
        }
    }
}

/* is some entry in this directory already using these eleven bytes? */
static bool short_taken(struct fat32 *fs, uint32_t dir_cluster,
                        const uint8_t *name11)
{
    uint32_t cluster = dir_cluster;

    while (cluster_ok(fs, cluster)) {
        for (uint32_t s = 0; s < fs->sectors_per_cluster; s++) {
            if (!read_sector(fs, cluster_lba(fs, cluster) + s, fs->scratch)) {
                return true;    /* cannot tell, so assume the worst */
            }
            for (uint32_t off = 0; off < FAT32_SECTOR; off += 32) {
                uint8_t *e = &fs->scratch[off];
                if (e[0] == 0x00) {
                    return false;
                }
                if (e[0] == 0xe5
                    || (e[11] & FAT32_ATTR_LFN) == FAT32_ATTR_LFN) {
                    continue;
                }
                if (memcmp(e, name11, 11) == 0) {
                    return true;
                }
            }
        }
        uint32_t next;
        if (!fat_get(fs, cluster, &next) || next >= EOC) {
            return false;
        }
        cluster = next;
    }
    return false;
}

/*
 * one long entry: thirteen characters of the name, the sequence number
 * that says which thirteen, and the checksum of the short name that
 * ties it to the entry it belongs in front of
 */
static void lfn_slot(uint8_t *out, uint32_t index, bool last, uint8_t checksum,
                     const char *name, size_t len)
{
    memset(out, 0, 32);
    out[0] = (uint8_t)(index | (last ? 0x40 : 0));
    out[11] = FAT32_ATTR_LFN;
    out[13] = checksum;

    size_t base = (size_t)(index - 1) * 13;
    for (int i = 0; i < 13; i++) {
        size_t k = base + (size_t)i;
        uint16_t c;
        if (k < len) {
            c = (uint8_t)name[k];
        } else if (k == len) {
            c = 0x0000;         /* the terminator, written once */
        } else {
            c = 0xffff;         /* and padding to the end of the piece */
        }
        wr16(&out[lfn_at[i]], c);
    }
}

/*
 * a long name is at most ten entries of thirteen characters, and the
 * short one behind them makes eleven records that have to go in
 * together or not at all
 */
#define MAX_SLOTS 11

struct name_slots {
    uint8_t  slot[MAX_SLOTS * 32];
    uint32_t count;         /* the short entry included, and always last */
};

/* the records a name needs, ready to be written. */
static bool build_name(struct fat32 *fs, uint32_t dir_cluster,
                       const char *name, struct name_slots *out)
{
    if (!long_name_ok(name)) {
        return false;
    }
    memset(out, 0, sizeof *out);

    uint8_t short11[11];
    uint8_t case_bits = 0;

    /*
     * the easy half, and most names: one 8.3 renders back to exactly,
     * which needs no long entry and no ~1
     */
    if (to_short(name, short11, &case_bits)
        && !short_taken(fs, dir_cluster, short11)) {
        memcpy(out->slot, short11, 11);
        out->slot[12] = case_bits;
        out->count = 1;
        return true;
    }

    uint32_t n;
    for (n = 1; n <= 999; n++) {
        mangle(name, n, short11);
        if (!short_taken(fs, dir_cluster, short11)) {
            break;
        }
    }
    if (n > 999) {
        return false;       /* a thousand names this alike is a mistake */
    }

    size_t len = strlen(name);
    uint32_t entries = (uint32_t)((len + 12) / 13);
    if (entries > MAX_SLOTS - 1) {
        return false;
    }

    /*
     * backwards: the last piece is written first, so that a reader
     * going forwards meets the one marked `last` before any other and
     * knows how many to expect
     */
    uint8_t sum = short_checksum(short11);
    for (uint32_t i = entries; i >= 1; i--) {
        lfn_slot(&out->slot[(entries - i) * 32], i, i == entries, sum,
                 name, len);
    }
    memcpy(&out->slot[entries * 32], short11, 11);
    out->count = entries + 1;
    return true;
}



static void slot_lba(const struct fat32 *fs, uint32_t cluster, uint32_t index,
                     uint64_t *lba_out, uint32_t *off_out)
{
    uint32_t per_sector = FAT32_SECTOR / 32;
    *lba_out = cluster_lba(fs, cluster) + index / per_sector;
    *off_out = (index % per_sector) * 32;
}

/*
 * mark slots gone without touching what is in them, which is what 0xe5
 * has always meant
 */
static bool strike_slots(struct fat32 *fs, uint32_t cluster, uint32_t start,
                         uint32_t count)
{
    uint32_t per_sector = FAT32_SECTOR / 32;

    while (count > 0) {
        uint64_t lba = cluster_lba(fs, cluster) + start / per_sector;
        if (!read_sector(fs, lba, fs->scratch)) {
            return false;
        }
        for (uint32_t at = start % per_sector; at < per_sector && count > 0;
             at++, start++, count--) {
            fs->scratch[at * 32] = 0xe5;
        }
        if (!write_sector(fs, lba, fs->scratch)) {
            return false;
        }
    }
    return true;
}

/* room for a run of entries, growing the directory by a cluster if there is none. */
static bool free_run(struct fat32 *fs, uint32_t dir_cluster, uint32_t slots,
                     uint64_t *lba_out, uint32_t *off_out)
{
    uint32_t per_cluster = fs->sectors_per_cluster * (FAT32_SECTOR / 32);
    if (slots == 0 || slots > per_cluster) {
        return false;           /* a cluster too small to hold one name */
    }

    uint32_t cluster = dir_cluster;
    for (;;) {
        uint32_t run = 0, start = 0, index = 0;

        for (uint32_t s = 0; s < fs->sectors_per_cluster; s++) {
            if (!read_sector(fs, cluster_lba(fs, cluster) + s, fs->scratch)) {
                return false;
            }
            for (uint32_t off = 0; off < FAT32_SECTOR; off += 32, index++) {
                if (fs->scratch[off] != 0x00 && fs->scratch[off] != 0xe5) {
                    run = 0;
                    continue;
                }
                if (run == 0) {
                    start = index;
                }
                if (++run == slots) {
                    slot_lba(fs, cluster, start, lba_out, off_out);
                    return true;
                }
            }
        }

        if (run > 0 && !strike_slots(fs, cluster, start, run)) {
            return false;
        }

        uint32_t next;
        if (!fat_get(fs, cluster, &next)) {
            return false;
        }
        if (next >= EOC) {
            /* the directory is full. */
            uint32_t fresh = alloc_cluster(fs);
            if (fresh == 0 || !fat_set(fs, cluster, fresh)) {
                return false;
            }
            memset(fs->scratch, 0, FAT32_SECTOR);
            for (uint32_t s = 0; s < fs->sectors_per_cluster; s++) {
                if (!write_sector(fs, cluster_lba(fs, fresh) + s, fs->scratch)) {
                    return false;
                }
            }
            next = fresh;
        }
        cluster = next;
    }
}

/* the group, written where free_run said there was room. */
static bool write_run(struct fat32 *fs, uint64_t lba, uint32_t off,
                      const uint8_t *slots, uint32_t count)
{
    while (count > 0) {
        if (!read_sector(fs, lba, fs->scratch)) {
            return false;
        }
        while (off < FAT32_SECTOR && count > 0) {
            memcpy(&fs->scratch[off], slots, 32);
            slots += 32;
            off += 32;
            count--;
        }
        if (!write_sector(fs, lba, fs->scratch)) {
            return false;
        }
        lba++;
        off = 0;
    }
    return true;
}

/* where the nth record of a group sits, given where the group starts */
static void slot_after(uint64_t lba, uint32_t off, uint32_t n,
                       uint64_t *lba_out, uint32_t *off_out)
{
    uint64_t at = (uint64_t)off + (uint64_t)n * 32;
    *lba_out = lba + at / FAT32_SECTOR;
    *off_out = (uint32_t)(at % FAT32_SECTOR);
}

bool fat32_create(struct fat32 *fs, const char *path, struct fat32_file *out)
{
    if (!fs->mounted || fs->write == NULL) {
        return false;
    }

    /* split off the last component; the rest has to exist already */
    const char *name = path;
    for (const char *p = path; *p != '\0'; p++) {
        if (*p == '/') {
            name = p + 1;
        }
    }
    if (*name == '\0') {
        return false;
    }

    char parent[FAT32_NAME_MAX];
    size_t plen = (size_t)(name - path);
    if (plen >= sizeof parent) {
        return false;
    }
    memcpy(parent, path, plen);
    parent[plen] = '\0';

    struct fat32_file dir;
    if (!fat32_lookup(fs, parent, &dir) || !dir.is_dir) {
        return false;
    }

    /* already there? then this is just an open */
    if (find_in(fs, dir.first_cluster, name, out)) {
        return !out->is_dir;
    }

    struct name_slots slots;
    if (!build_name(fs, dir.first_cluster, name, &slots)) {
        return false;       /* a name the kernel cannot store honestly */
    }
    uint8_t *entry = &slots.slot[(slots.count - 1) * 32];
    entry[11] = FAT32_ATTR_ARCHIVE;
    stamp(fs, entry, true);

    uint64_t lba;
    uint32_t off;
    if (!free_run(fs, dir.first_cluster, slots.count, &lba, &off)
        || !write_run(fs, lba, off, slots.slot, slots.count)) {
        return false;
    }

    memset(out, 0, sizeof *out);
    copy_name(out->name, name);
    out->attr = FAT32_ATTR_ARCHIVE;
    out->is_dir = false;
    out->size = 0;
    out->first_cluster = 0;         /* an empty file owns no clusters */
    slot_after(lba, off, slots.count - 1, &out->entry_sector,
               &out->entry_offset);
    if (slots.count > 1) {
        out->lfn_sector = lba;
        out->lfn_offset = off;
    }
    return true;
}



/*
 * a fresh cluster with nothing in it, which for a directory means every
 * byte zero, the first zero byte is what says "no more entries"
 */
static bool blank_cluster(struct fat32 *fs, uint32_t cluster)
{
    memset(fs->scratch, 0, FAT32_SECTOR);
    for (uint32_t s = 0; s < fs->sectors_per_cluster; s++) {
        if (!write_sector(fs, cluster_lba(fs, cluster) + s, fs->scratch)) {
            return false;
        }
    }
    return true;
}

bool fat32_mkdir(struct fat32 *fs, const char *path)
{
    if (!fs->mounted || fs->write == NULL) {
        return false;
    }

    /* where it goes, and what it is called */
    const char *name = path;
    for (const char *p = path; *p != '\0'; p++) {
        if (*p == '/') {
            name = p + 1;
        }
    }
    if (*name == '\0') {
        return false;
    }

    char parent[FAT32_NAME_MAX];
    size_t plen = (size_t)(name - path);
    if (plen >= sizeof parent) {
        return false;
    }
    memcpy(parent, path, plen);
    parent[plen] = '\0';

    struct fat32_file dir;
    if (!fat32_lookup(fs, parent, &dir) || !dir.is_dir) {
        return false;
    }

    struct fat32_file existing;
    if (find_in(fs, dir.first_cluster, name, &existing)) {
        return false;       /* something is already called that */
    }

    /*
     * the name first, so that one the kernel cannot write leaves no cluster
     * allocated to a directory that never got made
     */
    struct name_slots slots;
    if (!build_name(fs, dir.first_cluster, name, &slots)) {
        return false;
    }

    uint32_t cluster = alloc_cluster(fs);
    if (cluster == 0 || !blank_cluster(fs, cluster)) {
        return false;
    }

    /*
     * every directory but the root begins with two entries: itself, and
     * whatever it hangs from. the root is written as cluster 0 in the
     * second one, a convention rather than a real cluster number,
     * which is why looking one up has to translate it back
     */
    memset(fs->scratch, 0, FAT32_SECTOR);
    uint8_t *dot = fs->scratch;
    memcpy(dot, ".          ", 11);
    dot[11] = FAT32_ATTR_DIRECTORY;
    wr16(&dot[20], (uint16_t)(cluster >> 16));
    wr16(&dot[26], (uint16_t)(cluster & 0xffff));

    uint8_t *dotdot = fs->scratch + 32;
    memcpy(dotdot, "..         ", 11);
    dotdot[11] = FAT32_ATTR_DIRECTORY;
    uint32_t up = (dir.first_cluster == fs->root_cluster) ? 0 : dir.first_cluster;
    wr16(&dotdot[20], (uint16_t)(up >> 16));
    wr16(&dotdot[26], (uint16_t)(up & 0xffff));

    if (!write_sector(fs, cluster_lba(fs, cluster), fs->scratch)) {
        return false;
    }

    /* and the entry in the parent that makes it findable */
    uint8_t *entry = &slots.slot[(slots.count - 1) * 32];
    entry[11] = FAT32_ATTR_DIRECTORY;
    stamp(fs, entry, true);
    wr16(&entry[20], (uint16_t)(cluster >> 16));
    wr16(&entry[26], (uint16_t)(cluster & 0xffff));

    uint64_t lba;
    uint32_t off;
    return free_run(fs, dir.first_cluster, slots.count, &lba, &off)
        && write_run(fs, lba, off, slots.slot, slots.count);
}



/* let a chain of clusters go, following it rather than assuming its length. */
static bool free_chain(struct fat32 *fs, uint32_t cluster)
{
    while (cluster_ok(fs, cluster)) {
        uint32_t next;
        if (!fat_get(fs, cluster, &next)) {
            return false;
        }
        if (!fat_set(fs, cluster, 0)) {
            return false;
        }
        if (next >= EOC) {
            break;
        }
        cluster = next;
    }
    return true;
}

/*
 * two sectors of the same cluster, which is what makes going from one
 * to the next a matter of adding one
 */
static bool same_cluster(const struct fat32 *fs, uint64_t a, uint64_t b)
{
    if (a < fs->first_data_sector || b < fs->first_data_sector) {
        return false;
    }
    return (a - fs->first_data_sector) / fs->sectors_per_cluster
        == (b - fs->first_data_sector) / fs->sectors_per_cluster;
}

/* strike out an entry and every long-name entry standing in front of it. */
static bool strike_out(struct fat32 *fs, const struct fat32_file *f)
{
    uint64_t lba = f->entry_sector;
    uint32_t off = f->entry_offset;

    /*
     * the long ones as well, when they are somewhere the kernel can reach by
     * adding one to a sector number, which is everything written
     * here, since free_run keeps a group inside one cluster. a run some
     * other tool spread across two is left alone rather than guessed
     * at, and the leftovers are harmless: an orphan fails its checksum
     * against whatever entry ends up behind it, and is ignored
     */
    if (f->lfn_sector != 0 && same_cluster(fs, f->lfn_sector, f->entry_sector)
        && (f->lfn_sector < f->entry_sector
            || (f->lfn_sector == f->entry_sector
                && f->lfn_offset < f->entry_offset))) {
        lba = f->lfn_sector;
        off = f->lfn_offset;
    }

    while (lba <= f->entry_sector) {
        if (!read_sector(fs, lba, fs->scratch)) {
            return false;
        }
        bool done = false;
        for (; off < FAT32_SECTOR; off += 32) {
            fs->scratch[off] = 0xe5;
            if (lba == f->entry_sector && off == f->entry_offset) {
                done = true;
                break;
            }
        }
        if (!write_sector(fs, lba, fs->scratch)) {
            return false;
        }
        if (done) {
            return true;
        }
        lba++;
        off = 0;
    }
    return false;               /* the entry was never where it said */
}

bool fat32_rmdir(struct fat32 *fs, const char *path)
{
    if (!fs->mounted || fs->write == NULL) {
        return false;
    }

    struct fat32_file d;
    if (!fat32_lookup(fs, path, &d) || !d.is_dir) {
        return false;
    }
    if (d.first_cluster == fs->root_cluster || d.entry_sector == 0) {
        return false;       /* the root is nobody's to remove */
    }

    /*
     * it has to be empty. dot and dotdot are skipped by readdir, which
     * is exactly the question being asked here
     */
    struct fat32_file ignored;
    if (fat32_readdir(fs, d.first_cluster, 0, &ignored)) {
        return false;
    }

    if (!free_chain(fs, d.first_cluster)) {
        return false;
    }
    return strike_out(fs, &d);
}

bool fat32_unlink(struct fat32 *fs, const char *path)
{
    if (!fs->mounted || fs->write == NULL) {
        return false;
    }

    struct fat32_file f;
    if (!fat32_lookup(fs, path, &f)) {
        return false;
    }
    if (f.is_dir) {
        return false;       /* rmdir is a different question, asked differently */
    }
    if (f.entry_sector == 0) {
        return false;
    }
    if (f.attr & FAT32_ATTR_READ_ONLY) {
        return false;
    }

    /*
     * FIXME: the chain goes before the name does, and that is the wrong
     * way round. stop between these two lines and the disk holds a live
     * directory entry pointing at clusters the table now says are free,
     * so the next allocation takes them and two files own the same data.
     * nothing on a fat volume can be counted back from the table, so
     * unlike an ext4 link count there is no number left behind to repair.
     * ext4_unlink does this the other way and says why: the name goes
     * first, and the worst an interruption leaves is a leak, which a
     * checker can find. strike the entry out first, then free.
     */
    if (!free_chain(fs, f.first_cluster)) {
        return false;
    }
    return strike_out(fs, &f);
}

bool fat32_rename(struct fat32 *fs, const char *from, const char *to)
{
    if (!fs->mounted || fs->write == NULL) {
        return false;
    }

    struct fat32_file f;
    if (!fat32_lookup(fs, from, &f) || f.entry_sector == 0) {
        return false;
    }

    /* where the new name goes, and what it is */
    const char *name = to;
    for (const char *p = to; *p != '\0'; p++) {
        if (*p == '/') {
            name = p + 1;
        }
    }
    if (*name == '\0') {
        return false;
    }

    char parent[FAT32_NAME_MAX];
    size_t plen = (size_t)(name - to);
    if (plen >= sizeof parent) {
        return false;
    }
    memcpy(parent, to, plen);
    parent[plen] = '\0';

    struct fat32_file dir;
    if (!fat32_lookup(fs, parent, &dir) || !dir.is_dir) {
        return false;
    }

    struct fat32_file clash;
    if (find_in(fs, dir.first_cluster, name, &clash)) {
        return false;       /* something is already called that */
    }

    struct name_slots slots;
    if (!build_name(fs, dir.first_cluster, name, &slots)) {
        return false;
    }

    /* a new entry pointing at the same clusters. */
    uint8_t *entry = &slots.slot[(slots.count - 1) * 32];
    entry[11] = f.attr ? f.attr : FAT32_ATTR_ARCHIVE;
    stamp(fs, entry, true);
    wr16(&entry[20], (uint16_t)(f.first_cluster >> 16));
    wr16(&entry[26], (uint16_t)(f.first_cluster & 0xffff));
    wr32(&entry[28], f.size);

    uint64_t lba;
    uint32_t off;
    if (!free_run(fs, dir.first_cluster, slots.count, &lba, &off)
        || !write_run(fs, lba, off, slots.slot, slots.count)) {
        return false;
    }

    /* and only now let the old name go. */
    return strike_out(fs, &f);
}



bool fat32_usage(struct fat32 *fs, uint32_t *used, uint32_t *total)
{
    if (!fs->mounted) {
        return false;
    }
    *total = fs->cluster_count;
    *used = 0;

    /*
     * walk the table rather than trusting fsinfo, which is a cache and
     * is wrong on any disk that was not put away tidily
     */
    for (uint32_t c = 2; c < fs->cluster_count + 2; c++) {
        uint32_t value;
        if (!fat_get(fs, c, &value)) {
            return false;
        }
        if (value != 0) {
            (*used)++;
        }
    }
    return true;
}
