// SPDX-License-Identifier: GPL-2.0-only
/*
 * tests/test_ramfs.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * host-side test for the filesystem kept in memory.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdbool.h>

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
static uint64_t s_u_now = 1000;
uint64_t epoch_now(void)
{
    return s_u_now;
}

#include <sys/stat.h>

#include <velvet/ramfs.h>
#include <uapi/velvet/errno.h>

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
} while (0)

static bool make_file(size_t sz_fs, const char *str_path,
                      struct ramfs_entry *p_out)
{
    return ramfs_make(sz_fs, str_path, S_IFREG | 0644, 0, 0, 0, NULL, p_out);
}

static bool make_dir(size_t sz_fs, const char *str_path)
{
    return ramfs_make(sz_fs, str_path, S_IFDIR | 0755, 0, 0, 0, NULL, NULL);
}

static size_t count_in(size_t sz_fs, const char *str_dir)
{
    struct ramfs_entry e;
    size_t sz_count = 0;
    while (ramfs_readdir(sz_fs, str_dir, sz_count, &e)) {
        sz_count++;
    }
    return sz_count;
}

int main(void)
{
    struct ramfs_entry e;
    struct ramfs_entry f;
    char arr_buf[16384];

    int i_fs = ramfs_new(0755);
    CHECK(i_fs >= 0, "a filesystem is made");
    size_t fs = (size_t)i_fs;
    CHECK(ramfs_lookup(fs, "/", true, &e) && S_ISDIR(e.u_mode)
          && (e.u_mode & 07777) == 0755 && e.u_uid == 0,
          "holding a root, root's, with the mode it was given");
    CHECK(count_in(fs, "/") == 0, "and nothing else");

    /* files, and their bytes */
    CHECK(make_file(fs, "/hello", &e) && S_ISREG(e.u_mode) && e.u_size == 0,
          "a file is made, empty");
    CHECK(!make_file(fs, "/hello", &f), "and not made twice");
    CHECK(ramfs_write(e.u_ino, 0, "hello there", 11) == 11,
          "it takes a write");
    memset(arr_buf, 0, sizeof arr_buf);
    CHECK(ramfs_read(e.u_ino, 0, arr_buf, sizeof arr_buf) == 11
          && memcmp(arr_buf, "hello there", 11) == 0,
          "and reads it back, no more than it holds");
    CHECK(ramfs_lookup(fs, "/hello", true, &f) && f.u_size == 11
          && f.u_ino == e.u_ino, "and its size is what was written");
    CHECK(ramfs_read(e.u_ino, 11, arr_buf, 4) == 0, "the end is the end");
    CHECK(ramfs_read(e.u_ino, 6, arr_buf, 3) == 3
          && memcmp(arr_buf, "the", 3) == 0, "a read can start anywhere");

    /* across pages, and with a hole that reads as zeroes */
    CHECK(ramfs_write(e.u_ino, 4090, "ACROSSPAGE", 10) == 10,
          "a write across a page's edge");
    CHECK(ramfs_read(e.u_ino, 4090, arr_buf, 10) == 10
          && memcmp(arr_buf, "ACROSSPAGE", 10) == 0, "reads back whole");
    CHECK(ramfs_read(e.u_ino, 11, arr_buf, 100) == 100
          && arr_buf[0] == 0 && arr_buf[99] == 0,
          "and the gap it left reads as zeroes");
    CHECK(ramfs_write(e.u_ino, 3 * 4096 + 5, "far", 3) == 3
          && ramfs_lookup(fs, "/hello", true, &f)
          && f.u_size == 3 * 4096 + 8, "a write far past the end grows it");

    /* cut, and grown again: the cut part comes back as zeroes */
    CHECK(ramfs_truncate(e.u_ino, 5) && ramfs_lookup(fs, "/hello", true, &f)
          && f.u_size == 5, "a file is cut to length");
    CHECK(ramfs_truncate(e.u_ino, 4200)
          && ramfs_read(e.u_ino, 0, arr_buf, 4200) == 4200
          && memcmp(arr_buf, "hello", 5) == 0 && arr_buf[5] == 0
          && arr_buf[4095] == 0 && arr_buf[4199] == 0,
          "and grown again reads as zeroes past the cut, not as what it "
          "held");

    {
        uint64_t u_size = 0;
        uint64_t u_room = ramfs_room();
        CHECK(ramfs_size(e.u_ino, &u_size) && u_size == 4200,
              "its size can be asked by its number");
        CHECK(make_file(fs, "/counted", &f) && ramfs_room() == u_room - 1
              && ramfs_unlink(fs, "/counted") && ramfs_room() == u_room,
              "and every name takes one place of the room there is, and "
              "gives it back");
    }

    /* directories, and names in them */
    CHECK(make_dir(fs, "/etc") && make_dir(fs, "/etc/deep"),
          "directories are made, one in another");
    CHECK(make_file(fs, "/etc/deep/conf", &f), "and a file in the deepest");
    CHECK(!make_file(fs, "/nothing/conf", &f),
          "but not in a directory that is not there");
    CHECK(!make_file(fs, "/hello/conf", &f), "nor in a file");
    CHECK(!make_file(fs, "/etc/..", &f) && !make_file(fs, "/etc/.", &f),
          "and . and .. are not names to make");
    CHECK(ramfs_lookup(fs, "/etc/deep/../deep/./conf", true, &f)
          && S_ISREG(f.u_mode), ". and .. are walked");
    CHECK(ramfs_lookup(fs, "/../../etc", true, &f) && S_ISDIR(f.u_mode),
          "and .. at the root stays there");
    CHECK(ramfs_lookup(fs, "//etc//deep/", true, &f)
          && strcmp(f.arr_name, "deep") == 0,
          "doubled and trailing slashes change nothing");
    CHECK(make_file(fs, "/etc/a", &f) && make_file(fs, "/etc/b", &f),
          "two more");
    CHECK(count_in(fs, "/etc") == 3 && ramfs_readdir(fs, "/etc", 0, &f)
          && strcmp(f.arr_name, "deep") == 0
          && ramfs_readdir(fs, "/etc", 2, &f) && strcmp(f.arr_name, "b") == 0,
          "a directory lists in the order its names were made");

    /* links */
    CHECK(ramfs_make(fs, "/etc/rel", S_IFLNK | 0777, 0, 0, 0, "deep/conf",
                     &f) && f.u_size == 9, "a link is made, its size its "
          "target's length");
    CHECK(ramfs_lookup(fs, "/etc/rel", true, &f) && S_ISREG(f.u_mode),
          "a relative one leads from its own directory");
    CHECK(ramfs_lookup(fs, "/etc/rel", false, &f) && S_ISLNK(f.u_mode),
          "and is itself when not followed");
    CHECK(ramfs_readlink(fs, "/etc/rel", arr_buf, sizeof arr_buf)
          && strcmp(arr_buf, "deep/conf") == 0, "and says where it points");
    CHECK(ramfs_make(fs, "/up", S_IFLNK | 0777, 0, 0, 0, "/etc/deep", NULL)
          && ramfs_lookup(fs, "/up/conf", true, &f) && S_ISREG(f.u_mode),
          "an absolute one leads from the root, followed on the way even "
          "when the end is not");
    CHECK(ramfs_make(fs, "/loop", S_IFLNK | 0777, 0, 0, 0, "/loop", NULL)
          && !ramfs_lookup(fs, "/loop", true, &f)
          && ramfs_lookup(fs, "/loop", false, &f),
          "a link to itself leads nowhere, after a few tries");
    CHECK(!ramfs_readlink(fs, "/hello", arr_buf, sizeof arr_buf),
          "and a file is no link");

    /* devices are names with numbers */
    CHECK(ramfs_make(fs, "/null", S_IFCHR | 0666, 0, 0, 1 << 8 | 3, NULL, &f)
          && ramfs_lookup(fs, "/null", true, &f) && S_ISCHR(f.u_mode)
          && f.u_rdev == (1 << 8 | 3), "a device node keeps its number");

    /* permissions and owners */
    CHECK(ramfs_chmod(fs, "/hello", 0600) && ramfs_lookup(fs, "/hello", true,
                                                          &f)
          && f.u_mode == (S_IFREG | 0600), "chmod changes the permissions "
          "and not the type");
    CHECK(ramfs_chown(fs, "/hello", 1000, 1000)
          && ramfs_lookup(fs, "/hello", true, &f) && f.u_uid == 1000
          && f.u_gid == 1000, "and chown the owner");

    /* removing */
    CHECK(!ramfs_rmdir(fs, "/etc"), "a directory with things in it stays");
    CHECK(!ramfs_unlink(fs, "/etc/deep"), "and a directory is not unlinked");
    CHECK(!ramfs_rmdir(fs, "/etc/a"), "nor a file rmdir'd");
    CHECK(!ramfs_rmdir(fs, "/") && !ramfs_unlink(fs, "/"),
          "and the root goes nowhere");
    CHECK(ramfs_unlink(fs, "/etc/rel") && ramfs_lookup(fs, "/etc/deep/conf",
                                                       true, &f),
          "unlinking a link leaves what it pointed to");
    CHECK(ramfs_unlink(fs, "/etc/deep/conf") && ramfs_rmdir(fs, "/etc/deep")
          && !ramfs_lookup(fs, "/etc/deep", true, &f),
          "an emptied directory goes");

    /* renaming */
    CHECK(ramfs_rename(fs, "/etc/a", "/moved") && !ramfs_lookup(fs, "/etc/a",
                                                                 true, &f)
          && ramfs_lookup(fs, "/moved", true, &f), "a rename moves a name");
    CHECK(ramfs_rename(fs, "/moved", "/etc/b") && count_in(fs, "/etc") == 1,
          "and over a file of the same kind, which goes");
    CHECK(make_dir(fs, "/d1") && make_dir(fs, "/d1/d2")
          && !ramfs_rename(fs, "/d1", "/d1/d2/d3"),
          "a directory is never moved into itself");
    CHECK(!ramfs_rename(fs, "/etc/b", "/d1"),
          "nor a file over a directory");
    CHECK(make_dir(fs, "/d3") && ramfs_rename(fs, "/d3", "/d1/d2")
          && ramfs_lookup(fs, "/d1/d2", true, &f),
          "but a directory over an empty one is");
    CHECK(make_file(fs, "/d1/d2/x", &f) && make_dir(fs, "/d4")
          && !ramfs_rename(fs, "/d4", "/d1/d2"),
          "and not over one with something in it");

    /* a file unlinked while open is still there for whoever has it open */
    {
        struct ramfs_entry kept;
        CHECK(make_file(fs, "/open", &kept)
              && ramfs_write(kept.u_ino, 0, "still here", 10) == 10,
              "a file is written");
        ramfs_hold(kept.u_ino);
        CHECK(ramfs_unlink(fs, "/open") && !ramfs_lookup(fs, "/open", true,
                                                         &f),
              "and unlinked while open, so its name is gone");
        CHECK(ramfs_read(kept.u_ino, 0, arr_buf, 10) == 10
              && memcmp(arr_buf, "still here", 10) == 0,
              "but its bytes are not, to whoever has it open");
        CHECK(ramfs_busy(fs) && !ramfs_free(fs),
              "and the filesystem cannot go while it is open");
        ramfs_let_go(kept.u_ino);
        CHECK(ramfs_read(kept.u_ino, 0, arr_buf, 10) == -EINVAL,
              "after the last close it is gone too");
        CHECK(!ramfs_busy(fs), "and nothing is open");
    }

    /* a number outlives its file and finds nothing, not the next one there */
    {
        struct ramfs_entry old;
        struct ramfs_entry young;
        CHECK(make_file(fs, "/brief", &old) && ramfs_unlink(fs, "/brief"),
              "a file comes and goes");
        bool b_made = true;
        for (int i_at = 0; i_at < 4 && b_made; i_at++) {
            char arr_name[16];
            snprintf(arr_name, sizeof arr_name, "/n%d", i_at);
            b_made = make_file(fs, arr_name, &young);
        }
        CHECK(b_made && ramfs_read(old.u_ino, 0, arr_buf, 1) == -EINVAL
              && ramfs_write(old.u_ino, 0, "x", 1) == -EINVAL
              && !ramfs_truncate(old.u_ino, 0),
              "and its number reads, writes and cuts nothing afterwards, "
              "whatever has been made since in its place");
    }

    /* two of them are two of them */
    {
        int i_other = ramfs_new(0700);
        CHECK(i_other >= 0 && (size_t)i_other != fs, "a second filesystem");
        size_t other = (size_t)i_other;
        CHECK(!ramfs_lookup(other, "/hello", true, &f),
              "holds none of the first's names");
        CHECK(make_file(other, "/hello", &f) && f.u_ino != e.u_ino,
              "and a file of the same name is another file");
        uint64_t u_bytes = 0;
        uint64_t u_nodes = 0;
        ramfs_usage(other, &u_bytes, &u_nodes);
        CHECK(u_nodes == 2 && u_bytes == 0, "it counts its own names, the "
              "root and one, and no bytes");
        CHECK(ramfs_free(other) && !ramfs_lookup(other, "/", true, &f),
              "and goes when asked, with nothing open on it");
        CHECK(ramfs_lookup(fs, "/hello", true, &f),
              "leaving the first as it was");
    }

    /*
     * what matters most: for any run of writes and cuts, a file reads as
     * a plain array of bytes would. a shadow is kept and every read
     * checked against it
     */
    {
        static uint8_t s_arr_shadow[64 * 1024];
        static uint8_t s_arr_got[64 * 1024];
        struct ramfs_entry r;
        uint64_t u_size = 0;
        unsigned u_seed = 12345;
        #define NEXT() (u_seed = u_seed * 1103515245u + 12345u, \
                        (u_seed >> 16) & 0x7fff)

        CHECK(make_file(fs, "/random", &r), "a file for the random run");
        bool b_ok = true;
        for (int i_round = 0; i_round < 20000 && b_ok; i_round++) {
            uint64_t u_at = NEXT() % (sizeof s_arr_shadow - 9000);
            uint64_t u_len = NEXT() % 9000;
            unsigned u_what = NEXT() % 8;
            if (u_what < 4) {
                for (uint64_t u_i = 0; u_i < u_len; u_i++) {
                    s_arr_got[u_i] = (uint8_t)(i_round + u_i * 13);
                }
                b_ok = ramfs_write(r.u_ino, u_at, s_arr_got, u_len)
                       == (int64_t)u_len;
                memcpy(s_arr_shadow + u_at, s_arr_got, u_len);
                if (u_at + u_len > u_size) {
                    if (u_at > u_size) {
                        memset(s_arr_shadow + u_size, 0, u_at - u_size);
                    }
                    u_size = u_at + u_len;
                }
            } else if (u_what == 4) {
                uint64_t u_to = NEXT() % sizeof s_arr_shadow;
                b_ok = ramfs_truncate(r.u_ino, u_to);
                if (u_to > u_size) {
                    memset(s_arr_shadow + u_size, 0, u_to - u_size);
                }
                u_size = u_to;
            } else {
                uint64_t u_want = (u_at < u_size)
                                  ? ((u_len < u_size - u_at) ? u_len
                                                             : u_size - u_at)
                                  : 0;
                b_ok = ramfs_read(r.u_ino, u_at, s_arr_got, u_len)
                       == (int64_t)u_want
                       && memcmp(s_arr_got, s_arr_shadow + u_at, u_want) == 0;
            }
        }
        CHECK(b_ok, "twenty thousand random writes, cuts and reads agree "
              "with a plain array of bytes");
        CHECK(ramfs_lookup(fs, "/random", true, &f) && f.u_size == u_size,
              "and the size it ends at is the array's");
        #undef NEXT
    }

    CHECK(!ramfs_free(RAMFS_MAX) && ramfs_new(0755) >= 0,
          "a number that is no filesystem frees nothing, and there is room "
          "for more");

    if (failures == 0) printf("all good\n");
    return failures;
}
