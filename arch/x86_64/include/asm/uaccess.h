// SPDX-License-Identifier: GPL-2.0-only
/*
 * arch/x86_64/include/asm/uaccess.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * reading and writing ring 3's memory, which may not be there.
 */

#ifndef ARCH_X86_64_ASM_UACCESS_H
#define ARCH_X86_64_ASM_UACCESS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include <velvet/compiler.h>

/*
 * where ring 3 ends: the top of the lower half, the most a 48 bit
 * address can be and still be canonical with bit 47 clear
 */
#define USER_ADDRESS_END 0x0000800000000000ull

/*
 * the whole range is below the end of ring 3, without wrapping. says
 * nothing about whether anything is mapped there: the copy finds out,
 * and a page that is not the program's is -EFAULT rather than a fault
 */
ARCH_INLINE bool access_ok(uint64_t u_addr, uint64_t u_len)
{
    return u_len <= USER_ADDRESS_END && u_addr <= USER_ADDRESS_END - u_len;
}

/* 0, or -EFAULT when any byte of the range is not the program's */
int64_t copy_from_user(void *p_to, uint64_t u_from, size_t sz_len);
int64_t copy_to_user(uint64_t u_to, const void *p_from, size_t sz_len);
int64_t clear_user(uint64_t u_to, size_t sz_len);

/*
 * a string out of ring 3, NUL included, into sz_max bytes: its length,
 * -ENAMETOOLONG when there is no NUL within them, or -EFAULT
 */
int64_t strncpy_from_user(char *str_to, uint64_t u_from, size_t sz_max);

/* the one value, for the commonest case of all */
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

/*
 * for the fault handler: when the faulting instruction is one of the
 * few allowed to fault, the rip to carry on from instead. 0 otherwise
 */
uint64_t uaccess_fixup(uint64_t u_rip);

#endif
