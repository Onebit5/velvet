// SPDX-License-Identifier: GPL-2.0-only
/*
 * tools/testing/selftests/fs/mount.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * mount, umount2 and statfs: a filesystem in memory hung over a
 * directory and taken down again, /proc in a second place, and the
 * partitions of the drive the files are on. this test is installed so
 * only root may run it.
 */

#include "../kselftest.h"

#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statfs.h>

#define SECTOR      512
#define GUEST_UID   1000
#define TEXT_MAX    4096
#define RAMFS_TYPE  0x858458f6
#define EXT4_TYPE   0xef53
#define MSDOS_TYPE  0x4d44

#define AT_RAM      "/tmp/mount-ram"
#define AT_PROC     "/tmp/mount-proc"
#define AT_PART     "/tmp/mount-part"

static char s_arr_drive[16];

/* the drive with a gpt on it, which is the one the image's files are on */
static bool find_gpt(void)
{
    char arr_sector[SECTOR];

    for (char c_letter = 'a'; c_letter <= 'h'; c_letter++) {
        snprintf(s_arr_drive, sizeof s_arr_drive, "/dev/sd%c", c_letter);
        int i_fd = open(s_arr_drive, O_RDONLY);
        if (i_fd < 0) {
            continue;
        }
        bool b_gpt = pread(i_fd, arr_sector, SECTOR, SECTOR) == SECTOR
                     && memcmp(arr_sector, "EFI PART", 8) == 0;
        close(i_fd);
        if (b_gpt) {
            return true;
        }
    }
    return false;
}

/* partition n of that drive, as /dev names it */
static const char *part(int i_n)
{
    static char s_arr_part[sizeof s_arr_drive + 4];

    snprintf(s_arr_part, sizeof s_arr_part, "%s%d", s_arr_drive, i_n);
    return s_arr_part;
}

static bool same_file(const char *str_a, const char *str_b)
{
    static char s_arr_a[TEXT_MAX];
    static char s_arr_b[TEXT_MAX];
    int64_t i_a = ksft_read_file(str_a, s_arr_a, sizeof s_arr_a);
    int64_t i_b = ksft_read_file(str_b, s_arr_b, sizeof s_arr_b);

    return i_a > 0 && i_a == i_b && memcmp(s_arr_a, s_arr_b, (size_t)i_a) == 0;
}

static bool guest_refused(void)
{
    pid_t i_child = fork();
    if (i_child == 0) {
        bool b_mount = setgid(GUEST_UID) == 0 && setuid(GUEST_UID) == 0
                       && mount("none", AT_RAM, "ramfs", 0, NULL) == -1
                       && errno == EPERM;
        _exit(b_mount && umount2("/work", 0) == -1 && errno == EPERM ? 0 : 1);
    }
    int i_status = -1;
    waitpid(i_child, &i_status, 0);
    return ksft_exited(i_status, 0);
}

