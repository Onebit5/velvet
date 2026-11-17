// SPDX-License-Identifier: GPL-2.0-only
/*
 * tools/testing/selftests/dynamic.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * a program linked against the libc's shared library and built to be
 * loaded anywhere, the way most programs are. it is no test of its own:
 * sched/loader starts it, and it says on its standard output what it
 * found where it was put, for the test to hold against what was
 * promised.
 */

#define _GNU_SOURCE
#include "kselftest.h"

#include <elf.h>
#include <sys/auxv.h>

/* the program's own header, wherever it was put: the linker says where */
extern const Elf64_Ehdr __ehdr_start;

/*
 * a pointer the loader has to fix up, since where it points is known
 * only once the program is somewhere
 */
static int s_i_target;
static int *const volatile s_p_target = &s_i_target;

/* the file maps says the page at u_addr is read out of, or "-" */
static void file_at(uint64_t u_addr, char *str_out, size_t sz_size)
{
    static char s_arr_maps[16384];
    int64_t i_len = ksft_read_file("/proc/self/maps", s_arr_maps,
                                   sizeof s_arr_maps - 1);
    s_arr_maps[(i_len > 0) ? i_len : 0] = '\0';
    snprintf(str_out, sz_size, "-");
    for (char *p_line = s_arr_maps; *p_line != '\0';) {
        char *p_end = strchr(p_line, '\n');
        if (p_end != NULL) {
            *p_end = '\0';
        }
        unsigned long u_start = 0;
        unsigned long u_stop = 0;
        char *p_name = strrchr(p_line, ' ');
        if (sscanf(p_line, "%lx-%lx", &u_start, &u_stop) == 2
            && u_addr >= u_start && u_addr < u_stop && p_name != NULL
            && p_name[1] != '\0') {
            snprintf(str_out, sz_size, "%s", p_name + 1);
            return;
        }
        p_line = (p_end != NULL) ? p_end + 1 : p_line + strlen(p_line);
    }
}

/*
 * whether the auxiliary vector describes the program the kernel started,
 * which is in str_file: AT_PHDR where its program headers are, the same
 * bytes as in the file, and AT_ENTRY its entry, moved as far as they were
 */
static bool described(const char *str_file)
{
    static Elf64_Phdr s_arr_phdrs[64];
    Elf64_Ehdr ehdr;
    int i_fd = open(str_file, O_RDONLY);
    size_t sz_len = 0;
    bool b_read = i_fd >= 0 && pread(i_fd, &ehdr, sizeof ehdr, 0)
                                   == (ssize_t)sizeof ehdr
                  && getauxval(AT_PHNUM) == ehdr.e_phnum
                  && ehdr.e_phnum <= 64
                  && (sz_len = ehdr.e_phnum * sizeof *s_arr_phdrs) > 0
                  && pread(i_fd, s_arr_phdrs, sz_len, (off_t)ehdr.e_phoff)
                         == (ssize_t)sz_len;
    if (i_fd >= 0) {
        close(i_fd);
    }
    if (!b_read || memcmp((const void *)getauxval(AT_PHDR), s_arr_phdrs,
                          sz_len) != 0) {
        return false;
    }
    /* where it was linked to have them: PT_PHDR, or the segment they are in */
    uint64_t u_linked = 0;
    for (size_t sz_at = 0; sz_at < ehdr.e_phnum; sz_at++) {
        const Elf64_Phdr *p_ph = &s_arr_phdrs[sz_at];
        if (p_ph->p_type == PT_PHDR) {
            u_linked = p_ph->p_vaddr;
            break;
        }
        if (p_ph->p_type == PT_LOAD && p_ph->p_offset <= ehdr.e_phoff
            && ehdr.e_phoff < p_ph->p_offset + p_ph->p_filesz
            && u_linked == 0) {
            u_linked = p_ph->p_vaddr + (ehdr.e_phoff - p_ph->p_offset);
        }
    }
    return getauxval(AT_ENTRY) - ehdr.e_entry
           == getauxval(AT_PHDR) - u_linked;
}

int main(int i_argc, char **pp_argv)
{
    uint64_t u_base = (uint64_t)&__ehdr_start;
    char arr_name[256];

    printf("moved %d\n", u_base != 0);
    printf("relocated %d\n", s_p_target == &s_i_target);
    file_at(getauxval(AT_PHDR), arr_name, sizeof arr_name);
    printf("started %s\n", arr_name);
    printf("described %d\n", described(arr_name));
    file_at(u_base, arr_name, sizeof arr_name);
    printf("self %s\n", arr_name);
    file_at((uint64_t)&printf, arr_name, sizeof arr_name);
    printf("libc %s\n", arr_name);
    printf("base %lx\n", getauxval(AT_BASE));
    file_at(getauxval(AT_BASE), arr_name, sizeof arr_name);
    printf("loader %s\n", arr_name);
    for (int i_at = 0; i_at < i_argc; i_at++) {
        printf("arg %s\n", pp_argv[i_at]);
    }
    return 0;
}
