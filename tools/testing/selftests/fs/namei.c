// SPDX-License-Identifier: GPL-2.0-only
/*
 * tools/testing/selftests/fs/namei.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * names, from ring 3: files made, moved, linked, listed and taken away.
 */

#include "../kselftest.h"

#include <dirent.h>
#include <sys/stat.h>

/* where a guest may make things: /init makes it before any test runs */
#define TMP "/tmp"

/* root's, on the disk, readable by everyone and writable by root alone */
#define ROOTS_FILE "/welcome.txt"

static bool listed(const char *str_dir, const char *str_name)
{
    bool b_found = false;
    DIR *p_dir = opendir(str_dir);

    for (struct dirent *p_ent; p_dir != NULL
                               && (p_ent = readdir(p_dir)) != NULL; ) {
        b_found |= ksft_streq(p_ent->d_name, str_name);
    }
    if (p_dir != NULL) {
        closedir(p_dir);
    }
    return b_found;
}

static ssize_t slurp(const char *str_path, char *p_buf, size_t sz_len)
{
    int i_fd = open(str_path, O_RDONLY);
    if (i_fd < 0) {
        return -1;
    }
    ssize_t i_got = read(i_fd, p_buf, sz_len);
    close(i_fd);
    return i_got;
}

int main(void)
{
    char arr_buf[64];
    struct stat st;

    ksft_print_header();
    ksft_set_plan(16);

    umask(022);
    if (chdir(TMP) != 0) {
        ksft_print_msg("no %s to work in: %s\n", TMP, strerror(errno));
    }

    int i_fd = open("made", O_CREAT | O_EXCL | O_WRONLY, 0666);
    ksft_test_result(i_fd >= 0 && write(i_fd, "first", 5) == 5
                     && close(i_fd) == 0,
                     "O_CREAT makes a file, relative to where it stands\n");
    ksft_test_result(open("made", O_CREAT | O_EXCL | O_WRONLY, 0666) == -1
                     && errno == EEXIST,
                     "and O_EXCL will not make it twice\n");
    ksft_test_result(stat("made", &st) == 0 && S_ISREG(st.st_mode)
                     && (st.st_mode & 0777) == 0644 && st.st_size == 5
                     && st.st_uid == getuid(),
                     "it is the caller's, its mode less the umask\n");

    i_fd = open("made", O_WRONLY | O_APPEND);
    write(i_fd, "+more", 5);
    close(i_fd);
    ksft_test_result(slurp(TMP "/made", arr_buf, sizeof arr_buf) == 10
                     && memcmp(arr_buf, "first+more", 10) == 0,
                     "O_APPEND writes at the end\n");
    i_fd = open("made", O_WRONLY | O_TRUNC);
    close(i_fd);
    ksft_test_result(stat("made", &st) == 0 && st.st_size == 0
                     && truncate("made", 3) == 0 && stat("made", &st) == 0
                     && st.st_size == 3,
                     "O_TRUNC empties it, and truncate sets its length\n");

    ksft_test_result(mkdir("dir", 0755) == 0 && stat("dir", &st) == 0
                     && S_ISDIR(st.st_mode)
                     && mkdir("dir", 0755) == -1 && errno == EEXIST,
                     "mkdir makes a directory, once\n");
    ksft_test_result(rename("made", "dir/moved") == 0
                     && access("made", F_OK) == -1 && errno == ENOENT
                     && access("dir/moved", F_OK) == 0,
                     "rename moves a file into it\n");
    ksft_test_result(listed(TMP "/dir", "moved") && listed(TMP "/dir", ".")
                     && listed(TMP "/dir", ".."),
                     "where getdents lists it, with . and ..\n");
    ksft_test_result(rmdir("dir") == -1 && errno == ENOTEMPTY,
                     "a directory with something in it is not removed\n");

    ksft_test_result(symlink("dir/moved", "link") == 0
                     && readlink("link", arr_buf, sizeof arr_buf) == 9
                     && memcmp(arr_buf, "dir/moved", 9) == 0
                     && lstat("link", &st) == 0 && S_ISLNK(st.st_mode)
                     && stat("link", &st) == 0 && S_ISREG(st.st_mode),
                     "a symlink holds its text, and stat follows it\n");

    int i_dir = open("dir", O_RDONLY | O_DIRECTORY);
    ksft_test_result(i_dir >= 0
                     && faccessat(i_dir, "moved", R_OK, 0) == 0
                     && fchdir(i_dir) == 0
                     && getcwd(arr_buf, sizeof arr_buf) != NULL
                     && ksft_streq(arr_buf, TMP "/dir"),
                     "a directory's descriptor is where *at calls start, "
                     "and fchdir goes there\n");
    chdir(TMP);

    ksft_test_result(chmod("dir/moved", 0600) == 0
                     && stat("dir/moved", &st) == 0
                     && (st.st_mode & 0777) == 0600
                     && chown("dir/moved", 0, 0) == -1 && errno == EPERM,
                     "the owner may chmod, and may not give a file away\n");

    ksft_test_result(unlink("link") == 0 && unlink("dir/moved") == 0
                     && rmdir("dir") == 0 && access("dir", F_OK) == -1,
                     "and all of it is taken away again\n");

    ksft_test_result(access(ROOTS_FILE, R_OK) == 0
                     && access(ROOTS_FILE, W_OK) == -1 && errno == EACCES
                     && unlink(ROOTS_FILE) == -1 && errno == EACCES,
                     "a guest may read root's file, not write or remove "
                     "it\n");

    ksft_test_result(open("nothing/here", O_RDONLY) == -1 && errno == ENOENT
                     && open(ROOTS_FILE "/x", O_RDONLY) == -1
                     && errno == ENOTDIR,
                     "a name that leads nowhere says why\n");

    i_fd = open("hard", O_CREAT | O_WRONLY, 0644);
    close(i_fd);
    ksft_test_result(link("hard", "second") == -1 && errno == EPERM
                     && unlink("hard") == 0,
                     "a second name for a file is refused, not faked\n");
    ksft_finished();
}
