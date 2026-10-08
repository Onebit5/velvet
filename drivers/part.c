// SPDX-License-Identifier: GPL-2.0-only
/*
 * drivers/part.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * partition tables: mbr and gpt.
 */

#include "drivers/part.h"
#include "lib/string.h"
#include "lib/hash.h"


static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd64(const uint8_t *p)
{
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

const char *part_mbr_kind(uint8_t type)
{
    switch (type) {
    case 0x00: return "empty";
    case 0x01: case 0x04: case 0x06: case 0x0e: return "fat16";
    case 0x0b: case 0x0c: return "fat32";
    case 0x05: case 0x0f: return "extended";
    case 0x07: return "ntfs/exfat";
    case 0x82: return "linux swap";
    case 0x83: return "linux";
    case 0xee: return "gpt protective";
    case 0xef: return "efi system";
    default:   return "unknown";
    }
}

/*
 * stored the way microsoft writes guids: the first three fields
 * little-endian and the last two big-endian, which is why these look
 * shuffled next to the printed form. comparing raw bytes sidesteps the
 * whole question
 */
struct guid_name {
    uint8_t bytes[16];
    const char *name;
};

static const struct guid_name gpt_kinds[] = {
    /* 0FC63DAF-8483-4772-8E79-3D69D8477DE4 */
    { { 0xaf, 0x3d, 0xc6, 0x0f, 0x83, 0x84, 0x72, 0x47,
        0x8e, 0x79, 0x3d, 0x69, 0xd8, 0x47, 0x7d, 0xe4 }, "linux" },
    /* C12A7328-F81F-11D2-BA4B-00A0C93EC93B */
    { { 0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11,
        0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b }, "efi system" },
    /* EBD0A0A2-B9E5-4433-87C0-68B6B72699C7 */
    { { 0xa2, 0xa0, 0xd0, 0xeb, 0xe5, 0xb9, 0x33, 0x44,
        0x87, 0xc0, 0x68, 0xb6, 0xb7, 0x26, 0x99, 0xc7 }, "windows data" },
    /* 0657FD6D-A4AB-43C4-84E5-0933C84B4F4F */
    { { 0x6d, 0xfd, 0x57, 0x06, 0xab, 0xa4, 0xc4, 0x43,
        0x84, 0xe5, 0x09, 0x33, 0xc8, 0x4b, 0x4f, 0x4f }, "linux swap" },
};

static const char *gpt_kind(const uint8_t *guid)
{
    for (size_t i = 0; i < sizeof gpt_kinds / sizeof gpt_kinds[0]; i++) {
        if (memcmp(guid, gpt_kinds[i].bytes, 16) == 0) {
            return gpt_kinds[i].name;
        }
    }
    return "unknown";
}

static bool guid_is_zero(const uint8_t *guid)
{
    for (int i = 0; i < 16; i++) {
        if (guid[i] != 0) {
            return false;
        }
    }
    return true;
}



#define MBR_TABLE_AT   446
#define MBR_ENTRY_SIZE 16

static size_t scan_mbr(const uint8_t *sector, struct partition *out,
                       size_t max, bool *is_gpt)
{
    size_t found = 0;
    *is_gpt = false;

    for (int i = 0; i < 4 && found < max; i++) {
        const uint8_t *e = sector + MBR_TABLE_AT + i * MBR_ENTRY_SIZE;
        uint8_t type = e[4];
        uint32_t first = rd32(e + 8);
        uint32_t count = rd32(e + 12);

        if (type == 0xee) {
            /*
             * the protective entry a gpt disk carries so that old tools
             * see a full disk rather than an empty one. the real table
             * is elsewhere
             */
            *is_gpt = true;
            return 0;
        }
        if (type == 0x00 || count == 0) {
            continue;
        }

        memset(&out[found], 0, sizeof out[found]);
        out[found].first_lba = first;
        out[found].sectors = count;
        out[found].mbr_type = type;
        out[found].bootable = (e[0] == 0x80);
        out[found].index = (unsigned)(i + 1);
        out[found].kind = part_mbr_kind(type);
        found++;

        /* an extended partition is a linked list of further tables inside itself. */
    }
    return found;
}



#define GPT_SIG_0 0x20494645u       /* "EFI " */
#define GPT_SIG_1 0x54524150u       /* "PART" */

static size_t scan_gpt(part_io read, void *ctx, struct partition *out,
                       size_t max)
{
    uint8_t header[PART_SECTOR];
    if (!read(ctx, 1, 1, header)) {
        return 0;
    }
    if (rd32(header) != GPT_SIG_0 || rd32(header + 4) != GPT_SIG_1) {
        return 0;
    }

    uint32_t header_size = rd32(header + 12);
    if (header_size < 92 || header_size > PART_SECTOR) {
        return 0;
    }

    /*
     * the header's own crc is computed with its crc field zeroed, which
     * is the only way a checksum can cover the field that holds it
     */
    uint32_t claimed = rd32(header + 16);
    uint8_t copy[PART_SECTOR];
    memcpy(copy, header, header_size);
    memset(copy + 16, 0, 4);
    if (crc32_of(copy, header_size) != claimed) {
        return 0;       /* a table known to be corrupt is not followed */
    }

    uint64_t entries_lba = rd64(header + 72);
    uint32_t entry_count = rd32(header + 80);
    uint32_t entry_size = rd32(header + 84);
    uint32_t entries_crc = rd32(header + 88);

    if (entry_size < 128 || entry_size > PART_SECTOR
        || entry_count == 0 || entry_count > 256) {
        return 0;
    }

    /* the array, checksummed whole. */
    uint32_t total = entry_count * entry_size;
    uint32_t sectors = (total + PART_SECTOR - 1) / PART_SECTOR;

    /* the running answer, carried between sectors. */
    uint32_t crc = crc32_start();
    size_t found = 0;
    uint8_t buf[PART_SECTOR];
    uint32_t left = total;

    /*
     * two passes would mean reading it twice, so the crc is accumulated
     * as the entries are parsed and only trusted afterwards, which is
     * why nothing is written to `out` until the sum comes out right
     */
    struct partition scratch[PART_MAX];

    for (uint32_t s = 0; s < sectors; s++) {
        if (!read(ctx, entries_lba + s, 1, buf)) {
            return 0;
        }
        uint32_t take = (left > PART_SECTOR) ? PART_SECTOR : left;

        /* fold this sector into the running sum */
        crc = crc32_more(crc, buf, take);
        left -= take;

        for (uint32_t at = 0; at + entry_size <= PART_SECTOR
                              && found < PART_MAX; at += entry_size) {
            const uint8_t *e = buf + at;
            if (guid_is_zero(e)) {
                continue;       /* an unused slot, which gpt leaves in place */
            }

            uint64_t first = rd64(e + 32);
            uint64_t last = rd64(e + 40);
            if (last < first) {
                continue;
            }

            memset(&scratch[found], 0, sizeof scratch[found]);
            scratch[found].first_lba = first;
            scratch[found].sectors = last - first + 1;
            scratch[found].kind = gpt_kind(e);
            scratch[found].index = (unsigned)(s * (PART_SECTOR / entry_size)
                                              + at / entry_size + 1);

            /* the name is utf-16, and this console is not. */
            for (int i = 0; i < PART_NAME_MAX; i++) {
                uint16_t ch = (uint16_t)(e[56 + i * 2]
                                         | ((uint16_t)e[57 + i * 2] << 8));
                if (ch == 0) {
                    scratch[found].name[i] = '\0';
                    break;
                }
                scratch[found].name[i] = (ch >= ' ' && ch < 0x7f)
                                       ? (char)ch : '.';
            }
            scratch[found].name[PART_NAME_MAX] = '\0';
            found++;
        }
    }

    /*
     * not `~crc` any more: crc32_more hands back the finished value
     * each time rather than the inverted middle of one
     */
    if (crc != entries_crc) {
        return 0;       /* the entries do not add up. do not follow them */
    }

    if (found > max) {
        found = max;
    }
    for (size_t i = 0; i < found; i++) {
        out[i] = scratch[i];
    }
    return found;
}



size_t part_scan(part_io read, void *ctx, struct partition *out, size_t max,
                 enum part_scheme *scheme)
{
    *scheme = PART_NONE;
    if (read == NULL || max == 0) {
        return 0;
    }

    uint8_t sector[PART_SECTOR];
    if (!read(ctx, 0, 1, sector)) {
        return 0;
    }

    /* no signature means no table. */
    if (sector[510] != 0x55 || sector[511] != 0xaa) {
        return 0;
    }

    bool is_gpt = false;
    size_t found = scan_mbr(sector, out, max, &is_gpt);

    if (is_gpt) {
        found = scan_gpt(read, ctx, out, max);
        if (found > 0) {
            *scheme = PART_GPT;
        }
        /* a protective mbr with an unreadable gpt behind it is a disk whose real table is gone. */
        return found;
    }

    if (found > 0) {
        *scheme = PART_MBR;
    }
    return found;
}

/* the mbr's awkwardness in one sentence: the partition table lives inside the boot sector. */

/* the 1981 geometry, still. */
static void put_chs(uint8_t *out, uint64_t lba)
{
    const uint64_t heads = 255, spt = 63;

    uint64_t c = lba / (heads * spt);
    uint64_t h = (lba / spt) % heads;
    uint64_t s = (lba % spt) + 1;

    if (c > 1023) {
        c = 1023; h = 254; s = 63;      /* "past where chs can reach" */
    }
    out[0] = (uint8_t)h;
    out[1] = (uint8_t)(s | ((c >> 2) & 0xc0));
    out[2] = (uint8_t)c;
}

static void put32le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

bool part_write_mbr(part_io read, part_out write, void *ctx,
                    uint64_t disk_sectors,
                    const struct part_plan *plan, size_t count,
                    const char **error)
{
    *error = NULL;

    if (count == 0 || count > 4) {
        *error = "an mbr holds one to four partitions and no other number";
        return false;
    }

    for (size_t i = 0; i < count; i++) {
        const struct part_plan *p = &plan[i];

        if (p->sectors == 0) {
            *error = "a partition of no length is not a partition";
            return false;
        }
        if (p->first_lba == 0) {
            *error = "a partition cannot start at sector zero, that is "
                     "where the table itself lives";
            return false;
        }
        if (p->first_lba + p->sectors > disk_sectors) {
            *error = "that partition runs off the end of the disk";
            return false;
        }
        /*
         * 32-bit fields, which is where the two terabyte limit comes
         * from and is worth refusing rather than truncating into
         */
        if (p->first_lba > 0xffffffffull || p->sectors > 0xffffffffull) {
            *error = "an mbr cannot address that far into a disk";
            return false;
        }
        for (size_t j = 0; j < i; j++) {
            uint64_t a0 = p->first_lba, a1 = a0 + p->sectors;
            uint64_t b0 = plan[j].first_lba, b1 = b0 + plan[j].sectors;
            if (a0 < b1 && b0 < a1) {
                *error = "those two partitions overlap";
                return false;
            }
        }
    }

    uint8_t sector[PART_SECTOR];
    if (!read(ctx, 0, 1, sector)) {
        *error = "cannot read the first sector to put a table in it";
        return false;
    }

    /*
     * a protective mbr means the real table is a gpt, and the whole
     * point of one is that a tool which does not understand gpt sees a
     * full disk and leaves it alone. writing anyway would make this
     * exactly the tool that convention exists to stop
     */
    for (int i = 0; i < 4; i++) {
        if (sector[MBR_TABLE_AT + i * MBR_ENTRY_SIZE + 4] == 0xee) {
            *error = "this disk carries a gpt. refusing to write an mbr "
                     "over a table the kernel can read but not replace";
            return false;
        }
    }

    for (size_t i = 0; i < 4; i++) {
        uint8_t *e = sector + MBR_TABLE_AT + i * MBR_ENTRY_SIZE;

        if (i >= count) {
            memset(e, 0, MBR_ENTRY_SIZE);
            continue;
        }
        const struct part_plan *p = &plan[i];

        e[0] = p->bootable ? 0x80 : 0x00;
        put_chs(e + 1, p->first_lba);
        e[4] = p->type;
        put_chs(e + 5, p->first_lba + p->sectors - 1);
        put32le(e + 8,  (uint32_t)p->first_lba);
        put32le(e + 12, (uint32_t)p->sectors);
    }

    /*
     * and the signature, which is what makes any of it a table rather
     * than 512 bytes that happen to be there
     */
    sector[510] = 0x55;
    sector[511] = 0xaa;

    if (!write(ctx, 0, 1, sector)) {
        *error = "the drive would not take the table";
        return false;
    }
    return true;
}
