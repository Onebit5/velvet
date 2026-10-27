// SPDX-License-Identifier: GPL-2.0-only
/*
 * tools/testing/selftests/fs/blockdev.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the drives and their partitions in /dev: read and written by the byte,
 * through what a filesystem on them goes through, and root's. this test
 * is installed so only root may run it.
 */

#include "../kselftest.h"

#include <dirent.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

#define SECTOR      512
#define GUEST_UID   1000
#define EXT4_MAGIC  0xef53

/*
 * a gap nothing uses: a gpt's entries end at sector 33 and its first
 * partition starts well after, so a few bytes written here and put back
 * disturb nothing
 */
#define GAP_AT      (40 * SECTOR + 400)
#define GAP_LEN     300

static char s_arr_drive[16];
static char s_arr_part[sizeof s_arr_drive + 2];
static uint64_t s_u_first;

static uint64_t le64(const uint8_t *p_at)
{
    uint64_t u_value = 0;

    for (int i_at = 7; i_at >= 0; i_at--) {
        u_value = u_value << 8 | p_at[i_at];
    }
    return u_value;
}

/*
 * the drive with a gpt on it, which is the one the image's files are on,
 * and where its first partition starts: true and both, or false
 */
static bool find_gpt(void)
{
    uint8_t arr_sector[SECTOR];

    for (char c_letter = 'a'; c_letter <= 'h'; c_letter++) {
        snprintf(s_arr_drive, sizeof s_arr_drive, "/dev/sd%c", c_letter);
        int i_fd = open(s_arr_drive, O_RDONLY);
        if (i_fd < 0) {
            continue;
        }
        bool b_gpt = pread(i_fd, arr_sector, SECTOR, SECTOR) == SECTOR
                     && memcmp(arr_sector, "EFI PART", 8) == 0;
        uint64_t u_entries = b_gpt ? le64(arr_sector + 72) : 0;
        b_gpt = b_gpt && pread(i_fd, arr_sector, SECTOR,
                               (off_t)(u_entries * SECTOR)) == SECTOR;
        close(i_fd);
        if (b_gpt) {
            s_u_first = le64(arr_sector + 32);
            snprintf(s_arr_part, sizeof s_arr_part, "%s1", s_arr_drive);
            return true;
        }
    }
    return false;
}

static bool listed_as_block(const char *str_name)
{
    bool b_found = false;
    DIR *p_dir = opendir("/dev");

    for (struct dirent *p_ent; p_dir != NULL
                               && (p_ent = readdir(p_dir)) != NULL; ) {
        b_found |= ksft_streq(p_ent->d_name, str_name)
                   && p_ent->d_type == DT_BLK;
    }
    if (p_dir != NULL) {
        closedir(p_dir);
    }
    return b_found;
}

/*
 * the gap read, written over, read back through a second descriptor, and
 * put back as it was
 */
static bool write_reads_back(void)
{
    char arr_was[GAP_LEN];
    char arr_new[GAP_LEN];
    char arr_got[GAP_LEN];

    for (size_t sz_at = 0; sz_at < GAP_LEN; sz_at++) {
        arr_new[sz_at] = (char)(sz_at * 7 + 3);
    }
    int i_fd = open(s_arr_drive, O_RDWR);
    int i_other = open(s_arr_drive, O_RDONLY);
    bool b_ok = i_fd >= 0 && i_other >= 0
                && pread(i_fd, arr_was, GAP_LEN, GAP_AT) == GAP_LEN
                && pwrite(i_fd, arr_new, GAP_LEN, GAP_AT) == GAP_LEN
                && pread(i_other, arr_got, GAP_LEN, GAP_AT) == GAP_LEN
                && memcmp(arr_got, arr_new, GAP_LEN) == 0;
    bool b_back = i_fd >= 0
                  && pwrite(i_fd, arr_was, GAP_LEN, GAP_AT) == GAP_LEN
                  && fsync(i_fd) == 0;
    if (i_fd >= 0) {
        close(i_fd);
    }
    if (i_other >= 0) {
        close(i_other);
    }
    return b_ok && b_back;
}

int main(void)
{
    struct stat st;
    uint8_t arr_a[4096];
    uint8_t arr_b[4096];

    ksft_print_header();
    ksft_set_plan(10);

    ksft_test_result(stat("/dev/sda", &st) == 0 && S_ISBLK(st.st_mode)
                     && major(st.st_rdev) == 8 && minor(st.st_rdev) == 0
                     && (st.st_mode & 07777) == 0600 && st.st_uid == 0,
                     "a drive is a block device with its number, and "
                     "root's\n");

    bool b_gpt = find_gpt();
    ksft_test_result(b_gpt, "the data drive's table is where gpt puts it, "
                     "on %s\n", s_arr_drive);
    ksft_test_result(b_gpt && listed_as_block(s_arr_drive + 5)
                     && listed_as_block(s_arr_part + 5),
                     "/dev lists the drive and its partitions\n");

    int i_fd = open(s_arr_drive, O_RDONLY);
    uint64_t u_bytes = 0;
    unsigned long u_sectors = 0;
    int i_sector = 0;
    bool b_size = i_fd >= 0
                  && ioctl(i_fd, BLKGETSIZE64, &u_bytes) == 0
                  && ioctl(i_fd, BLKGETSIZE, &u_sectors) == 0
                  && ioctl(i_fd, BLKSSZGET, &i_sector) == 0;
    ksft_test_result(b_size && u_bytes > 0 && u_bytes == u_sectors * SECTOR
                     && i_sector == SECTOR
                     && lseek(i_fd, 0, SEEK_END) == (off_t)u_bytes,
                     "it says how big it is, three ways that agree\n");

    int i_part = open(s_arr_part, O_RDONLY);
    ksft_test_result(i_fd >= 0 && i_part >= 0
                     && pread(i_part, arr_a, sizeof arr_a, 0)
                        == (ssize_t)sizeof arr_a
                     && pread(i_fd, arr_b, sizeof arr_b,
                              (off_t)(s_u_first * SECTOR))
                        == (ssize_t)sizeof arr_b
                     && memcmp(arr_a, arr_b, sizeof arr_a) == 0,
                     "a partition is the drive's bytes from where the "
                     "table says it starts\n");
    ksft_test_result((arr_a[1024 + 56] | arr_a[1024 + 57] << 8)
                     == EXT4_MAGIC,
                     "and holds the filesystem the table says it does\n");

    ksft_test_result(i_part >= 0
                     && pread(i_part, arr_b, 700, 300) == 700
                     && memcmp(arr_b, arr_a + 300, 700) == 0,
                     "a read need not start or end on a sector\n");

    ksft_test_result(b_gpt && write_reads_back(),
                     "a write across a sector's edge lands, and reads back "
                     "through another descriptor\n");

    char c_byte = 0;
    int i_rw = open(s_arr_drive, O_RDWR);
    ksft_test_result(i_rw >= 0
                     && pread(i_rw, &c_byte, 1, (off_t)u_bytes) == 0
                     && pwrite(i_rw, &c_byte, 1, (off_t)u_bytes) == -1
                     && errno == ENOSPC,
                     "the end is the end: nothing to read there, and no "
                     "room to write\n");

    pid_t i_child = fork();
    if (i_child == 0) {
        _exit(setgid(GUEST_UID) == 0 && setuid(GUEST_UID) == 0
              && open("/dev/sda", O_RDONLY) == -1 && errno == EACCES
              ? 0 : 1);
    }
    int i_status = -1;
    waitpid(i_child, &i_status, 0);
    ksft_test_result(ksft_exited(i_status, 0),
                     "and a guest may not open one\n");

    ksft_finished();
}
