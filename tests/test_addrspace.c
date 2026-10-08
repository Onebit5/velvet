// SPDX-License-Identifier: GPL-2.0-only
/*
 * tests/test_addrspace.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * per-process address spaces: what they share, what they own, and whether
 * letting one go really hands everything back.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdbool.h>

#define ARENA_PAGES 8192
static uint8_t *arena;
static uint8_t taken[ARENA_PAGES];
static uint64_t next_free = 1;
static uint64_t outstanding;            /* frames handed out and not returned */

/* -1 means as much as the arena holds. */
static long budget = -1;

uint64_t pmm_alloc_pages(size_t count)
{
    if (budget >= 0) {
        if (budget < (long)count) return 0;
        budget -= (long)count;
    }
    if (next_free + count > ARENA_PAGES) return 0;
    uint64_t phys = next_free * 4096;

    /* whatever was in it before, which is what a real allocator hands back. */
    memset(arena + phys, 0xde, count * 4096);
    for (size_t i = 0; i < count; i++) {
        if (taken[next_free + i]) { printf("FAIL: pmm handed out a live frame\n"); exit(1); }
        taken[next_free + i] = 1;
    }
    next_free += count;
    outstanding += count;
    return phys;
}
void pmm_free_pages(uint64_t phys, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        uint64_t idx = phys / 4096 + i;
        if (idx >= ARENA_PAGES || !taken[idx]) {
            printf("FAIL: freed frame %#lx which was not allocated (double free?)\n",
                   (unsigned long)(idx * 4096));
            exit(1);
        }
        taken[idx] = 0;
    }
    outstanding -= count;
}
void *pmm_phys_to_virt(uint64_t phys)
{
    return arena + phys;
}
uint64_t pmm_alloc(void)
{
    return pmm_alloc_pages(1);
}
void     pmm_free(uint64_t phys)
{
    pmm_free_pages(phys, 1);
}

/*
 * how many *extra* holders each frame has, which is what copy on write
 * needs and the real pmm keeps as one byte per frame. zero means one
 * owner, which is every frame nobody ever shared
 */
static uint8_t shares[ARENA_PAGES];

void pmm_ref(uint64_t phys)
{
    uint64_t i = phys / 4096;
    if (i < ARENA_PAGES) shares[i]++;
}
bool pmm_unref(uint64_t phys)
{
    uint64_t i = phys / 4096;
    if (i < ARENA_PAGES && shares[i] > 0) {
        shares[i]--;
        return false;
    }
    pmm_free_pages(phys, 1);
    return true;
}
/*
 * the real one answers false on a machine too small to hold the share
 * table, and a fork has to refuse rather than go ahead. the arena here
 * is always big enough
 */
static bool can_share = true;
bool pmm_can_share(void)
{
    return can_share;
}

unsigned pmm_shares(uint64_t phys)
{
    uint64_t i = phys / 4096;
    return (i < ARENA_PAGES) ? shares[i] : 0;
}

/*
 * nothing to flush on a host, but it has to exist for the fault handler
 * to link, and forgetting it on the real thing means a write that
 * faults for ever, since the tlb keeps saying read-only
 */
static int flushes;
void vmm_flush_page(uint64_t virt)
{
    (void)virt; flushes++;
}
/*
 * the slab caches turn an object pointer back into a physical address
 * by subtracting this, so it has to be where the arena really is
 */
uint64_t pmm_hhdm_offset(void)
{
    return (uint64_t)arena;
}

/* on a machine with one core there is nobody to tell */
void smp_tlb_shootdown(void)
{
}
void kprintf(const char *f, ...)
{
    (void)f;
}
void panic(const char *f, ...)
{
    (void)f; printf("PANIC\n"); exit(1);
}
void *kmalloc(size_t n)
{
    return malloc(n);
}
void kfree(void *p)
{
    free(p);
}

#include "mm/vmm.h"
#include "mm/addrspace.h"

static int failures;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); failures++; } } while (0)

static uint64_t kernel_space;

/*
 * addrspace_switch is the only part that touches a real cpu, so the
 * kernel build keeps it and the host build supplies this instead
 */
void addrspace_switch(struct addrspace *as)
{
    (void)as;
}

/* a program's page is refused when it would take the machine below its reserve. */
uint64_t pmm_free_bytes(void)
{
    return 1024ull * 1024 * 1024;
}

