// SPDX-License-Identifier: GPL-2.0-only
/*
 * tools/testing/selftests/fs/install.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the medium the machine booted from, copied whole onto a blank drive by
 * the byte, which is all an installer's first half is. that the copy
 * boots on its own is the boot test's to show, by booting it. this test
 * is installed so only root may run it.
 */

#include "../kselftest.h"

#include <sys/ioctl.h>
#include <sys/mount.h>

#define SECTOR      512
#define TABLE_LBA   32                      /* where the boot table is */
#define BOOT_MAGIC  0x50484c4d4e30ull
#define CHUNK       (64 * 1024)
#define BLANK_LOOK  (64 * SECTOR)           /* how much of it must be empty */

static char s_arr_from[16];
static char s_arr_to[16];
static uint64_t s_u_from_bytes;

static uint64_t size_of(int i_fd)
{
    uint64_t u_bytes = 0;

    return (ioctl(i_fd, BLKGETSIZE64, &u_bytes) == 0) ? u_bytes : 0;
}

static bool is_medium(int i_fd)
{
    uint64_t u_magic = 0;

    return pread(i_fd, &u_magic, sizeof u_magic, TABLE_LBA * SECTOR)
           == (ssize_t)sizeof u_magic && u_magic == BOOT_MAGIC;
}

static bool is_blank(int i_fd)
{
    static uint8_t s_arr_look[BLANK_LOOK];

    if (pread(i_fd, s_arr_look, sizeof s_arr_look, 0)
        != (ssize_t)sizeof s_arr_look) {
        return false;
    }
    for (size_t sz_at = 0; sz_at < sizeof s_arr_look; sz_at++) {
        if (s_arr_look[sz_at] != 0) {
            return false;
        }
    }
    return true;
}

/* the drive the machine came from, and a blank one at least as big */
static bool find_drives(void)
{
    char arr_dev[16];

    s_arr_from[0] = '\0';
    s_arr_to[0] = '\0';
    for (char c_letter = 'a'; c_letter <= 'h'; c_letter++) {
        snprintf(arr_dev, sizeof arr_dev, "/dev/sd%c", c_letter);
        int i_fd = open(arr_dev, O_RDONLY);
        if (i_fd < 0) {
            continue;
        }
        if (s_arr_from[0] == '\0' && is_medium(i_fd)) {
            strcpy(s_arr_from, arr_dev);
            s_u_from_bytes = size_of(i_fd);
        } else if (s_arr_to[0] == '\0' && is_blank(i_fd)) {
            strcpy(s_arr_to, arr_dev);
        }
        close(i_fd);
    }
    return s_arr_from[0] != '\0' && s_arr_to[0] != '\0';
}

/* every byte of the medium onto the blank drive, and out to it */
static bool copy(void)
{
    static uint8_t s_arr_chunk[CHUNK];
    int i_from = open(s_arr_from, O_RDONLY);
    int i_to = open(s_arr_to, O_WRONLY);
    bool b_ok = i_from >= 0 && i_to >= 0 && size_of(i_to) >= s_u_from_bytes;

    for (uint64_t u_done = 0; b_ok && u_done < s_u_from_bytes; ) {
        size_t sz_want = (s_u_from_bytes - u_done < CHUNK)
                         ? (size_t)(s_u_from_bytes - u_done) : CHUNK;
        ssize_t i_got = read(i_from, s_arr_chunk, sz_want);
        b_ok = i_got == (ssize_t)sz_want
               && write(i_to, s_arr_chunk, sz_want) == (ssize_t)sz_want;
        u_done += sz_want;
    }
    b_ok = b_ok && fsync(i_to) == 0;
    if (i_from >= 0) {
        close(i_from);
    }
    if (i_to >= 0) {
        b_ok = close(i_to) == 0 && b_ok;
    }
    return b_ok;
}

/* the same bytes at an offset of both, through descriptors opened afresh */
static bool same_at(uint64_t u_at, size_t sz_len)
{
    static uint8_t s_arr_a[CHUNK];
    static uint8_t s_arr_b[CHUNK];
    int i_a = open(s_arr_from, O_RDONLY);
    int i_b = open(s_arr_to, O_RDONLY);
    bool b_same = i_a >= 0 && i_b >= 0 && sz_len <= CHUNK
                  && pread(i_a, s_arr_a, sz_len, (off_t)u_at)
                     == (ssize_t)sz_len
                  && pread(i_b, s_arr_b, sz_len, (off_t)u_at)
                     == (ssize_t)sz_len
                  && memcmp(s_arr_a, s_arr_b, sz_len) == 0;
    if (i_a >= 0) {
        close(i_a);
    }
    if (i_b >= 0) {
        close(i_b);
    }
    return b_same;
}

int main(void)
{
    ksft_print_header();
    ksft_set_plan(4);

    bool b_found = find_drives();
    ksft_test_result(b_found && s_u_from_bytes > 0,
                     "the medium is %s, by its boot table, and %s is "
                     "blank\n", s_arr_from, s_arr_to);
    ksft_test_result(b_found && copy(), "every byte of it is copied onto "
                     "the blank drive, and out to it\n");

    int i_to = b_found ? open(s_arr_to, O_RDONLY) : -1;
    ksft_test_result(i_to >= 0 && is_medium(i_to),
                     "which carries the boot table now\n");
    if (i_to >= 0) {
        close(i_to);
    }
    uint64_t u_tail = (s_u_from_bytes > CHUNK) ? s_u_from_bytes - CHUNK : 0;
    ksft_test_result(b_found && same_at(0, CHUNK)
                     && same_at(TABLE_LBA * SECTOR, SECTOR)
                     && same_at(s_u_from_bytes / 2, CHUNK)
                     && same_at(u_tail, (size_t)(s_u_from_bytes - u_tail)),
                     "and reads as the medium does, at its start, its "
                     "middle and its end\n");
    ksft_finished();
}
