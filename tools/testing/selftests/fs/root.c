// SPDX-License-Identifier: GPL-2.0-only
/*
 * tools/testing/selftests/fs/root.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the root: the disk /init moved into, the archive under it, a mount
 * moved with what stands on it, a root of a process's own, and the root
 * exchanged for another and back. this test is installed so only root
 * may run it.
 */

#include "../kselftest.h"

#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>

#define GUEST_UID   1000
#define RAMFS_TYPE  0x858458f6
#define EXT4_TYPE   0xef53

#define AT_A        "/tmp/root-a"
#define AT_C        "/tmp/root-c"
#define AT_NEW      "/tmp/root-new"

static int pivot(const char *str_new, const char *str_old)
{
    return (int)syscall(SYS_pivot_root, str_new, str_old);
}

static bool is_type(const char *str_path, uint32_t u_type)
{
    struct statfs sf;

    return statfs(str_path, &sf) == 0 && (uint32_t)sf.f_type == u_type;
}

static bool exited_ok(pid_t i_child)
{
    int i_status = -1;

    waitpid(i_child, &i_status, 0);
    return ksft_exited(i_status, 0);
}

static bool guest_refused(void)
{
    pid_t i_child = fork();
    if (i_child == 0) {
        bool b_guest = setgid(GUEST_UID) == 0 && setuid(GUEST_UID) == 0;
        bool b_pivot = pivot("/", "/boot") == -1 && errno == EPERM;
        _exit(b_guest && b_pivot && chroot("/boot") == -1 && errno == EPERM
              ? 0 : 1);
    }
    return exited_ok(i_child);
}

/* a mount moved, with one on it, and a process standing in that one */
static bool move_takes_along(void)
{
    char arr_cwd[64];

    bool b_set = mkdir(AT_A, 0755) == 0 && mkdir(AT_C, 0755) == 0
                 && mount("none", AT_A, "ramfs", 0, NULL) == 0
                 && mkdir(AT_A "/b", 0755) == 0
                 && mount("none", AT_A "/b", "ramfs", 0, NULL) == 0
                 && close(open(AT_A "/b/kept", O_CREAT | O_WRONLY, 0644)) == 0
                 && chdir(AT_A "/b") == 0;
    bool b_moved = b_set
                   && mount(AT_A, AT_C, NULL, MS_MOVE, NULL) == 0
                   && access(AT_C "/b/kept", F_OK) == 0
                   && access(AT_A "/b", F_OK) == -1
                   && getcwd(arr_cwd, sizeof arr_cwd) != NULL
                   && ksft_streq(arr_cwd, AT_C "/b");
    chdir("/");
    bool b_down = umount2(AT_C "/b", 0) == 0 && umount2(AT_C, 0) == 0;
    rmdir(AT_A);
    rmdir(AT_C);
    return b_moved && b_down;
}

/* the names a process rooted in the archive has, and the ones it has not */
static bool rooted_in_archive(void)
{
    pid_t i_child = fork();
    if (i_child == 0) {
        char arr_cwd[64];
        bool b_in = chroot("/boot") == 0 && chdir("/") == 0
                    && access("/init", X_OK) == 0
                    && access("/../../init", X_OK) == 0
                    && access("/boot/init", F_OK) == -1
                    && access("/tmp", F_OK) == -1
                    && getcwd(arr_cwd, sizeof arr_cwd) != NULL
                    && ksft_streq(arr_cwd, "/");
        /* and its children are rooted where it is */
        pid_t i_grandchild = fork();
        if (i_grandchild == 0) {
            _exit(access("/init", X_OK) == 0 && access("/tmp", F_OK) == -1
                  ? 0 : 1);
        }
        _exit(b_in && exited_ok(i_grandchild) ? 0 : 1);
    }
    return exited_ok(i_child);
}

int main(void)
{
    ksft_print_header();
    ksft_set_plan(10);

    ksft_test_result(is_type("/", EXT4_TYPE),
                     "the root is the disk /init moved into\n");
    ksft_test_result(is_type("/boot", RAMFS_TYPE)
                     && access("/boot/init", X_OK) == 0,
                     "with the archive the kernel booted into under it, "
                     "in memory\n");
    ksft_test_result(guest_refused(), "a guest may neither exchange the "
                     "root nor take one of its own\n");

    ksft_test_result(pivot("/tmp", "/tmp") == -1 && errno == EINVAL
                     && pivot("/boot", "/tmp") == -1 && errno == EINVAL,
                     "pivot_root takes a mount point for the new root, "
                     "and somewhere under it for the old\n");
    ksft_test_result(mount("/tmp", "/proc", NULL, MS_MOVE, NULL) == -1
                     && errno == EINVAL,
                     "and a move takes a mount point to move\n");

    ksft_test_result(move_takes_along(), "a mount moved takes the mount on "
                     "it along, and the process standing in that\n");
    ksft_test_result(rooted_in_archive(), "a process rooted in the archive "
                     "sees the archive as /, cannot climb out of it, and "
                     "its children are rooted there too\n");

    /* the whole root exchanged for one in memory, and back */
    bool b_made = mkdir(AT_NEW, 0755) == 0
                  && mount("none", AT_NEW, "ramfs", 0, NULL) == 0
                  && mkdir(AT_NEW "/old", 0755) == 0;
    bool b_there = b_made && pivot(AT_NEW, AT_NEW "/old") == 0
                   && is_type("/", RAMFS_TYPE)
                   && is_type("/old", EXT4_TYPE)
                   && access("/old/boot/init", X_OK) == 0
                   && access("/old/proc/version", R_OK) == 0;
    ksft_test_result(b_there, "the root exchanged for a filesystem in "
                     "memory has the old one under it, with everything "
                     "that was mounted on it\n");
    bool b_back = b_there && pivot("/old", "/old" AT_NEW) == 0
                  && is_type("/", EXT4_TYPE)
                  && access("/boot/init", X_OK) == 0
                  && access("/proc/version", R_OK) == 0;
    ksft_test_result(b_back, "and exchanged back, everything is where it "
                     "was\n");
    ksft_test_result(umount2(AT_NEW, 0) == 0 && rmdir(AT_NEW) == 0,
                     "with the one in memory taken down again\n");
    ksft_finished();
}
