// SPDX-License-Identifier: GPL-2.0-only
/*
 * tests/test_cpuinfo.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * host-side test for a core's block of /proc/cpuinfo: its layout over a
 * cpu made up for the purpose, and what cpuid says on the machine the
 * build is on, held against what that machine's own kernel says.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* the lock complains through this when something is wrong with it */
void panic(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    exit(1);
}

#include <asm/cpuinfo.h>
#include <velvet/kprintf.h>

/* what kprintf.c leans on, done the plainest way there is */
void serial_putchar(char c_ch)
{
    (void)c_ch;
}
bool console_ready(void)
{
    return false;
}
void console_putchar(char c_ch)
{
    (void)c_ch;
}

static struct kcapture *s_p_capture;

static struct kcapture *capturing(void)
{
    return s_p_capture;
}

static int s_i_failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); s_i_failures++; } \
} while (0)

/* the block cpu_show_core writes for core u_cpu of u_cores */
static const char *shown(const struct cpu_ident *p_id, uint32_t u_cpu,
                         uint32_t u_cores, uint32_t u_apic)
{
    static char s_arr_text[4096];
    struct kcapture cap = { s_arr_text, sizeof s_arr_text - 1, 0, false };
    s_p_capture = &cap;
    cpu_show_core(p_id, u_cpu, u_cores, u_apic);
    s_p_capture = NULL;
    s_arr_text[cap.sz_len] = '\0';
    return s_arr_text;
}

/* the value after `str_key<tabs>: ` in str_text, to the end of its line */
static const char *value_of(const char *str_text, const char *str_key,
                            char *str_out, size_t sz_size)
{
    size_t sz_key = strlen(str_key);
    str_out[0] = '\0';
    for (const char *p_line = str_text; *p_line != '\0';) {
        const char *p_end = strchr(p_line, '\n');
        size_t sz_line = (p_end != NULL) ? (size_t)(p_end - p_line)
                                         : strlen(p_line);
        if (sz_line == 0) {
            break;      /* the first core's block is enough */
        }
        if (strncmp(p_line, str_key, sz_key) == 0
            && (p_line[sz_key] == '\t' || p_line[sz_key] == ':')) {
            const char *p_value = strstr(p_line, ": ");
            const char *p_colon = strchr(p_line, ':');
            p_value = (p_value != NULL && p_value < p_line + sz_line)
                      ? p_value + 2 : p_colon + 1;
            size_t sz_value = (size_t)(p_line + sz_line - p_value);
            sz_value = (sz_value < sz_size - 1) ? sz_value : sz_size - 1;
            memcpy(str_out, p_value, sz_value);
            str_out[sz_value] = '\0';
            return str_out;
        }
        p_line = (p_end != NULL) ? p_end + 1 : p_line + sz_line;
    }
    return str_out;
}

/* whether every word of str_some is a word of str_all */
static bool words_within(const char *str_some, const char *str_all,
                         char *str_missing, size_t sz_size)
{
    str_missing[0] = '\0';
    char arr_word[64];
    for (const char *p_at = str_some; *p_at != '\0';) {
        while (*p_at == ' ') {
            p_at++;
        }
        size_t sz_len = strcspn(p_at, " ");
        if (sz_len == 0) {
            break;
        }
        snprintf(arr_word, sizeof arr_word, " %.*s ", (int)sz_len, p_at);
        char arr_all[2048];
        snprintf(arr_all, sizeof arr_all, " %s ", str_all);
        if (strstr(arr_all, arr_word) == NULL) {
            strncat(str_missing, arr_word, sz_size - strlen(str_missing) - 1);
        }
        p_at += sz_len;
    }
    return str_missing[0] == '\0';
}

