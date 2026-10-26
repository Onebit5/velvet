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

/* the lower half is per-process, the upper half is everyone's. */
#define USER_PML4_ENTRIES   256

/* what cr3 holds, one per core. */
static uint64_t live_pml4[CPU_MAX];

static uint64_t *table_at(uint64_t phys)
{
    return pmm_phys_to_virt(phys);
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

struct addrspace *addrspace_fork(const struct addrspace *from,
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

    const uint64_t *parent = table_at(from->pml4);
    uint64_t *mine = table_at(as->pml4);

    for (size_t i = 0; i < USER_PML4_ENTRIES; i++) {
        if (!(parent[i] & PTE_PRESENT)) {
            continue;
        }
        uint64_t child = copy_level(parent[i] & PTE_ADDR_MASK, 3);
        if (child == 0) {
            addrspace_destroy(as);
            return NULL;
        }
        mine[i] = child | (parent[i] & ~PTE_ADDR_MASK);
    }

    /* the records come too, and where the break and the environment are */
    for (size_t i = 0; i < VMA_MAX; i++) {
        as->vmas[i] = from->vmas[i];
    }
    as->u_brk_start = from->u_brk_start;
    as->u_brk = from->u_brk;
    as->u_env_start = from->u_env_start;
    as->u_env_end = from->u_env_end;

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

    /*
     * the parent's own entries just changed under it, and every core
     * that has run it may be holding a translation that still says
     * writable
     */
    /*
     * XXX: those entries were rewritten with nothing holding them.
     * copy_level() clears PTE_WRITE and sets PTE_COW in the parent's own
     * tables, and another core running a thread of the parent can be
     * faulting on the same tables, or writing through a translation
     * that still says writable until the shootdown below. a write in
     * that window is not copied, so the child sees it. a single threaded
     * process never reaches it; the failure mode is a parent that forks
     * while another of its threads is writing. quiesce the address space
     * first, or take a per address space lock and make the shootdown
     * fence the writers.
     */
    smp_tlb_shootdown();
    return as;
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

bool addrspace_add_region(struct addrspace *as, uint64_t start, uint64_t end,
                          uint64_t flags, enum vma_kind kind,
                          const uint8_t *image, uint64_t image_offset,
                          uint64_t file_end)
{
    if (as == NULL || end <= start) {
        return false;
    }

    start &= ~(PAGE_SIZE - 1);
    end = (end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    struct vma *slot = NULL;
    for (size_t i = 0; i < VMA_MAX; i++) {
        if (as->vmas[i].kind == VMA_NONE) {
            if (slot == NULL) {
                slot = &as->vmas[i];
            }
            continue;
        }
        /*
         * two records disagreeing about one address is worse than
         * refusing to make the second
         */
        if (start < as->vmas[i].end && end > as->vmas[i].start) {
            return false;
        }
    }
    if (slot == NULL) {
        return false;
    }

    slot->kind = kind;
    slot->start = start;
    slot->end = end;
    slot->flags = flags;
    slot->image = image;
    slot->image_offset = image_offset;
    slot->file_end = file_end;
    return true;
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

/* every page made in [start, end) given back */
static void drop_pages(struct addrspace *p_as, uint64_t u_start,
                       uint64_t u_end)
{
    for (uint64_t u_addr = u_start; u_addr < u_end; u_addr += PAGE_SIZE) {
        uint64_t *p_pte = leaf_for(p_as->pml4, u_addr);
        if (p_pte == NULL || !(*p_pte & PTE_PRESENT)) {
            continue;
        }
        uint64_t u_phys = *p_pte & PTE_ADDR_MASK;
        *p_pte = 0;
        pmm_unref(u_phys);
        vmm_flush_page(u_addr);
        if (p_as->pages_in_use > 0) {
            p_as->pages_in_use--;
        }
    }
}

bool addrspace_unmap(struct addrspace *p_as, uint64_t u_start, uint64_t u_len)
{
    if (p_as == NULL || u_len == 0 || (u_start & (PAGE_SIZE - 1)) != 0
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
    drop_pages(p_as, u_start, u_end);
    smp_tlb_shootdown();
    return true;
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

bool addrspace_protect(struct addrspace *p_as, uint64_t u_start,
                       uint64_t u_len, uint64_t u_flags)
{
    if (p_as == NULL || u_len == 0 || (u_start & (PAGE_SIZE - 1)) != 0
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
            }
        }
    }
    smp_tlb_shootdown();
    return true;
}

bool addrspace_discard(struct addrspace *p_as, uint64_t u_start,
                       uint64_t u_len)
{
    if (p_as == NULL || u_len == 0 || (u_start & (PAGE_SIZE - 1)) != 0
        || u_start + PAGE_UP(u_len) < u_start) {
        return false;
    }
    uint64_t u_end = u_start + PAGE_UP(u_len);
    if (!range_covered(p_as, u_start, u_end)) {
        return false;
    }
    drop_pages(p_as, u_start, u_end);
    smp_tlb_shootdown();
    return true;
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
    return addrspace_add_region(p_as, u_start, u_end, u_flags, VMA_ANON,
                                NULL, 0, 0);
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

uint64_t addrspace_map_anon(struct addrspace *p_as, uint64_t u_hint,
                            uint64_t u_len, uint64_t u_flags, bool b_fixed)
{
    if (p_as == NULL || u_len == 0 || PAGE_UP(u_len) < u_len) {
        return 0;
    }
    u_len = PAGE_UP(u_len);

    if (b_fixed) {
        if ((u_hint & (PAGE_SIZE - 1)) != 0 || u_hint < MAP_MIN
            || u_hint + u_len < u_hint || u_hint + u_len > USER_TOP
            || !addrspace_unmap(p_as, u_hint, u_len)) {
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

uint64_t addrspace_brk(struct addrspace *p_as, uint64_t u_want,
                       uint64_t u_flags)
{
    if (p_as == NULL) {
        return 0;
    }
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
        if (!addrspace_unmap(p_as, u_new_top, u_old_top - u_new_top)) {
            return p_as->u_brk;
        }
    }
    p_as->u_brk = u_want;
    return u_want;
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

bool addrspace_fault(struct addrspace *as, uint64_t virt, bool write,
                     bool present)
{
    if (as == NULL) {
        return false;
    }

    if (!present) {
        /*
         * nothing here yet. the only thing that makes this answerable
         * rather than fatal is a record saying the range was agreed to
         */
        struct vma *v = region_for(as, virt);
        if (v == NULL) {
            return false;
        }
        /*
         * a region that allows nothing is there to be in the way, and a
         * page made for it would only fault again
         */
        if (!(v->flags & PTE_USER)) {
            return false;
        }
        if (write && !(v->flags & PTE_WRITE)) {
            return false;   /* a write to a region that was never writable */
        }
        return populate(as, v, virt);
    }

    if (!write) {
        return false;       /* a read of a present page never faults on the kernel */
    }

    uint64_t *pte = leaf_for(as->pml4, virt);
    if (pte == NULL || !(*pte & PTE_PRESENT) || !(*pte & PTE_COW)) {
        return false;       /* not the kernel's. a real fault */
    }

    /*
     * shared after a fork, and its region since made read-only: the copy
     * would hand the program a writable page it was told it may not have
     */
    const struct vma *p_region = region_for(as, virt);
    if (p_region == NULL || !(p_region->flags & PTE_WRITE)) {
        return false;
    }

    uint64_t old = *pte & PTE_ADDR_MASK;

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
    if (pmm_shares(old) == 0) {
        *pte = (*pte | PTE_WRITE) & ~PTE_COW;
        vmm_flush_page(virt);
        return true;
    }

    uint64_t fresh = pmm_alloc_pages(1);
    if (fresh == 0) {
        return false;       /* out of memory. it really is a fault now */
    }
    memcpy(table_at(fresh), table_at(old), PAGE_SIZE);

    *pte = fresh | ((*pte & ~PTE_ADDR_MASK) | PTE_WRITE);
    *pte &= ~PTE_COW;

    pmm_unref(old);
    vmm_flush_page(virt);
    return true;
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

void addrspace_destroy(struct addrspace *as)
{
    if (as == NULL) {
        return;
    }
    /* if this is somehow still loaded, get off it first. */
    if (live_pml4[cpu_id()] == as->pml4) {
        addrspace_switch(NULL);
    }

    /*
     * every core that ever ran a thread in this space may still be
     * holding translations out of tables the kernel is about to hand back to the
     * allocator
     */
    smp_tlb_shootdown();

    free_level(as->pml4, 4);
    slab_free(as);
}

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

#ifndef VELVET_HOSTED

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

#endif