int main(void)
{
    arena = aligned_alloc(4096, (size_t)ARENA_PAGES * 4096);
    memset(arena, 0, (size_t)ARENA_PAGES * 4096);

    /* a stand-in kernel: a direct map high up, and the kernel image */
    kernel_space = vmm_new_address_space();
    vmm_map_range(kernel_space, 0xffff800000000000ull, 0, 4 * 1024 * 1024,
                  PTE_WRITE | PTE_NX);
    vmm_map_range(kernel_space, 0xffffffff80000000ull, 0x100000, 0x8000, 0);


    struct addrspace *a = addrspace_create(kernel_space);
    CHECK(a != NULL && a->pml4 != 0, "a space is born");

    CHECK(vmm_translate(a->pml4, 0xffff800000000000ull) == 0,
          "the direct map is visible in it");
    CHECK(vmm_translate(a->pml4, 0xffffffff80001000ull) == 0x101000,
          "and so is the kernel image, it must be, the stack is up there");
    CHECK(vmm_translate(a->pml4, 0x400000) == VMM_NO_MAPPING,
          "but the lower half starts empty");

    /*
     * shared by reference, not by copy: a change the kernel makes later
     * has to be visible in spaces that already exist
     */
    uint64_t late = pmm_alloc_pages(1);
    vmm_map_range(kernel_space, 0xffff800000400000ull, late, 4096, PTE_WRITE);
    CHECK(vmm_translate(a->pml4, 0xffff800000400000ull) == late,
          "a later kernel mapping shows up in a space made before it");

    addrspace_destroy(a);

    /*
     * everything outstanding now belongs to the kernel and nothing
     * else, which makes it the reference the leak check needs
     */
    uint64_t kernel_only = outstanding;

    /*
     * the frames come from the pmm the way elf_load gets them, so
     * destroying a space hands back exactly what was taken
     */
    struct addrspace *x = addrspace_create(kernel_space);
    struct addrspace *y = addrspace_create(kernel_space);

    uint64_t page_x = pmm_alloc_pages(1);
    uint64_t page_y = pmm_alloc_pages(1);
    vmm_map_range(x->pml4, 0x400000, page_x, 4096, PTE_USER | PTE_WRITE);
    vmm_map_range(y->pml4, 0x400000, page_y, 4096, PTE_USER | PTE_WRITE);

    CHECK(vmm_translate(x->pml4, 0x400000) == page_x, "x sees its own page");
    CHECK(vmm_translate(y->pml4, 0x400000) == page_y, "y sees a different one");
    CHECK(vmm_translate(x->pml4, 0x400000) != vmm_translate(y->pml4, 0x400000),
          "the same address means different memory in each, the whole point");

    CHECK(addrspace_frames(x) == 1, "x is holding one page");
    uint64_t more = pmm_alloc_pages(4);
    vmm_map_range(x->pml4, 0x800000, more, 4 * 4096, PTE_USER | PTE_WRITE);
    CHECK(addrspace_frames(x) == 5, "and five after a bigger mapping");
    CHECK(addrspace_frames(y) == 1, "without disturbing y's count");


    uint64_t before = outstanding;
    addrspace_destroy(y);
    CHECK(outstanding < before, "destroying a space returns frames");

    CHECK(vmm_translate(x->pml4, 0x400000) == page_x,
          "x still works after y was destroyed");
    CHECK(vmm_translate(x->pml4, 0xffff800000000000ull) == 0,
          "and the shared kernel half was not freed with y");

    addrspace_destroy(x);
    CHECK(outstanding == kernel_only,
          "with both spaces gone the pmm is exactly level again, nothing "
          "leaked, and nothing shared was taken with them");

    /* the kernel's own mappings must have survived all of that */
    CHECK(vmm_translate(kernel_space, 0xffff800000000000ull) == 0,
          "the kernel direct map is intact");
    CHECK(vmm_translate(kernel_space, 0xffffffff80001000ull) == 0x101000,
          "and so is the kernel image");
    CHECK(vmm_translate(kernel_space, 0xffff800000400000ull) == late,
          "including the mapping made while spaces existed");

    /*
     * the whole claim of copy on write is that forking is cheap and
     * stays correct, and both halves of that are checkable here: the
     * frames outstanding after a fork, and whether writing through one
     * space is visible in the other
     */
    {
        uint64_t level = outstanding;

        struct addrspace *p = addrspace_create(kernel_space);
        uint64_t code = pmm_alloc_pages(1);
        uint64_t data = pmm_alloc_pages(1);
        vmm_map_range(p->pml4, 0x400000, code, 4096, PTE_USER);
        vmm_map_range(p->pml4, 0x401000, data, 4096, PTE_USER | PTE_WRITE);

        uint8_t *code_bytes = pmm_phys_to_virt(code);
        uint8_t *data_bytes = pmm_phys_to_virt(data);
        memset(code_bytes, 0xc3, 4096);
        memset(data_bytes, 'a', 4096);

        uint64_t before_fork = outstanding;
        struct addrspace *c = addrspace_fork(p, kernel_space);
        CHECK(c != NULL, "a space can be forked");

        /* the tables are copied and the pages are not. */
        CHECK(outstanding == before_fork + 4,
              "forking costs the page tables and not one page of memory");

        CHECK(vmm_translate(c->pml4, 0x401000) == data,
              "the child points at the very same frame");
        CHECK(vmm_translate(c->pml4, 0x400000) == code, "for all of them");

        /* both sides gave up the right to write. */
        CHECK(!(vmm_flags(p->pml4, 0x401000) & PTE_WRITE),
              "the parent's writable page stopped being writable");
        CHECK(vmm_flags(p->pml4, 0x401000) & PTE_COW,
              "and says why, which is the difference between copying the "
              "page and killing the program");
        CHECK(!(vmm_flags(c->pml4, 0x401000) & PTE_WRITE),
              "and so did the child's");

        /*
         * a page that was already read-only is not marked: there is
         * nothing to copy on, because nobody may write it
         */
        CHECK(!(vmm_flags(p->pml4, 0x400000) & PTE_COW),
              "a read-only page needs no note, nobody will ever write it");
        CHECK(pmm_shares(data) == 1, "the frame knows it has two holders");


        uint64_t was = outstanding;
        CHECK(addrspace_fault(c, 0x401000, true, true),
              "a write to a shared page is handled rather than fatal");
        CHECK(outstanding == was + 1, "by taking exactly one new frame");

        uint64_t child_data = vmm_translate(c->pml4, 0x401000);
        CHECK(child_data != data, "the child has its own now");
        CHECK(vmm_flags(c->pml4, 0x401000) & PTE_WRITE, "and may write to it");
        CHECK(!(vmm_flags(c->pml4, 0x401000) & PTE_COW), "with the note gone");
        CHECK(memcmp(pmm_phys_to_virt(child_data), data_bytes, 4096) == 0,
              "holding a copy of what was there, which is the whole point");

        /* writing to it now must not reach the parent */
        memset(pmm_phys_to_virt(child_data), 'z', 4096);
        CHECK(data_bytes[0] == 'a',
              "and a write through the child leaves the parent alone");

        /*
         * the parent is the last holder, so its own first write costs
         * nothing at all, there is nobody left to protect it from
         */
        CHECK(pmm_shares(data) == 0, "the frame is down to one holder");
        was = outstanding;
        CHECK(addrspace_fault(p, 0x401000, true, true),
              "the parent's write is handled too");
        CHECK(outstanding == was,
              "and takes no frame, because the last holder needs no copy of "
              "anything, which is what stops a chain of forks leaving "
              "copies behind");
        CHECK(vmm_flags(p->pml4, 0x401000) & PTE_WRITE, "it just gets it back");

        /*
         * what is *not* a copy-on-write fault has to be refused, or a
         * wild pointer becomes a page silently conjured out of nowhere
         */
        CHECK(!addrspace_fault(p, 0x400000, true, true),
              "a write to a genuinely read-only page is still a real fault");
        CHECK(!addrspace_fault(p, 0x900000, true, false),
              "and so is one to an address nobody ever agreed to");
        CHECK(!addrspace_fault(p, 0x401000, false, true),
              "a read never needs one of these");

        addrspace_destroy(c);
        addrspace_destroy(p);
        CHECK(outstanding == level,
              "and with both gone the books are level, a shared frame "
              "freed twice is the failure this is really looking for");
    }

    /* forking, then letting the child go without it ever writing. */
    {
        uint64_t level = outstanding;
        struct addrspace *p = addrspace_create(kernel_space);
        uint64_t page = pmm_alloc_pages(1);
        vmm_map_range(p->pml4, 0x400000, page, 4096, PTE_USER | PTE_WRITE);

        struct addrspace *c = addrspace_fork(p, kernel_space);
        addrspace_destroy(c);
        CHECK(vmm_translate(p->pml4, 0x400000) == page,
              "a child that never wrote leaves the parent's page alone");

        /* and the parent may write again without paying for a copy */
        CHECK(addrspace_fault(p, 0x400000, true, true), "the parent takes it back");
        CHECK(vmm_translate(p->pml4, 0x400000) == page,
              "the same frame it always had");

        addrspace_destroy(p);
        CHECK(outstanding == level, "with nothing left over");
    }

    /* running out of memory halfway through a fork must leave nothing behind. */
    {
        uint64_t level = outstanding;
        struct addrspace *p = addrspace_create(kernel_space);
        /*
         * two pages far enough apart to need two page tables, so the
         * walk has somewhere to fail halfway down
         */
        uint64_t one = pmm_alloc_pages(1);
        uint64_t two = pmm_alloc_pages(1);
        vmm_map_range(p->pml4, 0x400000, one, 4096, PTE_USER | PTE_WRITE);
        vmm_map_range(p->pml4, 0x40000000, two, 4096, PTE_USER | PTE_WRITE);

        /*
         * enough for the new pml4 and a table or two, and then no
         * more, so the walk gets partway down and stops
         */
        budget = 3;
        struct addrspace *c = addrspace_fork(p, kernel_space);
        CHECK(c == NULL, "a fork with no memory left fails");
        budget = -1;
        addrspace_destroy(p);
        CHECK(outstanding == level,
              "and leaves nothing behind, the half-built tree is nobody's "
              "to free but its own");
    }

    /* a machine with nowhere to keep the counts cannot fork at all. */
    {
        struct addrspace *p = addrspace_create(kernel_space);
        can_share = false;
        CHECK(addrspace_fork(p, kernel_space) == NULL,
              "a fork with nowhere to count holders is refused");
        can_share = true;
        addrspace_destroy(p);
    }

    /* a fault could already mean "copy this page". */
    {
        uint64_t level = outstanding;
        struct addrspace *p = addrspace_create(kernel_space);
        uint64_t flags = PTE_USER | PTE_WRITE;

        /*
         * measured from here, so the space's own pml4 is not counted
         * as something a region cost
         */
        uint64_t empty = outstanding;

        CHECK(addrspace_add_region(p, 0x700000, 0x710000, flags,
                                   VMA_ANON, NULL, 0, 0),
              "a range can be agreed to");
        CHECK(outstanding == empty,
              "and agreeing to sixteen pages costs not one of them");
        CHECK(vmm_translate(p->pml4, 0x700000) == VMM_NO_MAPPING,
              "with nothing behind any of it yet");

        /* touching it is answerable */
        CHECK(addrspace_fault(p, 0x704abc, true, false),
              "touching a page inside it is answered rather than fatal");
        CHECK(vmm_translate(p->pml4, 0x704000) != VMM_NO_MAPPING,
              "and there is a page there now");
        CHECK(vmm_translate(p->pml4, 0x705000) == VMM_NO_MAPPING,
              "but only the one that was touched");

        /*
         * it has to be zeroes. handing a program a page with somebody
         * else's data still in it is the oldest leak there is
         */
        const uint8_t *fresh = pmm_phys_to_virt(
            vmm_translate(p->pml4, 0x704000));
        int nonzero = 0;
        for (int i = 0; i < 4096; i++) {
            if (fresh[i] != 0) nonzero++;
        }
        CHECK(nonzero == 0, "and it is zeroes, not whatever was in it before");

        /*
         * and what is *not* agreed to stays fatal, which is the entire
         * reason the record exists
         */
        CHECK(!addrspace_fault(p, 0x6fffff, true, false),
              "a page just below the range is still a wild pointer");
        CHECK(!addrspace_fault(p, 0x710000, true, false),
              "and so is one just above it, that is the guard page, and "
              "it costs nothing because there is nothing there");

        /* a read-only region must not be written into */
        CHECK(addrspace_add_region(p, 0x800000, 0x801000, PTE_USER,
                                   VMA_ANON, NULL, 0, 0),
              "a read-only range can be agreed to");
        CHECK(!addrspace_fault(p, 0x800000, true, false),
              "and a write to it is refused rather than quietly served");
        CHECK(addrspace_fault(p, 0x800000, false, false),
              "though a read of it is fine");

        /*
         * overlapping records are refused: two of them disagreeing
         * about one address is worse than not having the second
         */
        CHECK(!addrspace_add_region(p, 0x708000, 0x720000, flags,
                                    VMA_ANON, NULL, 0, 0),
              "a range overlapping one already there is refused");

        /* taking it back frees whatever was ever touched */
        uint64_t before_drop = outstanding;
        CHECK(addrspace_drop_region(p, 0x700000), "a range can be taken back");
        CHECK(outstanding == before_drop - 1,
              "and the one page that was made goes back with it");
        CHECK(vmm_translate(p->pml4, 0x704000) == VMM_NO_MAPPING,
              "with nothing left mapped");
        CHECK(!addrspace_fault(p, 0x704000, true, false),
              "and touching it is a wild pointer again");
        CHECK(!addrspace_drop_region(p, 0x700000), "dropping it twice does nothing");
        CHECK(!addrspace_drop_region(p, 0x704000),
              "and it goes by the address it was made at, not one inside it");

        addrspace_destroy(p);
        CHECK(outstanding == level, "and nothing leaked");
    }

    /* the same record, with somewhere for the bytes to come from. */
    {
        uint64_t level = outstanding;
        static uint8_t fake_image[8192];
        for (int i = 0; i < 8192; i++) {
            fake_image[i] = (uint8_t)(i & 0xff);
        }

        struct addrspace *p = addrspace_create(kernel_space);

        /*
         * a segment of 6000 bytes in a range of two pages: the tail of
         * the second page is bss, and one page is both
         */
        CHECK(addrspace_add_region(p, 0x400000, 0x402000, PTE_USER,
                                   VMA_FILE, fake_image, 0, 0x400000 + 6000),
              "a file-backed range can be agreed to");

        CHECK(addrspace_fault(p, 0x400000, false, false), "the first page arrives");
        const uint8_t *page0 = pmm_phys_to_virt(vmm_translate(p->pml4, 0x400000));
        CHECK(memcmp(page0, fake_image, 4096) == 0,
              "holding the bytes that were in the image");

        CHECK(addrspace_fault(p, 0x401000, false, false), "and so does the second");
        const uint8_t *page1 = pmm_phys_to_virt(vmm_translate(p->pml4, 0x401000));
        CHECK(memcmp(page1, fake_image + 4096, 6000 - 4096) == 0,
              "with the bytes that exist");
        int tail_nonzero = 0;
        for (int i = 6000 - 4096; i < 4096; i++) {
            if (page1[i] != 0) tail_nonzero++;
        }
        CHECK(tail_nonzero == 0,
              "and zeroes past where the file stopped, one page being both "
              "is exactly what the end of a segment looks like");

        addrspace_destroy(p);
        CHECK(outstanding == level, "with nothing left over");
    }


    {
        uint64_t level = outstanding;
        struct addrspace *p = addrspace_create(kernel_space);
        uint64_t flags = PTE_USER | PTE_WRITE;

        uint64_t empty = outstanding;

        uint64_t a = addrspace_reserve(p, 4096, flags);
        uint64_t b = addrspace_reserve(p, 100 * 4096, flags);
        CHECK(a != 0 && b != 0, "two ranges can be asked for");
        CHECK(a != b, "and they are different");
        CHECK(b >= a + 4096 || a >= b + 100 * 4096, "and do not overlap");
        CHECK(outstanding == empty,
              "and a hundred and one pages of address space cost not one "
              "page of memory, which is what makes asking for a lot "
              "reasonable rather than rude");

        CHECK(addrspace_fault(p, b + 50 * 4096, true, false),
              "touching the middle of one works");
        CHECK(vmm_translate(p->pml4, b + 50 * 4096) != VMM_NO_MAPPING,
              "and makes exactly that page");

        /* a length that would not fit anywhere */
        CHECK(addrspace_reserve(p, 0, flags) == 0, "asking for nothing gets nothing");

        CHECK(addrspace_drop_region(p, b), "and it can be given back");
        CHECK(!addrspace_fault(p, b + 50 * 4096, true, false),
              "after which it is nobody's memory again");

        addrspace_destroy(p);
        CHECK(outstanding == level, "with nothing leaked");
    }

    /*
     * a child whose stack cannot grow is a child that dies the first
     * time it calls anything
     */
    {
        uint64_t level = outstanding;
        struct addrspace *p = addrspace_create(kernel_space);
        addrspace_add_region(p, 0x700000, 0x710000, PTE_USER | PTE_WRITE,
                             VMA_ANON, NULL, 0, 0);

        struct addrspace *c = addrspace_fork(p, kernel_space);
        CHECK(c != NULL, "a space with a region can be forked");
        CHECK(addrspace_fault(c, 0x707000, true, false),
              "and the child can still grow into it");
        CHECK(vmm_translate(c->pml4, 0x707000) != VMM_NO_MAPPING,
              "getting a page of its own");
        CHECK(vmm_translate(p->pml4, 0x707000) == VMM_NO_MAPPING,
              "which the parent knows nothing about");

        addrspace_destroy(c);
        addrspace_destroy(p);
        CHECK(outstanding == level, "and nothing leaked");
    }


    before = outstanding;
    struct addrspace *empty = addrspace_create(kernel_space);
    addrspace_destroy(empty);
    CHECK(outstanding == before, "an unused space costs nothing once freed");

    if (!failures) printf("all good\n");
    return failures;
}
