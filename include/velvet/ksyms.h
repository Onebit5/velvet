// SPDX-License-Identifier: GPL-2.0-only
/*
 * include/velvet/ksyms.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the kernel's own symbol table, baked in at build time by
 * tools/gensyms.py so a panic can say where it happened in words.
 */

/* the design notes for ksyms.h are in docs/subsystems/mm.rst */

#ifndef VELVET_KSYMS_H
#define VELVET_KSYMS_H

#include <stdint.h>
#include <stddef.h>

/*
 * the kernel's own symbol table, baked in at build time by
 * tools/gensyms.py so a panic can say where it happened in words.
 *
 * names are offsets into one big blob rather than pointers, because
 * pointers would need relocating and the whole table has to survive
 * being generated from one link and used in the next
 */

struct ksym {
    uint64_t addr;
    uint32_t name_off;
};

extern const struct ksym ksym_table[];
extern const char ksym_names[];
extern const unsigned long ksym_count;

const char *ksym_lookup(uint64_t addr, uint64_t *offset);

const char *ksym_lookup_in(const struct ksym *table, size_t count,
                           const char *names, uint64_t addr,
                           uint64_t *offset);

#endif
