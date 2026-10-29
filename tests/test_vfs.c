// SPDX-License-Identifier: GPL-2.0-only
/*
 * tests/test_vfs.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * host-side test for the one namespace: a root in memory, made of the
 * archive, and the mounts on it.
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

#include <velvet/vfs.h>
#include <velvet/disk.h>
#include <velvet/ramdisk.h>
#include <velvet/ramfs.h>
#include <velvet/blockdev.h>
#include <velvet/process.h>
#include <velvet/epoch.h>
#include <uapi/velvet/errno.h>

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
} while (0)



/* which slot is which disk, here, as mount would number them */
#define SLOT_ROOT 0
#define SLOT_WORK 1

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

/*
 * /proc, faked with one file, so what is tested here is the routing: the
 * real files and what they say are tests/test_proc.c's
 */
bool proc_find(const char *str_name, size_t *p_index)
{
    if (strcmp(str_name, "version") == 0) {
        *p_index = 0;
        return true;
    }
    if (strcmp(str_name, "secret") == 0) {
        *p_index = 1;
        return true;
    }
    return false;
}

const char *proc_name(size_t sz_index)
{
    return (sz_index == 0) ? "version" : (sz_index == 1) ? "secret" : NULL;
}

uint32_t proc_mode(size_t sz_index)
{
    return (sz_index == 0) ? 0444 : (sz_index == 1) ? 0400 : 0;
}

int64_t proc_read(size_t sz_index, uint64_t u_offset, void *p_buf,
                  uint64_t u_len)
{
    const char *str_text = "velvet version 0\n";
    uint64_t u_size = strlen(str_text);

    if (sz_index != 0) {
        return -1;
    }
    if (u_offset >= u_size) {
        return 0;
    }
    if (u_len > u_size - u_offset) {
        u_len = u_size - u_offset;
    }
    memcpy(p_buf, str_text + u_offset, u_len);
    return (int64_t)u_len;
}

