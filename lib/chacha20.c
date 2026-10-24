// SPDX-License-Identifier: GPL-2.0-only
/*
 * lib/chacha20.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the chacha20 block function: twenty rounds of add, rotate and xor.
 */

#include <velvet/chacha20.h>

static uint32_t rotl(uint32_t u_value, int i_by)
{
    return (u_value << i_by) | (u_value >> (32 - i_by));
}

#define QUARTER(arr, a, b, c, d)                                  \
    do {                                                          \
        arr[a] += arr[b]; arr[d] = rotl(arr[d] ^ arr[a], 16);     \
        arr[c] += arr[d]; arr[b] = rotl(arr[b] ^ arr[c], 12);     \
        arr[a] += arr[b]; arr[d] = rotl(arr[d] ^ arr[a], 8);      \
        arr[c] += arr[d]; arr[b] = rotl(arr[b] ^ arr[c], 7);      \
    } while (0)

void chacha20_block(const uint32_t arr_key[CHACHA20_KEY_WORDS],
                    uint64_t u_counter, uint64_t u_nonce,
                    uint8_t arr_out[CHACHA20_BLOCK])
{
    /* "expand 32-byte k", which is all the constants there are */
    uint32_t arr_in[16] = {
        0x61707865, 0x3320646e, 0x79622d32, 0x6b206574,
    };
    for (int i_at = 0; i_at < CHACHA20_KEY_WORDS; i_at++) {
        arr_in[4 + i_at] = arr_key[i_at];
    }
    arr_in[12] = (uint32_t)u_counter;
    arr_in[13] = (uint32_t)(u_counter >> 32);
    arr_in[14] = (uint32_t)u_nonce;
    arr_in[15] = (uint32_t)(u_nonce >> 32);

    uint32_t arr_x[16];
    for (int i_at = 0; i_at < 16; i_at++) {
        arr_x[i_at] = arr_in[i_at];
    }
    for (int i_round = 0; i_round < 10; i_round++) {
        QUARTER(arr_x, 0, 4, 8, 12);
        QUARTER(arr_x, 1, 5, 9, 13);
        QUARTER(arr_x, 2, 6, 10, 14);
        QUARTER(arr_x, 3, 7, 11, 15);
        QUARTER(arr_x, 0, 5, 10, 15);
        QUARTER(arr_x, 1, 6, 11, 12);
        QUARTER(arr_x, 2, 7, 8, 13);
        QUARTER(arr_x, 3, 4, 9, 14);
    }

    /* the input added back, so the rounds cannot simply be run backwards */
    for (int i_at = 0; i_at < 16; i_at++) {
        uint32_t u_word = arr_x[i_at] + arr_in[i_at];
        arr_out[4 * i_at + 0] = (uint8_t)u_word;
        arr_out[4 * i_at + 1] = (uint8_t)(u_word >> 8);
        arr_out[4 * i_at + 2] = (uint8_t)(u_word >> 16);
        arr_out[4 * i_at + 3] = (uint8_t)(u_word >> 24);
    }
}