static void check_layout(void)
{
    struct cpu_ident id = {
        .arr_vendor = "GenuineIntel",
        .arr_brand = "Imaginary CPU @ 1.00GHz",
        .u_family = 6, .u_model = 158, .u_stepping = 10, .u_level = 22,
        .u_clflush = 64, .u_phys_bits = 39, .u_virt_bits = 48,
    };
    /* sse and sse2, lm, pni, and avx, which needs what is never on here */
    id.arr_words[CPUID_1_EDX] = 1u << 0 | 1u << 25 | 1u << 26;
    id.arr_words[CPUID_EXT_EDX] = 1u << 29;
    id.arr_words[CPUID_1_ECX] = 1u << 0 | 1u << 28;
    id.arr_words[CPUID_7_EBX] = 1u << 3 | 1u << 5;

    CHECK(strcmp(shown(&id, 1, 2, 2),
                 "processor\t: 1\n"
                 "vendor_id\t: GenuineIntel\n"
                 "cpu family\t: 6\n"
                 "model\t\t: 158\n"
                 "model name\t: Imaginary CPU @ 1.00GHz\n"
                 "stepping\t: 10\n"
                 "physical id\t: 0\n"
                 "siblings\t: 2\n"
                 "core id\t\t: 1\n"
                 "cpu cores\t: 2\n"
                 "apicid\t\t: 2\n"
                 "initial apicid\t: 2\n"
                 "fpu\t\t: yes\n"
                 "fpu_exception\t: yes\n"
                 "cpuid level\t: 22\n"
                 "wp\t\t: yes\n"
                 "flags\t\t: fpu sse sse2 lm pni bmi1\n"
                 "clflush size\t: 64\n"
                 "cache_alignment\t: 64\n"
                 "address sizes\t: 39 bits physical, 48 bits virtual\n"
                 "power management:\n\n") == 0,
          "a core's block, key by key, and its flags in order, avx and "
          "avx2 left out");
}

/*
 * the machine the build is on: what cpuid says here, and what this
 * machine's own kernel makes of the same cpuid
 */
static void check_against_host(void)
{
    static char s_arr_host[1 << 16];
    FILE *p_file = fopen("/proc/cpuinfo", "r");
    size_t sz_len = (p_file != NULL)
                    ? fread(s_arr_host, 1, sizeof s_arr_host - 1, p_file) : 0;
    s_arr_host[sz_len] = '\0';
    if (p_file != NULL) {
        fclose(p_file);
    }

    struct cpu_ident id;
    cpu_identify(&id);
    const char *str_mine = shown(&id, 0, 1, 0);

    static const char *const s_arr_keys[] = {
        "vendor_id", "cpu family", "model", "model name", "stepping",
        "cpuid level", "clflush size",
    };
    bool b_same = sz_len > 0;
    for (size_t sz_at = 0; sz_at < sizeof s_arr_keys / sizeof *s_arr_keys;
         sz_at++) {
        char arr_host[256];
        char arr_mine[256];
        value_of(s_arr_host, s_arr_keys[sz_at], arr_host, sizeof arr_host);
        value_of(str_mine, s_arr_keys[sz_at], arr_mine, sizeof arr_mine);
        if (strcmp(arr_host, arr_mine) != 0) {
            printf("  %s: here \"%s\", the host's kernel \"%s\"\n",
                   s_arr_keys[sz_at], arr_mine, arr_host);
            b_same = false;
        }
    }
    CHECK(b_same, "this machine's cpu is what its own kernel says it is");

    char arr_host_flags[2048];
    char arr_flags[2048];
    char arr_missing[512];
    value_of(s_arr_host, "flags", arr_host_flags, sizeof arr_host_flags);
    value_of(str_mine, "flags", arr_flags, sizeof arr_flags);
    bool b_within = words_within(arr_flags, arr_host_flags, arr_missing,
                                 sizeof arr_missing);
    if (!b_within) {
        printf("  flags its own kernel does not have:%s\n", arr_missing);
    }
    CHECK(arr_flags[0] != '\0' && b_within,
          "and every flag named here is one its kernel names too");
}

int main(void)
{
    kprintf_capture_source(capturing);
    check_layout();
    check_against_host();

    if (s_i_failures == 0) {
        printf("all good\n");
    }
    return s_i_failures;
}
