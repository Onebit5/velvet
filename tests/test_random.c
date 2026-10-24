// SPDX-License-Identifier: GPL-2.0-only
/*
 * tests/test_random.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the generator: bytes that differ, and look like they could be anything.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>

#include <velvet/random.h>

void panic(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    exit(1);
}
void kprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

/* a clock that has not started, so the seed is the cycle counter's alone */
uint64_t epoch_now(void)
{
    return 0;
}

static int s_i_failures;
#define CHECK(c, m) \
    do { if (!(c)) { printf("FAIL: %s\n", m); s_i_failures++; } } while (0)

int main(void)
{
    static uint8_t s_arr_a[4096];
    static uint8_t s_arr_b[4096];
    static const uint8_t s_arr_zero[4096];

    random_init();
    random_bytes(s_arr_a, sizeof s_arr_a);
    random_bytes(s_arr_b, sizeof s_arr_b);
    CHECK(memcmp(s_arr_a, s_arr_zero, sizeof s_arr_a) != 0,
          "the bytes are not nothing");
    CHECK(memcmp(s_arr_a, s_arr_b, sizeof s_arr_a) != 0,
          "and two requests are not the same bytes");

    size_t sz_ones = 0;
    for (size_t sz_at = 0; sz_at < sizeof s_arr_a; sz_at++) {
        sz_ones += (size_t)__builtin_popcount(s_arr_a[sz_at]);
    }
    CHECK(sz_ones > 4096 * 8 * 45 / 100 && sz_ones < 4096 * 8 * 55 / 100,
          "about as many ones as zeroes");

    uint8_t arr_odd[3] = { 0 };
    random_mix("stirred", 7);
    random_bytes(arr_odd, sizeof arr_odd);
    CHECK(arr_odd[0] != 0 || arr_odd[1] != 0 || arr_odd[2] != 0,
          "a request shorter than a block is filled all the same");

    if (s_i_failures == 0) {
        printf("all good\n");
    }
    return s_i_failures;
}
