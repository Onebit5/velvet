// SPDX-License-Identifier: GPL-2.0-only
/*
 * arch/x86_64/machine.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the machine: reboot, halt, and the firmware's handoff.
 */

#include <asm/machine.h>
#include <asm/io.h>
#include <asm/gdt.h>
#include <asm/idt.h>
#include <asm/pic.h>
#include <asm/interrupts.h>
#include <asm/smp.h>
#include <asm/tss.h>
#include <asm/syscall.h>
#include <velvet/serial.h>
#include <velvet/keyboard.h>
#include <velvet/mouse.h>
#include <velvet/pit.h>
#include <velvet/pci.h>
#include <velvet/e1000.h>
#include <velvet/net.h>
#include <velvet/kprintf.h>
#include <velvet/vmm.h>
#include <asm/irq.h>
#include <asm/cpu.h>
#include <stdint.h>

/*
 * the three ways this box answers to something other than software, and
 * the end of the line.
 *
 * these used to be `system.c`, mixed in with the farewell text that gets
 * printed before a reboot. the text is not architecture and never was,
 * it is what this kernel says when it stops, and it now lives with the
 * code that decides to stop, which is init. what is left here is three
 * pokes at a motherboard.
 */

void cpu_stop(void)
{
    irq_disable();
    for (;;) {
        cpu_idle();
    }
}

void machine_reset(void)
{
    irq_disable();
    outb(0x64, 0xfe);   /* pulse the 8042 reset line, the traditional way */

    /*
     * if that did not take, sit here in the dark. there is nothing to
     * return to, whoever called this has already said goodbye
     */
    cpu_stop();
}

bool machine_poweroff(void)
{
    irq_disable();

    outw(0x604,  0x2000);   /* qemu, and anything modern enough */
    outw(0xb004, 0x2000);   /* older qemu / bochs */
    outw(0x4004, 0x3400);   /* virtualbox */

    /*
     * still here, so nobody was listening. saying so is worth more than
     * halting quietly: a machine that sits there with the fan running
     * looks broken, and "close the window" is a complete answer
     */
    return false;
}

bool machine_key_pressed(void)
{
    /*
     * interrupts are off and never coming back when this is asked, so
     * the keyboard driver is no help, it is built entirely around an
     * interrupt that will not arrive. this talks to the 8042 directly
     */
    if (inb(0x64) & 1) {
        uint8_t scancode = inb(0x60);
        /*
         * bit 7 set means a key came *up*, which is probably just
         * somebody releasing whatever they were holding when it all went
         * wrong. waiting for a press means waiting for a decision
         */
        if (!(scancode & 0x80)) {
            return true;
        }
    }

    /* and over the serial line, for anyone driving this headless */
    if (inb(0x3f8 + 5) & 1) {
        inb(0x3f8);
        return true;
    }

    return false;
}


/*
 * every line below is a step in bringing a pc up, in this order, and the
 * order is the interesting part: the gdt before the idt because an
 * interrupt gate names a code segment, the pic before anything that
 * registers a handler, the tss after the pmm because it wants stacks.
 *
 * none of that is wrong and none of it is portable. a kernel with one
 * machine can write its boot sequence out longhand in main.c; a kernel
 * with two finds that main.c has become a machine description with a
 * kernel wrapped round it
 */

void machine_bring_up_early(void)
{
    /* first, so that everything after it has somewhere to complain to */
    serial_init();

    gdt_init();
    idt_init();
    pic_init();
    keyboard_init();
    mouse_init();
    serial_input_init();
}

void machine_bring_up_late(void)
{
    kprintf("gdt         : loaded, tss slot reserved for later\n");
    kprintf("idt         : 256 gates armed, exceptions get caught now\n");
    kprintf("pic         : 8259 remapped to vectors 32-47, ghosts filtered\n");
    kprintf("keyboard    : ps/2 on irq1, me layout, listening\n");
    kprintf("serial in   : com1 on irq4, and what is typed there counts\n");
    kprintf("timer       : pit channel 0 at %u hz, %ums per tick\n",
            PIT_HZ, 1000 / PIT_HZ);
    kprintf("\n");

    kprintf("building its own page tables:\n");
    vmm_init();

    /* needs the pmm for its stacks, so it waits until now */
    tss_init();
    idt_set_ist(8, IST_DOUBLE_FAULT);
    kprintf("  -> tss loaded, double faults land on their own stack\n");

    syscall_init();
    kprintf("  -> syscall/sysret armed, ring 3 has a way in\n\n");

    /*
     * before init reclaims the loader's memory, since the ramdisk was
     * read out of it
     */
    pci_scan();
    kprintf("pci        : %zu devices on the bus\n", pci_count());

    /*
     * the second device found by walking pci rather than by knowing
     * where it is. after the disk none of that is new, which is the
     * point of having done it once properly
     */
    if (e1000_init()) {
        char mac[20];
        mac_format(e1000_mac(), mac, sizeof mac);
        kprintf("network    : intel %s, hardware address %s\n",
                e1000_model(), mac);
    } else {
        kprintf("network    : no card. this machine is alone\n");
    }
}

void machine_start_clock(void)
{
    pit_init();

    /*
     * and now, if the firmware will say where they are, move every
     * interrupt off the 8259 and onto the apics. this is lateral on its
     * own, the same interrupts by a better road, and it is the
     * thing a second cpu would need. if acpi tells the kernel nothing the kernel stays
     * on the old chip, which works perfectly well
     */
    if (!interrupts_use_apic()) {
        kprintf("interrupts : staying on the 8259 and the pit\n");
    }

    /*
     * and then wake everything else this machine has. they climb out
     * into long mode, say which core they are, and halt, giving them
     * work needs locks that do not exist yet. before init reclaims the
     * loader's memory, because the page they start on is in it
     */
    smp_init(interrupts_acpi(), interrupts_timer_rate());
}
