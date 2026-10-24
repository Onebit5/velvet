// SPDX-License-Identifier: GPL-2.0-only
/*
 * drivers/random.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * a chacha20 keystream, keyed from what the machine offers.
 */

#include <velvet/random.h>
#include <velvet/chacha20.h>
#include <velvet/spinlock.h>
#include <velvet/string.h>
#include <velvet/epoch.h>
#include <asm/cpu.h>

/*
 * the key makes the bytes, and changes after every request: the next
 * key is the first block the old one makes, which is never handed out,
 * so bytes already given away cannot be worked back to from the key
 * that follows them. what is stirred in waits in the pool and is folded
 * into the key the next time anybody asks
 */
static struct spinlock random_lock = SPINLOCK("random", LOCK_RANK_DEVICE);
static uint32_t s_arr_key[CHACHA20_KEY_WORDS];
static uint32_t s_arr_pool[CHACHA20_KEY_WORDS];
static uint64_t s_u_counter;
static size_t s_sz_stir;

/* the timer's contribution, kept apart so that a tick never takes the lock */
static uint64_t s_u_ticks;

static void stir_locked(const uint8_t *p_bytes, size_t sz_len)
{
    for (size_t sz_at = 0; sz_at < sz_len; sz_at++) {
        size_t sz_word = s_sz_stir % CHACHA20_KEY_WORDS;
        uint32_t u_word = s_arr_pool[sz_word];
        s_arr_pool[sz_word] = ((u_word << 7) | (u_word >> 25))
                              ^ p_bytes[sz_at] ^ (uint32_t)s_sz_stir;
        s_sz_stir++;
    }
}

static void stir_u64_locked(uint64_t u_value)
{
    stir_locked((const uint8_t *)&u_value, sizeof u_value);
}

/* the pool folded in, and the key replaced by a block nobody will see */
static void rekey_locked(void)
{
    uint8_t arr_block[CHACHA20_BLOCK];

    for (int i_at = 0; i_at < CHACHA20_KEY_WORDS; i_at++) {
        s_arr_key[i_at] ^= s_arr_pool[i_at];
        s_arr_pool[i_at] = 0;
    }
    chacha20_block(s_arr_key, s_u_counter++, 0, arr_block);
    memcpy(s_arr_key, arr_block, sizeof s_arr_key);
    memset(arr_block, 0, sizeof arr_block);
}

void random_mix(const void *p_buf, size_t sz_len)
{
    uint64_t u_flags = spin_lock_irq(&random_lock);
    stir_locked(p_buf, sz_len);
    spin_unlock_irq(&random_lock, u_flags);
}

void random_tick(void)
{
    __atomic_fetch_xor(&s_u_ticks, cpu_cycles(), __ATOMIC_RELAXED);
    __atomic_fetch_add(&s_u_ticks, 0x9e3779b97f4a7c15ull, __ATOMIC_RELAXED);
}

void random_bytes(void *p_buf, size_t sz_len)
{
    uint8_t *p_out = p_buf;
    uint8_t arr_block[CHACHA20_BLOCK];

    uint64_t u_flags = spin_lock_irq(&random_lock);
    stir_u64_locked(__atomic_exchange_n(&s_u_ticks, 0, __ATOMIC_RELAXED));
    stir_u64_locked(cpu_cycles());
    rekey_locked();
    while (sz_len > 0) {
        size_t sz_take = (sz_len < CHACHA20_BLOCK) ? sz_len : CHACHA20_BLOCK;
        chacha20_block(s_arr_key, s_u_counter++, 1, arr_block);
        memcpy(p_out, arr_block, sz_take);
        p_out += sz_take;
        sz_len -= sz_take;
    }
    rekey_locked();
    spin_unlock_irq(&random_lock, u_flags);
    memset(arr_block, 0, sizeof arr_block);
}

/*
 * the seed is everything the machine has to offer: the core's own
 * generator when it has one, the wall clock, and the cycle count at
 * each of many points, which drifts with every cache miss and every
 * interrupt on the way to boot. a machine with no generator and a
 * deterministic emulator under it has little of the last, and the
 * timer's ticks add more as it runs
 */
void random_init(void)
{
    uint64_t u_flags = spin_lock_irq(&random_lock);
    stir_u64_locked(epoch_now());
    for (int i_at = 0; i_at < 64; i_at++) {
        uint64_t u_value = 0;
        if (cpu_random(&u_value)) {
            stir_u64_locked(u_value);
        }
        stir_u64_locked(cpu_cycles());
    }
    rekey_locked();
    spin_unlock_irq(&random_lock, u_flags);
}
