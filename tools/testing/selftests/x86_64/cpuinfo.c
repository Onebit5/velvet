// SPDX-License-Identifier: GPL-2.0-only
/*
 * tools/testing/selftests/x86_64/cpuinfo.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the processors, read from /proc by a program and held against what
 * cpuid tells the program itself.
 */

#define _GNU_SOURCE
#include "../kselftest.h"

#include <sched.h>

#define TEXT_MAX 65536

static char s_arr_text[TEXT_MAX];

static int64_t slurp(const char *str_path)
{
    int64_t i_len = ksft_read_file(str_path, s_arr_text, TEXT_MAX - 1);
    s_arr_text[(i_len > 0) ? i_len : 0] = '\0';
    return i_len;
}

static void cpuid(uint32_t u_leaf, uint32_t *p_regs)
{
    __asm__ volatile ("cpuid"
                      : "=a"(p_regs[0]), "=b"(p_regs[1]), "=c"(p_regs[2]),
                        "=d"(p_regs[3])
                      : "a"(u_leaf), "c"(0));
}

/* how many lines start with str_key and a tab, and that many blocks */
static int lines_of(const char *str_key)
{
    int i_count = 0;
    size_t sz_key = strlen(str_key);
    for (const char *p_line = s_arr_text; *p_line != '\0';) {
        i_count += (strncmp(p_line, str_key, sz_key) == 0
                    && p_line[sz_key] == '\t') ? 1 : 0;
        const char *p_end = strchr(p_line, '\n');
        p_line = (p_end != NULL) ? p_end + 1 : "";
    }
    return i_count;
}

/* the first block's value for str_key, after its ": " */
static const char *first(const char *str_key, char *str_out, size_t sz_size)
{
    char arr_find[64];
    snprintf(arr_find, sizeof arr_find, "\n%s\t", str_key);
    const char *p_at = strstr(s_arr_text, arr_find);
    p_at = (p_at != NULL) ? strstr(p_at, ": ") : NULL;
    str_out[0] = '\0';
    if (p_at != NULL) {
        size_t sz_len = strcspn(p_at + 2, "\n");
        sz_len = (sz_len < sz_size - 1) ? sz_len : sz_size - 1;
        memcpy(str_out, p_at + 2, sz_len);
        str_out[sz_len] = '\0';
    }
    return str_out;
}

/* whether a program may use avx here: the kernel keeps its registers */
static bool avx_usable(void)
{
    uint32_t arr_regs[4];
    cpuid(1, arr_regs);
    bool b_os = (arr_regs[2] >> 27) & 1;
    bool b_avx = (arr_regs[2] >> 28) & 1;
    if (!b_os || !b_avx) {
        return false;
    }
    uint32_t u_lo;
    uint32_t u_hi;
    __asm__ volatile ("xgetbv" : "=a"(u_lo), "=d"(u_hi) : "c"(0));
    return (u_lo & 6) == 6;
}

int main(void)
{
    cpu_set_t set;
    char arr_value[2048];
    char arr_want[64];
    uint32_t arr_regs[4];

    ksft_print_header();
    ksft_set_plan(5);

    slurp("/proc/cpuinfo");
    CPU_ZERO(&set);
    int i_mine = (sched_getaffinity(0, sizeof set, &set) == 0)
                 ? CPU_COUNT(&set) : -1;
    int i_blocks = lines_of("processor");
    ksft_test_result(i_blocks >= 1 && i_blocks == i_mine
                     && strncmp(s_arr_text, "processor\t: 0\n", 14) == 0,
                     "/proc/cpuinfo has a block for each core a thread may "
                     "run on, from 0\n");

    static const char *const s_arr_keys[] = {
        "vendor_id", "cpu family", "model", "model name", "stepping",
        "cpuid level", "flags", "address sizes",
    };
    bool b_keys = true;
    for (size_t sz_at = 0; sz_at < sizeof s_arr_keys / sizeof *s_arr_keys;
         sz_at++) {
        b_keys = b_keys && lines_of(s_arr_keys[sz_at]) == i_blocks;
    }
    ksft_test_result(b_keys, "each with what the programs that read it "
                     "look for\n");

    cpuid(0, arr_regs);
    memcpy(arr_want, &arr_regs[1], 4);
    memcpy(arr_want + 4, &arr_regs[3], 4);
    memcpy(arr_want + 8, &arr_regs[2], 4);
    arr_want[12] = '\0';
    bool b_vendor = ksft_streq(first("vendor_id", arr_value,
                                     sizeof arr_value), arr_want);
    cpuid(1, arr_regs);
    uint32_t u_family = (arr_regs[0] >> 8) & 0xf;
    u_family += (u_family == 0xf) ? (arr_regs[0] >> 20) & 0xff : 0;
    snprintf(arr_want, sizeof arr_want, "%u", u_family);
    ksft_test_result(b_vendor
                     && ksft_streq(first("cpu family", arr_value,
                                         sizeof arr_value), arr_want),
                     "and says what cpuid says to this program\n");

    char arr_flags[2048 + 2];
    snprintf(arr_flags, sizeof arr_flags, " %s ",
             first("flags", arr_value, sizeof arr_value));
    bool b_basic = strstr(arr_flags, " fpu ") != NULL
                   && strstr(arr_flags, " sse2 ") != NULL
                   && strstr(arr_flags, " lm ") != NULL;
    bool b_avx = strstr(arr_flags, " avx ") != NULL;
    ksft_test_result(b_basic && (!b_avx || avx_usable()),
                     "its flags are what a program may use: long mode and "
                     "sse2, and avx only where it works\n");

    int64_t i_len = slurp("/proc/threads");
    ksft_test_result(ksft_contains(s_arr_text, i_len, "cpu  apic")
                     && ksft_contains(s_arr_text, i_len, "taking work"),
                     "/proc/threads ends with the cores, and which take "
                     "work\n");
    ksft_finished();
}
