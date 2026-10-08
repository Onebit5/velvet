// SPDX-License-Identifier: GPL-2.0-only
/*
 * tests/test_vfs.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * host-side test for the one namespace.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>

void panic(const char *fmt, ...)
{
    printf("PANIC: ");
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    printf("\n");
    exit(1);
}
void *kmalloc(size_t n)
{
    return malloc(n);
}
void kfree(void *p)
{
    free(p);
}

#include "fs/vfs.h"
#include "fs/disk.h"
#include "fs/ramdisk.h"
#include "fs/source.h"
#include "philemon.h"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
} while (0)



static bool have_disk = true;

static const char disk_welcome[] = "the disk's copy\n";
static const char disk_notes[]   = "notes on the disk\n";

/* a filesystem that has opinions about its own files. */
static const struct {
    const char *path, *dir, *name, *body;
    bool is_dir;
    unsigned mode, uid;
} disk_tree[] = {
    { "/welcome.txt", "/", "welcome.txt", disk_welcome, false, 0644, 0 },
    { "/notes.txt",   "/", "notes.txt",   disk_notes,   false, 0644, 0 },
    { "/deep",        "/", "deep",        NULL,         true,  0755, 0 },
    { "/deep/x.txt",  "/deep", "x.txt",   "nested\n",   false, 0600, 1000 },
};
#define DISK_COUNT (sizeof disk_tree / sizeof disk_tree[0])

/*
 * and a second filesystem, which is what the mount table exists for: it
 * holds one file with the *same name* as one on the
 * root, because that is the case that catches a router which forwards
 * the path but forgets the mount: /notes.txt and /work/notes.txt are
 * two files, and a layer that reads one when asked for the other is
 * wrong in a way that looks like a corrupt disk
 */
static bool have_work = true;

static const struct {
    const char *path, *dir, *name, *body;
    bool is_dir;
    unsigned mode, uid;
} work_tree[] = {
    { "/notes.txt", "/", "notes.txt", "notes on the work disk\n",
      false, 0644, 0 },
    { "/job",       "/", "job",       NULL, true, 0755, 0 },
};
#define WORK_COUNT (sizeof work_tree / sizeof work_tree[0])

bool disk_ready(size_t which)
{
    return which == DISK_WORK ? have_work : have_disk;
}

/*
 * fat32 strips every leading slash and skips empty components, so "//"
 * and "/" are the same directory to it. the stub has to agree, or it
 * would be testing itself rather than the thing above it
 */
static const char *tidy(const char *path)
{
    while (path[0] == '/' && path[1] == '/') path++;
    return path;
}

bool disk_lookup(size_t which, const char *path, struct disk_entry *out)
{
    path = tidy(path);
    if (!disk_ready(which)) return false;
    if (which == DISK_WORK) {
        for (size_t i = 0; i < WORK_COUNT; i++) {
            if (strcmp(work_tree[i].path, path) != 0) continue;
            memset(out, 0, sizeof *out);
            strcpy(out->name, work_tree[i].name);
            out->is_dir = work_tree[i].is_dir;
            out->size = work_tree[i].body ? strlen(work_tree[i].body) : 0;
            out->cluster = (uint32_t)(i + 50);
            out->mode = work_tree[i].mode;
            out->uid = work_tree[i].uid;
            return true;
        }
        return false;
    }
    for (size_t i = 0; i < DISK_COUNT; i++) {
        if (strcmp(disk_tree[i].path, path) != 0) continue;
        memset(out, 0, sizeof *out);
        strcpy(out->name, disk_tree[i].name);
        out->is_dir = disk_tree[i].is_dir;
        out->size = disk_tree[i].body ? strlen(disk_tree[i].body) : 0;
        out->cluster = (uint32_t)(i + 2);
        out->entry_sector = 100 + i;
        out->mode = disk_tree[i].mode;
        out->uid = disk_tree[i].uid;
        return true;
    }
    return false;
}

