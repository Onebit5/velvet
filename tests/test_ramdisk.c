// SPDX-License-Identifier: GPL-2.0-only
/*
 * tests/test_ramdisk.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the ustar reader, fed archives byte by byte.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "fs/ramdisk.h"

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

static void add_file(const char *name, const char *body, char typeflag)
{
    uint8_t *h = archive + used;
    memset(h, 0, BLK);
    strcpy((char *)h, name);
    memcpy(h + 257, "ustar", 5);
    h[263] = '0'; h[264] = '0';
    h[156] = typeflag;
    size_t len = strlen(body);
    put_octal((char *)h + 124, 12, len);
    used += BLK;

    memcpy(archive + used, body, len);
    used += (len + BLK - 1) / BLK * BLK;
}

static void finish(void)
{
    memset(archive + used, 0, 2 * BLK);   /* the two zero blocks */
    used += 2 * BLK;
}

int main(void)
{
    struct ramdisk_file f;


    ramdisk_mount(NULL, 0);
    CHECK(!ramdisk_present(), "an unmounted ramdisk admits it");
    CHECK(!ramdisk_stat(0, &f), "and has no files to walk");
    CHECK(!ramdisk_open("anything", &f), "and finds nothing");


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

    CHECK(ramdisk_stat(0, &f) && strcmp(f.name, "first.txt") == 0
          && f.size == 5 && memcmp(f.data, "hello", 5) == 0,
          "the first file reads back whole");
    CHECK(ramdisk_stat(1, &f) && strcmp(f.name, "second.txt") == 0
          && f.size == 18, "and so does the second, past a data block");
    CHECK(ramdisk_stat(2, &f) && f.size == 0,
          "an empty file has no bytes and does not derail the walk");
    CHECK(!ramdisk_stat(3, &f), "walking past the end stops");

    CHECK(ramdisk_open("second.txt", &f) && f.size == 18
          && memcmp(f.data, "a longer body here", 18) == 0,
          "open finds a file by name");
    CHECK(!ramdisk_open("nope.txt", &f), "and says no to one that isnt there");
    CHECK(!ramdisk_open("first", &f), "a prefix is not a match");
    CHECK(!ramdisk_open("first.txt.bak", &f), "nor is a longer name");


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
        CHECK(ramdisk_open("after.txt", &f)
              && memcmp(f.data, "still here", 10) == 0,
              "and the file after it is still found");
    }


    used = 0;
    add_file("./dotted.txt", "content", '0');
    finish();
    ramdisk_mount(archive, used);
    CHECK(ramdisk_open("dotted.txt", &f), "./name is findable as name");
    CHECK(ramdisk_open("./dotted.txt", &f), "and as ./name");


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
    CHECK(!ramdisk_stat(1, &f), "a lying size field cannot walk the test off the end");


    {
        FILE *fp = fopen("bin/ramdisk.tar", "rb");
        if (fp == NULL) {
            printf("  (skipping the real archive: run `make bin/ramdisk.tar`)\n");
        } else {
            static uint8_t real[1024 * 1024];
            size_t n = fread(real, 1, sizeof real, fp);
            fclose(fp);
            ramdisk_mount(real, n);

            CHECK(ramdisk_present(), "the archive the build makes is readable");
            CHECK(ramdisk_open("motd.txt", &f), "motd.txt is in there");
            CHECK(f.size > 0 && memcmp(f.data, "\"Thou art I", 11) == 0,
                  "and its contents come back intact");
            CHECK(ramdisk_open("arcana.txt", &f), "so is arcana.txt");
            CHECK(ramdisk_open("README", &f), "and README");

            /*
             * a name below the top, the way the shell asks for one. this
             * medium carries no programs, a system built on the kernel
             * brings those, so the nested name is the profile the shell
             * reads at login
             */
            CHECK(ramdisk_open("etc/profile", &f),
                  "etc/profile opens by the path the shell asks for");
            CHECK(ramdisk_open("./etc/profile", &f),
                  "and by the path tar actually stored");
            CHECK(!ramdisk_open("profile", &f),
                  "but not by the bare name, there is no path search");
            CHECK(!ramdisk_open("etc", &f),
                  "and a directory is not a file");
        }
    }

    if (!failures) printf("all good\n");
    return failures;
}
