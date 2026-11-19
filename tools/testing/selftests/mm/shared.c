// SPDX-License-Identifier: GPL-2.0-only
/*
 * tools/testing/selftests/mm/shared.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * memory mapped shared, from ring 3: a file's, which every mapping of it
 * and every read of it see alike and which reaches the file, and memory
 * of no file, which a fork shares.
 */

#include "../kselftest.h"

#include <sys/mman.h>
#include <sys/stat.h>

#define PAGE 4096

static char s_arr_path[64];

/* a file of sz_len bytes of c_fill at s_arr_path, open for both */
static int made(size_t sz_len, char c_fill)
{
    static char s_arr_fill[2 * PAGE];
    memset(s_arr_fill, c_fill, sizeof s_arr_fill);
    int i_fd = open(s_arr_path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (i_fd >= 0 && write(i_fd, s_arr_fill, sz_len) != (ssize_t)sz_len) {
        close(i_fd);
        return -1;
    }
    return i_fd;
}

/* the byte at u_offset of the file, by a read of it */
static int byte_at(int i_fd, off_t i_offset)
{
    char c_got = 0;
    return (pread(i_fd, &c_got, 1, i_offset) == 1) ? c_got : -1;
}

/* the permissions and name maps gives the region holding p_at */
static bool region_of(const void *p_at, char *str_perms, char *str_name,
                      size_t sz_name)
{
    static char s_arr_maps[16384];
    int64_t i_len = ksft_read_file("/proc/self/maps", s_arr_maps,
                                   sizeof s_arr_maps - 1);
    s_arr_maps[(i_len > 0) ? i_len : 0] = '\0';
    for (char *p_line = strtok(s_arr_maps, "\n"); p_line != NULL;
         p_line = strtok(NULL, "\n")) {
        unsigned long u_start = 0;
        unsigned long u_end = 0;
        int i_name = 0;
        if (sscanf(p_line, "%lx-%lx %4s %*s %*s %*s %n", &u_start, &u_end,
                   str_perms, &i_name) >= 3
            && (unsigned long)p_at >= u_start
            && (unsigned long)p_at < u_end) {
            snprintf(str_name, sz_name, "%s",
                     (i_name > 0) ? p_line + i_name : "");
            return true;
        }
    }
    return false;
}

int main(void)
{
    char arr_perms[8];
    char arr_name[64];

    ksft_print_header();
    ksft_set_plan(13);
    snprintf(s_arr_path, sizeof s_arr_path, "/tmp/shared-%d", (int)getpid());

    int i_fd = made(2 * PAGE, 'a');
    char *p_map = (i_fd >= 0) ? mmap(NULL, 2 * PAGE, PROT_READ | PROT_WRITE,
                                     MAP_SHARED, i_fd, 0)
                              : MAP_FAILED;
    if (p_map == MAP_FAILED) {
        ksft_print_msg("a file would not be mapped shared: %s\n",
                       strerror(errno));
        ksft_finished();
    }
    p_map[0] = 'X';
    ksft_test_result(byte_at(i_fd, 0) == 'X',
                     "a store to a file mapped shared is what a read of the "
                     "file says\n");
    ksft_test_result(pwrite(i_fd, "W", 1, PAGE) == 1 && p_map[PAGE] == 'W',
                     "and a write to the file is in the mapping at once\n");

    int i_ro = open(s_arr_path, O_RDONLY);
    char *p_other = (i_ro >= 0) ? mmap(NULL, 2 * PAGE, PROT_READ, MAP_SHARED,
                                       i_ro, 0)
                                : MAP_FAILED;
    p_map[1] = 'Y';
    ksft_test_result(p_other != MAP_FAILED && p_other[0] == 'X'
                     && p_other[1] == 'Y',
                     "two mappings of a file, by two opens of it, are one\n");

    int i_status = -1;
    pid_t i_pid = fork();
    if (i_pid == 0) {
        p_map[2] = 'Z';
        _exit(0);
    }
    waitpid(i_pid, &i_status, 0);
    ksft_test_result(ksft_exited(i_status, 0) && p_map[2] == 'Z'
                     && byte_at(i_fd, 2) == 'Z',
                     "and a child's store to it is its parent's\n");

    ksft_test_result(region_of(p_map, arr_perms, arr_name, sizeof arr_name)
                     && strcmp(arr_perms, "rw-s") == 0
                     && strcmp(arr_name, s_arr_path) == 0,
                     "maps says the mapping is shared, and of which file\n");

    ksft_test_result(mprotect(p_other, PAGE, PROT_READ | PROT_WRITE) == -1
                     && errno == EACCES,
                     "a mapping by a descriptor that may not write the file "
                     "may not be made writable\n");
    ksft_test_result(mmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_SHARED,
                          i_ro, 0) == MAP_FAILED && errno == EACCES,
                     "nor mapped writable to begin with\n");

    ksft_test_result(msync(p_map, 2 * PAGE, MS_SYNC) == 0
                     && msync(p_map + 1, PAGE, MS_SYNC) == -1
                     && errno == EINVAL
                     && msync(p_map, PAGE, MS_SYNC | MS_ASYNC) == -1
                     && errno == EINVAL,
                     "msync takes a page's start and one of its two ways\n");
    munmap(p_other, 2 * PAGE);
    ksft_test_result(munmap(p_map, 2 * PAGE) == 0
                     && msync(p_map, PAGE, MS_SYNC) == -1 && errno == ENOMEM,
                     "and memory not mapped is ENOMEM\n");
    close(i_ro);
    close(i_fd);

    char arr_got[4] = { 0 };
    i_fd = open(s_arr_path, O_RDONLY);
    ksft_test_result(i_fd >= 0 && read(i_fd, arr_got, 3) == 3
                     && memcmp(arr_got, "XYZ", 3) == 0,
                     "what was stored is in the file once the mappings are "
                     "gone\n");
    close(i_fd);

    /* the last page of a file has room past its end */
    struct stat st;
    i_fd = made(100, 'b');
    p_map = (i_fd >= 0) ? mmap(NULL, PAGE, PROT_READ | PROT_WRITE,
                               MAP_SHARED, i_fd, 0)
                        : MAP_FAILED;
    if (p_map != MAP_FAILED) {
        p_map[50] = 'c';
        p_map[200] = 'd';
        munmap(p_map, PAGE);
    }
    ksft_test_result(p_map != MAP_FAILED && fstat(i_fd, &st) == 0
                     && st.st_size == 100 && byte_at(i_fd, 50) == 'c'
                     && byte_at(i_fd, 200) == -1,
                     "a store past the end of the file is not in it, and "
                     "does not make it longer\n");
    close(i_fd);
    unlink(s_arr_path);

    /* memory of no file */
    int *p_word = mmap(NULL, PAGE, PROT_READ | PROT_WRITE,
                       MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    i_pid = (p_word != MAP_FAILED) ? fork() : -1;
    if (i_pid == 0) {
        *p_word = 42;
        _exit(0);
    }
    waitpid(i_pid, &i_status, 0);
    ksft_test_result(p_word != MAP_FAILED && ksft_exited(i_status, 0)
                     && *p_word == 42,
                     "memory mapped shared with no file is shared with a "
                     "child, a page it makes too\n");
    ksft_test_result(region_of(p_word, arr_perms, arr_name, sizeof arr_name)
                     && strcmp(arr_perms, "rw-s") == 0
                     && strcmp(arr_name, "/dev/zero (deleted)") == 0,
                     "and maps names it as a mapping of /dev/zero whose name "
                     "is gone\n");
    ksft_finished();
}