bool disk_readdir(size_t which, const char *path, size_t index,
                  struct disk_entry *out)
{
    path = tidy(path);
    if (!disk_ready(which)) return false;
    size_t seen = 0;
    if (which == DISK_WORK) {
        for (size_t i = 0; i < WORK_COUNT; i++) {
            if (strcmp(work_tree[i].dir, path) != 0) continue;
            if (seen++ != index) continue;
            memset(out, 0, sizeof *out);
            strcpy(out->name, work_tree[i].name);
            out->is_dir = work_tree[i].is_dir;
            out->size = work_tree[i].body ? strlen(work_tree[i].body) : 0;
            out->cluster = (uint32_t)(i + 50);
            out->mode = work_tree[i].mode;
            out->uid = work_tree[i].uid;
            return true;
        }
        return false;
    }
    for (size_t i = 0; i < DISK_COUNT; i++) {
        if (strcmp(disk_tree[i].dir, path) != 0) continue;
        if (seen++ != index) continue;
        memset(out, 0, sizeof *out);
        strcpy(out->name, disk_tree[i].name);
        out->is_dir = disk_tree[i].is_dir;
        out->size = disk_tree[i].body ? strlen(disk_tree[i].body) : 0;
        out->cluster = (uint32_t)(i + 2);
        out->mode = disk_tree[i].mode;
        out->uid = disk_tree[i].uid;
        return true;
    }
    return false;
}

int64_t disk_read(size_t which, uint32_t cluster, uint64_t size,
                  uint64_t offset, void *buf, uint64_t len)
{
    (void)size;
    if (!disk_ready(which)) return -1;
    const char *body = NULL;
    if (which == DISK_WORK) {
        size_t i = cluster - 50;
        if (i >= WORK_COUNT) return -1;
        body = work_tree[i].body;
    } else {
        size_t i = cluster - 2;
        if (i >= DISK_COUNT) return -1;
        body = disk_tree[i].body;
    }
    if (body == NULL) return -1;
    uint64_t total = strlen(body);
    if (offset >= total) return 0;
    if (offset + len > total) len = total - offset;
    memcpy(buf, body + offset, len);
    return (int64_t)len;
}

static int creates;
static size_t last_mount = 99;      /* which one the last call went to */
bool disk_create(size_t which, const char *path, struct disk_entry *out)
{
    if (!disk_ready(which)) return false;
    creates++;
    last_mount = which;
    memset(out, 0, sizeof *out);
    strcpy(out->name, "made.txt");
    out->entry_sector = 999;
    (void)path;
    return true;
}
int64_t disk_write_at(size_t which, struct disk_entry *e, uint64_t offset,
                      const void *buf, uint64_t len)
{
    (void)buf;
    if (!disk_ready(which)) return -1;
    last_mount = which;
    e->size = offset + len;
    return (int64_t)len;
}
const char *disk_model(void)
{
    return "STUB DISK";
}
static int mkdirs, rmdirs, unlinks, renames;
bool disk_mkdir(size_t w, const char *path)
{
    (void)path; last_mount = w; mkdirs++; return disk_ready(w);
}
bool disk_rmdir(size_t w, const char *path)
{
    (void)path; last_mount = w; rmdirs++; return disk_ready(w);
}
bool disk_unlink(size_t w, const char *path)
{
    (void)path; last_mount = w; unlinks++; return disk_ready(w);
}
/* the things a filesystem with opinions can be told. */
bool disk_lookup_nofollow(size_t which, const char *path,
                          struct disk_entry *out)
{
    return disk_lookup(which, path, out);
}
bool disk_readlink(size_t w, const char *p, char *o, size_t n)
{
    (void)w; (void)p; (void)o; (void)n; return false;
}
bool disk_chmod(size_t w, const char *p, uint32_t m)
{
    (void)w; (void)p; (void)m; return false;
}
/* which filesystem answered. */
const char *disk_kind_name(size_t w)
{
    (void)w; return "stub";
}
enum disk_kind disk_which(size_t w)
{
    (void)w; return DISK_FAT32;
}
bool disk_chown(size_t w, const char *p, uint32_t u, uint32_t g)
{
    (void)w; (void)p; (void)u; (void)g; return false;
}
bool disk_symlink(size_t w, const char *p, const char *t)
{
    (void)w; (void)p; (void)t; return false;
}

bool disk_rename(size_t w, const char *from, const char *to)
{
    (void)from; (void)to; last_mount = w; renames++; return disk_ready(w);
}



