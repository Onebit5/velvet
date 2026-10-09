// SPDX-License-Identifier: GPL-2.0-only
/*
 * mm/slab.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * object caches, and the lists threaded through their pages.
 */

#include <velvet/slab.h>
#include <velvet/pmm.h>
#include <velvet/panic.h>
#include <asm/irq.h>
#include <velvet/spinlock.h>
#include <stdbool.h>

#define SLAB_MAGIC 0x51ab7a11ed900dull

/* sits at the top of every page a cache owns. the objects follow it */
struct slab {
    uint64_t magic;
    struct slab_cache *cache;
    struct slab *next, *prev;
    void *free;                 /* free list, threaded through the objects */
    uint32_t in_use;
    uint32_t total;
};

#define SLAB_HEADER ((sizeof(struct slab) + 15) & ~15ull)

/* every cache, and the lists threaded through their pages. */
static struct spinlock slab_lock = SPINLOCK("slab", LOCK_RANK_HEAP);

static struct slab_cache *all_caches;

static struct slab *page_of(const void *object)
{
    return (struct slab *)((uintptr_t)object & ~(uintptr_t)(PAGE_SIZE - 1));
}

void slab_cache_init(struct slab_cache *cache, const char *name,
                     size_t obj_size)
{
    /*
     * XXX: this check is not atomic and the list below is appended to with
     * nothing holding it. two cores making an address space at the same
     * instant both find obj_size zero and both run the rest of this, so
     * the second links the same node in twice: cache->next ends up
     * pointing at the cache, and every walk of all_caches then runs
     * forever, which is what the shell's slab command and the heap
     * counters do. kmalloc's own classes get away with it because they
     * are built long before a second core exists; addrspace_create calls
     * this lazily on the first fork. take the slab lock across the whole
     * of it.
     */
    if (cache->obj_size != 0) {
        return;             /* already set up */
    }

    size_t size = (obj_size + 15) & ~15ull;
    if (size < 16) {
        size = 16;
    }
    if (size > PAGE_SIZE - SLAB_HEADER) {
        panic("slab: %s wants %zu bytes, which does not fit in a page",
              name, obj_size);
    }

    cache->name = name;
    cache->obj_size = size;
    cache->per_slab = (PAGE_SIZE - SLAB_HEADER) / size;
    cache->partial = NULL;
    cache->full = NULL;
    cache->pages = 0;
    cache->in_use = 0;
    cache->high_water = 0;

    cache->next = all_caches;
    all_caches = cache;
}



static void list_insert(struct slab **list, struct slab *s)
{
    s->prev = NULL;
    s->next = *list;
    if (*list != NULL) {
        (*list)->prev = s;
    }
    *list = s;
}

static void list_remove(struct slab **list, struct slab *s)
{
    if (s->prev != NULL) {
        s->prev->next = s->next;
    } else {
        *list = s->next;
    }
    if (s->next != NULL) {
        s->next->prev = s->prev;
    }
    s->next = s->prev = NULL;
}



static struct slab *new_slab(struct slab_cache *cache)
{
    uint64_t phys = pmm_alloc();
    if (phys == 0) {
        return NULL;
    }

    struct slab *s = pmm_phys_to_virt(phys);
    s->magic = SLAB_MAGIC;
    s->cache = cache;
    s->in_use = 0;
    s->total = (uint32_t)cache->per_slab;
    s->free = NULL;

    /* thread a free list through every object. */
    uint8_t *base = (uint8_t *)s + SLAB_HEADER;
    for (size_t i = cache->per_slab; i > 0; i--) {
        void *object = base + (i - 1) * cache->obj_size;
        *(void **)object = s->free;
        s->free = object;
    }

    cache->pages++;
    list_insert(&cache->partial, s);
    return s;
}



void *slab_alloc(struct slab_cache *cache)
{
    uint64_t flags = spin_lock_irq(&slab_lock);

    struct slab *s = cache->partial;
    if (s == NULL) {
        s = new_slab(cache);
        if (s == NULL) {
            spin_unlock_irq(&slab_lock, flags);
            return NULL;
        }
    }

    void *object = s->free;
    s->free = *(void **)object;
    s->in_use++;

    /*
     * a page with nothing left moves off the partial list, so the next
     * allocation does not have to look at it and find it wanting
     */
    if (s->free == NULL) {
        list_remove(&cache->partial, s);
        list_insert(&cache->full, s);
    }

    cache->in_use++;
    if (cache->in_use > cache->high_water) {
        cache->high_water = cache->in_use;
    }

    spin_unlock_irq(&slab_lock, flags);
    return object;
}

void slab_free(void *object)
{
    if (object == NULL) {
        return;
    }

    struct slab *s = page_of(object);
    if (s->magic != SLAB_MAGIC) {
        panic("slab_free: %p came from no cache of the kernel's", object);
    }

    struct slab_cache *cache = s->cache;
    uint64_t flags = spin_lock_irq(&slab_lock);

    /*
     * FIXME: this guard can never fire, and the double free it is named
     * for goes undetected. when in_use reaches zero the page is handed
     * back to the pmm and magic is cleared on the spot, so a later free
     * of anything from that page trips the magic check above instead. a
     * real double free that is made while other objects are still live,
     * takes the branch below and links the object into the free list a
     * second time, which makes the list cyclic and hands the same
     * object out twice. give each object an allocated bit, or poison a
     * freed one, so freeing what is already free is refused.
     */
    if (s->in_use == 0) {
        panic("slab_free: %p returned to %s twice over", object, cache->name);
    }

    bool was_full = (s->free == NULL);

    *(void **)object = s->free;
    s->free = object;
    s->in_use--;
    cache->in_use--;

    if (was_full) {
        list_remove(&cache->full, s);
        list_insert(&cache->partial, s);
    }

    /* a page nobody is using goes back to the pmm. */
    if (s->in_use == 0) {
        list_remove(&cache->partial, s);
        s->magic = 0;
        cache->pages--;
        uint64_t phys = (uint64_t)s - pmm_hhdm_offset();
        spin_unlock_irq(&slab_lock, flags);
        pmm_free(phys);
        return;
    }

    spin_unlock_irq(&slab_lock, flags);
}

int slab_owns(const void *object)
{
    if (object == NULL) {
        return 0;
    }
    return page_of(object)->magic == SLAB_MAGIC;
}

struct slab_cache *slab_first_cache(void)
{
    return all_caches;
}
