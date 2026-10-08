// SPDX-License-Identifier: GPL-2.0-only
/*
 * mm/kmalloc.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the heap, which is now mostly not a heap.
 */

#include "mm/kmalloc.h"
#include "mm/slab.h"
#include "mm/pmm.h"
#include "lib/panic.h"
#include "arch/irq.h"
#include "sched/spinlock.h"

/*
 * requests sorted by size rather than one free list walked from the
 * front, which gets slower the longer the machine runs. anything in a
 * size class comes from a slab cache, so there is nothing to search.
 * anything larger takes whole pages from the pmm with a small header on
 * the front. the classes are powers of two, so the worst case wastes
 * just under half, a good trade in a kernel where nearly every
 * allocation is one of a dozen structs
 */

#define LARGE_MAGIC 0x1a46e51a6e900d5ull

/*
 * exactly 16 bytes, so the payload after it stays 16-byte aligned and a
 * request of 4080 still fits in a single page
 */
struct large {
    uint64_t magic;
    uint64_t pages;
};

static const size_t class_size[] = { 16, 32, 64, 128, 256, 512, 1024 };
#define CLASS_COUNT (sizeof(class_size) / sizeof(class_size[0]))
#define LARGEST_CLASS 1024

static struct slab_cache classes[CLASS_COUNT];
static const char *const class_name[CLASS_COUNT] = {
    "kmalloc-16",  "kmalloc-32",  "kmalloc-64", "kmalloc-128",
    "kmalloc-256", "kmalloc-512", "kmalloc-1024",
};

/* only the counters below. */
static struct spinlock large_lock = SPINLOCK("kmalloc", LOCK_RANK_HEAP);

static uint64_t large_pages;    /* pages held by over-sized allocations */

static void ensure_classes(void)
{
    if (classes[0].obj_size != 0) {
        return;
    }
    for (size_t i = 0; i < CLASS_COUNT; i++) {
        slab_cache_init(&classes[i], class_name[i], class_size[i]);
    }
}

void *kmalloc(size_t size)
{
    if (size == 0) {
        return NULL;
    }

    ensure_classes();

    if (size <= LARGEST_CLASS) {
        for (size_t i = 0; i < CLASS_COUNT; i++) {
            if (size <= class_size[i]) {
                return slab_alloc(&classes[i]);
            }
        }
    }

    /*
     * too big for a class. whole pages, with a header saying how many,
     * since kfree is given only the pointer and has to know
     */
    size_t pages = (size + sizeof(struct large) + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t phys = pmm_alloc_pages(pages);
    if (phys == 0) {
        return NULL;        /* memory hath forsaken me */
    }

    struct large *h = pmm_phys_to_virt(phys);
    h->magic = LARGE_MAGIC;
    h->pages = pages;

    uint64_t flags = spin_lock_irq(&large_lock);
    large_pages += pages;
    spin_unlock_irq(&large_lock, flags);

    return (uint8_t *)h + sizeof(struct large);
}

void kfree(void *ptr)
{
    if (ptr == NULL) {
        return;
    }

    /*
     * which of the two it came from is written on the page it sits in,
     * so this costs one read rather than a search
     */
    if (slab_owns(ptr)) {
        slab_free(ptr);
        return;
    }

    struct large *h = (struct large *)((uint8_t *)ptr - sizeof(struct large));
    if (h->magic != LARGE_MAGIC) {
        panic("kfree: %p knows not this heap. whence came it?", ptr);
    }

    uint64_t pages = h->pages;
    h->magic = 0;               /* so a second kfree is caught, not repeated */

    uint64_t flags = spin_lock_irq(&large_lock);
    large_pages -= pages;
    spin_unlock_irq(&large_lock, flags);

    pmm_free_pages((uint64_t)h - pmm_hhdm_offset(), pages);
}

uint64_t kheap_total_bytes(void)
{
    uint64_t pages = large_pages;
    for (struct slab_cache *c = slab_first_cache(); c != NULL; c = c->next) {
        pages += c->pages;
    }
    return pages * PAGE_SIZE;
}

uint64_t kheap_used_bytes(void)
{
    uint64_t used = large_pages * PAGE_SIZE;
    for (struct slab_cache *c = slab_first_cache(); c != NULL; c = c->next) {
        used += c->in_use * c->obj_size;
    }
    return used;
}
