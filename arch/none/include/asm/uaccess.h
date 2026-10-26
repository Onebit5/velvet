// SPDX-License-Identifier: GPL-2.0-only
/*
 * arch/none/include/asm/uaccess.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * a program's memory, on an architecture that runs no programs.
 */

#ifndef ARCH_NONE_ASM_UACCESS_H
#define ARCH_NONE_ASM_UACCESS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include <velvet/compiler.h>

/*
 * no address is a program's. a portable file that copies anyway is
 * answered -EFAULT by whatever port defines the copies, and until one
 * does they are on the list of what a port owes
 */
ARCH_INLINE bool access_ok(uint64_t u_addr, uint64_t u_len)
{
    (void)u_addr;
    (void)u_len;
    return false;
}

int64_t copy_from_user(void *p_to, uint64_t u_from, size_t sz_len);
int64_t copy_to_user(uint64_t u_to, const void *p_from, size_t sz_len);
int64_t clear_user(uint64_t u_to, size_t sz_len);
int64_t strncpy_from_user(char *str_to, uint64_t u_from, size_t sz_max);

ARCH_INLINE int64_t get_user_u64(uint64_t *p_to, uint64_t u_from)
{
    return copy_from_user(p_to, u_from, sizeof *p_to);
}

ARCH_INLINE int64_t put_user_u64(uint64_t u_to, uint64_t u_value)
{
    return copy_to_user(u_to, &u_value, sizeof u_value);
}

ARCH_INLINE int64_t put_user_i32(uint64_t u_to, int32_t i_value)
{
    return copy_to_user(u_to, &i_value, sizeof i_value);
}

#endif
