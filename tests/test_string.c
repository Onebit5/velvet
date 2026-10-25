// SPDX-License-Identifier: GPL-2.0-only
/*
 * tests/test_string.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * host-side test for the kernel's memcpy, memmove and memset.
 */

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <velvet/string.h>

static int s_i_failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); s_i_failures++; } \
} while (0)

/* room for every offset and length below, with guard bytes either side */
#define AREA 512
#define GUARD 0xee

static uint8_t s_arr_src[AREA];
static uint8_t s_arr_dst[AREA];
static uint8_t s_arr_want[AREA];

static void fill(uint8_t *p_area, uint8_t u_seed)
{
    for (size_t sz_at = 0; sz_at < AREA; sz_at++) {
        p_area[sz_at] = (uint8_t)(sz_at * 7 + u_seed);
    }
}

static bool same(const uint8_t *p_a, const uint8_t *p_b)
{
    for (size_t sz_at = 0; sz_at < AREA; sz_at++) {
        if (p_a[sz_at] != p_b[sz_at]) {
            return false;
        }
    }
    return true;
}

/* what each should have done, a byte at a time, and nothing more */
static void copy_by_bytes(uint8_t *p_to, const uint8_t *p_from, size_t sz_len)
{
    uint8_t arr_tmp[AREA];
    for (size_t sz_at = 0; sz_at < sz_len; sz_at++) {
        arr_tmp[sz_at] = p_from[sz_at];
    }
    for (size_t sz_at = 0; sz_at < sz_len; sz_at++) {
        p_to[sz_at] = arr_tmp[sz_at];
    }
}

int main(void)
{
    static const size_t arr_lens[] = { 0, 1, 7, 8, 9, 15, 16, 17, 63, 64,
                                       65, 200, 300 };
    size_t sz_lens = sizeof arr_lens / sizeof arr_lens[0];
    bool b_cpy = true;
    bool b_fwd = true;
    bool b_back = true;
    bool b_set = true;

    for (size_t sz_from = 0; sz_from < 16; sz_from++) {
        for (size_t sz_to = 0; sz_to < 16; sz_to++) {
            for (size_t sz_i = 0; sz_i < sz_lens; sz_i++) {
                size_t sz_len = arr_lens[sz_i];

                fill(s_arr_src, 1);
                fill(s_arr_dst, GUARD);
                fill(s_arr_want, GUARD);
                copy_by_bytes(s_arr_want + 16 + sz_to, s_arr_src + sz_from,
                              sz_len);
                b_cpy &= memcpy(s_arr_dst + 16 + sz_to, s_arr_src + sz_from,
                                sz_len) == s_arr_dst + 16 + sz_to
                         && same(s_arr_dst, s_arr_want);

                /* within one buffer, the destination below the source */
                fill(s_arr_dst, 3);
                fill(s_arr_want, 3);
                copy_by_bytes(s_arr_want + sz_to, s_arr_want + 32 + sz_from,
                              sz_len);
                memmove(s_arr_dst + sz_to, s_arr_dst + 32 + sz_from, sz_len);
                b_fwd &= same(s_arr_dst, s_arr_want);

                /* and above it, which a forward copy would trample */
                fill(s_arr_dst, 5);
                fill(s_arr_want, 5);
                copy_by_bytes(s_arr_want + 32 + sz_to, s_arr_want + sz_from,
                              sz_len);
                memmove(s_arr_dst + 32 + sz_to, s_arr_dst + sz_from, sz_len);
                b_back &= same(s_arr_dst, s_arr_want);

                fill(s_arr_dst, 9);
                fill(s_arr_want, 9);
                for (size_t sz_at = 0; sz_at < sz_len; sz_at++) {
                    s_arr_want[16 + sz_to + sz_at] = 0xa5;
                }
                b_set &= memset(s_arr_dst + 16 + sz_to, 0x1a5, sz_len)
                         == s_arr_dst + 16 + sz_to
                         && same(s_arr_dst, s_arr_want);
            }
        }
    }
    CHECK(b_cpy, "memcpy copies every length at every pair of alignments, "
                 "and not a byte past either end");
    CHECK(b_fwd, "memmove down over its own source copies what was there");
    CHECK(b_back, "and so does memmove up over it, from the far end");
    CHECK(b_set, "memset fills with the low byte of what it is given, "
                 "exactly as far as asked");

    if (s_i_failures == 0) {
        printf("all string tests passed\n");
    }
    return s_i_failures == 0 ? 0 : 1;
}
