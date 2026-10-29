// SPDX-License-Identifier: GPL-2.0-only
/*
 * tools/testing/selftests/fs/mknod.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * mknod: a node naming a device by its number, on the disk and in
 * memory, which opens as that device wherever it is. this test is
 * installed so only root may run it.
 */

#include "../kselftest.h"

#include <dirent.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

#define SECTOR      512
#define GUEST_UID   1000

#define AT_NULL     "/tmp/mknod-null"
#define AT_SDA      "/tmp/mknod-sda"
#define AT_NONE     "/tmp/mknod-none"
#define AT_FILE     "/tmp/mknod-file"
#define AT_RAM      "/tmp/mknod-ram"

static bool listed_as(const char *str_dir, const char *str_name,
                      unsigned char u_type)
{
    bool b_found = false;
    DIR *p_dir = opendir(str_dir);

    for (struct dirent *p_ent; p_dir != NULL
                               && (p_ent = readdir(p_dir)) != NULL; ) {
        b_found |= ksft_streq(p_ent->d_name, str_name)
                   && p_ent->d_type == u_type;
    }
    if (p_dir != NULL) {
        closedir(p_dir);
    }
    return b_found;
}

/* a guest may make a file with mknod, and not a device */
static bool guest_limited(void)
{
    pid_t i_child = fork();
    if (i_child == 0) {
        bool b_guest = setgid(GUEST_UID) == 0 && setuid(GUEST_UID) == 0;
        bool b_device = mknod("/tmp/mknod-guest", S_IFCHR | 0666,
                              makedev(1, 3)) == -1 && errno == EPERM;
        bool b_file = mknod("/tmp/mknod-guest", S_IFREG | 0644, 0) == 0
                      && unlink("/tmp/mknod-guest") == 0;
        _exit(b_guest && b_device && b_file ? 0 : 1);
    }
    int i_status = -1;
    waitpid(i_child, &i_status, 0);
    return ksft_exited(i_status, 0);
}

/* the same first sector, through the node and through /dev */
static bool same_sector(const char *str_a, const char *str_b)
{
    char arr_a[SECTOR];
    char arr_b[SECTOR];
    int i_a = open(str_a, O_RDONLY);
    int i_b = open(str_b, O_RDONLY);
    bool b_same = i_a >= 0 && i_b >= 0
                  && read(i_a, arr_a, SECTOR) == SECTOR
                  && read(i_b, arr_b, SECTOR) == SECTOR
                  && memcmp(arr_a, arr_b, SECTOR) == 0;
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
    struct stat st;
    char arr_buf[8];

    ksft_print_header();
    ksft_set_plan(10);
    umask(0);

    ksft_test_result(mknod(AT_NULL, S_IFCHR | 0666, makedev(1, 3)) == 0
                     && stat(AT_NULL, &st) == 0 && S_ISCHR(st.st_mode)
                     && major(st.st_rdev) == 1 && minor(st.st_rdev) == 3
                     && (st.st_mode & 07777) == 0666,
                     "a character device is made on the disk, with its "
                     "number\n");
    ksft_test_result(listed_as("/tmp", "mknod-null", DT_CHR),
                     "and listed as one\n");

    int i_fd = open(AT_NULL, O_RDWR);
    ksft_test_result(i_fd >= 0 && write(i_fd, "gone", 4) == 4
                     && read(i_fd, arr_buf, sizeof arr_buf) == 0,
                     "it opens as the device it names, wherever it is\n");
    if (i_fd >= 0) {
        close(i_fd);
    }

    ksft_test_result(mknod(AT_SDA, S_IFBLK | 0600, makedev(8, 0)) == 0
                     && stat(AT_SDA, &st) == 0 && S_ISBLK(st.st_mode)
                     && same_sector(AT_SDA, "/dev/sda"),
                     "a node for the first drive reads what /dev/sda does\n");

    ksft_test_result(mknod(AT_NONE, S_IFCHR | 0600, makedev(99, 99)) == 0
                     && open(AT_NONE, O_RDONLY) == -1 && errno == ENXIO,
                     "one whose number nothing answers to opens as "
                     "nothing\n");

    ksft_test_result(mknod(AT_FILE, 0644, 0) == 0
                     && stat(AT_FILE, &st) == 0 && S_ISREG(st.st_mode)
                     && st.st_size == 0,
                     "with no type, it makes an empty file\n");

    ksft_test_result(mknod(AT_NULL, S_IFCHR | 0666, makedev(1, 3)) == -1
                     && errno == EEXIST
                     && mknod("/tmp/mknod-fifo", S_IFIFO | 0644, 0) == -1
                     && errno == EPERM
                     && mknod("/tmp/mknod-dir", S_IFDIR | 0755, 0) == -1
                     && errno == EPERM,
                     "a name taken is not made again, and a fifo and a "
                     "directory are not made at all\n");

    bool b_ram = mkdir(AT_RAM, 0755) == 0
                 && mount("none", AT_RAM, "ramfs", 0, NULL) == 0
                 && mknod(AT_RAM "/zero", S_IFCHR | 0666, makedev(1, 5)) == 0;
    i_fd = b_ram ? open(AT_RAM "/zero", O_RDONLY) : -1;
    memset(arr_buf, 7, sizeof arr_buf);
    ksft_test_result(i_fd >= 0
                     && read(i_fd, arr_buf, sizeof arr_buf)
                        == (ssize_t)sizeof arr_buf
                     && arr_buf[0] == 0 && arr_buf[7] == 0,
                     "one made in memory opens as its device too\n");
    if (i_fd >= 0) {
        close(i_fd);
    }
    ksft_test_result(umount2(AT_RAM, 0) == 0 && rmdir(AT_RAM) == 0,
                     "and goes with the filesystem it was on\n");

    ksft_test_result(guest_limited(), "a guest may make a file with it, "
                     "and not a device\n");

    unlink(AT_NULL);
    unlink(AT_SDA);
    unlink(AT_NONE);
    unlink(AT_FILE);
    ksft_finished();
}
