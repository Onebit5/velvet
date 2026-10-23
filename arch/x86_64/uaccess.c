// SPDX-License-Identifier: GPL-2.0-only
/*
 * arch/x86_64/uaccess.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the C half of reaching into ring 3: ranges checked, faults answered.
 */

#include <asm/uaccess.h>
#include <uapi/velvet/errno.h>
#include <velvet/string.h>

#ifdef VELVET_HOSTED

/*
 * a host test has no mmu to fault on its behalf, so it says itself
 * whether a range would have been reachable, from the pages it pretends
 * a program has, and the copies are plain copies
 */
bool host_uaccess_reachable(uint64_t u_addr, uint64_t u_len, bool b_write);

#define reachable(addr, len, write) host_uaccess_reachable(addr, len, write)

static uint64_t uaccess_copy(void *p_to, const void *p_from, uint64_t u_len)
{
    memcpy(p_to, p_from, u_len);
    return 0;
}

static uint64_t uaccess_clear(void *p_to, uint64_t u_len)
{
    memset(p_to, 0, u_len);
    return 0;
}

static int64_t uaccess_strncpy(char *str_to, const char *str_from,
                               uint64_t u_max)
{
    for (uint64_t u_at = 0; u_at < u_max; u_at++) {
        uint64_t u_addr = (uint64_t)(uintptr_t)(str_from + u_at);
        if ((u_at == 0 || (u_addr & 0xfff) == 0)
            && !host_uaccess_reachable(u_addr, 1, false)) {
            return -1;      /* where the real one would have faulted */
        }
        str_to[u_at] = str_from[u_at];
        if (str_from[u_at] == '\0') {
            return (int64_t)u_at;
        }
    }
    return (int64_t)u_max;
}

uint64_t uaccess_fixup(uint64_t u_rip)
{
    (void)u_rip;
    return 0;
}

#else

/* the copy itself finds out, by faulting, and the fault handler answers */
#define reachable(addr, len, write) true

uint64_t uaccess_copy(void *p_to, const void *p_from, uint64_t u_len);
uint64_t uaccess_clear(void *p_to, uint64_t u_len);
int64_t uaccess_strncpy(char *str_to, const char *str_from, uint64_t u_max);

/* one pair per instruction in uaccess.asm that may fault */
struct ex_entry {
    uint64_t u_insn;
    uint64_t u_fixup;
};

extern const struct ex_entry __ex_table_start[];
extern const struct ex_entry __ex_table_end[];

uint64_t uaccess_fixup(uint64_t u_rip)
{
    for (const struct ex_entry *p_at = __ex_table_start;
         p_at < __ex_table_end; p_at++) {
        if (p_at->u_insn == u_rip) {
            return p_at->u_fixup;
        }
    }
    return 0;
}

#endif

int64_t copy_from_user(void *p_to, uint64_t u_from, size_t sz_len)
{
    if (!access_ok(u_from, sz_len) || !reachable(u_from, sz_len, false)) {
        return -EFAULT;
    }
    return uaccess_copy(p_to, (const void *)u_from, sz_len) == 0 ? 0
                                                                 : -EFAULT;
}

int64_t copy_to_user(uint64_t u_to, const void *p_from, size_t sz_len)
{
    if (!access_ok(u_to, sz_len) || !reachable(u_to, sz_len, true)) {
        return -EFAULT;
    }
    return uaccess_copy((void *)u_to, p_from, sz_len) == 0 ? 0 : -EFAULT;
}

int64_t clear_user(uint64_t u_to, size_t sz_len)
{
    if (!access_ok(u_to, sz_len) || !reachable(u_to, sz_len, true)) {
        return -EFAULT;
    }
    return uaccess_clear((void *)u_to, sz_len) == 0 ? 0 : -EFAULT;
}

int64_t strncpy_from_user(char *str_to, uint64_t u_from, size_t sz_max)
{
    if (sz_max == 0) {
        return -ENAMETOOLONG;
    }
    /*
     * the string's length is not known until its NUL is found, so the
     * range checked is the most it could be, cut at the end of ring 3: a
     * string that runs up to that end and stops is the program's, and
     * one that would run past it is not
     */
    uint64_t u_room = (u_from < USER_ADDRESS_END) ? USER_ADDRESS_END - u_from
                                                  : 0;
    uint64_t u_max = (sz_max < u_room) ? sz_max : u_room;
    if (u_max == 0 || !reachable(u_from, 1, false)) {
        return -EFAULT;
    }
    int64_t i_len = uaccess_strncpy(str_to, (const char *)u_from, u_max);
    if (i_len < 0) {
        return -EFAULT;
    }
    if ((uint64_t)i_len == u_max) {
        return (u_max < sz_max) ? -EFAULT : -ENAMETOOLONG;
    }
    return i_len;
}
