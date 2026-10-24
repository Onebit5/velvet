// SPDX-License-Identifier: GPL-2.0-only
/*
 * tests/test_chacha20.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the block function, held to the published test vector.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include <velvet/chacha20.h>

static int s_i_failures;
#define CHECK(c, m) \
    do { if (!(c)) { printf("FAIL: %s\n", m); s_i_failures++; } } while (0)

int main(void)
{
    /*
     * RFC 8439, 2.3.2: key 00 01 .. 1f, block counter 1, nonce
     * 00 00 00 09 00 00 00 4a 00 00 00 00. its 32 bit counter and 96 bit
     * nonce lie in the same four words as a 64 bit counter and a 64 bit
     * nonce, so the vector checks this layout too
     */
    static const uint8_t s_arr_want[CHACHA20_BLOCK] = {
        0x10, 0xf1, 0xe7, 0xe4, 0xd1, 0x3b, 0x59, 0x15,
        0x50, 0x0f, 0xdd, 0x1f, 0xa3, 0x20, 0x71, 0xc4,
        0xc7, 0xd1, 0xf4, 0xc7, 0x33, 0xc0, 0x68, 0x03,
        0x04, 0x22, 0xaa, 0x9a, 0xc3, 0xd4, 0x6c, 0x4e,
        0xd2, 0x82, 0x64, 0x46, 0x07, 0x9f, 0xaa, 0x09,
        0x14, 0xc2, 0xd7, 0x05, 0xd9, 0x8b, 0x02, 0xa2,
        0xb5, 0x12, 0x9c, 0xd1, 0xde, 0x16, 0x4e, 0xb9,
        0xcb, 0xd0, 0x83, 0xe8, 0xa2, 0x50, 0x3c, 0x4e,
    };
    uint32_t arr_key[CHACHA20_KEY_WORDS];
    for (int i_at = 0; i_at < CHACHA20_KEY_WORDS; i_at++) {
        uint32_t u_byte = (uint32_t)(4 * i_at);
        arr_key[i_at] = u_byte | (u_byte + 1) << 8 | (u_byte + 2) << 16
                        | (u_byte + 3) << 24;
    }
    uint8_t arr_got[CHACHA20_BLOCK];
    chacha20_block(arr_key, (0x09000000ull << 32) | 1, 0x4a000000ull,
                   arr_got);
    CHECK(memcmp(arr_got, s_arr_want, sizeof arr_got) == 0,
          "the block matches the published vector");

    uint8_t arr_next[CHACHA20_BLOCK];
    chacha20_block(arr_key, (0x09000000ull << 32) | 2, 0x4a000000ull,
                   arr_next);
    CHECK(memcmp(arr_got, arr_next, sizeof arr_got) != 0,
          "and the next block is another");

    if (s_i_failures == 0) {
        printf("all good\n");
    }
    return s_i_failures;
}