bool disk_ready(size_t which)
{
    return which == SLOT_WORK ? have_work : have_disk;
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
    if (which == SLOT_WORK) {
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
    if (which == SLOT_WORK) {
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
    if (which == SLOT_WORK) {
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
bool disk_create(size_t which, const char *path, uint32_t u_mode,
                 uint32_t u_uid, uint32_t u_gid, struct disk_entry *out)
{
    (void)u_mode; (void)u_uid; (void)u_gid;
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
static uint64_t s_u_truncated_to = 1;
bool disk_truncate(size_t sz_which, struct disk_entry *p_entry,
                   uint64_t u_size)
{
    if (!disk_ready(sz_which)) {
        return false;
    }
    last_mount = sz_which;
    s_u_truncated_to = u_size;
    p_entry->size = u_size;
    return true;
}
const char *disk_model(void)
{
    return "STUB DISK";
}
static int mkdirs, rmdirs, unlinks, renames;
bool disk_mkdir(size_t w, const char *path, uint32_t u_mode, uint32_t u_uid,
                uint32_t u_gid)
{
    (void)u_mode; (void)u_uid; (void)u_gid;
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
bool disk_symlink(size_t w, const char *p, const char *t, uint32_t u_uid,
                  uint32_t u_gid)
{
    (void)u_uid; (void)u_gid;
    (void)w; (void)p; (void)t; return false;
}

static uint32_t s_u_mknod_rdev;
bool disk_mknod(size_t w, const char *p, uint32_t u_mode, uint32_t u_uid,
                uint32_t u_gid, uint32_t u_rdev)
{
    (void)p; (void)u_mode; (void)u_uid; (void)u_gid;
    last_mount = w;
    s_u_mknod_rdev = u_rdev;
    return disk_ready(w);
}

bool disk_rename(size_t w, const char *from, const char *to)
{
    (void)from; (void)to; last_mount = w; renames++; return disk_ready(w);
}

/* nobody has anything open, unless the test says somebody does */
static bool s_b_open;
bool process_any_file(bool (*p_test)(const struct file *p_file,
                                      const void *p_ctx),
                      const void *p_ctx)
{
    (void)p_test;
    (void)p_ctx;
    return s_b_open;
}
bool process_any_cwd(bool (*p_test)(const char *str_cwd, const void *p_ctx),
                     const void *p_ctx)
{
    (void)p_test;
    (void)p_ctx;
    return false;
}
/* one process, standing somewhere, so a move can be seen to take it along */
static char s_arr_cwd[PATH_MAX] = "/";
void process_map_paths(void (*p_map)(char *str_path, bool b_place,
                                     const void *p_ctx),
                       const void *p_ctx)
{
    p_map(s_arr_cwd, true, p_ctx);
}
uint64_t epoch_now(void)
{
    return 0;
}
void epoch_to_date(uint64_t u_seconds, struct rtc_time *p_out)
{
    (void)u_seconds;
    memset(p_out, 0, sizeof *p_out);
}

/* no drives here: /dev is the devices dev.c has of its own */
size_t disk_block_count(void)
{
    return 0;
}
const struct disk_block *disk_block_at(size_t sz_index)
{
    (void)sz_index;
    return NULL;
}
struct file *blockdev_open(uint32_t u_rdev, uint32_t u_mode)
{
    (void)u_rdev; (void)u_mode;
    return NULL;
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

/* the generator and the console are other suites' to check */
void random_bytes(void *p_buf, size_t sz_len)
{
    memset(p_buf, 0x5a, sz_len);
}
void random_mix(const void *p_buf, size_t sz_len)
{
    (void)p_buf;
    (void)sz_len;
}
struct file *tty_open(unsigned u_console, uint32_t u_mode)
{
    (void)u_console;
    (void)u_mode;
    return NULL;
}
struct file *tty_open_own(uint32_t u_mode)
{
    (void)u_mode;
    return NULL;
}

/* the slab a file comes from, as plain malloc */
#include <velvet/file.h>
#include <velvet/dev.h>
#include <velvet/slab.h>
#include <uapi/velvet/errno.h>
void slab_cache_init(struct slab_cache *cache, const char *name,
                     size_t obj_size)
{
    (void)name;
    cache->obj_size = obj_size;
}
void *slab_alloc(struct slab_cache *cache)
{
    return malloc(cache->obj_size);
}
void slab_free(void *object)
{
    free(object);
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
    CHECK(ramdisk_present(), "the ramdisk mounted");

    struct vfs_file f;
    char buf[128];
    size_t sz_files = 0;

    /*
     * the root is the archive, unpacked into memory, with the devices on
     * it. a system mounts its disk somewhere and moves into it; here it
     * hangs at /mnt, with the work filesystem on it, and /proc beside it
     */
    CHECK(vfs_root_init(&sz_files) && sz_files == 4,
          "the root is made of the archive, every file of it");
    CHECK(vfs_mkdir("/mnt", 0755, 0, 0) && vfs_mkdir("/proc", 0555, 0, 0),
          "and is a filesystem a directory can be made on");
    CHECK(vfs_mount_add("/mnt", VFS_DISK, SLOT_ROOT, "/dev/sdb1") == 0
          && vfs_mount_add("/mnt/work", VFS_DISK, SLOT_WORK, "/dev/sdb2") == 0
          && vfs_mount_add("/proc", VFS_PROC, 0, "proc") == 0,
          "the disks and /proc are mounted");

    have_disk = true;

    CHECK(vfs_open("/welcome.txt", &f) && f.kind == VFS_RAMFS,
          "an absolute name at the root is the archive's");
    CHECK(vfs_read(&f, 0, buf, f.size) == (int64_t)f.size, "and reads");
    buf[f.size] = '\0';
    CHECK(strcmp(buf, "the ramdisk's copy\n") == 0,
          "with what the archive held");
    CHECK(vfs_open("welcome.txt", &f) && f.kind == VFS_RAMFS,
          "a bare name from inside the kernel is the root's too");
    CHECK(vfs_open("/bin/hello", &f) && vfs_open("bin/hello", &f)
          && f.mode == 0755, "a name below the top, with its mode");
    CHECK(vfs_open("/bin", &f) && f.is_dir,
          "and the directory it is in, which the archive never named");

    CHECK(!vfs_open("/nothing-at-all", &f), "a name nowhere is nowhere");
    CHECK(!vfs_open("/mntleg/x", &f), "and /mntleg is not /mnt");
    CHECK(!vfs_open("", &f), "an empty path is nothing");

    CHECK(vfs_open("/", &f) && f.is_dir && f.kind == VFS_RAMFS,
          "the root is a directory, the archive's");
    CHECK(vfs_open("/mnt", &f) && f.is_dir && f.kind == VFS_DISK,
          "and a mount point is the root of what is mounted there");
    CHECK(vfs_open("/mnt/", &f) && f.is_dir, "however it is spelled");

    /* the root is memory a program may write */
    CHECK(vfs_create("/new.txt", 0644, 1000, 1000, &f)
          && f.kind == VFS_RAMFS && f.uid == 1000,
          "a file can be made at the root, it is in memory");
    CHECK(vfs_write(&f, 0, "written", 7) == 7 && vfs_truncate(&f, 3)
          && f.size == 3, "written to and cut to length");
    CHECK(vfs_mkdir("/somedir", 0755, 0, 0) && vfs_rmdir("/somedir"),
          "a directory made there, and given back");
    CHECK(vfs_rename("/new.txt", "/bin/new.txt")
          && vfs_open("/bin/new.txt", &f) && vfs_unlink("/bin/new.txt"),
          "renamed within it, and removed");
    CHECK(vfs_open("/proc/version", &f) && !vfs_truncate(&f, 0)
          && vfs_open("/bin", &f) && !vfs_truncate(&f, 0),
          "but /proc is not cut to length, nor a directory");
    CHECK(!vfs_create("/proc/new", 0644, 0, 0, &f),
          "and nothing can be made in /proc, which keeps no names");

    /* /proc, which no disk or archive holds */
    CHECK(vfs_open("/proc", &f) && f.is_dir && f.kind == VFS_PROC,
          "/proc is a directory of its own");
    CHECK(vfs_open("/proc/version", &f) && !f.is_dir && f.kind == VFS_PROC
          && f.size == 0 && f.mode == 0444,
          "a file in it says 0 bytes, and anyone may read it");
    CHECK(vfs_may_read(&f, 1000), "including a guest");
    {
        char arr_text[32];
        int64_t i_got = vfs_read(&f, 0, arr_text, sizeof arr_text);
        CHECK(i_got == 17 && memcmp(arr_text, "velvet version 0\n", 17) == 0,
              "and reading it reads what /proc writes");
        CHECK(vfs_read(&f, 17, arr_text, sizeof arr_text) == 0,
              "to the end and no further");
    }
    CHECK(!vfs_open("/proc/nothing", &f), "a name /proc does not have");
    CHECK(!vfs_open("/processes", &f), "and /processes is not /proc");
    CHECK(vfs_readdir("/proc", 0, &f) && strcmp(f.name, "version") == 0
          && f.kind == VFS_PROC, "the listing has its file");
    CHECK(vfs_open("/proc/secret", &f) && f.mode == 0400,
          "a file can be root's alone");
    CHECK(!vfs_may_read(&f, 1000) && vfs_may_read(&f, 0),
          "so a guest may not read it, and root may");
    CHECK(vfs_readdir("/proc", 1, &f) && strcmp(f.name, "secret") == 0
          && f.mode == 0400, "it is listed with the mode it has");
    CHECK(!vfs_readdir("/proc", 2, &f), "and the listing ends there");

    /* the root's listing: the archive, and the directories made on it */
    {
        int i_proc = 0;
        int i_mnt = 0;
        int saw_welcome = 0;
        int count = 0;
        for (size_t i = 0; vfs_readdir("/", i, &f); i++) {
            i_proc += strcmp(f.name, "proc") == 0 && f.is_dir;
            i_mnt += strcmp(f.name, "mnt") == 0 && f.is_dir;
            saw_welcome |= strcmp(f.name, "welcome.txt") == 0;
            count++;
        }
        CHECK(saw_welcome, "the root lists what the archive held");
        CHECK(i_proc == 1 && i_mnt == 1,
              "and each mount over a directory of it once, as that "
              "directory");
        CHECK(count == 7, "exactly the archive's four names, the two "
              "directories made on it and /dev");
        count = 0;
        for (size_t i = 0; vfs_readdir("//", i, &f); i++) count++;
        CHECK(count == 7, "and a doubled slash is the root");
        count = 0;
        for (size_t i = 0; vfs_readdir("bin/", i, &f); i++) count++;
        CHECK(count == 1, "a relative directory is the root's, and a "
              "trailing slash changes nothing");
    }

    /* the disk, under its mount */
    CHECK(vfs_open("/mnt/welcome.txt", &f) && f.kind == VFS_DISK
          && f.mount == SLOT_ROOT, "a name under /mnt is the disk's");
    CHECK(vfs_read(&f, 0, buf, f.size) == (int64_t)f.size, "and reads");
    buf[f.size] = '\0';
    CHECK(strcmp(buf, "the disk's copy\n") == 0,
          "with the disk's contents, which are different");
    CHECK(vfs_open("/mnt/welcome.txt", &f) && vfs_truncate(&f, 3)
          && f.size == 3 && s_u_truncated_to == 3,
          "a file on the disk is cut to length");
    CHECK(vfs_open("/mnt/deep/x.txt", &f) && f.kind == VFS_DISK,
          "a nested disk path resolves");
    CHECK(f.mode == 0600 && f.uid == 1000,
          "with its own mode and its own owner");
    CHECK(vfs_may_read(&f, 1000) && !vfs_may_read(&f, 2000)
          && vfs_may_read(&f, 0),
          "readable by whoever owns it, and the master, and nobody else");
    {
        int count = 0;
        int i_work = 0;
        for (size_t i = 0; vfs_readdir("/mnt", i, &f); i++) {
            i_work += strcmp(f.name, "work") == 0 && f.is_dir;
            count++;
        }
        CHECK(i_work == 1 && count == (int)DISK_COUNT - 1 + 1,
              "/mnt lists the disk's root, and the mount standing in it "
              "where the disk has no directory of that name");
    }

    CHECK(vfs_open("/secret.txt", &f) && vfs_may_read(&f, 0)
          && !vfs_may_read(&f, 1000),
          "a private file of the archive is the master's alone");
    CHECK(vfs_open("/mnt/notes.txt", &f) && vfs_writable(&f)
          && vfs_write(&f, 0, "x", 1) == 1, "the disk takes a write");

    unlinks = renames = 0;
    CHECK(vfs_unlink("/mnt/notes.txt") && unlinks == 1,
          "a file on the disk goes, by asking the disk exactly once");
    CHECK(vfs_rename("/mnt/a.txt", "/mnt/b.txt") && renames == 1,
          "and a rename on it is passed on, once");
    renames = 0;
    CHECK(!vfs_rename("/welcome.txt", "/mnt/moved.txt")
          && !vfs_rename("/mnt/notes.txt", "/moved.txt") && renames == 0,
          "a rename between the archive and the disk is refused, with "
          "the disk never asked to attempt it");

    /*
     * one namespace, two disks under it, and the question this layer
     * exists to answer: which one does a name belong to. the case worth
     * building the stub around is a name that exists on *both*:
     * /mnt/notes.txt and /mnt/work/notes.txt are two different files,
     * and a router that forwards the path but loses the mount reads one
     * when asked for the other, which looks like a corrupt disk and is
     * not
     */
    {
        struct vfs_file w;
        char got[64];

        CHECK(vfs_open("/mnt/work/notes.txt", &w) && w.kind == VFS_DISK
              && w.mount == SLOT_WORK,
              "a name under the second mount remembers which filesystem it "
              "came from");
        int64_t n = vfs_read(&w, 0, got, sizeof got - 1);
        got[n > 0 ? n : 0] = '\0';
        CHECK(n > 0 && strcmp(got, "notes on the work disk\n") == 0,
              "and reading it gives the work disk's copy rather than the "
              "root disk's file of the same name");
        CHECK(vfs_open("/mnt/notes.txt", &w) && w.mount == SLOT_ROOT,
              "while the first disk's own notes.txt is still its own");

        int count = 0;
        int saw_job = 0;
        for (size_t i = 0; vfs_readdir("/mnt/work", i, &w); i++) {
            saw_job |= strcmp(w.name, "job") == 0 && w.is_dir;
            count++;
        }
        CHECK(count == (int)WORK_COUNT && saw_job,
              "listing it lists the second filesystem's root");

        last_mount = 99;
        CHECK(vfs_mkdir("/mnt/work/made", 0755, 0, 0)
              && last_mount == SLOT_WORK,
              "a directory made under it is made on the second filesystem");
        last_mount = 99;
        CHECK(vfs_mkdir("/mnt/made", 0755, 0, 0) && last_mount == SLOT_ROOT,
              "and one above it on the first");
        last_mount = 99;
        CHECK(vfs_create("/mnt/work/new.txt", 0644, 0, 0, &w)
              && last_mount == SLOT_WORK && w.mount == SLOT_WORK,
              "a file made under it is made there, and knows it");
        CHECK(!vfs_rename("/mnt/notes.txt", "/mnt/work/notes.txt")
              && !vfs_rename("/mnt/work/notes.txt", "/mnt/notes.txt"),
              "a rename from one filesystem to the other is refused either "
              "way, it would be a copy and a delete wearing a rename's "
              "name");
        last_mount = 99;
        CHECK(vfs_rename("/mnt/work/a", "/mnt/work/b")
              && last_mount == SLOT_WORK,
              "while one inside the second filesystem is an ordinary "
              "rename");

        /*
         * a mount with nothing on it is not there: the names under it
         * are nobody's, and it is not listed, a directory in a listing
         * that cannot be entered looks like a fault
         */
        have_work = false;
        CHECK(!vfs_open("/mnt/work/notes.txt", &w),
              "with nothing mounted there, nothing is under it");
        int saw_work = 0;
        for (size_t i = 0; vfs_readdir("/mnt", i, &w); i++) {
            saw_work |= strcmp(w.name, "work") == 0;
        }
        CHECK(!saw_work, "and it is not listed either");
        have_work = true;
    }

    /*
     * a filesystem in memory mounted over one of the disk's directories:
     * the names under it are the new one's, and the disk's are back when
     * it is taken down
     */
    {
        struct vfs_file r;
        struct vfs_file q;
        char arr_got[16];
        enum vfs_kind e_kind = VFS_NOWHERE;
        size_t sz_slot = 99;

        int i_fs = ramfs_new(01777);
        CHECK(i_fs >= 0
              && vfs_mount_add("/mnt/deep", VFS_RAMFS, (size_t)i_fs, "none")
                 == 0,
              "a ramfs is mounted over a directory of the disk's");
        CHECK(vfs_mount_add("/mnt/deep", VFS_RAMFS, (size_t)i_fs, "none")
              == -EBUSY, "and nothing else can be mounted there now");
        CHECK(!vfs_open("/mnt/deep/x.txt", &r),
              "the disk's file under it is out of sight");
        CHECK(vfs_open("/mnt/deep", &r) && r.is_dir && r.kind == VFS_RAMFS
              && r.mode == 01777 && strcmp(r.name, "deep") == 0,
              "and the mount point is the new filesystem's root, with the "
              "mode it was made with");
        CHECK(vfs_create("/mnt/deep/made", 0644, 1000, 1000, &r)
              && r.kind == VFS_RAMFS && vfs_write(&r, 0, "in memory", 9) == 9
              && vfs_open("/mnt/deep/made", &q)
              && vfs_read(&q, 0, arr_got, sizeof arr_got) == 9
              && memcmp(arr_got, "in memory", 9) == 0,
              "a file made there is made in memory, and reads back by its "
              "name");
        int i_deeps = 0;
        for (size_t sz_at = 0; vfs_readdir("/mnt", sz_at, &q); sz_at++) {
            i_deeps += strcmp(q.name, "deep") == 0;
        }
        CHECK(i_deeps == 1, "the disk lists it once: a mount over a "
              "directory it has is that directory");
        char arr_at[64];
        CHECK(vfs_where(VFS_RAMFS, (size_t)i_fs, arr_at, sizeof arr_at)
              && strcmp(arr_at, "/mnt/deep") == 0,
              "and it can be asked where it is");

        int i_inner = ramfs_new(0755);
        CHECK(i_inner >= 0 && vfs_mkdir("/mnt/deep/sub", 0755, 0, 0)
              && vfs_mount_add("/mnt/deep/sub", VFS_RAMFS, (size_t)i_inner,
                               "inner") == 0
              && vfs_mount_remove("/mnt/deep", &e_kind, &sz_slot) == -EBUSY,
              "with another mounted inside it, it cannot be taken down");
        CHECK(vfs_mount_remove("/mnt/deep/sub", &e_kind, &sz_slot) == 0
              && e_kind == VFS_RAMFS && sz_slot == (size_t)i_inner
              && ramfs_free(sz_slot), "until that one is");
        s_b_open = true;
        CHECK(vfs_mount_remove("/mnt/deep", &e_kind, &sz_slot) == -EBUSY,
              "nor with a file open on it");
        s_b_open = false;
        CHECK(vfs_mount_remove("/mnt/deep", &e_kind, &sz_slot) == 0
              && e_kind == VFS_RAMFS && sz_slot == (size_t)i_fs
              && ramfs_free(sz_slot),
              "and then it comes down, saying what it was");
        CHECK(vfs_open("/mnt/deep/x.txt", &q) && q.kind == VFS_DISK,
              "and the disk's file is back where it was");
        CHECK(vfs_mount_remove("/mnt/deep", &e_kind, &sz_slot) == -EINVAL,
              "a second time nothing is mounted there");
        CHECK(vfs_mount_remove("/", &e_kind, &sz_slot) == -EBUSY,
              "and the root never comes down");
    }

    /* a mount moved goes where it is sent, with whoever stands in it */
    {
        strcpy(s_arr_cwd, "/proc");
        CHECK(vfs_mkdir("/mnt/proc", 0555, 0, 0)
              && vfs_mount_move("/proc", "/mnt/proc") == 0
              && vfs_open("/mnt/proc/version", &f) && f.kind == VFS_PROC
              && !vfs_open("/proc/version", &f),
              "a mount is moved, and is where it was moved to and not "
              "where it was");
        CHECK(strcmp(s_arr_cwd, "/mnt/proc") == 0,
              "with a process standing in it moved along");
        CHECK(vfs_mount_move("/mnt/proc", "/mnt/proc/inside") == -EINVAL
              && vfs_mount_move("/welcome.txt", "/mnt/x") == -EINVAL
              && vfs_mount_move("/", "/mnt/x") == -EINVAL
              && vfs_mount_move("/mnt/proc", "/mnt/work") == -EBUSY,
              "never under itself, never one that is not a mount or is "
              "the root, and never onto another");
        CHECK(vfs_mount_move("/mnt/proc", "/proc") == 0
              && vfs_open("/proc/version", &f)
              && strcmp(s_arr_cwd, "/proc") == 0, "and moved back");
        strcpy(s_arr_cwd, "/");
    }

    /* the root exchanged for the disk, and back */
    {
        strcpy(s_arr_cwd, "/bin");
        CHECK(vfs_pivot("/mnt", "/mnt/old") == 0,
              "the disk mounted at /mnt is made the root");
        CHECK(vfs_open("/welcome.txt", &f) && f.kind == VFS_DISK
              && f.mount == SLOT_ROOT,
              "so its names are at the top now");
        CHECK(vfs_open("/old/welcome.txt", &f) && f.kind == VFS_RAMFS,
              "the old root is where it was put, under it");
        CHECK(vfs_open("/old/proc/version", &f) && f.kind == VFS_PROC,
              "with the mounts that were not under the new root on it");
        CHECK(vfs_open("/work/notes.txt", &f) && f.mount == SLOT_WORK,
              "and the ones that were where they were, under the new root");
        CHECK(strcmp(s_arr_cwd, "/old/bin") == 0,
              "a process standing in the old root still stands in the "
              "same directory, which is under /old now");
        CHECK(vfs_pivot("/old", "/old/mnt") == 0
              && vfs_open("/welcome.txt", &f) && f.kind == VFS_RAMFS
              && vfs_open("/mnt/work/notes.txt", &f) && f.mount == SLOT_WORK
              && vfs_open("/proc/version", &f),
              "and exchanged back, everything is where it was");
        CHECK(strcmp(s_arr_cwd, "/bin") == 0, "it included");
        CHECK(vfs_pivot("/proc", "/tmp") == -EINVAL
              && vfs_pivot("/bin", "/bin/old") == -EINVAL
              && vfs_pivot("/", "/old") == -EINVAL
              && vfs_pivot("/mnt", "/mnt/work") == -EBUSY,
              "the old root goes under the new one, the new one is a mount "
              "that is not the root already, and nothing may be mounted "
              "where the old one goes");
        strcpy(s_arr_cwd, "/");
    }

    {
        struct vfs_mount m;
        size_t sz_rows = 0;
        bool b_root = false;
        bool b_dev = false;
        for (size_t sz_at = 0; vfs_mount_at(sz_at, &m); sz_at++) {
            sz_rows++;
            b_root |= strcmp(m.arr_at, "/") == 0 && m.present && m.writable
                      && strcmp(m.what, "ramfs") == 0
                      && strstr(m.arr_where, "bootloader") != NULL;
            b_dev |= strcmp(m.arr_at, "/dev") == 0
                     && strcmp(m.what, "devtmpfs") == 0;
        }
        CHECK(b_root, "the table has the root, in memory, from what the "
              "bootloader handed over");
        CHECK(b_dev, "and /dev on it");
        CHECK(sz_rows == 5 && sz_rows == vfs_mount_count(),
              "and every mount on it, and nothing more");
    }

    const void *data;
    uint64_t size;
    bool owned;

    CHECK(vfs_slurp("bin/hello", &data, &size, &owned), "a program slurps");
    CHECK(owned && size == 8 && memcmp(data, "ELF-ish\n", 8) == 0,
          "copied out of memory, the whole of it");
    vfs_release(data, owned);
    CHECK(vfs_slurp("/mnt/notes.txt", &data, &size, &owned) && owned
          && size == strlen(disk_notes)
          && memcmp(data, disk_notes, size) == 0,
          "and one off the disk the same way");
    vfs_release(data, owned);

    /*
     * a file opened from a node: one position, moved by reads and by
     * seeks, which stay within the file. the archive's welcome is 19
     * bytes long
     */
    {
        struct file *p_file = NULL;
        char arr_got[32];

        CHECK(vfs_open("/welcome.txt", &f)
              && (p_file = vfs_open_file(&f, FILE_READ)) != NULL
              && p_file->u_refs == 1 && p_file->e_type == FILE_TYPE_REGULAR,
              "a node opens as a regular file, with one reference");
        CHECK(p_file->p_ops->read(p_file, arr_got, 4) == 4
              && p_file->p_ops->read(p_file, arr_got + 4, 4) == 4
              && memcmp(arr_got, "the ramd", 8) == 0 && p_file->u_pos == 8,
              "and reads carry on from where the last one stopped");
        CHECK(p_file->p_ops->seek(p_file, 10, SEEK_SET) == 10
              && p_file->p_ops->seek(p_file, 5, SEEK_CUR) == 15
              && p_file->p_ops->seek(p_file, -4, SEEK_END) == 15,
              "from the start, from where it is, and from the end");
        CHECK(p_file->p_ops->seek(p_file, -200, SEEK_CUR) == -EINVAL
              && p_file->p_ops->seek(p_file, 0, 7) == -EINVAL
              && p_file->u_pos == 15,
              "a place before the start, or a whence it does not know, "
              "moves nothing");
        CHECK(p_file->p_ops->read(p_file, arr_got, sizeof arr_got) == 4
              && p_file->p_ops->read(p_file, arr_got, sizeof arr_got) == 0,
              "a read stops at the end, and the next one is the end");
        CHECK(p_file->p_ops->seek(p_file, 1, SEEK_END) == 20
              && p_file->p_ops->read(p_file, arr_got, sizeof arr_got) == 0,
              "a place past the end is somewhere, with nothing to read");

        /* and unlinked while open, it is still there for whoever has it */
        CHECK(vfs_unlink("/welcome.txt") && !vfs_open("/welcome.txt", &f)
              && p_file->p_ops->seek(p_file, 0, SEEK_SET) == 0
              && p_file->p_ops->read(p_file, arr_got, 4) == 4
              && memcmp(arr_got, "the ", 4) == 0,
              "a file of the root unlinked while open reads on, nameless");
        file_put(p_file);

        CHECK(vfs_open("/proc/version", &f)
              && (p_file = vfs_open_file(&f, FILE_READ)) != NULL
              && p_file->p_ops->seek(p_file, 0, SEEK_SET) == -ESPIPE,
              "a /proc file, which has no size, has no position to move");
        file_put(p_file);
    }

    /* /dev: a table of devices, opened by name */
    {
        struct file *p_file = NULL;
        char arr_got[8];
        size_t sz_count = 0;

        CHECK(vfs_open("/dev", &f) && f.is_dir && f.kind == VFS_DEV,
              "/dev is a directory");
        for (size_t sz_at = 0; vfs_readdir("/dev", sz_at, &f); sz_at++) {
            sz_count++;
        }
        CHECK(sz_count == dev_count(), "listing every device it has");
        CHECK(vfs_open("/dev/null", &f) && !f.is_dir && f.kind == VFS_DEV
              && f.mode == 0666 && vfs_may_read(&f, 1000),
              "/dev/null is a device anybody may use");
        CHECK(!vfs_open("/dev/nothing", &f), "and a name it lacks is not");

        vfs_open("/dev/null", &f);
        p_file = vfs_open_file(&f, FILE_READ | FILE_WRITE);
        CHECK(p_file != NULL && p_file->e_type == FILE_TYPE_CHAR
              && p_file->p_ops->read(p_file, arr_got, 8) == 0
              && p_file->p_ops->write(p_file, "gone", 4) == 4,
              "null reads as the end and swallows what is written");
        file_put(p_file);

        vfs_open("/dev/zero", &f);
        p_file = vfs_open_file(&f, FILE_READ);
        memset(arr_got, 7, sizeof arr_got);
        CHECK(p_file != NULL && p_file->p_ops->read(p_file, arr_got, 8) == 8
              && arr_got[0] == 0 && arr_got[7] == 0,
              "zero reads as zeroes");
        file_put(p_file);

        vfs_open("/dev/full", &f);
        p_file = vfs_open_file(&f, FILE_WRITE);
        CHECK(p_file != NULL
              && p_file->p_ops->write(p_file, "x", 1) == -ENOSPC,
              "full has no room for anything");
        file_put(p_file);

        vfs_open("/dev/urandom", &f);
        p_file = vfs_open_file(&f, FILE_READ);
        CHECK(p_file != NULL && p_file->p_ops->read(p_file, arr_got, 8) == 8
              && arr_got[0] == 0x5a,
              "urandom reads from the generator");
        file_put(p_file);
    }

    /*
     * a device node, wherever it is, opens the device it names by its
     * number: one in memory for /dev/null opens /dev/null
     */
    {
        struct file *p_file = NULL;
        char arr_got[8];

        CHECK(vfs_mknod("/null", 0020000 | 0666, 0, 0, 1 << 8 | 3)
              && vfs_open("/null", &f) && f.kind == VFS_RAMFS
              && f.u_type == 0020000 && f.u_rdev == (1 << 8 | 3)
              && vfs_is_device(&f),
              "a device node is made in memory, with its type and number");
        p_file = vfs_open_file(&f, FILE_READ | FILE_WRITE);
        CHECK(p_file != NULL && p_file->e_type == FILE_TYPE_CHAR
              && p_file->p_ops->read(p_file, arr_got, 8) == 0
              && p_file->p_ops->write(p_file, "gone", 4) == 4,
              "and opens as the device it names");
        file_put(p_file);
        CHECK(vfs_mknod("/nothing", 0020000 | 0666, 0, 0, 99 << 8 | 99)
              && vfs_open("/nothing", &f) && vfs_open_file(&f, FILE_READ)
                 == NULL,
              "one whose number nothing answers to opens as nothing");
        CHECK(vfs_mknod("/mnt/node", 0060000 | 0600, 0, 0, 8 << 8 | 1)
              && last_mount == SLOT_ROOT && s_u_mknod_rdev == (8 << 8 | 1),
              "one under the disk's mount is made on the disk, with its "
              "number");
        CHECK(!vfs_mknod("/proc/node", 0020000 | 0666, 0, 0, 1 << 8 | 3),
              "and /proc keeps none");
        CHECK(vfs_unlink("/null") && vfs_unlink("/nothing"),
              "and they go like any name");
    }

    /* and a machine whose disk went away has the root it booted with */
    have_disk = false;
    have_work = false;
    CHECK(!vfs_open("/mnt/notes.txt", &f),
          "with no disk, nothing is under its mount");
    {
        int count = 0;
        for (size_t i = 0; vfs_readdir("/mnt", i, &f); i++) count++;
        CHECK(count == 0, "which is the empty directory of the root's it "
              "hung over");
    }
    CHECK(vfs_open("/bin/hello", &f) && f.kind == VFS_RAMFS,
          "and every program is still found");

    if (failures == 0) printf("all good\n");
    return failures;
}
