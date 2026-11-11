// SPDX-License-Identifier: GPL-2.0-only
/*
 * mm/addrspace.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * user address spaces: create, fork, destroy, switch.
 */

#include <velvet/addrspace.h>
#include <velvet/pmm.h>
#include <velvet/pressure.h>
#include <velvet/vmm.h>
#include <velvet/kmalloc.h>
#include <velvet/slab.h>
#include <asm/smp.h>
#include <asm/cpu.h>
#include <asm/mmu.h>
#include <velvet/string.h>
#include <velvet/usermode.h>

/* the lower half is per-process, the upper half is everyone's. */
#define USER_PML4_ENTRIES   256

/* what cr3 holds, one per core. */
static uint64_t live_pml4[CPU_MAX];

static uint64_t *table_at(uint64_t phys)
{
    return pmm_phys_to_virt(phys);
}

/*
 * whether a core other than this one has the space loaded. one that has
 * not holds no translation into it, having thrown every one away when
 * it loaded the tables it has now, and one that loads it after this
 * looks reads the tables as they already are: the change was made
 * before the look, the fence orders the two, and loading a table is a
 * fence of its own. so a change is told to the other cores only when
 * one of them is running the space, which a program of one thread
 * never has them doing
 */
static bool loaded_elsewhere(const struct addrspace *as)
{
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    uint32_t u_me = cpu_id();
    for (uint32_t u_at = 0; u_at < CPU_MAX; u_at++) {
        if (u_at != u_me
            && __atomic_load_n(&live_pml4[u_at], __ATOMIC_RELAXED)
               == as->pml4) {
            return true;
        }
    }
    return false;
}

/* every other core told to throw away what it holds of the space */
static void shoot(const struct addrspace *as)
{
    if (loaded_elsewhere(as)) {
        smp_tlb_shootdown();
    }
}

static struct slab_cache addrspace_cache;

struct addrspace *addrspace_create(uint64_t kernel_pml4)
{
    slab_cache_init(&addrspace_cache, "addrspace", sizeof(struct addrspace));

    struct addrspace *as = slab_alloc(&addrspace_cache);
    if (as == NULL) {
        return NULL;
    }

    /*
     * the slab hands back whatever was in the object last time, and a
     * region table full of somebody else's leftovers is worse than
     * broken, it is a record saying a wild pointer is legitimate.
     * every field here has to be set on purpose
     */
    memset(as, 0, sizeof *as);
    spin_init_unlisted(&as->lock, "addrspace", LOCK_RANK_MM);
    as->u_users = 1;

    as->pml4 = vmm_new_address_space();
    if (as->pml4 == 0) {
        slab_free(as);
        return NULL;
    }

    /* share the kernel half by copying its top level entries. */
    const uint64_t *kernel = table_at(kernel_pml4);
    uint64_t *mine = table_at(as->pml4);
    for (size_t i = USER_PML4_ENTRIES; i < 512; i++) {
        mine[i] = kernel[i];
    }

    return as;
}

/* the page tables are copied and the pages underneath them are not. */

static uint64_t free_level(uint64_t phys, int level);

/* one level of the walk, copied. */
static uint64_t copy_level(uint64_t phys, int level)
{
    uint64_t fresh = pmm_alloc_pages(1);
    if (fresh == 0) {
        return 0;
    }
    uint64_t *out = table_at(fresh);
    uint64_t *in = table_at(phys);
    memset(out, 0, PAGE_SIZE);

    for (size_t i = 0; i < 512; i++) {
        if (!(in[i] & PTE_PRESENT)) {
            continue;
        }

        if (level == 1 || (in[i] & PTE_HUGE)) {
            /*
             * a leaf. both sides give up the right to write to it and
             * both remember why, and the frame learns it has two
             * holders. the *parent's* entry is changed too, a fork
             * where only the child is protected is a fork where the
             * parent quietly writes through the child's memory
             */
            uint64_t entry = in[i];
            if (entry & PTE_WRITE) {
                entry = (entry & ~PTE_WRITE) | PTE_COW;
                in[i] = entry;
            }
            out[i] = entry;
            pmm_ref(entry & PTE_ADDR_MASK);
            continue;
        }

        uint64_t child = copy_level(in[i] & PTE_ADDR_MASK, level - 1);
        if (child == 0) {
            /* out of memory partway down. */
            free_level(fresh, level);
            return 0;
        }
        out[i] = child | (in[i] & ~PTE_ADDR_MASK);
    }

    return fresh;
}

static void destroy(struct addrspace *as);

