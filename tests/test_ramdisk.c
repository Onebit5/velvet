// SPDX-License-Identifier: GPL-2.0-only
/*
 * tests/test_ramdisk.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the ustar reader, fed archives byte by byte, and the archive unpacked
 * into a filesystem in memory, which is what the root is.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/stat.h>

void panic(const char *fmt, ...)
{
    printf("PANIC: ");
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    printf("\n");
    exit(1);
}
void kprintf(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
}
void *kmalloc(size_t n)
{
    return malloc(n);
}
void kfree(void *p)
{
    free(p);
}
uint64_t epoch_now(void)
{
    return 0;
}

#include <velvet/ramdisk.h>
#include <velvet/ramfs.h>

static int failures;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); failures++; } } while (0)



#define BLK 512
static uint8_t archive[64 * 1024];
static size_t used;

static void put_octal(char *field, size_t width, uint64_t v)
{
    memset(field, '0', width - 1);
    field[width - 1] = '\0';
    for (size_t i = width - 1; v != 0 && i > 0; ) {
        field[--i] = (char)('0' + (v & 7));
        v >>= 3;
    }
}

/* one entry, with its type, mode and, for a link, where it points */
static uint8_t *add_entry(const char *name, const char *body, char typeflag,
                          unsigned mode, const char *link)
{
    uint8_t *h = archive + used;
    memset(h, 0, BLK);
    strcpy((char *)h, name);
    put_octal((char *)h + 100, 8, mode);
    put_octal((char *)h + 108, 8, 0);
    put_octal((char *)h + 116, 8, 0);
    memcpy(h + 257, "ustar", 5);
    h[263] = '0'; h[264] = '0';
    h[156] = typeflag;
    if (link != NULL) {
        strcpy((char *)h + 157, link);
    }
    size_t len = strlen(body);
    put_octal((char *)h + 124, 12, len);
    used += BLK;

    memcpy(archive + used, body, len);
    used += (len + BLK - 1) / BLK * BLK;
    return h;
}

static void add_file(const char *name, const char *body, char typeflag)
{
    add_entry(name, body, typeflag, 0644, NULL);
}

static void finish(void)
{
    memset(archive + used, 0, 2 * BLK);   /* the two zero blocks */
    used += 2 * BLK;
}

/* a file of a ramfs, its bytes into a NUL-terminated buffer */
static bool reads(size_t sz_fs, const char *str_path, char *str_out,
                  size_t sz_size)
{
    struct ramfs_entry e;
    if (!ramfs_lookup(sz_fs, str_path, true, &e) || !S_ISREG(e.u_mode)) {
        return false;
    }
    int64_t i_got = ramfs_read(e.u_ino, 0, str_out, sz_size - 1);
    str_out[i_got > 0 ? i_got : 0] = '\0';
    return i_got >= 0;
}

