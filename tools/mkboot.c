// SPDX-License-Identifier: GPL-2.0-only
/*
 * tools/mkboot.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * a bootable disk image for philemon.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#define SECTOR              512
#define LOADER_MAX_SECTORS  32
#define BOOT_TABLE_LBA      32
#define MAGIC               0x50484c4d4e30ull   /* "PHLMN0" */

static uint8_t *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        fprintf(stderr, "mkboot: cannot read %s\n", path);
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = calloc(1, (size_t)n + 1);
    if (data == NULL || fread(data, 1, (size_t)n, f) != (size_t)n) {
        fprintf(stderr, "mkboot: %s would not read\n", path);
        exit(1);
    }
    fclose(f);
    *len = (size_t)n;
    return data;
}

static size_t sectors_for(size_t n)
{
    return (n + SECTOR - 1) / SECTOR;
}

/* `NAME equ VALUE` out of the nasm include, and `#define NAME VALUE` out of the C header. */

static int find_number(const char *text, const char *name, const char *form,
                       uint64_t *out)
{
    size_t namelen = strlen(name);
    for (const char *p = text; (p = strstr(p, name)) != NULL; p += namelen) {
        if (p != text && p[-1] != '\n' && p[-1] != ' ' && p[-1] != '\t') {
            continue;
        }
        const char *q = p + namelen;
        while (*q == ' ' || *q == '\t') {
            q++;
        }
        if (form[0] == 'e') {           /* equ */
            if (strncmp(q, "equ", 3) != 0) {
                continue;
            }
            q += 3;
            while (*q == ' ' || *q == '\t') {
                q++;
            }
        }
        *out = strtoull(q, NULL, 0);
        return 1;
    }
    return 0;
}

static char *read_text(const char *root, const char *rest)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", root, rest);
    size_t len;
    return (char *)slurp(path, &len);
}

static void check_agreement(const char *root)
{
    char *inc = read_text(root, "boot/philemon.inc");
    char *hdr = read_text(root, "boot/philemon.h");
    char *asmtext = read_text(root, "boot/philemon.asm");

    static const char *pairs[][2] = {
        { "TABLE_LINEAR",     "PH_TABLE_ADDR" },
        { "EARLY_LINEAR",     "PH_HANDOFF_ADDR" },
        { "MEMMAP_LINEAR",    "PH_MEMMAP_ADDR" },
        { "BOUNCE_LINEAR",    "PH_BOUNCE_ADDR" },
        { "HANDOFF64_LINEAR", "PH_HANDOFF64" },
        { "E820_LINEAR",      "PH_E820_ADDR" },
        { "PT_BASE",          "PH_PAGETABLE" },
        { "KERNEL_LINEAR",    "PH_KERNEL_IMAGE" },
        { "RAMDISK_LINEAR",   "PH_RAMDISK_ADDR" },
        { "KERNEL_PHYS",      "PH_KERNEL_PHYS" },
        { NULL, NULL }
    };

    for (int i = 0; pairs[i][0] != NULL; i++) {
        uint64_t a = 0, c = 0;
        char define[128];
        snprintf(define, sizeof define, "#define %s", pairs[i][1]);
        if (!find_number(inc, pairs[i][0], "equ", &a)
            || !find_number(hdr, define, "define", &c)) {
            fprintf(stderr, "mkboot: cannot find %s / %s to compare\n",
                    pairs[i][0], pairs[i][1]);
            exit(1);
        }
        if (a != c) {
            fprintf(stderr, "mkboot: %s is %#llx in the assembly but %s is "
                    "%#llx in the C\n", pairs[i][0], (unsigned long long)a,
                    pairs[i][1], (unsigned long long)c);
            exit(1);
        }
    }

    /* the magic, which is what stops the loader believing a stray sector */
    uint64_t lo = 0, hi = 0;
    if (!find_number(inc, "PH_MAGIC_LO", "equ", &lo)
        || !find_number(inc, "PH_MAGIC_HI", "equ", &hi)
        || ((hi << 32) | lo) != MAGIC) {
        fprintf(stderr, "mkboot: the boot magic does not match between asm "
                "and this tool\n");
        exit(1);
    }

    /* and the boot table's field offsets, which stage 2 reads by hand. */
    for (const char *p = asmtext;
         (p = strstr(p, "TABLE_LINEAR + ")) != NULL; p += 15) {
        long off = strtol(p + 15, NULL, 10);
        if (off % 8 != 0 || off <= 0 || off >= 9 * 8) {
            fprintf(stderr, "mkboot: stage2 reads the boot table at +%ld, "
                    "which is not the start of any field. the two have "
                    "drifted apart\n", off);
            exit(1);
        }
    }
    free(inc);
    free(hdr);
    free(asmtext);
}

