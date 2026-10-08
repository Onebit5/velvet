// SPDX-License-Identifier: GPL-2.0-only
/*
 * arch/none/context.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the portable arch, and the two hooks it leaves undefined on purpose.
 */

#ifndef ARCH_NONE_CONTEXT_H
#define ARCH_NONE_CONTEXT_H

#include <stdint.h>
#include "arch/inline.h"

/*
 * left undefined rather than stubbed, deliberately. these two are the
 * heart of what an architecture has to supply, and the point of this
 * directory is to make the linker say so out loud
 */
extern void switch_context(uint64_t *save_sp, uint64_t *load_sp);

void context_enter_user(uint64_t entry, uint64_t stack_top,
                        uint64_t argc, uint64_t argv)
    __attribute__((noreturn));

ARCH_INLINE void context_set_kernel_stack(uint64_t top)
{
    (void)top;
}

/*
 * the stack is handed back exactly as it came in, with nothing written
 * on it. an architecture that does nothing cannot make a thread, and a
 * stub that fabricated something plausible-looking would only be a way
 * of hiding that
 */
ARCH_INLINE uint64_t context_make_stack(uint64_t stack_top,
                                        void (*entry)(void))
{
    (void)entry;
    return stack_top;
}

#endif