struct addrspace *addrspace_fork(struct addrspace *from,
                                 uint64_t kernel_pml4)
{
    if (from == NULL) {
        return NULL;
    }
    /* sharing a frame means being able to count who holds it. */
    if (!pmm_can_share()) {
        return NULL;
    }

    struct addrspace *as = addrspace_create(kernel_pml4);
    if (as == NULL) {
        return NULL;
    }

    /*
     * the parent's tables are rewritten as they are copied, and its
     * other threads fault on them meanwhile: they wait here for the copy
     * to be whole
     */
    uint64_t u_flags = spin_lock_irq(&from->lock);
    const uint64_t *parent = table_at(from->pml4);
    uint64_t *mine = table_at(as->pml4);

    for (size_t i = 0; i < USER_PML4_ENTRIES; i++) {
        if (!(parent[i] & PTE_PRESENT)) {
            continue;
        }
        uint64_t child = copy_level(parent[i] & PTE_ADDR_MASK, 3);
        if (child == 0) {
            spin_unlock_irq(&from->lock, u_flags);
            shoot(from);
            destroy(as);
            return NULL;
        }
        mine[i] = child | (parent[i] & ~PTE_ADDR_MASK);
    }

    /*
     * the records come too, and where the break, the arguments and the
     * environment are
     */
    for (size_t i = 0; i < VMA_MAX; i++) {
        as->vmas[i] = from->vmas[i];
    }
    as->u_brk_start = from->u_brk_start;
    as->u_brk = from->u_brk;
    as->u_arg_start = from->u_arg_start;
    as->u_arg_end = from->u_arg_end;
    as->u_env_start = from->u_env_start;
    as->u_env_end = from->u_env_end;
    as->u_stack_start = from->u_stack_start;

    /*
     * and this core's own memory of the parent's entries, when the parent
     * is what it is running, which a fork's parent always is. the
     * shootdown below reaches every other core and not this one, and a
     * translation this core kept from before still says writable: the
     * parent's next store would go straight into the page it now shares
     * with the child. loading the same table again throws them away
     */
    if (live_pml4[cpu_id()] == from->pml4) {
        mmu_load_table(from->pml4);
    }
    spin_unlock_irq(&from->lock, u_flags);

    /*
     * the parent's own entries just changed under it, and every core
     * running another of its threads may be holding a translation that
     * still says writable. until this is done such a thread writes
     * through it into the page the child now shares, which is the child
     * seeing a write made before its fork; the first write after makes
     * a copy that is the parent's alone. a thread with no translation
     * faults, and waited above for the copy to be whole. so the fork is
     * one moment, here, for every thread of the parent, and the call
     * does not return before it
     */
    shoot(from);
    return as;
}

struct addrspace *addrspace_get(struct addrspace *as)
{
    __atomic_fetch_add(&as->u_users, 1, __ATOMIC_ACQ_REL);
    return as;
}

void addrspace_put(struct addrspace *as)
{
    if (as != NULL
        && __atomic_sub_fetch(&as->u_users, 1, __ATOMIC_ACQ_REL) == 0) {
        destroy(as);
    }
}

/*
 * where mmap hands things out: above everything a program links to and
 * a long way below the stack, so the two can never meet
 */
#define MMAP_BASE  0x0000600000000000ull
#define MMAP_LIMIT 0x00006fff00000000ull

/* find the leaf entry for an address, or NULL. */
static uint64_t *leaf_for(uint64_t pml4_phys, uint64_t virt)
{
    uint64_t *table = table_at(pml4_phys);
    int shift = 39;

    for (int level = 4; level > 1; level--) {
        size_t i = (virt >> shift) & 0x1ff;
        if (!(table[i] & PTE_PRESENT)) {
            return NULL;
        }
        if (table[i] & PTE_HUGE) {
            return &table[i];
        }
        table = table_at(table[i] & PTE_ADDR_MASK);
        shift -= 9;
    }
    return &table[(virt >> 12) & 0x1ff];
}

/* the record that makes a not-present fault answerable. */

static struct vma *region_for(struct addrspace *as, uint64_t virt)
{
    for (size_t i = 0; i < VMA_MAX; i++) {
        struct vma *v = &as->vmas[i];
        if (v->kind != VMA_NONE && virt >= v->start && virt < v->end) {
            return v;
        }
    }
    return NULL;
}

