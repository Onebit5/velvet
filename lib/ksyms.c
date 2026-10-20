// SPDX-License-Identifier: GPL-2.0-only
/*
 * lib/ksyms.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the kernel's own symbol table, and the lookup by address.
 */

#include <velvet/ksyms.h>

/*
 * the table is sorted by address, so this is a binary search for the
 * last symbol at or below addr, the function the address falls
 * inside. the kernel has no symbol *sizes*, only start addresses, so an
 * address past the end of the last function still reports as being
 * deep inside it. the offset gives that away when it looks absurd
 */
const char *ksym_lookup_in(const struct ksym *table, size_t count,
                           const char *names, uint64_t addr,
                           uint64_t *offset)
{
    if (count == 0 || addr < table[0].addr) {
        return NULL;
    }

    size_t lo = 0, hi = count - 1;
    while (lo < hi) {
        size_t mid = lo + (hi - lo + 1) / 2;   /* bias up, the kernel wants the last <= */
        if (table[mid].addr <= addr) {
            lo = mid;
        } else {
            hi = mid - 1;
        }
    }

    if (offset != NULL) {
        *offset = addr - table[lo].addr;
    }
    return &names[table[lo].name_off];
}

#ifndef VELVET_HOSTED
const char *ksym_lookup(uint64_t addr, uint64_t *offset)
{
    return ksym_lookup_in(ksym_table, ksym_count, ksym_names, addr, offset);
}
#endif
