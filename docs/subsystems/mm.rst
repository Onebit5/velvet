memory
======

``mm/`` is everything between "the loader said there is memory
here" and "a program has its own view of it".

the physical frame allocator
----------------------------

``pmm.c`` hands out physical frames and hands them back. under it is a
buddy allocator (``buddy.c``) rather than the linear bitmap scan the
earlier versions used.

why a buddy allocator
   the old pmm walked a bitmap looking for a run of free bits, which is
   fine until the run is long or the memory is fragmented, and gets slower
   the fuller it becomes. a buddy keeps a free list per block size, so
   taking a block is following one pointer. the trick is on the way back:
   every block has exactly one partner it could have been split from.
   found by flipping one bit of its address, so freeing asks "is its buddy
   also free?" and if so merges into one block of the next size up, then
   asks again. large blocks reassemble themselves without anyone keeping a
   list of what was split from what.

   the cost is rounding: ask for five pages and you get eight. that waste
   is real and is counted honestly, so ``/proc/meminfo`` shows it.

the interface leaks one consequence: sizes are rounded up to a power of
two, so **the count you free with must be the count you allocated with**,
that is what says which list the block belongs on. ``PAGE_SIZE`` is 4096,
frame 0 is never handed out, and 0 is therefore safe as the "no" answer.

``pmm_init_from_map()`` is the real entry point; ``pmm_init()`` is the
loader plumbing around it. splitting them is what lets the host suite
fabricate a memory map and poke at the allocator with no machine involved.

frames with more than one owner
-------------------------------

copy on write means two address spaces pointing at one frame, and whichever
ends first must not hand it back while the other is still reading it. so a
frame can be *shared*: ``pmm_ref()`` adds a holder, ``pmm_unref()`` removes
one and frees only when the last lets go. the count kept is of *extra*
holders, so zero means one owner and the ordinary path costs nothing but a
byte's worth of lookup.

on a machine too small to have paid for the share table,
``pmm_can_share()`` returns false, and a ``fork`` must refuse rather than go
ahead, without it a frame with two owners is freed by whichever finishes
first.

giving the loader its memory back
---------------------------------

``pmm_reclaim_bootloader()`` takes back the ~1 MiB the loader's page tables,
structures and stack occupy. three things have to be true first: the kernel
must be on its own page tables, anything still wanted must have been copied
out of the loader's structures, and **nothing may still be standing on the
loader's stack**.

that last one is why init's is its own thread. ``kmain`` runs on the stack
the loader handed it, so it creates the thread that brings the system up on
a pmm-allocated stack and then genuinely exits; the boot thread dies, the scheduler moves on, and
only then is that memory free.

the sharp edge: the loader's *responses* live in that memory too, so every
``*_request.response`` becomes a dangling pointer the moment the reclaim
returns. everything that needs them reads them during early boot, long
before. and the reclaimable regions sit *above* the last usable one on a
typical pc, so a bitmap sized only to cover usable ram has no bits for those
frames and reclaiming them silently does nothing at all. the bitmap covers
both.

the heap
--------

``slab.c`` is object caches for the things a kernel allocates over and over.
a general heap has to ask "how big?" and "where does it fit?" every single
time, but a kernel spends most of its allocations on a handful of structs
whose size never changes. a thread, a process, an address space, and for
those both questions have the same answer every time. a slab cache answers
them once: take a page, cut it into objects of exactly that size, thread a
free list through the ones nobody is using.

each page carries a small header saying which cache it belongs to, which is
what lets ``slab_free()`` take a bare pointer and work out where it came
from (mask off the low twelve bits) and what lets a page whose objects have
all come home go back to the pmm rather than being held forever. caches are
caller-owned structs. usually a static in whichever file allocates the
thing, so there is no chicken-and-egg with the heap.

``kmalloc.c`` is the general heap on top: a first-fit free list that grabs
whole pages from the pmm when it runs low, 16-byte aligned payloads, and
headers with a magic so ``kfree()`` can tell an honest pointer from a lie.

page tables
-----------

``vmm.c`` builds and walks four levels of page tables. pml4, pdpt, pd, pt,
which is what the kernel actually runs on after ``vmm_init()``. the
flags worth knowing:

