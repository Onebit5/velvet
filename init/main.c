// SPDX-License-Identifier: GPL-2.0-only
/*
 * init/main.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * where the kernel starts: the loader's handoff, then init.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <velvet/random.h>
#include <velvet/proc.h>
#include <velvet/meminfo.h>
#include <velvet/pci.h>
#include <velvet/diskinfo.h>
#include <velvet/epoch.h>
#include <velvet/rtc.h>
#include <velvet/boot.h>
#include <asm/irq.h>
#include <asm/machine.h>
#include <velvet/console.h>
#include <velvet/input.h>
#include <velvet/kprintf.h>
#include <velvet/panic.h>
#include <velvet/string.h>
#include <velvet/pmm.h>
#include <velvet/kmalloc.h>
#include <velvet/ramdisk.h>
#include <velvet/disk.h>
#include <velvet/vfs.h>
#include <velvet/init.h>
#include <velvet/sched.h>
#include <velvet/thread.h>
#include <generated/version.h>



/* put the fresh allocators through their paces at boot. */
static void memory_selftest(void)
{
    uint64_t free_before = pmm_free_bytes();

    /* pmm round trip: 8 frames, distinct patterns, verify, return */
    uint64_t frames[8];
    for (int i = 0; i < 8; i++) {
        frames[i] = pmm_alloc();
        if (frames[i] == 0) {
            panic("selftest: the pmm ran dry after %d pages", i);
        }
        memset(pmm_phys_to_virt(frames[i]), 0xa5 + i, PAGE_SIZE);
    }
    for (int i = 0; i < 8; i++) {
        uint8_t *p = pmm_phys_to_virt(frames[i]);
        for (int j = 0; j < PAGE_SIZE; j++) {
            if (p[j] != (uint8_t)(0xa5 + i)) {
                panic("selftest: frame %d forgot its pattern at byte %d", i, j);
            }
        }
        pmm_free(frames[i]);
    }
    if (pmm_free_bytes() != free_before) {
        panic("selftest: pmm books dont balance after round trip");
    }

    /*
     * heap round trip: mixed sizes incl one bigger than a whole page,
     * scribble, verify, free in shuffled order, books must balance
     */
    uint64_t used_before = kheap_used_bytes();
    size_t sizes[5] = { 24, 1000, 16384, 1, 512 };
    uint8_t *ptrs[5];
    for (int i = 0; i < 5; i++) {
        ptrs[i] = kmalloc(sizes[i]);
        if (ptrs[i] == NULL) {
            panic("selftest: kmalloc(%zu) said no", sizes[i]);
        }
        memset(ptrs[i], 0x30 + i, sizes[i]);
    }
    for (int i = 0; i < 5; i++) {
        for (size_t j = 0; j < sizes[i]; j++) {
            if (ptrs[i][j] != (uint8_t)(0x30 + i)) {
                panic("selftest: heap block %d got trampled at byte %zu", i, j);
            }
        }
    }
    int order[5] = { 2, 0, 4, 1, 3 };
    for (int i = 0; i < 5; i++) {
        kfree(ptrs[order[i]]);
    }
    if (kheap_used_bytes() != used_before) {
        panic("selftest: heap books dont balance after round trip");
    }

    /* the buddy's whole point is that memory comes back *together*, not merely back. */
    uint64_t big = pmm_alloc_pages(512);
    if (big == 0) {
        panic("selftest: 2 MiB contiguous is already gone. blocks are not merging");
    }
    pmm_free_pages(big, 512);
}

/* which console the calling thread belongs to. */
static unsigned which_console(void)
{
    struct thread *t = sched_current();
    return (t != NULL) ? t->console : console_active();
}

/* and whether it should go down the wire as well. */
static bool writing_to_the_shown_console(void)
{
    return which_console() == console_active();
}

