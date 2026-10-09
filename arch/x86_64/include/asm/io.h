// SPDX-License-Identifier: GPL-2.0-only
/*
 * arch/x86_64/include/asm/io.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * port io wrappers. the "Nd" constraint lets gcc encode ports < 256 as an
 * immediate instead of going through dx, not that it matters much
 */

#ifndef ARCH_X86_64_ASM_IO_H
#define ARCH_X86_64_ASM_IO_H

#include <stdint.h>

/*
 * port io wrappers. the "Nd" constraint lets gcc encode ports < 256 as an
 * immediate instead of going through dx, not that it matters much
 */

static inline void outb(uint16_t port, uint8_t val)
{
    asm volatile ("outb %b0, %w1" : : "a"(val), "Nd"(port) : "memory");
}

static inline void outw(uint16_t port, uint16_t val)
{
    asm volatile ("outw %w0, %w1" : : "a"(val), "Nd"(port) : "memory");
}

static inline void outl(uint16_t port, uint32_t val)
{
    asm volatile ("outl %0, %w1" : : "a"(val), "Nd"(port) : "memory");
}

static inline uint32_t inl(uint16_t port)
{
    uint32_t ret;
    asm volatile ("inl %w1, %0" : "=a"(ret) : "Nd"(port) : "memory");
    return ret;
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t ret;
    asm volatile ("inb %w1, %b0" : "=a"(ret) : "Nd"(port) : "memory");
    return ret;
}

/*
 * tiny delay for ancient hardware that cant keep up with back to back
 * port writes. port 0x80 is the post-code port, writing junk there is
 * the traditional no-op
 */
static inline void io_wait(void)
{
    outb(0x80, 0);
}

#endif