static void octal(char *dst, uint64_t v, size_t width)
{
    for (size_t i = width - 1; i > 0; i--) {
        dst[i - 1] = (char)('0' + (v & 7));
        v >>= 3;
    }
    dst[width - 1] = '\0';
}

static size_t add_file(uint8_t *tar, size_t at, const char *name,
                       const char *body, unsigned mode)
{
    uint8_t *h = tar + at;
    memset(h, 0, 512);
    strcpy((char *)h, name);
    octal((char *)h + 100, mode, 8);
    octal((char *)h + 124, strlen(body), 12);
    memcpy(h + 257, "ustar", 5);
    h[156] = '0';

    unsigned sum = 0;
    memset(h + 148, ' ', 8);
    for (int i = 0; i < 512; i++) sum += h[i];
    octal((char *)h + 148, sum, 8);

    at += 512;
    memcpy(tar + at, body, strlen(body));
    at += (strlen(body) + 511) / 512 * 512;
    return at;
}

/*
 * /boot/src is read off the boot medium a sector at a time, so what
 * this suite has to supply is not an archive but a *drive* with one on
 * it, philemon's table in sector 32 saying where, and bytes there.
 * the reader is the real one; only the drive is made up
 */

#define SRC_SECTORS 128
static uint8_t medium[SRC_SECTORS * 512];
#define SRC_AT 64

static bool medium_read(void *ctx, uint64_t lba, uint32_t count, void *buf)
{
    (void)ctx;
    if (lba + count > SRC_SECTORS) {
        return false;
    }
    memcpy(buf, medium + lba * 512, (size_t)count * 512);
    return true;
}

static void make_source(void)
{
    memset(medium, 0, sizeof medium);

    uint8_t *archive = medium + SRC_AT * 512;
    size_t at = 0;
    at = add_file(archive, at, "GNUmakefile", "the recipe\n", 0644);
    at = add_file(archive, at, "kernel/main.c", "int kmain(void);\n", 0644);
    at = add_file(archive, at, "kernel/fs/vfs.c", "one namespace\n", 0644);
    at = add_file(archive, at, "welcome.txt", "the source's copy\n", 0644);
    at += 1024;     /* the two zero blocks that end an archive */

    struct ph_table t;
    memset(&t, 0, sizeof t);
    t.magic = PHILEMON_MAGIC;
    t.kernel_lba = 33;
    t.ramdisk_lba = 40;
    t.ramdisk_sectors = 8;
    t.source_lba = SRC_AT;
    t.source_sectors = (at + 511) / 512;
    t.source_size = at;
    memcpy(medium + PH_TABLE_LBA * 512, &t, sizeof t);

    source_mount(medium_read, NULL);
}