int main(void)
{
    char arr_text[TEXT_MAX];
    struct stat st;
    struct statfs sf;
    struct statfs sf_fd;

    ksft_print_header();
    ksft_set_plan(17);

    mkdir(AT_RAM, 0755);
    mkdir(AT_PROC, 0755);
    mkdir(AT_PART, 0755);
    int i_under = open(AT_RAM "/under", O_CREAT | O_WRONLY, 0644);
    if (i_under >= 0) {
        close(i_under);
    }

    ksft_test_result(guest_refused(), "a guest may neither mount nor take "
                     "a mount down\n");

    ksft_test_result(mount("none", AT_RAM, "ramfs", 0, "mode=1777") == 0,
                     "a filesystem in memory is mounted over a directory\n");
    ksft_test_result(access(AT_RAM "/under", F_OK) == -1 && errno == ENOENT,
                     "what the directory held is out of sight\n");
    ksft_test_result(stat(AT_RAM, &st) == 0 && S_ISDIR(st.st_mode)
                     && (st.st_mode & 07777) == 01777,
                     "and its root has the mode it was given\n");
    ksft_test_result(statfs(AT_RAM, &sf) == 0
                     && (uint32_t)sf.f_type == RAMFS_TYPE && sf.f_bfree > 0,
                     "statfs says what it is, and that there is room\n");

    int i_fd = open(AT_RAM "/note", O_CREAT | O_RDWR, 0600);
    bool b_rw = i_fd >= 0 && write(i_fd, "kept in memory", 14) == 14
                && pread(i_fd, arr_text, sizeof arr_text, 0) == 14
                && memcmp(arr_text, "kept in memory", 14) == 0
                && mkdir(AT_RAM "/dir", 0755) == 0;
    ksft_test_result(b_rw, "a file is made, written and read back there, "
                     "and a directory made\n");

    int64_t i_len = ksft_read_file("/proc/mounts", arr_text, sizeof arr_text);
    ksft_test_result(ksft_contains(arr_text, i_len, AT_RAM),
                     "/proc/mounts lists it\n");

    ksft_test_result(umount2(AT_RAM, 0) == -1 && errno == EBUSY,
                     "it cannot be taken down with a file open on it\n");
    if (i_fd >= 0) {
        close(i_fd);
    }
    bool b_cwd = chdir(AT_RAM "/dir") == 0 && umount2(AT_RAM, 0) == -1
                 && errno == EBUSY;
    chdir("/");
    ksft_test_result(b_cwd, "nor with a working directory inside it\n");

    ksft_test_result(umount2(AT_RAM, 0) == 0
                     && access(AT_RAM "/under", F_OK) == 0
                     && access(AT_RAM "/note", F_OK) == -1,
                     "and once let go of it comes down, and what was "
                     "under it is back\n");
    ksft_test_result(umount2(AT_RAM, 0) == -1 && errno == EINVAL,
                     "a second time, nothing is mounted there\n");

    ksft_test_result(mount("proc", AT_PROC, "proc", 0, NULL) == 0
                     && same_file(AT_PROC "/version", "/proc/version")
                     && umount2(AT_PROC, 0) == 0,
                     "/proc mounted in a second place reads the same\n");

    bool b_gpt = find_gpt();
    ksft_test_result(b_gpt && mount(part(2), AT_PART, "ext4", 0, NULL) == -1
                     && errno == EBUSY,
                     "a partition mounted already is not mounted twice\n");

    ksft_test_result(b_gpt && mount(part(3), AT_PART, "ext4", 0, NULL) == -1
                     && errno == EINVAL
                     && mount(part(3), AT_PART, "vfat", 0, NULL) == 0
                     && statfs(AT_PART, &sf) == 0
                     && (uint32_t)sf.f_type == MSDOS_TYPE
                     && access(AT_PART "/welcome.txt", F_OK) == 0
                     && umount2(AT_PART, 0) == 0,
                     "the fat one is not ext4, and mounts as what it is, "
                     "with its files\n");

    ksft_test_result(mount("none", AT_PART, "nothing", 0, NULL) == -1
                     && errno == ENODEV
                     && mount("/dev/null", AT_PART, "ext4", 0, NULL) == -1
                     && errno == ENOTBLK
                     && mount("none", "/proc/version", "ramfs", 0, NULL) == -1
                     && errno == ENOTDIR
                     && mount("none", "/tmp/nowhere", "ramfs", 0, NULL) == -1
                     && errno == ENOENT
                     && mount("none", AT_PART, "ramfs", MS_RDONLY, NULL) == -1
                     && errno == EINVAL,
                     "a type it has never heard of, a source that is no "
                     "drive, a target that is no directory or is not there, "
                     "and a flag it does not keep are refused\n");

    ksft_test_result(statfs("/", &sf) == 0 && (uint32_t)sf.f_type == EXT4_TYPE
                     && sf.f_bsize > 0 && sf.f_blocks >= sf.f_bfree
                     && sf.f_files > 0,
                     "statfs says the root is ext4, and how full\n");
    int i_root = open("/", O_RDONLY | O_DIRECTORY);
    ksft_test_result(i_root >= 0 && fstatfs(i_root, &sf_fd) == 0
                     && sf_fd.f_type == sf.f_type
                     && sf_fd.f_blocks == sf.f_blocks,
                     "and fstatfs on the same directory agrees\n");
    if (i_root >= 0) {
        close(i_root);
    }

    unlink(AT_RAM "/under");
    rmdir(AT_RAM);
    rmdir(AT_PROC);
    rmdir(AT_PART);
    ksft_finished();
}