static void put64(uint8_t *at, uint64_t v)
{
    for (int i = 0; i < 8; i++) {
        at[i] = (uint8_t)(v >> (i * 8));
    }
}

int main(int argc, char **argv)
{
    if (argc < 5) {
        fprintf(stderr, "usage: mkboot <out.img> <philemon.bin> "
                "<I64.bin> <kernel.elf> [ramdisk.tar "
                "[source.tar]]\n");
        return 2;
    }
    /* where boot/philemon.inc and boot/philemon.h are. */
    const char *root = getenv("MKBOOT_ROOT");
    check_agreement(root != NULL ? root : ".");

    size_t phlen, holen, krlen, rdlen = 0, srlen = 0;
    uint8_t *ph = slurp(argv[2], &phlen);
    uint8_t *ho = slurp(argv[3], &holen);
    uint8_t *kr = slurp(argv[4], &krlen);
    uint8_t *rd = argc > 5 ? slurp(argv[5], &rdlen) : NULL;
    uint8_t *sr = argc > 6 ? slurp(argv[6], &srlen) : NULL;

    if (phlen < SECTOR) {
        fprintf(stderr, "mkboot: philemon is %zu bytes, less than one "
                "sector\n", phlen);
        return 1;
    }
    if (ph[510] != 0x55 || ph[511] != 0xaa) {
        fprintf(stderr, "mkboot: philemon has no boot signature at 510; no "
                "bios would run it\n");
        return 1;
    }
    if (sectors_for(phlen) > LOADER_MAX_SECTORS) {
        fprintf(stderr, "mkboot: philemon is %zu sectors and only %d fit "
                "before the table\n", sectors_for(phlen), LOADER_MAX_SECTORS);
        return 1;
    }

    size_t handoff_lba = BOOT_TABLE_LBA + 1;
    size_t kernel_lba = handoff_lba + sectors_for(holen);
    size_t ramdisk_lba = kernel_lba + sectors_for(krlen);
    /*
     * the source goes last, because it is the one piece nothing reads
     * at boot, so every offset that matters stays where it was, and
     * an image built without one is this image minus a tail
     */
    size_t source_lba = ramdisk_lba + sectors_for(rdlen);
    size_t total = source_lba + sectors_for(srlen);

    /* pad out to something a bios will accept as a hard disk. */
    size_t cylinder = 16 * 63 * SECTOR;
    size_t floor_bytes = 8 * 1024 * 1024;
    if (cylinder * 4 > floor_bytes) {
        floor_bytes = cylinder * 4;
    }
    if (total * SECTOR < floor_bytes) {
        total = floor_bytes / SECTOR;
    }
    total = ((total * SECTOR + cylinder - 1) / cylinder) * cylinder / SECTOR;

    uint8_t *image = calloc(total, SECTOR);
    if (image == NULL) {
        fprintf(stderr, "mkboot: out of memory\n");
        return 1;
    }
    memcpy(image, ph, phlen);

    uint8_t *table = image + BOOT_TABLE_LBA * SECTOR;
    put64(table + 0,  MAGIC);
    put64(table + 8,  handoff_lba);
    put64(table + 16, sectors_for(holen));
    put64(table + 24, kernel_lba);
    put64(table + 32, sectors_for(krlen));
    put64(table + 40, krlen);
    put64(table + 48, ramdisk_lba);
    put64(table + 56, sectors_for(rdlen));
    put64(table + 64, rdlen);
    put64(table + 72, srlen > 0 ? source_lba : 0);
    put64(table + 80, sectors_for(srlen));
    put64(table + 88, srlen);

    memcpy(image + handoff_lba * SECTOR, ho, holen);
    memcpy(image + kernel_lba * SECTOR, kr, krlen);
    if (rd != NULL && rdlen > 0) {
        memcpy(image + ramdisk_lba * SECTOR, rd, rdlen);
    }
    if (sr != NULL && srlen > 0) {
        memcpy(image + source_lba * SECTOR, sr, srlen);
    }

    FILE *f = fopen(argv[1], "wb");
    if (f == NULL || fwrite(image, SECTOR, total, f) != total) {
        fprintf(stderr, "mkboot: cannot write %s\n", argv[1]);
        return 1;
    }
    fclose(f);

    printf("%s: philemon %zuB (%zu sectors), 64-bit half %zuB, kernel %zuB, "
           "ramdisk %zuB, source %zuB -> %zu sectors\n",
           argv[1], phlen, sectors_for(phlen), holen, krlen, rdlen, srlen,
           total);
    return 0;
}