- ``PTE_NX`` needs ``EFER.NXE`` or it is a *reserved bit* and faults on
  every access rather than doing nothing. that is why ``vmm_nx()`` is a
  runtime decision. ``CR0.WP`` is the same shape: without it ring 0 may
  scribble on read-only pages regardless of what the tables say. see
  `mmu_enforce_write_protect <arch.rst>`_.
- ``PTE_COW`` is a bit the cpu ignores entirely. it is the "the read-only
  bit above is a lie told on purpose" bit, without it a copy-on-write
  page and a genuinely read-only one are indistinguishable at fault time,
  and the difference is copying the page versus killing the program.

``vmm_init()`` walks its own tables in software and refuses to load cr3
unless the kernel, the direct map, the framebuffer, the page tables and
**the stack being stood on** all resolve to the addresses they should with
the permissions they should. switching cr3 is the one operation here with no
diagnostics when it goes wrong. a bad entry is a triple fault, no message,
no register dump, so panicking with an explanation beats rebooting in
silence.

the direct map covers all of physical memory with 2 MiB pages. the kernel
image is mapped a section at a time with only the rights each one needs:

::

    hhdm        0xffff800000000000 ..  rw-   all of physical memory, 2MiB pages
    the loader  0xffffffff80000000 ..  r--   the request markers
    text        0xffffffff80001000 ..  r-x   executable, not writable
    rodata      0xffffffff80008000 ..  r--   neither
    data        0xffffffff8000c000 ..  rw-   writable, never executable

every function here takes the pml4 by physical address rather than assuming
the current one, which is what lets the host tests build a whole address
space over a ``malloc``'d arena and inspect it with no cpu involved.

address spaces, demand paging and copy on write
-----------------------------------------------

``addrspace.c`` gives each program a private view. only the lower half
differs; the upper half. the kernel and the direct map. is shared by
reference, because the stack being stood on when a space is switched is up
there.

a page fault stopped being purely an error in 0.2.11, when it could mean
"copy this page". it can also mean "there was never a page here yet, and
there was always going to be one". the difference between that and a wild
pointer is a *record*. a VMA, in ``addrspace.h``. saying which addresses
are legitimately empty. every not-present fault is answered out of that
table or not at all, so a stack that wants to grow and a program
dereferencing nonsense are told apart.

- ``addrspace_map_anon()`` records a range of anonymous memory without
  mapping it, which is the whole of an anonymous ``mmap``, and
  ``addrspace_discard()`` gives a range's pages back and keeps the range,
  which is ``MADV_DONTNEED``.
- ``addrspace_unmap()`` and ``addrspace_protect()`` take any range, cutting
  a region where the range crosses its edge, so a hole in the middle of a
  region leaves two, and ``addrspace_brk()`` grows and shrinks the heap
  above the program's last segment. anonymous neighbours with the same
  flags merge, so a heap grown a page at a time is still one region.
- every segment of a program is a region, whether its pages arrive as
  they are touched or were read in at exec, because the region is what
  says what a page there may be. a page still shared after a fork is
  copied to be written only when its region allows writing.
- ``addrspace_fault()`` either makes a private copy of a
  copy-on-write page, or populates a page inside an agreed region, or
  reports that the fault was real.
- ``addrspace_fork()`` copies the page *tables* and nothing else: every
  writable page is marked read-only and ``PTE_COW``, the frame gains a
  second holder, and the first write on either side faults and becomes
  private. see `fork and copy on write <sched.rst>`_.
- ``addrspace_destroy()`` frees the lower half only, and only once nothing
  is running on it. the reaper does it from another thread after the
  switch away has happened.

running out of memory
---------------------

``pressure.c`` is the policy, kept in a file of its own because it is a
decision about numbers and the allocator is a data structure.

the rule: **a program in ring 3 may not take the last of the memory.** a
reserve (``PRESSURE_RESERVE``, 256 pages) is held back that only the kernel
may draw on, so that when a program asks for more than there is, *the
program* fails, and the kernel still has enough to write the file, print
the message and reap the process.