/* the lock is held */
static bool add_region(struct addrspace *as, uint64_t u_start, uint64_t u_end,
                       uint64_t u_flags, enum vma_kind e_kind,
                       const uint8_t *p_image, uint64_t u_image_offset,
                       uint64_t u_file_end)
{
    if (u_end <= u_start) {
        return false;
    }

    u_start &= ~(PAGE_SIZE - 1);
    u_end = (u_end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    struct vma *p_slot = NULL;
    for (size_t i = 0; i < VMA_MAX; i++) {
        if (as->vmas[i].kind == VMA_NONE) {
            if (p_slot == NULL) {
                p_slot = &as->vmas[i];
            }
            continue;
        }
        /*
         * two records disagreeing about one address is worse than
         * refusing to make the second
         */
        if (u_start < as->vmas[i].end && u_end > as->vmas[i].start) {
            return false;
        }
    }
    if (p_slot == NULL) {
        return false;
    }

    p_slot->kind = e_kind;
    p_slot->start = u_start;
    p_slot->end = u_end;
    p_slot->flags = u_flags;
    p_slot->image = p_image;
    p_slot->image_offset = u_image_offset;
    p_slot->file_end = u_file_end;
    return true;
}

bool addrspace_add_region(struct addrspace *as, uint64_t start, uint64_t end,
                          uint64_t flags, enum vma_kind kind,
                          const uint8_t *image, uint64_t image_offset,
                          uint64_t file_end)
{
    if (as == NULL) {
        return false;
    }
    uint64_t u_flags = spin_lock_irq(&as->lock);
    bool b_added = add_region(as, start, end, flags, kind, image,
                              image_offset, file_end);
    spin_unlock_irq(&as->lock, u_flags);
    return b_added;
}

/*
 * the lowest a program may map anything. page 0 and the few after it are
 * never anybody's, so a NULL pointer, or a small offset from one, faults
 * rather than reading something a program put there
 */
#define MAP_MIN 0x10000ull

/* and the most: the top of the lower half */
#define USER_TOP 0x0000800000000000ull

#define PAGE_UP(x) (((x) + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1))

static struct vma *free_slot(struct addrspace *p_as)
{
    for (size_t sz_at = 0; sz_at < VMA_MAX; sz_at++) {
        if (p_as->vmas[sz_at].kind == VMA_NONE) {
            return &p_as->vmas[sz_at];
        }
    }
    return NULL;
}

static size_t free_slots(const struct addrspace *p_as)
{
    size_t sz_count = 0;
    for (size_t sz_at = 0; sz_at < VMA_MAX; sz_at++) {
        sz_count += (p_as->vmas[sz_at].kind == VMA_NONE) ? 1 : 0;
    }
    return sz_count;
}

/* nothing at all is in [start, end) */
static bool range_free(const struct addrspace *p_as, uint64_t u_start,
                       uint64_t u_end)
{
    for (size_t sz_at = 0; sz_at < VMA_MAX; sz_at++) {
        const struct vma *p_vma = &p_as->vmas[sz_at];
        if (p_vma->kind != VMA_NONE && u_start < p_vma->end
            && u_end > p_vma->start) {
            return false;
        }
    }
    return true;
}

/* every page of [start, end) is in some region */
static bool range_covered(const struct addrspace *p_as, uint64_t u_start,
                          uint64_t u_end)
{
    uint64_t u_at = u_start;
    for (bool b_moved = true; b_moved && u_at < u_end; ) {
        b_moved = false;
        for (size_t sz_at = 0; sz_at < VMA_MAX; sz_at++) {
            const struct vma *p_vma = &p_as->vmas[sz_at];
            if (p_vma->kind != VMA_NONE && u_at >= p_vma->start
                && u_at < p_vma->end) {
                u_at = p_vma->end;
                b_moved = true;
            }
        }
    }
    return u_at >= u_end;
}

/*
 * cut p_vma in two at u_at, which is inside it: the part from u_at up
 * becomes a region of its own in p_slot, starting that far into the image
 */
static void split_at(struct vma *p_vma, struct vma *p_slot, uint64_t u_at)
{
    *p_slot = *p_vma;
    p_slot->start = u_at;
    if (p_vma->kind == VMA_FILE) {
        p_slot->image_offset += u_at - p_vma->start;
    }
    p_vma->end = u_at;
}

/* how many cuts at start and end would split a region in two */
static size_t cuts_needed(const struct addrspace *p_as, uint64_t u_start,
                          uint64_t u_end)
{
    size_t sz_count = 0;
    for (size_t sz_at = 0; sz_at < VMA_MAX; sz_at++) {
        const struct vma *p_vma = &p_as->vmas[sz_at];
        if (p_vma->kind == VMA_NONE) {
            continue;
        }
        sz_count += (p_vma->start < u_start && p_vma->end > u_start) ? 1 : 0;
        sz_count += (p_vma->start < u_end && p_vma->end > u_end) ? 1 : 0;
    }
    return sz_count;
}

/*
 * cut every region crossing start or end there, so that afterwards each
 * region is either wholly inside [start, end) or wholly out of it. the
 * caller has made sure of the slots
 */
static void cut_at_edges(struct addrspace *p_as, uint64_t u_start,
                         uint64_t u_end)
{
    for (size_t sz_at = 0; sz_at < VMA_MAX; sz_at++) {
        struct vma *p_vma = &p_as->vmas[sz_at];
        if (p_vma->kind == VMA_NONE) {
            continue;
        }
        if (p_vma->start < u_start && p_vma->end > u_start) {
            split_at(p_vma, free_slot(p_as), u_start);
        }
        if (p_vma->start < u_end && p_vma->end > u_end) {
            split_at(p_vma, free_slot(p_as), u_end);
        }
    }
}

/*
 * frames taken out of a space and not given back yet. a core running
 * another thread of the space may still be holding a translation to one,
 * and a frame given back to the allocator while it is would be some
 * other program's memory written through it, so each is let go of only
 * once every core has thrown its translations away
 */
#define DROPPED_MAX 64

struct dropped {
    size_t sz_count;
    uint64_t arr_phys[DROPPED_MAX];
    bool b_changed;             /* an entry some core may hold changed */
};

/* every core's translations thrown away, and then the frames let go of */
static void let_go(const struct addrspace *p_as, struct dropped *p_drop)
{
    if (p_drop->b_changed) {
        shoot(p_as);
    }
    for (size_t sz_at = 0; sz_at < p_drop->sz_count; sz_at++) {
        pmm_unref(p_drop->arr_phys[sz_at]);
    }
    p_drop->sz_count = 0;
    p_drop->b_changed = false;
}

/*
 * every page made in [start, end) taken out, into p_drop. one range with
 * more pages in it than that holds is let go of a batch at a time with
 * the lock still held, the one shootdown made under it, which holds up
 * the space's other threads for as long as it takes, so only a range
 * that big pays that. the lock is held
 */
static void drop_pages(struct addrspace *p_as, uint64_t u_start,
                       uint64_t u_end, struct dropped *p_drop)
{
    for (uint64_t u_addr = u_start; u_addr < u_end; u_addr += PAGE_SIZE) {
        uint64_t *p_pte = leaf_for(p_as->pml4, u_addr);
        if (p_pte == NULL || !(*p_pte & PTE_PRESENT)) {
            continue;
        }
        if (p_drop->sz_count == DROPPED_MAX) {
            let_go(p_as, p_drop);
        }
        p_drop->arr_phys[p_drop->sz_count++] = *p_pte & PTE_ADDR_MASK;
        p_drop->b_changed = true;
        *p_pte = 0;
        vmm_flush_page(u_addr);
        if (p_as->pages_in_use > 0) {
            p_as->pages_in_use--;
        }
    }
}

/* the lock is held, and what is dropped is the caller's to let go of */
static bool unmap(struct addrspace *p_as, uint64_t u_start, uint64_t u_len,
                  struct dropped *p_drop)
{
    if (u_len == 0 || (u_start & (PAGE_SIZE - 1)) != 0
        || u_start + PAGE_UP(u_len) < u_start) {
        return false;
    }
    uint64_t u_end = u_start + PAGE_UP(u_len);

    /*
     * trimming either end of a region keeps it one region. only a hole
     * strictly inside one makes two, and the second needs a slot
     */
    for (size_t sz_at = 0; sz_at < VMA_MAX; sz_at++) {
        const struct vma *p_vma = &p_as->vmas[sz_at];
        if (p_vma->kind != VMA_NONE && p_vma->start < u_start
            && p_vma->end > u_end && free_slots(p_as) == 0) {
            return false;
        }
    }
    for (size_t sz_at = 0; sz_at < VMA_MAX; sz_at++) {
        struct vma *p_vma = &p_as->vmas[sz_at];
        if (p_vma->kind == VMA_NONE || p_vma->end <= u_start
            || p_vma->start >= u_end) {
            continue;
        }
        if (p_vma->start < u_start && p_vma->end > u_end) {
            split_at(p_vma, free_slot(p_as), u_end);
            p_vma->end = u_start;
        } else if (p_vma->start < u_start) {
            p_vma->end = u_start;
        } else if (p_vma->end > u_end) {
            if (p_vma->kind == VMA_FILE) {
                p_vma->image_offset += u_end - p_vma->start;
            }
            p_vma->start = u_end;
        } else {
            p_vma->kind = VMA_NONE;
        }
    }
    drop_pages(p_as, u_start, u_end, p_drop);
    return true;
}

bool addrspace_unmap(struct addrspace *p_as, uint64_t u_start, uint64_t u_len)
{
    if (p_as == NULL) {
        return false;
    }
    struct dropped drop = { 0 };
    uint64_t u_flags = spin_lock_irq(&p_as->lock);
    bool b_done = unmap(p_as, u_start, u_len, &drop);
    spin_unlock_irq(&p_as->lock, u_flags);
    let_go(p_as, &drop);
    return b_done;
}

/*
 * what a page already there becomes under new flags. a page still shared
 * after a fork stays read-only and marked, whatever the region allows:
 * the first write makes the copy, and writing through to the other
 * space's page would be the bug
 */
static uint64_t reprotect(uint64_t u_pte, uint64_t u_flags)
{
    uint64_t u_kept = u_pte & (PTE_ADDR_MASK | PTE_COW);
    uint64_t u_want = u_flags & ~(uint64_t)PTE_ADDR_MASK;
    if (u_pte & PTE_COW) {
        u_want &= ~(uint64_t)PTE_WRITE;
    }
    return u_kept | u_want | PTE_PRESENT;
}

/* the lock is held */
static bool protect(struct addrspace *p_as, uint64_t u_start, uint64_t u_len,
                    uint64_t u_flags, struct dropped *p_drop)
{
    if (u_len == 0 || (u_start & (PAGE_SIZE - 1)) != 0
        || u_start + PAGE_UP(u_len) < u_start) {
        return false;
    }
    uint64_t u_end = u_start + PAGE_UP(u_len);
    if (!range_covered(p_as, u_start, u_end)
        || cuts_needed(p_as, u_start, u_end) > free_slots(p_as)) {
        return false;
    }
    cut_at_edges(p_as, u_start, u_end);
    for (size_t sz_at = 0; sz_at < VMA_MAX; sz_at++) {
        struct vma *p_vma = &p_as->vmas[sz_at];
        if (p_vma->kind == VMA_NONE || p_vma->start < u_start
            || p_vma->end > u_end) {
            continue;
        }
        p_vma->flags = u_flags;
        for (uint64_t u_addr = p_vma->start; u_addr < p_vma->end;
             u_addr += PAGE_SIZE) {
            uint64_t *p_pte = leaf_for(p_as->pml4, u_addr);
            if (p_pte != NULL && (*p_pte & PTE_PRESENT)) {
                *p_pte = reprotect(*p_pte, u_flags);
                vmm_flush_page(u_addr);
                p_drop->b_changed = true;
            }
        }
    }
    return true;
}

/*
 * a thread of the space running on another core may write through a
 * translation that still says writable until the shootdown, so the call
 * is not done until it is
 */
bool addrspace_protect(struct addrspace *p_as, uint64_t u_start,
                       uint64_t u_len, uint64_t u_flags)
{
    if (p_as == NULL) {
        return false;
    }
    struct dropped drop = { 0 };
    uint64_t u_lock = spin_lock_irq(&p_as->lock);
    bool b_done = protect(p_as, u_start, u_len, u_flags, &drop);
    spin_unlock_irq(&p_as->lock, u_lock);
    let_go(p_as, &drop);
    return b_done;
}

bool addrspace_discard(struct addrspace *p_as, uint64_t u_start,
                       uint64_t u_len)
{
    if (p_as == NULL || u_len == 0 || (u_start & (PAGE_SIZE - 1)) != 0
        || u_start + PAGE_UP(u_len) < u_start) {
        return false;
    }
    uint64_t u_end = u_start + PAGE_UP(u_len);
    struct dropped drop = { 0 };
    uint64_t u_flags = spin_lock_irq(&p_as->lock);
    bool b_covered = range_covered(p_as, u_start, u_end);
    if (b_covered) {
        drop_pages(p_as, u_start, u_end, &drop);
    }
    spin_unlock_irq(&p_as->lock, u_flags);
    let_go(p_as, &drop);
    return b_covered;
}

/*
 * an anonymous region at [start, end), merged with an anonymous
 * neighbour of the same flags on either side rather than taking a slot
 * of its own. the caller has checked the range is free
 */
static bool add_anon(struct addrspace *p_as, uint64_t u_start,
                     uint64_t u_end, uint64_t u_flags)
{
    struct vma *p_below = NULL;
    struct vma *p_above = NULL;
    for (size_t sz_at = 0; sz_at < VMA_MAX; sz_at++) {
        struct vma *p_vma = &p_as->vmas[sz_at];
        if (p_vma->kind != VMA_ANON || p_vma->flags != u_flags) {
            continue;
        }
        if (p_vma->end == u_start) {
            p_below = p_vma;
        }
        if (p_vma->start == u_end) {
            p_above = p_vma;
        }
    }
    if (p_below != NULL && p_above != NULL) {
        p_below->end = p_above->end;
        p_above->kind = VMA_NONE;
        return true;
    }
    if (p_below != NULL) {
        p_below->end = u_end;
        return true;
    }
    if (p_above != NULL) {
        p_above->start = u_start;
        return true;
    }
    return add_region(p_as, u_start, u_end, u_flags, VMA_ANON, NULL, 0, 0);
}

/* the lowest free stretch of u_len in the mmap area, or 0 */
static uint64_t find_room(const struct addrspace *p_as, uint64_t u_len)
{
    uint64_t u_at = MMAP_BASE;
    for (bool b_moved = true; b_moved; ) {
        b_moved = false;
        for (size_t sz_at = 0; sz_at < VMA_MAX; sz_at++) {
            const struct vma *p_vma = &p_as->vmas[sz_at];
            if (p_vma->kind != VMA_NONE && u_at < p_vma->end
                && u_at + u_len > p_vma->start) {
                u_at = p_vma->end;
                b_moved = true;
            }
        }
        if (u_at + u_len > MMAP_LIMIT) {
            return 0;
        }
    }
    return u_at;
}

/* the lock is held */
static uint64_t map_anon(struct addrspace *p_as, uint64_t u_hint,
                         uint64_t u_len, uint64_t u_flags, bool b_fixed,
                         struct dropped *p_drop)
{
    if (u_len == 0 || PAGE_UP(u_len) < u_len) {
        return 0;
    }
    u_len = PAGE_UP(u_len);

    if (b_fixed) {
        if ((u_hint & (PAGE_SIZE - 1)) != 0 || u_hint < MAP_MIN
            || u_hint + u_len < u_hint || u_hint + u_len > USER_TOP
            || !unmap(p_as, u_hint, u_len, p_drop)) {
            return 0;
        }
        return add_anon(p_as, u_hint, u_hint + u_len, u_flags) ? u_hint : 0;
    }

    /* a hint is taken when it is free, and is only a hint when it is not */
    u_hint = PAGE_UP(u_hint);
    bool b_hint = u_hint >= MAP_MIN && u_hint + u_len > u_hint
                  && u_hint + u_len <= USER_TOP
                  && range_free(p_as, u_hint, u_hint + u_len);
    uint64_t u_at = b_hint ? u_hint : find_room(p_as, u_len);
    if (u_at == 0) {
        return 0;
    }
    return add_anon(p_as, u_at, u_at + u_len, u_flags) ? u_at : 0;
}

uint64_t addrspace_map_anon(struct addrspace *p_as, uint64_t u_hint,
                            uint64_t u_len, uint64_t u_flags, bool b_fixed)
{
    if (p_as == NULL) {
        return 0;
    }
    struct dropped drop = { 0 };
    uint64_t u_lock = spin_lock_irq(&p_as->lock);
    uint64_t u_at = map_anon(p_as, u_hint, u_len, u_flags, b_fixed, &drop);
    spin_unlock_irq(&p_as->lock, u_lock);
    let_go(p_as, &drop);
    return u_at;
}

/* the lock is held */
static uint64_t brk(struct addrspace *p_as, uint64_t u_want,
                    uint64_t u_flags, struct dropped *p_drop)
{
    /*
     * a space with no break has none to move, and growing one from 0
     * would put memory where NULL points
     */
    if (p_as->u_brk_start < MAP_MIN || u_want < p_as->u_brk_start) {
        return p_as->u_brk;
    }
    uint64_t u_old_top = PAGE_UP(p_as->u_brk);
    uint64_t u_new_top = PAGE_UP(u_want);

    if (u_new_top > u_old_top) {
        if (u_new_top > MMAP_BASE || !range_free(p_as, u_old_top, u_new_top)
            || !add_anon(p_as, u_old_top, u_new_top, u_flags)) {
            return p_as->u_brk;
        }
    } else if (u_new_top < u_old_top) {
        if (!unmap(p_as, u_new_top, u_old_top - u_new_top, p_drop)) {
            return p_as->u_brk;
        }
    }
    p_as->u_brk = u_want;
    return u_want;
}

uint64_t addrspace_brk(struct addrspace *p_as, uint64_t u_want,
                       uint64_t u_flags)
{
    if (p_as == NULL) {
        return 0;
    }
    struct dropped drop = { 0 };
    uint64_t u_lock = spin_lock_irq(&p_as->lock);
    uint64_t u_at = brk(p_as, u_want, u_flags, &drop);
    spin_unlock_irq(&p_as->lock, u_lock);
    let_go(p_as, &drop);
    return u_at;
}

/* give a not-present address the page it was always going to have */
static bool populate(struct addrspace *as, struct vma *v, uint64_t virt)
{
    uint64_t page = virt & ~(PAGE_SIZE - 1);

    /* a program may not take the last of the machine's memory. */
    if (!pressure_allow(pmm_free_bytes() / PAGE_SIZE, 1, PRESSURE_USER)) {
        return false;
    }

    uint64_t phys = pmm_alloc_pages(1);
    if (phys == 0) {
        return false;
    }
    uint8_t *mem = pmm_phys_to_virt(phys);

    if (v->kind == VMA_FILE && v->image != NULL && page < v->file_end) {
        /* the bytes that exist, then zeroes. */
        uint64_t have = v->file_end - page;
        if (have > PAGE_SIZE) {
            have = PAGE_SIZE;
        }
        memcpy(mem, v->image + v->image_offset + (page - v->start), have);
        memset(mem + have, 0, PAGE_SIZE - have);
    } else {
        /*
         * anonymous memory is zeroes, and has to be: handing a program
         * a page with somebody else's data still in it is the oldest
         * information leak there is
         */
        memset(mem, 0, PAGE_SIZE);
    }

    as->pages_in_use++;

    if (!vmm_map_range(as->pml4, page, phys, PAGE_SIZE, v->flags)) {
        pmm_free_pages(phys, 1);
        return false;
    }
    vmm_flush_page(page);
    return true;
}

/*
 * a write to a page a fork left shared: a copy of its own for this
 * space, or the page itself made writable when this space is the last
 * holder. the frame the copy leaves behind goes in *p_old, for the caller
 * to let go of once no core is translating to it. the lock is held
 */
static bool copy_on_write(struct addrspace *as, uint64_t *p_pte,
                          uint64_t u_virt, uint64_t *p_old)
{
    if (!(*p_pte & PTE_COW)) {
        return false;       /* not the kernel's. a real fault */
    }

    /*
     * shared after a fork, and its region since made read-only: the copy
     * would hand the program a writable page it was told it may not have
     */
    const struct vma *p_region = region_for(as, u_virt);
    if (p_region == NULL || !(p_region->flags & PTE_WRITE)) {
        return false;
    }

    uint64_t u_old = *p_pte & PTE_ADDR_MASK;

    /*
     * FIXME: the count is read under the pmm lock and acted on after it
     * is dropped, so a fork on another core can take a reference to this
     * frame in between. the frame looks unshared, this core takes the
     * page back and makes it writable, and the child ends up sharing a
     * writable page. pmm_unref() has the same window in reverse: it
     * frees after the lock is dropped, so a late pmm_ref() can take a
     * reference to a frame already on its way back. give the pmm one
     * operation that decrements under the lock and reports whether the
     * caller is now the only holder.
     */
    /* the last holder does not need a copy of anything. */
    if (pmm_shares(u_old) == 0) {
        *p_pte = (*p_pte | PTE_WRITE) & ~PTE_COW;
        vmm_flush_page(u_virt);
        return true;
    }

    uint64_t u_fresh = pmm_alloc_pages(1);
    if (u_fresh == 0) {
        return false;       /* out of memory. it really is a fault now */
    }
    memcpy(table_at(u_fresh), table_at(u_old), PAGE_SIZE);

    *p_pte = u_fresh | ((*p_pte & ~PTE_ADDR_MASK) | PTE_WRITE);
    *p_pte &= ~PTE_COW;
    vmm_flush_page(u_virt);
    *p_old = u_old;
    return true;
}

/* the fault, with the lock held */
static bool fault(struct addrspace *as, uint64_t u_virt, bool b_write,
                  bool b_present, uint64_t *p_old)
{
    uint64_t *p_pte = leaf_for(as->pml4, u_virt);
    if (p_pte != NULL && (*p_pte & PTE_PRESENT)) {
        /*
         * there, though it was not when the access was tried: another
         * thread of the space made it while this one waited for the
         * lock. tried again, an access the page does not allow faults
         * again, on a page that is present this time
         */
        if (!b_present) {
            return true;
        }
        if (!b_write || !(*p_pte & PTE_USER)) {
            return false;   /* a read of a page there is never the kernel's */
        }

        /*
         * writable already: another thread made its copy, or made it
         * writable as its last holder, while this core still held a
         * translation saying read-only, which the fault threw away
         */
        if (*p_pte & PTE_WRITE) {
            return true;
        }
        return copy_on_write(as, p_pte, u_virt, p_old);
    }

    /*
     * nothing here yet. the only thing that makes this answerable rather
     * than fatal is a record saying the range was agreed to
     */
    struct vma *p_vma = region_for(as, u_virt);
    if (p_vma == NULL) {
        return false;
    }
    /*
     * a region that allows nothing is there to be in the way, and a page
     * made for it would only fault again
     */
    if (!(p_vma->flags & PTE_USER)) {
        return false;
    }
    if (b_write && !(p_vma->flags & PTE_WRITE)) {
        return false;       /* a write to a region that was never writable */
    }
    return populate(as, p_vma, u_virt);
}

bool addrspace_fault(struct addrspace *as, uint64_t virt, bool write,
                     bool present)
{
    if (as == NULL) {
        return false;
    }
    uint64_t u_old = 0;
    uint64_t u_flags = spin_lock_irq(&as->lock);
    bool b_answered = fault(as, virt, write, present, &u_old);
    if (b_answered) {
        as->u_faults++;
    }
    spin_unlock_irq(&as->lock, u_flags);

    /*
     * the copy is this thread's own now, and another thread of the space
     * on another core may still be reading the page it was copied from
     * through a translation from before, missing every write made to the
     * copy. the write that faulted is not made until every core has
     * thrown that away, and the frame is let go of after, for the reason
     * any is
     */
    if (u_old != 0) {
        shoot(as);
        pmm_unref(u_old);
    }
    return b_answered;
}

/* free a table's children and then the table itself. */
static uint64_t free_level(uint64_t phys, int level)
{
    uint64_t freed = 0;
    uint64_t *table = table_at(phys);

    size_t entries = (level == 4) ? USER_PML4_ENTRIES : 512;
    for (size_t i = 0; i < entries; i++) {
        if (!(table[i] & PTE_PRESENT)) {
            continue;
        }
        uint64_t child = table[i] & PTE_ADDR_MASK;

        if (level == 1 || (table[i] & PTE_HUGE)) {
            /* somebody else may still be reading it. */
            pmm_unref(child);
            freed++;
        } else {
            freed += free_level(child, level - 1);
        }
        table[i] = 0;
    }

    pmm_free_pages(phys, 1);            /* and the table that held them */
    return freed + 1;
}

/* the last user has let go */
static void destroy(struct addrspace *as)
{
    /* if this is somehow still loaded, get off it first. */
    if (live_pml4[cpu_id()] == as->pml4) {
        addrspace_switch(NULL);
    }

    /*
     * a core still running a thread in this space may be holding
     * translations out of tables the kernel is about to hand back to the
     * allocator
     */
    shoot(as);

    free_level(as->pml4, 4);
    slab_free(as);
}

/*
 * counted without the lock, for ps, which says what it saw: another
 * thread may be making or dropping a page while it counts, and the
 * tables it walks are freed only with the space
 */
uint64_t addrspace_frames(struct addrspace *as)
{
    if (as == NULL) {
        return 0;
    }
    uint64_t count = 0;
    const uint64_t *pml4 = table_at(as->pml4);

    for (size_t i = 0; i < USER_PML4_ENTRIES; i++) {
        if (!(pml4[i] & PTE_PRESENT)) {
            continue;
        }
        const uint64_t *pdpt = table_at(pml4[i] & PTE_ADDR_MASK);
        for (size_t j = 0; j < 512; j++) {
            if (!(pdpt[j] & PTE_PRESENT)) {
                continue;
            }
            if (pdpt[j] & PTE_HUGE) { count += 512 * 512; continue; }
            const uint64_t *pd = table_at(pdpt[j] & PTE_ADDR_MASK);
            for (size_t k = 0; k < 512; k++) {
                if (!(pd[k] & PTE_PRESENT)) {
                    continue;
                }
                if (pd[k] & PTE_HUGE) { count += 512; continue; }
                const uint64_t *pt = table_at(pd[k] & PTE_ADDR_MASK);
                for (size_t l = 0; l < 512; l++) {
                    if (pt[l] & PTE_PRESENT) {
                        count++;
                    }
                }
            }
        }
    }
    return count;
}

void addrspace_switch(struct addrspace *as)
{
    uint64_t want = (as != NULL) ? as->pml4 : vmm_kernel_pml4();

    /*
     * writing cr3 throws away the whole tlb, so it is worth not doing
     * it when nothing has changed, which is every switch between two
     * kernel threads, i.e. most of them. the memory of what is loaded is
     * this core's, not the machine's
     */
    uint64_t *live = &live_pml4[cpu_id()];
    if (want == *live) {
        return;
    }
    *live = want;
    mmu_load_table(want);
}

size_t addrspace_read(struct addrspace *p_as, uint64_t u_virt, void *p_out,
                      size_t sz_len)
{
    /*
     * under the lock, so that a page read here is not given back while
     * it is: a frame leaves a space only once its entry has been cleared,
     * which is done holding this
     */
    size_t sz_done = 0;
    uint64_t u_flags = spin_lock_irq(&p_as->lock);
    while (sz_done < sz_len) {
        uint64_t u_at = u_virt + sz_done;
        uint64_t *p_pte = (u_at < USER_TOP) ? leaf_for(p_as->pml4, u_at)
                                                 : NULL;
        if (p_pte == NULL || (*p_pte & (PTE_PRESENT | PTE_USER | PTE_HUGE))
                             != (PTE_PRESENT | PTE_USER)) {
            break;
        }
        size_t sz_in_page = PAGE_SIZE - (size_t)(u_at & (PAGE_SIZE - 1));
        size_t sz_take = (sz_len - sz_done < sz_in_page) ? sz_len - sz_done
                                                         : sz_in_page;
        const uint8_t *p_page = pmm_phys_to_virt(*p_pte & PTE_ADDR_MASK);
        memcpy((uint8_t *)p_out + sz_done, p_page + (u_at & (PAGE_SIZE - 1)),
               sz_take);
        sz_done += sz_take;
    }
    spin_unlock_irq(&p_as->lock, u_flags);
    return sz_done;
}

/*
 * the pages there are in [u_start, u_end): a stretch with no table under
 * it is passed over whole, so a region reserved and never touched costs
 * nothing to count. the lock is held
 */
static uint64_t present_in(uint64_t u_pml4, uint64_t u_start, uint64_t u_end)
{
    uint64_t u_count = 0;
    uint64_t u_at = u_start;
    while (u_at < u_end) {
        const uint64_t *p_table = table_at(u_pml4);
        for (int i_shift = 39; ; i_shift -= 9) {
            uint64_t u_entry = p_table[(u_at >> i_shift) & 0x1ff];
            uint64_t u_span = 1ull << i_shift;
            bool b_leaf = i_shift == 12 || (u_entry & PTE_HUGE);
            if (!(u_entry & PTE_PRESENT) || b_leaf) {
                if (u_entry & PTE_PRESENT) {
                    u_count += u_span / PAGE_SIZE;
                }
                u_at = (u_at & ~(u_span - 1)) + u_span;
                break;
            }
            p_table = table_at(u_entry & PTE_ADDR_MASK);
        }
    }
    return u_count;
}

/* the tables of the lower half below phys, a table at level i_level */
static uint64_t tables_under(uint64_t u_phys, int i_level, size_t sz_entries)
{
    const uint64_t *p_table = table_at(u_phys);
    uint64_t u_count = 1;
    for (size_t sz_at = 0; i_level > 1 && sz_at < sz_entries; sz_at++) {
        if ((p_table[sz_at] & PTE_PRESENT) && !(p_table[sz_at] & PTE_HUGE)) {
            u_count += tables_under(p_table[sz_at] & PTE_ADDR_MASK,
                                    i_level - 1, 512);
        }
    }
    return u_count;
}

void addrspace_sizes(struct addrspace *p_as, struct addrspace_sizes *p_out)
{
    memset(p_out, 0, sizeof *p_out);
    uint64_t u_flags = spin_lock_irq(&p_as->lock);
    for (size_t sz_at = 0; sz_at < VMA_MAX; sz_at++) {
        const struct vma *v = &p_as->vmas[sz_at];
        if (v->kind == VMA_NONE) {
            continue;
        }
        uint64_t u_pages = (v->end - v->start) / PAGE_SIZE;
        uint64_t u_there = present_in(p_as->pml4, v->start, v->end);
        p_out->u_size += u_pages;
        p_out->u_resident += u_there;
        bool b_exec = (v->flags & PTE_USER) && !(v->flags & PTE_NX);
        if (v->kind == VMA_FILE) {
            p_out->u_file += u_there;
        }
        if (v->end == USER_STACK_TOP) {
            p_out->u_stack += u_pages;
        } else if (v->kind == VMA_FILE && b_exec) {
            p_out->u_exe += u_pages;
        } else if (v->flags & PTE_WRITE) {
            p_out->u_data += u_pages;
        }
    }
    p_out->u_tables = tables_under(p_as->pml4, 4, USER_PML4_ENTRIES);
    spin_unlock_irq(&p_as->lock, u_flags);
}

size_t addrspace_regions(struct addrspace *p_as, struct vma *p_out,
                         size_t sz_max)
{
    size_t sz_count = 0;
    uint64_t u_flags = spin_lock_irq(&p_as->lock);
    /* each the lowest of those above the last one taken */
    uint64_t u_above = 0;
    for (;;) {
        const struct vma *p_next = NULL;
        for (size_t sz_at = 0; sz_at < VMA_MAX; sz_at++) {
            const struct vma *v = &p_as->vmas[sz_at];
            if (v->kind != VMA_NONE && v->start >= u_above
                && (p_next == NULL || v->start < p_next->start)) {
                p_next = v;
            }
        }
        if (p_next == NULL) {
            break;
        }
        if (sz_count < sz_max) {
            p_out[sz_count] = *p_next;
        }
        sz_count++;
        u_above = p_next->end;
    }
    spin_unlock_irq(&p_as->lock, u_flags);
    return sz_count;
}