void kmain(const struct boot_handoff *handoff)
{
    boot_take_handoff(handoff);

    /*
     * boot_take_handoff has already refused to come back if the struct
     * does not carry the boot magic, which is the only check worth making
     * this early, there is no console and no serial to complain to
     */

    /* the machine, brought up by whoever knows what this machine is. */
    machine_bring_up_early();

    const struct boot_framebuffer *fb = &boot_handoff()->fb;
    if (fb->width == 0) {
        kprintf("no video mode. serial only, which is enough to see by\n");
    } else {
        console_init(fb);
        if (!console_ready()) {
            kprintf("console refused %u bpp, serial only from here\n", fb->bpp);
        }
    }

    /*
     * from here until the scheduler is about to start, everything goes
     * to serial and the log but not to the screen. all of it is worth
     * having when something breaks and none of it is worth reading when
     * it doesnt
     */
    kprintf_to_console(false);

    kprintf("velvet v%s\n", VELVET_VERSION);
    kprintf("framebuffer : %ux%u @ %u bpp, pitch %lu bytes, at %016lx\n",
            fb->width, fb->height, fb->bpp, fb->pitch, fb->address);
    kprintf("font        : the vga 8x16, from linux\n");
    kprintf("kernel      : loaded at %p\n", (void *)kmain);

    pmm_init();
    memory_selftest();
    kprintf("  -> selftest: frames and heap blocks round-tripped, blocks merge, "
            "books balance\n");
    kprintf("memory      : %lu MiB free of %lu MiB, heap warmed to %lu KiB\n\n",
            pmm_free_bytes() / (1024 * 1024),
            pmm_total_bytes() / (1024 * 1024),
            kheap_total_bytes() / 1024);

    /* and the rest of the machine, which needed the allocators */
    machine_bring_up_late();

    /*
     * the first time anything is done with a device the kernel found
     * rather than merely counted: the drives, and what their tables say
     * they hold, named in /dev. which of them is mounted where is for
     * /init to say, and a machine with no disk is one with nothing to
     * mount
     */
    if (disk_scan()) {
        kprintf("disk       : %zu drives and partitions, in /dev\n",
                disk_block_count());
    } else {
        kprintf("disk       : none found. the archive is all there is, "
                "which is enough\n");
    }

    /* a drive carrying the boot table at sector 32 is a boot medium */
    {
        int i_home = disk_system_drive();
        if (i_home < 0) {
            kprintf("boot       : no boot medium found. this kernel was "
                    "started by something else\n");
        } else {
            kprintf("boot       : from sd%c, which carries the boot "
                    "table\n", 'a' + i_home);
        }
    }

    /*
     * the root is the archive the loader handed over, unpacked into
     * memory so a program may write to it, with the devices on it. a
     * system's /init mounts its disk and moves into it from there
     */
    ramdisk_init();
    {
        size_t sz_files = 0;
        if (!vfs_root_init(&sz_files)) {
            panic("no memory for a root to stand in");
        }
        kprintf("root       : the archive, %zu entries, in memory, with "
                "the devices at /dev\n", sz_files);
    }

    proc_init();
    mm_proc_init();
    sched_proc_init();
    pci_proc_init();
    fs_proc_init();
    kprintf("\n");

    /* from here on this function is a thread like any other */
    sched_init();

    /* the clock that preempts, and the other cores if this machine has any. */
    machine_start_clock();

    /* and the clock that *means* something. */
    {
        struct rtc_time boot_time;
        rtc_read(&boot_time);
        epoch_start(epoch_from_date(&boot_time));
    }

    /* and a seed for the random generator, now there is a clock to add */
    random_init();

    /* the console driver has to be able to ask who is writing before anything writes. */
    console_set_owner_hook(which_console);
    kprintf_serial_filter(writing_to_the_shown_console);

    /* the screen is the user's from here. */
    kprintf_to_console(true);

    kprintf("threads     : the wheel turns, %lums quantum\n",
            sched_quantum_ms());

    /* and this is the last thing kmain decides. */
    if (!init_boot()) {
        panic("no init. there is nobody to bring the machine up");
    }

    irq_enable();


    /* the boot thread's work is finished. */
    thread_exit(0);
}