without it the order of failure is decided by whoever happens to ask next,
which is how a machine that should have lost one compile loses the ability
to tell you about it.

guard pages and stack overflow
------------------------------

every thread stack is allocated one page larger than it needs, and that
bottom page is unmapped. a thread that runs off the end hits the hole and
takes a page fault naming itself, instead of quietly chewing through
whatever the pmm handed out next.

a guard page is only half the story. when a thread runs off its stack,
``rsp`` is already inside the unmapped page by the time the fault happens,
so the cpu's attempt to push an exception frame faults too, which is a
double fault, and with nowhere to push that either it becomes a triple
fault, which on real hardware reboots the machine with no message. the fix
is the tss's interrupt stack table, which gives the double-fault vector a
stack of its own. see `the x86 parts <arch.rst>`_.

punching a 4 KiB hole into a 2 MiB direct-map page means splitting that page
into 512 small ones with identical flags first, which ``vmm_unmap_page()``
does on demand. the stack comes back the same way round: the guard page gets
re-mapped before the frames go back to the pmm, because whoever gets them
next will expect to be able to reach them.

stats
-----

``/proc/meminfo`` is the pmm and heap figures: total, used, free, peak,
metadata cost, and ``buddy_blocks_at()`` per order. the shape of the
free memory, not just how much of it there is. a machine with plenty free
and none of it contiguous is a machine about to fail a large allocation.
``pressure_available()`` is the honest answer to "how much is left" for a
program, which is not the same number as the free count.

``/proc/slabinfo`` is every object cache, with what it holds against what
it uses, and ``mm/meminfo.c`` writes both. ``/proc/vmm`` is the kernel's
page tables, shown by where one of each kind of thing lands, and it is
root's alone, since it is a map of where the kernel keeps things.

the addrspace interface
-----------------------

a program's memory. the lower half of the tables belongs to it and the upper
half is the kernel's, shared by reference so the kernel is reachable
whichever tables are loaded, which it has to be: the stack the kernel stands
on when it switches is up there. what a space has agreed to is a table of
regions, and a fault inside one of them is a page arriving rather than a
program going wrong.

``#define VMA_MAX 64``
    how many regions one space may hold. a program that wants more has to be
    told no rather than quietly given a smaller machine.

``bool addrspace_add_region(as, start, end, flags, kind, image, image_offset, file_end)``
    agree to a range without mapping any of it. the pages arrive on first
    touch, which is what makes a large stack or a large allocation cost
    nothing until it is used.

``uint64_t addrspace_map_anon(as, hint, len, flags, fixed)``
    somewhere `len` bytes of memory with no file behind it will fit,
    agreed to: at the hint when it is free, or at the hint and over
    whatever was there when fixed. nothing is mapped below 64 KiB, where
    NULL points.

``bool addrspace_discard(struct addrspace *as, uint64_t start, uint64_t len)``
    every page made in a range given back, the regions kept: the next
    touch makes each again, as zeroes or from the image, the way it was
    made the first time. false when part of the range is in no region.

``bool addrspace_unmap(struct addrspace *as, uint64_t start, uint64_t len)``
    nothing in a range any more, whichever regions it cut through. false
    only when a hole needs a slot there is no room for, with nothing changed

``bool addrspace_protect(struct addrspace *as, uint64_t start, uint64_t len, uint64_t flags)``
    a range allowed to be flags from now on, the pages already there with
    it. false when part of it is in no region

``uint64_t addrspace_brk(struct addrspace *as, uint64_t want, uint64_t flags)``
    the program break moved: where it is afterwards.

``struct addrspace *addrspace_create(uint64_t kernel_pml4)``
    a new space with the kernel half already visible and nothing else.

``struct addrspace *addrspace_fork(const struct addrspace *from, uint64_t kernel_pml4)``
    a copy that shares every page with the original rather than copying any
    of them, with copy on write doing the rest.

``void addrspace_destroy(struct addrspace *as)``
    free the lower half: the program's pages, its stack, and the tables that
    described them. the upper half is shared by everybody and is not this
    space's to free. only safe once nothing is running on it, which is why
    the reaper does it from another thread after the switch away has already
    happened.

``void addrspace_switch(struct addrspace *as)``
    make this the live space on this core.