int main(void)
{
    struct ramdisk_file f;
    struct ramfs_entry e;
    char arr_got[64];


    ramdisk_mount(NULL, 0);
    CHECK(!ramdisk_present(), "an unmounted ramdisk admits it");
    CHECK(!ramdisk_stat(0, &f), "and has no files to walk");


    used = 0;
    finish();
    ramdisk_mount(archive, used);
    CHECK(!ramdisk_present(), "an archive of nothing holds nothing");
    CHECK(ramdisk_count() == 0, "and counts zero");


    used = 0;
    add_file("first.txt",  "hello",              '0');
    add_file("second.txt", "a longer body here", '0');
    add_file("empty.txt",  "",                   '0');
    finish();
    ramdisk_mount(archive, used);

    CHECK(ramdisk_present(), "an archive with files is present");
    CHECK(ramdisk_count() == 3, "all three files were counted");

    CHECK(ramdisk_stat(0, &f) && strcmp(f.arr_name, "first.txt") == 0
          && f.size == 5 && memcmp(f.data, "hello", 5) == 0
          && f.typeflag == '0' && f.mode == 0644,
          "the first file reads back whole, with its type and mode");
    CHECK(ramdisk_stat(1, &f) && strcmp(f.arr_name, "second.txt") == 0
          && f.size == 18, "and so does the second, past a data block");
    CHECK(ramdisk_stat(2, &f) && f.size == 0,
          "an empty file has no bytes and does not derail the walk");
    CHECK(!ramdisk_stat(3, &f), "walking past the end stops");


    {
        char exact[BLK + 1];
        memset(exact, 'x', BLK);
        exact[BLK] = '\0';
        used = 0;
        add_file("exact.bin", exact, '0');
        add_file("after.txt", "still here", '0');
        finish();
        ramdisk_mount(archive, used);
        CHECK(ramdisk_count() == 2, "a block-sized file doesnt eat its neighbour");
        CHECK(ramdisk_stat(1, &f) && strcmp(f.arr_name, "after.txt") == 0
              && memcmp(f.data, "still here", 10) == 0,
              "and the file after it is still found");
    }


    used = 0;
    add_file("./dotted.txt", "content", '0');
    {
        /* a name too long for its field, split into the prefix */
        uint8_t *h = add_entry("deep.txt", "far down", '0', 0644, NULL);
        strcpy((char *)h + 345, "a/long/way");
    }
    finish();
    ramdisk_mount(archive, used);
    CHECK(ramdisk_stat(0, &f) && strcmp(f.arr_name, "dotted.txt") == 0,
          "a ./ in front of a name is no part of it");
    CHECK(ramdisk_stat(1, &f) && strcmp(f.arr_name, "a/long/way/deep.txt") == 0,
          "and a prefix is the front of the name, which a reader that "
          "ignored it would get wrong without a word");


    used = 0;
    add_file("good.txt", "fine", '0');
    memset(archive + used, 0x41, BLK);      /* 'A' everywhere, no ustar magic */
    used += BLK;
    ramdisk_mount(archive, used);
    CHECK(ramdisk_count() == 1,
          "a header without the ustar magic stops the walk rather than "
          "wandering into the bytes");


    used = 0;
    add_file("liar.txt", "short", '0');
    put_octal((char *)archive + 124, 12, 999999);   /* claim it is huge */
    ramdisk_mount(archive, used);
    CHECK(ramdisk_count() == 0 && !ramdisk_stat(0, &f),
          "an entry whose size runs off the end is not trusted at all, "
          "and nothing is read past the end of the archive");



    /*
     * unpacked: every entry made in a filesystem in memory, with the
     * directories it needs whether or not the archive named them, and
     * each with its own mode
     */
    used = 0;
    add_entry("./", "", '5', 0755, NULL);
    add_entry("./etc/", "", '5', 0700, NULL);
    add_entry("./etc/passwd", "root:x:0:0\n", '0', 0600, NULL);
    add_entry("./bin/hello", "ELF-ish\n", '0', 0755, NULL);
    add_entry("./sh", "", '2', 0777, "bin/hello");
    {
        uint8_t *h = add_entry("./null", "", '3', 0666, NULL);
        put_octal((char *)h + 329, 8, 1);
        put_octal((char *)h + 337, 8, 3);
    }
    add_entry("./again", "", '1', 0644, "etc/passwd");
    add_entry("./etc/", "", '5', 0750, NULL);
    finish();
    ramdisk_mount(archive, used);

    int i_fs = ramfs_new(0755);
    CHECK(i_fs >= 0, "a filesystem in memory to unpack into");
    size_t fs = (size_t)i_fs;
    CHECK(ramdisk_unpack(fs) == 6,
          "every entry is made but the root's own, and the hard link, "
          "which nothing here has");
    CHECK(ramfs_lookup(fs, "/etc", true, &e) && S_ISDIR(e.u_mode)
          && (e.u_mode & 07777) == 0750,
          "a directory named twice has the mode it was named with last");
    CHECK(reads(fs, "/etc/passwd", arr_got, sizeof arr_got)
          && strcmp(arr_got, "root:x:0:0\n") == 0
          && ramfs_lookup(fs, "/etc/passwd", true, &e)
          && (e.u_mode & 07777) == 0600 && e.u_uid == 0,
          "a file has its bytes, its mode and its owner");
    CHECK(ramfs_lookup(fs, "/bin", true, &e) && S_ISDIR(e.u_mode)
          && (e.u_mode & 07777) == 0755,
          "a directory the archive did not name is made on the way");
    CHECK(ramfs_lookup(fs, "/sh", false, &e) && S_ISLNK(e.u_mode)
          && reads(fs, "/sh", arr_got, sizeof arr_got)
          && strcmp(arr_got, "ELF-ish\n") == 0,
          "a link leads where the archive says it does");
    CHECK(ramfs_lookup(fs, "/null", true, &e) && S_ISCHR(e.u_mode)
          && e.u_rdev == (1 << 8 | 3), "a device keeps its number");
    CHECK(!ramfs_lookup(fs, "/again", false, &e),
          "and the hard link is left out rather than made something else");



    {
        FILE *fp = fopen("bin/ramdisk.tar", "rb");
        if (fp == NULL) {
            printf("  (skipping the real archive: run `make bin/ramdisk.tar`)\n");
        } else {
            /* all of it, however big the programs on it have grown */
            fseek(fp, 0, SEEK_END);
            long i_size = ftell(fp);
            fseek(fp, 0, SEEK_SET);
            uint8_t *real = malloc(i_size > 0 ? (size_t)i_size : 1);
            size_t n = (real != NULL) ? fread(real, 1, (size_t)i_size, fp) : 0;
            fclose(fp);
            ramdisk_mount(real, n);

            CHECK(ramdisk_present(), "the archive the build makes is readable");
            int i_root = ramfs_new(0755);
            size_t sz_made = (i_root >= 0) ? ramdisk_unpack((size_t)i_root) : 0;
            CHECK(sz_made > 0 && sz_made + 1 >= ramdisk_count(),
                  "and unpacks whole");
            size_t root = (size_t)i_root;
            char arr_elf[8];
            CHECK(ramfs_lookup(root, "/init", true, &e) && S_ISREG(e.u_mode)
                  && (e.u_mode & 0111) != 0
                  && ramfs_read(e.u_ino, 0, arr_elf, 4) == 4
                  && memcmp(arr_elf, "\x7f" "ELF", 4) == 0,
                  "with /init in it, a program that may be run");
            CHECK(!ramfs_lookup(root, "/passwd", true, &e)
                  && !ramfs_lookup(root, "/motd.txt", true, &e),
                  "and no accounts or greetings, which are a system's");
            CHECK(ramfs_lookup(root, "/selftests/sched/wait", true, &e)
                  && S_ISREG(e.u_mode)
                  && ramfs_lookup(root, "/selftests/sched", true, &e)
                  && S_ISDIR(e.u_mode),
                  "and the selftests where /init looks for them");
            CHECK(ramfs_lookup(root, "/selftests/fs/blockdev", true, &e)
                  && (e.u_mode & 07777) == 0700,
                  "one only root may run is installed so");
            free(real);
        }
    }

    if (!failures) printf("all good\n");
    return failures;
}
