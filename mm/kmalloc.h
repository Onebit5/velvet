// SPDX-License-Identifier: GPL-2.0-only
/*
 * mm/kmalloc.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the kernel heap. first-fit free list that grabs whole pages from the pmm
 * when it runs low.
 */

/* the design notes for kmalloc.h are in docs/subsystems/mm.rst */

#ifndef MM_KMALLOC_H
#define MM_KMALLOC_H

#include <stddef.h>
#include <stdint.h>

void *kmalloc(size_t size);
void  kfree(void *ptr);

uint64_t kheap_total_bytes(void);
uint64_t kheap_used_bytes(void);

#endif