int main(void)
{
    static uint8_t tar[64 * 1024];
    size_t at = 0;
    at = add_file(tar, at, "./welcome.txt", "the ramdisk's copy\n", 0644);
    at = add_file(tar, at, "./passwd", "igor:velvet:0\n", 0600);
    at = add_file(tar, at, "./bin/hello", "ELF-ish\n", 0755);
    at = add_file(tar, at, "./secret.txt", "no peeking\n", 0600);
    ramdisk_mount(tar, sizeof tar);
    make_source();

    CHECK(ramdisk_present(), "the ramdisk mounted");
    CHECK(source_present(), "and the source is out on the medium");

    struct vfs_file f;
    char buf[128];


    have_disk = true;

    CHECK(vfs_open("/welcome.txt", &f) && f.kind == VFS_DISK,
          "an absolute name is the disk's");
    CHECK(vfs_read(&f, 0, buf, f.size) == (int64_t)f.size, "and reads");
    buf[f.size] = '\0';
    CHECK(strcmp(buf, "the disk's copy\n") == 0, "with the disk's contents");

    CHECK(vfs_open("/boot/welcome.txt", &f) && f.kind == VFS_RAMDISK,
          "a name under /boot is the ramdisk's");
    CHECK(vfs_read(&f, 0, buf, f.size) == (int64_t)f.size, "and reads");
    buf[f.size] = '\0';
    CHECK(strcmp(buf, "the ramdisk's copy\n") == 0,
          "with the ramdisk's contents, which are different");

    /* the search order, which is the whole point of the arrangement */
    CHECK(vfs_open("welcome.txt", &f) && f.kind == VFS_DISK,
          "a bare name finds the disk's copy first");
    CHECK(vfs_open("bin/hello", &f) && f.kind == VFS_RAMDISK,
          "and falls through to the ramdisk when the disk has none");
    CHECK(vfs_open("passwd", &f) && f.kind == VFS_RAMDISK,
          "which is how passwd is still found");

    CHECK(!vfs_open("nothing-at-all", &f), "a name on neither is on neither");
    CHECK(!vfs_open("/bootleg/x", &f), "and /bootleg is not /boot");
    CHECK(!vfs_open("", &f), "an empty path is nothing");

    /* subdirectories on the disk */
    CHECK(vfs_open("/deep/x.txt", &f) && f.kind == VFS_DISK,
          "a nested disk path resolves");

    /*
     * neither of these is on any filesystem: `/` is where the mounts
     * hang from and `/boot` *is* the ramdisk. something still has to be
     * able to say they are directories, or nobody can stand in one
     */
    CHECK(vfs_open("/", &f) && f.is_dir, "the root is a directory");
    CHECK(vfs_open("/boot", &f) && f.is_dir, "and so is the mount point");
    CHECK(vfs_open("/boot/", &f) && f.is_dir, "however it is spelled");



    int saw_boot = 0, saw_welcome = 0, count = 0;
    for (size_t i = 0; vfs_readdir("/", i, &f); i++) {
        if (strcmp(f.name, "boot") == 0 && f.is_dir) saw_boot = 1;
        if (strcmp(f.name, "welcome.txt") == 0) saw_welcome = 1;
        count++;
    }
    CHECK(saw_welcome, "the root lists what is on the disk");
    CHECK(saw_boot, "and the mount standing on it, which is on no filesystem");
    CHECK(count == (int)DISK_COUNT - 1 + 2,
          "exactly the root filesystem plus the two mounts standing on it");

    int saw_hello = 0, saw_dirs = 0;
    count = 0;
    for (size_t i = 0; vfs_readdir("/boot", i, &f); i++) {
        if (strcmp(f.name, "bin/hello") == 0) saw_hello = 1;
        if (f.is_dir && strcmp(f.name, "src") != 0) saw_dirs++;
        count++;
    }
    CHECK(saw_hello, "/boot lists the ramdisk");
    CHECK(count == 5, "all of it, and the source standing in it");
    CHECK(saw_dirs == 0, "and none of tar's directory records");

    /*
     * a relative filename always worked; a relative directory did not,
     * and there is no reason for anyone to expect that difference
     */

    count = 0;
    for (size_t i = 0; vfs_readdir("boot", i, &f); i++) count++;
    CHECK(count == 5, "a relative directory lists what the absolute one does");

    count = 0;
    for (size_t i = 0; vfs_readdir("boot/", i, &f); i++) count++;
    CHECK(count == 5, "and a trailing slash changes nothing");

    count = 0;
    for (size_t i = 0; vfs_readdir("/boot/", i, &f); i++) count++;
    CHECK(count == 5, "nor does one on an absolute path");

    count = 0;
    for (size_t i = 0; vfs_readdir("deep", i, &f); i++) count++;
    CHECK(count == 1, "a relative directory on the disk works too");

    count = 0;
    for (size_t i = 0; vfs_readdir("//", i, &f); i++) count++;
    CHECK(count == (int)DISK_COUNT - 1 + 2, "and a doubled slash is the root");

    CHECK(!vfs_readdir("bootleg", 0, &f), "but bootleg is still not boot");

    /*
     * the archive is flat: it holds a name like "bin/hello", not a
     * directory called bin with a hello in it. but /boot/bin/hello can
     * be opened, so /boot/bin has to be listable too, or the namespace
     * says two different things depending on which question is asked
     */
    count = 0;
    int saw_bare_hello = 0;
    for (size_t i = 0; vfs_readdir("/boot/bin", i, &f); i++) {
        if (strcmp(f.name, "hello") == 0) saw_bare_hello = 1;
        count++;
    }
    CHECK(count == 1, "a directory inside the ramdisk lists what is in it");
    CHECK(saw_bare_hello,
          "under the name it has there, not the one the archive stores");
    CHECK(vfs_open("/boot/bin/hello", &f),
          "and the same name opens, which is the point");

    CHECK(!vfs_readdir("/boot/nothing", 0, &f),
          "a directory that is in no name lists nothing");



    CHECK(vfs_open("/boot/secret.txt", &f), "a private file is found");
    CHECK(vfs_may_read(&f, 0), "the master may read it");
    CHECK(!vfs_may_read(&f, 1000), "and nobody else may");
    CHECK(vfs_open("/boot/welcome.txt", &f) && vfs_may_read(&f, 1000),
          "a 0644 file is readable by anyone");

    /*
     * every file on the disk was 0644 owned by root by
     * decree, because fat has nowhere to record anything else. now the
     * filesystem answers for itself, and two files on one disk can
     * disagree, which is the entire point of that version
     */
    CHECK(vfs_open("/welcome.txt", &f) && vfs_may_read(&f, 1000),
          "a 0644 file on the disk is readable by anyone");
    CHECK(vfs_open("/welcome.txt", &f) && f.mode == 0644,
          "with the mode the filesystem gave it rather than one the test decreed");

    CHECK(vfs_open("/deep/x.txt", &f), "and a 0600 one is found");
    CHECK(f.mode == 0600 && f.uid == 1000,
          "with its own mode and its own owner");
    CHECK(!vfs_may_read(&f, 1000) ? false : true,
          "readable by whoever owns it");
    CHECK(!vfs_may_read(&f, 2000),
          "and not by somebody else, a sentence the disk could not say "
          "at all before there was a filesystem to say it in");
    CHECK(vfs_may_read(&f, 0), "though the master may read anything");



    CHECK(vfs_open("/notes.txt", &f) && vfs_writable(&f), "the disk is writable");
    CHECK(vfs_write(&f, 0, "x", 1) == 1, "and takes a write");
    CHECK(vfs_open("/boot/welcome.txt", &f) && !vfs_writable(&f),
          "the ramdisk is not");
    CHECK(vfs_write(&f, 0, "x", 1) == -1, "and refuses one");
    CHECK(!vfs_create("/boot/new.txt", &f),
          "nothing can be made under /boot, it is read-only memory");
    CHECK(!vfs_mkdir("/boot/somedir"), "nor a directory under it");
    CHECK(!vfs_rmdir("/boot"), "and it is not removable either");
    CHECK(vfs_mkdir("/somedir"), "but the disk takes one");
    CHECK(vfs_rmdir("/somedir"), "and gives it back");
    CHECK(!vfs_unlink("/boot/welcome.txt"), "nor a file removed from it");
    CHECK(vfs_create("/new.txt", &f), "but the disk will make a file");



    unlinks = renames = 0;
    CHECK(!vfs_unlink("/boot/welcome.txt"),
          "nothing under /boot can be removed either");
    CHECK(unlinks == 0, "and the disk is not even asked about it");
    CHECK(vfs_unlink("/notes.txt"), "but a file on the disk goes");
    CHECK(unlinks == 1, "by asking the disk exactly once");

    CHECK(vfs_rename("/a.txt", "/b.txt"), "a rename on the disk is passed on");
    CHECK(renames == 1, "once");

    /*
     * a rename with either end on the ramdisk would be a copy pretending
     * to be a rename, and one of those quietly costs a whole file's
     * worth of reading and writing where a rename costs none
     */
    renames = 0;
    CHECK(!vfs_rename("/boot/welcome.txt", "/moved.txt"),
          "a rename out of /boot is refused");
    CHECK(!vfs_rename("/notes.txt", "/boot/moved.txt"),
          "and so is one into it");
    CHECK(renames == 0, "with the disk never asked to attempt either");

    CHECK(!vfs_unlink(""), "an empty path removes nothing");
    CHECK(!vfs_rename("/a.txt", ""), "and renames nothing");



    const void *data;
    uint64_t size;
    bool owned;

    CHECK(vfs_slurp("bin/hello", &data, &size, &owned), "a program slurps");
    CHECK(!owned, "off the ramdisk without being copied at all");
    vfs_release(data, owned);

    CHECK(vfs_slurp("/notes.txt", &data, &size, &owned), "and one off the disk");
    CHECK(owned, "which had to be read into memory first");
    CHECK(size == strlen(disk_notes) && memcmp(data, disk_notes, size) == 0,
          "with the right bytes");
    vfs_release(data, owned);

    /*
     * one namespace, two disks under it, and the question this layer
     * exists to answer: which one does a name belong to. the case worth
     * building the stub around is a name that exists on *both*,
     * /notes.txt and /work/notes.txt are two different files, and a
     * router that forwards the path but loses the mount reads one when
     * asked for the other, which looks like a corrupt disk and is not
     */
    {
        struct vfs_file w;
        CHECK(vfs_open("/work", &w) && w.is_dir,
              "the second mount point is a directory");
        CHECK(vfs_open("/work/", &w) && w.is_dir, "however it is spelled");

        CHECK(vfs_open("/work/notes.txt", &w) && w.kind == VFS_DISK,
              "a name under it resolves");
        CHECK(w.mount == DISK_WORK,
              "and remembers which filesystem it came from, a descriptor "
              "that did not would read the right offset out of the wrong "
              "disk");

        char got[64];
        int64_t n = vfs_read(&w, 0, got, sizeof got - 1);
        got[n > 0 ? n : 0] = '\0';
        CHECK(n > 0 && strcmp(got, "notes on the work disk\n") == 0,
              "and reading it gives the work disk's copy rather than the "
              "root's file of the same name");

        CHECK(vfs_open("/notes.txt", &w) && w.mount == DISK_ROOT,
              "while the root's own notes.txt is still the root's");
        n = vfs_read(&w, 0, got, sizeof got - 1);
        got[n > 0 ? n : 0] = '\0';
        CHECK(n > 0 && strcmp(got, "notes on the disk\n") == 0,
              "and reads as itself");

        /*
         * the mount point stands in the root's listing, and listing it
         * lists the *other* filesystem's root rather than a directory
         * of that name on this one
         */
        int saw_work = 0;
        for (size_t i = 0; vfs_readdir("/", i, &w); i++) {
            if (strcmp(w.name, "work") == 0 && w.is_dir) saw_work = 1;
        }
        CHECK(saw_work, "and /work stands in the root's listing");

        count = 0;
        int saw_job = 0;
        for (size_t i = 0; vfs_readdir("/work", i, &w); i++) {
            if (strcmp(w.name, "job") == 0 && w.is_dir) saw_job = 1;
            count++;
        }
        CHECK(count == (int)WORK_COUNT && saw_job,
              "listing it lists the second filesystem's root");

        /*
         * and every change goes to the right one, which is the half a
         * router gets wrong silently
         */
        last_mount = 99;
        CHECK(vfs_mkdir("/work/made"), "a directory can be made under it");
        CHECK(last_mount == DISK_WORK, "on the second filesystem");
        last_mount = 99;
        CHECK(vfs_mkdir("/made"), "and one on the root");
        CHECK(last_mount == DISK_ROOT, "on the root filesystem");

        last_mount = 99;
        CHECK(vfs_create("/work/new.txt", &w) && last_mount == DISK_WORK,
              "a file made under /work is made there");
        CHECK(w.mount == DISK_WORK, "and knows it");
        last_mount = 99;
        CHECK(vfs_unlink("/work/notes.txt") && last_mount == DISK_WORK,
              "and one removed from there is removed from there");

        /* a rename across the two is refused rather than quietly turned into a copy. */
        CHECK(!vfs_rename("/notes.txt", "/work/notes.txt"),
              "a rename from one filesystem to the other is refused, it "
              "would be a copy and a delete wearing a rename's name");
        CHECK(!vfs_rename("/work/notes.txt", "/notes.txt"),
              "and the same the other way round");
        last_mount = 99;
        CHECK(vfs_rename("/work/a", "/work/b") && last_mount == DISK_WORK,
              "while one inside the second filesystem is an ordinary "
              "rename");

        /*
         * and a machine whose drive holds one filesystem loses nothing:
         * the mount point stops being there rather than becoming a
         * directory nobody can enter
         */
        have_work = false;
        CHECK(!vfs_open("/work/notes.txt", &w),
              "with nothing mounted there, nothing is under it");
        saw_work = 0;
        for (size_t i = 0; vfs_readdir("/", i, &w); i++) {
            if (strcmp(w.name, "work") == 0) saw_work = 1;
        }
        CHECK(!saw_work,
              "and it is not listed either, a directory in a listing "
              "that cannot be entered looks like a fault, and a missing "
              "one is a fact about the machine");
        have_work = true;
    }

    /*
     * a third kind of thing in one namespace: not a disk, not memory,
     * but sectors on the medium this machine booted from. what has to
     * be true is that it resolves like everything else and cannot be
     * written like nothing else
     */
    {
        struct vfs_file sf;

        CHECK(vfs_open("/boot/src", &sf) && sf.is_dir
              && sf.kind == VFS_SOURCE,
              "/boot/src is a directory, and on no filesystem");
        CHECK(vfs_open("/boot/src/kernel/main.c", &sf)
              && sf.kind == VFS_SOURCE,
              "a file in it opens");
        CHECK(vfs_read(&sf, 0, buf, sf.size) == (int64_t)sf.size,
              "and reads off the medium");
        buf[sf.size] = '\0';
        CHECK(strcmp(buf, "int kmain(void);\n") == 0, "with its contents");

        /*
         * the name that exists in all three places, which is the case a
         * router that forwards a path but loses the mount gets wrong
         */
        CHECK(vfs_open("/welcome.txt", &sf) && sf.kind == VFS_DISK,
              "/welcome.txt is still the disk's");
        CHECK(vfs_open("/boot/welcome.txt", &sf) && sf.kind == VFS_RAMDISK,
              "/boot/welcome.txt is still the ramdisk's");
        CHECK(vfs_open("/boot/src/welcome.txt", &sf)
              && sf.kind == VFS_SOURCE,
              "and /boot/src/welcome.txt is a third file again");
        CHECK(vfs_read(&sf, 0, buf, sf.size) == (int64_t)sf.size, "which reads");
        buf[sf.size] = '\0';
        CHECK(strcmp(buf, "the source's copy\n") == 0,
              "as itself rather than as either of the others");

        /*
         * /boot/src is under /boot and has to be asked about first, or
         * the ramdisk answers for it
         */
        CHECK(!vfs_open("/boot/src/nothing.c", &sf),
              "a name that is not in the archive is not found");

        /*
         * listing: the archive is flat, so its subdirectories are
         * prefixes, the same arrangement /boot/bin has
         */
        size_t seen = 0, saw_make = 0;
        for (size_t i = 0; vfs_readdir("/boot/src", i, &sf); i++) {
            if (strcmp(sf.name, "GNUmakefile") == 0) saw_make = 1;
            seen++;
        }
        CHECK(seen == 4 && saw_make, "listing it shows every file, whole");

        seen = 0;
        int saw_shifted = 0;
        for (size_t i = 0; vfs_readdir("/boot/src/kernel", i, &sf); i++) {
            if (strcmp(sf.name, "main.c") == 0) saw_shifted = 1;
            seen++;
        }
        CHECK(seen == 2 && saw_shifted,
              "and listing a directory inside it shows what is under that "
              "prefix, named from there");

        /* and it stands in /boot the way /work stands in / */
        int saw_src = 0;
        seen = 0;
        for (size_t i = 0; vfs_readdir("/boot", i, &sf); i++) {
            if (strcmp(sf.name, "src") == 0 && sf.is_dir) saw_src = 1;
            seen++;
        }
        CHECK(saw_src && seen == 5, "/boot lists it, after its own files");

        /* nothing may write to it. */
        CHECK(!vfs_create("/boot/src/new.c", &sf), "nothing can be created");
        CHECK(!vfs_mkdir("/boot/src/new"), "no directory can be made");
        CHECK(!vfs_unlink("/boot/src/GNUmakefile"), "nothing can be removed");
        CHECK(!vfs_rename("/boot/src/GNUmakefile", "/boot/src/other"),
              "and nothing renamed");
        CHECK(vfs_open("/boot/src/GNUmakefile", &sf) && !vfs_writable(&sf),
              "and it says so when asked");

        /* a whole file, which the loader path uses. */
        const void *sdata; uint64_t ssize; bool sowned;
        CHECK(vfs_slurp("/boot/src/kernel/fs/vfs.c", &sdata, &ssize,
                        &sowned),
              "a whole file can be taken");
        CHECK(sowned && ssize == strlen("one namespace\n")
              && memcmp(sdata, "one namespace\n", ssize) == 0,
              "and it is a copy, because there is nothing to point at");
        vfs_release(sdata, sowned);

        /*
         * an image built without a source tree, which is every image
         * the mount point goes away rather than becoming
         * an empty directory somebody has to explain
         */
        source_mount(NULL, NULL);
        CHECK(!source_present(), "an image with no source carries none");
        CHECK(!vfs_open("/boot/src", &sf), "and /boot/src is nowhere");
        CHECK(!vfs_open("/boot/src/GNUmakefile", &sf), "with nothing under it");
        seen = 0;
        for (size_t i = 0; vfs_readdir("/boot", i, &sf); i++) {
            seen++;
        }
        CHECK(seen == 4, "and /boot is the four files it always was");
        make_source();
    }


    have_disk = false;
    have_work = false;

    CHECK(vfs_open("bin/hello", &f) && f.kind == VFS_RAMDISK,
          "every program is still found with no disk");
    CHECK(vfs_open("passwd", &f) && f.kind == VFS_RAMDISK,
          "and so is passwd, so the machine can still be logged into");
    CHECK(vfs_open("welcome.txt", &f) && f.kind == VFS_RAMDISK,
          "a bare name falls all the way through to the ramdisk");
    CHECK(vfs_open("/boot/welcome.txt", &f), "and /boot is where it always was");

    /* an absolute name still finds the ramdisk's copy. */
    CHECK(vfs_open("/welcome.txt", &f) && f.kind == VFS_RAMDISK,
          "an absolute name falls through to the ramdisk with no disk");
    CHECK(!vfs_open("/notes.txt", &f),
          "but a name on neither is still on neither");
    CHECK(!vfs_create("/anything.txt", &f), "and nothing can be made");
    CHECK(!vfs_unlink("/welcome.txt"),
          "nor unmade, the copy that is left lives in read-only memory");
    CHECK(!vfs_rename("/welcome.txt", "/other.txt"), "nor renamed");

    count = 0;
    saw_boot = 0;
    for (size_t i = 0; vfs_readdir("/", i, &f); i++) {
        if (strcmp(f.name, "boot") == 0) saw_boot = 1;
        count++;
    }
    CHECK(saw_boot && count == 1,
          "the root holds nothing but /boot, which is honest");

    count = 0;
    for (size_t i = 0; vfs_readdir("/boot", i, &f); i++) {
        count++;
    }
    CHECK(count == 5, "and /boot still lists everything, source and all");

    CHECK(vfs_slurp("bin/hello", &data, &size, &owned) && !owned,
          "programs still load, which is the whole reason to keep it");
    vfs_release(data, owned);



    struct vfs_mount m;
    CHECK(vfs_mount_at(0, &m) && strcmp(m.at, "/") == 0, "/ is a mount");
    CHECK(!m.present, "and says so when there is no disk behind it");
    CHECK(vfs_mount_at(1, &m) && strcmp(m.at, DISK_WORK_AT) == 0,
          "/work is one, listed whether or not anything is on it");
    CHECK(!m.present,
          "and says plainly when there is nowhere to put work that is not "
          "the system, which is worth being told rather than discovering "
          "by filling the disk");
    CHECK(vfs_mount_at(2, &m) && strcmp(m.at, VFS_BOOT) == 0, "/boot is one");
    CHECK(m.present && !m.writable, "always there, never writable");
    CHECK(vfs_mount_at(3, &m) && strcmp(m.at, VFS_SRC) == 0,
          "and the source is the fourth, which is a mount on no disk and "
          "in no memory");
    CHECK(m.present && !m.writable, "there, and never writable either");
    CHECK(!vfs_mount_at(4, &m), "and there are only the four");

    if (failures == 0) printf("all good\n");
    return failures;
}
