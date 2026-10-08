// SPDX-License-Identifier: GPL-2.0-only
/*
 * mm/slab.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * object caches, for the things a kernel allocates over and over.
 */

/* the design notes for slab.h are in docs/subsystems/mm.rst */

#ifndef MM_SLAB_H
#define MM_SLAB_H

#include <stdint.h>
#include <stddef.h>

struct slab;

struct slab_cache {
    const char *name;
    size_t obj_size;            /* rounded up to 16 */
    size_t per_slab;            /* objects that fit in one page */

    struct slab *partial;       /* pages with at least one object free */
    struct slab *full;          /* pages with none */

    size_t pages;               /* how many pages this cache is holding */
    size_t in_use;              /* objects currently handed out */
    size_t high_water;          /* the most that were ever out at once */

    struct slab_cache *next;    /* every cache, so `slabs` can list them */
};

void slab_cache_init(struct slab_cache *cache, const char *name,
                     size_t obj_size);

void *slab_alloc(struct slab_cache *cache);

void slab_free(void *object);

int slab_owns(const void *object);

struct slab_cache *slab_first_cache(void);

#endif