``uint64_t addrspace_frames(struct addrspace *as)``
    how many frames the space is holding, which is one of the numbers
    ``/proc/threads`` reports.

``const uint8_t *image``, ``uint64_t image_offset``, ``uint64_t file_end``
    for a region backed by a file: where the bytes come from, where in the
    file this region starts, and where the file stops. past that end the
    pages are zero rather than read, which is what a stack's guard or an
    over-sized mapping gets.

``uint64_t pages_in_use``
    how many pages the program has actually been given, as opposed to how
    many it has been promised.

the buddy interface
-------------------

the frame allocator underneath the pmm. a free list per size, so taking a
block is following one pointer, and freeing merges with the one block it
could have been split from, found by flipping one bit of its address, so
large blocks reassemble without a list of what came from what. the cost is
rounding: ask for five frames and get eight.

``uint64_t buddy_metadata_bytes(uint64_t frames)``
    what the bitmaps cost for a given number of frames. this is asked before
    there is anywhere to put them, which is why it is a function rather than
    a constant.

``void buddy_init(uint64_t frames, void *metadata, void *(*to_virt)(uint64_t frame))``
    set up over frames 0..frames, with ``metadata`` pointing at that many
    bytes of scratch. ``to_virt`` is how the allocator reaches the frames it
    is describing, since it runs before the direct map exists.

``void buddy_add_range(uint64_t first_frame, uint64_t count)``
    hand it a run of frames, splitting it into the largest aligned blocks
    that fit. the memory map's usable regions are neither aligned nor powers
    of two, which is why this takes a run rather than a size.

``unsigned buddy_order_for(uint64_t frames)``
    the smallest order that holds that many frames.

``bool buddy_is_free_block(uint64_t frame, unsigned order)``
    is this exact block sitting on a free list?

``uint64_t buddy_alloc(unsigned order)``
    take a block of 2^order frames.

``uint64_t buddy_blocks_at(unsigned order)``
    how many blocks are on a free list, so ``/proc/meminfo`` can show the
    shape of the free memory rather than only how much of it there is: a
    machine with plenty free and none of it contiguous is a machine about
    to fail a large allocation.

the kmalloc interface
---------------------

the kernel heap. requests are sorted by size rather than walked from the
front of one free list, so a request that fits a size class comes from a
slab cache and has nothing to search, and anything larger takes whole pages
from the pmm with a header on the front. payloads are 16-byte aligned and
carry that header, which is what lets a free tell an honest pointer from a
lie.

``void *kmalloc(size_t size)``
    a block, or NULL when the machine is out of memory. NULL is a real answer
    here and every caller checks it, which is the price of the reserve in
    pressure.h being the thing that runs out first.

``void kfree(void *ptr)``
    give it back. freeing something this did not allocate panics rather than
    corrupting the heap quietly, since a heap that is wrong is a machine
    that fails somewhere unrelated, later.

``uint64_t kheap_total_bytes(void)``
    how much has been handed out, headers included. that is the honest
    number, and it is the one ``/proc/meminfo`` prints.

the pmm interface
-----------------

frames of physical memory. the buddy allocator from buddy.h does the
choosing and this is the bookkeeping around it: which frames are described,
who is holding each one, and what the bootloader left behind. sizes round up
to a power of two, so the count freed has to be the count allocated.

``void pmm_init_from_map(const struct ph_memmap_entry *entries, size_t count, uint64_t hhdm)``
    the real work, split from the boot plumbing so a host test can hand it a
    memory map written by hand.

``uint64_t pmm_alloc_pages(size_t count)``
    a run of contiguous frames, or 0.

``uint64_t pmm_alloc(void)``
    one frame, which is the common case and the only one most callers want.

``void pmm_ref(uint64_t phys)``, ``bool pmm_unref(uint64_t phys)``
    another holder of a frame, and one fewer. the frame is freed only when
    the last holder lets go, and ``unref`` says whether it did.

``unsigned pmm_shares(uint64_t phys)``
    how many extra holders a frame has.

``bool pmm_can_share(void)``
    whether anything may be shared yet, which is false until the tables and
    the trap handler that catches a write to a shared page are both up.

``void *pmm_phys_to_virt(uint64_t phys)``
    a usable pointer for a physical address, through the direct map.

``uint64_t pmm_hhdm_offset(void)``
    where the loader mirrored physical memory for the kernel.

``uint64_t pmm_highest_address(void)``
    the top of what is worth having in the direct map, across every memory
    map entry that is not reserved or broken. the holes up at the 1 TiB mark
    are deliberately left out: mapping them would cost megabytes of page
    tables and buy nothing.

``uint64_t pmm_reclaim_bootloader(void)``
    hand back the memory the loader was using for itself: its page tables,
    its stack, its structures, once nothing points into them any more.

``uint64_t pmm_metadata_bytes(void)``
    what the allocator's own bookkeeping costs.

``bool pmm_translate_is_tracked(uint64_t phys)``
    is this address covered by the allocator at all?

``uint64_t pmm_peak_bytes(void)``
    the most memory that has ever been in use at once, which is the number
    that says how close the machine came.

the pressure interface
----------------------

what happens when the machine runs out. a reserve is held back that only the
kernel may draw on, so a program asking for more than exists is the thing
that fails, and the kernel still has enough left to write the file, print the
message and reap the process. without it the order of failure is decided by
whoever asks next, and a machine that should have lost one compile loses the
ability to say so.

``#define PRESSURE_RESERVE 256``
    how many pages are kept back from ring 3. enough for a few filesystem
    buffers, a stack for the reaper and the console. a bigger reserve wastes
    memory a program could have used, and a smaller one is a machine that
    dies politely instead of a program that dies noisily.

``bool pressure_allow(uint64_t free_pages, size_t want, enum pressure_who who)``
    may this allocation go ahead? the caller says who is asking, because
    that is the entire question.

``uint64_t pressure_available(uint64_t free_pages)``
    how much a program may still ask for, in pages.

the slab interface
------------------

object caches for the things a kernel allocates over and over. a page is cut
into objects of one size with a free list threaded through the ones nobody is
using, so allocating is taking the head of a list rather than searching. each
page carries a header saying which cache it belongs to and how many of its
objects are out, which is what lets a page whose objects have all come home
go back to the pmm instead of being held forever.

``void slab_cache_init(struct slab_cache *cache, const char *name, size_t obj_size)``
    set a cache up. the struct belongs to the caller, usually a static beside
    the thing being allocated, which avoids a chicken-and-egg with the heap.
    calling it twice on the same cache is ignored, since two threads reaching
    the same initialiser is not a reason to die.

``void slab_free(void *object)``
    note there is no cache argument: the page the object sits on says which
    cache it is, so a caller cannot pass the wrong one.

``int slab_owns(const void *object)``
    is this pointer the start of a live object? this is what keeps a stale
    pointer from being taken for a good one.

``struct slab_cache *slab_first_cache(void)``
    walk the registered caches, which is what ``/proc/slabinfo`` does.

the vmm interface
-----------------

the page tables: building them, walking them, and the flags, of which some
mean something to the cpu and the rest mean something only to us.

``#define PTE_PRESENT (1ull << 0)``
    the architectural flags.

``#define PTE_COW (1ull << 9)``
    bits 9, 10 and 11 are not defined by the architecture, so they belong to
    whoever is writing the tables. the cpu ignores them entirely, which is
    what makes them free to use.

``#define VMM_NO_MAPPING UINT64_MAX``
    what a walk says when nothing is mapped there.

``void vmm_init(void)``
    build the kernel's own address space, check that it would actually work,
    and move onto it.

``uint64_t vmm_new_address_space(void)``
    a fresh empty pml4, as a physical address, or 0.

``bool vmm_map_range(uint64_t pml4, uint64_t virt, uint64_t phys, uint64_t size, uint64_t flags)``
    map a range, rounding the address, the frame and the size out to page
    boundaries.

``uint64_t vmm_translate(uint64_t pml4, uint64_t virt)``
    walk the tables in software, for a diagnostic rather than for a load.

``uint64_t vmm_flags(uint64_t pml4, uint64_t virt)``
    and the flags on whatever maps the address, or 0.
