// SPDX-License-Identifier: GPL-2.0-only
/*
 * drivers/e1000.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the intel 82540em network card.
 */

#include <velvet/e1000.h>
#include <asm/irq.h>
#include <velvet/pci.h>
#include <velvet/pmm.h>
#include <velvet/vmm.h>
#include <velvet/kprintf.h>
#include <velvet/string.h>
#include <velvet/spinlock.h>

/* NOTE: this file has never been run. */

#define CLASS_NETWORK  0x02
#define SUBCLASS_ETHER 0x00

/* the ones qemu can present. */
static const struct { uint16_t id; const char *name; } known[] = {
    { 0x100e, "82540EM" },
    { 0x1004, "82543GC" },
    { 0x10d3, "82574L" },
};



#define REG_CTRL   0x0000
#define REG_STATUS 0x0008
#define REG_ICR    0x00c0       /* what happened, and reading it clears */
#define REG_IMS    0x00d0       /* interrupt mask set */
#define REG_IMC    0x00d8       /* interrupt mask clear */
#define REG_RCTL   0x0100
#define REG_TCTL   0x0400
#define REG_RDBAL  0x2800
#define REG_RDBAH  0x2804
#define REG_RDLEN  0x2808
#define REG_RDH    0x2810
#define REG_RDT    0x2818
#define REG_TDBAL  0x3800
#define REG_TDBAH  0x3804
#define REG_TDLEN  0x3808
#define REG_TDH    0x3810
#define REG_TDT    0x3818
#define REG_RAL    0x5400       /* the card's own address */
#define REG_RAH    0x5404
#define REG_MTA    0x5200       /* the multicast table, 128 words */

#define CTRL_RST  (1u << 26)
#define CTRL_ASDE (1u << 5)
#define CTRL_SLU  (1u << 6)     /* set link up */

#define RAH_VALID (1u << 31)

#define RCTL_EN        (1u << 1)
#define RCTL_UPE       (1u << 3)    /* every unicast, not just the kernel's */
#define RCTL_MPE       (1u << 4)
#define RCTL_BAM       (1u << 15)   /* broadcast, which arp needs */
#define RCTL_SECRC     (1u << 26)   /* strip the crc, which is four bytes
                                     * of nobody's business */
#define RCTL_BSIZE_2048 0           /* the default, and an mtu fits */

#define TCTL_EN   (1u << 1)
#define TCTL_PSP  (1u << 3)     /* pad short packets to the 60-byte
                                 * minimum, so arp does not need to */

#define TXD_CMD_EOP (1u << 0)   /* this descriptor ends a frame */
#define TXD_CMD_IFCS (1u << 1)  /* and the card appends the crc */
#define TXD_CMD_RS  (1u << 3)   /* report when it has gone */
#define TXD_STAT_DD (1u << 0)   /* ...which it does by setting this */

#define RXD_STAT_DD  (1u << 0)
#define RXD_STAT_EOP (1u << 1)

/* the causes worth being woken for. */
#define ICR_LSC     (1u << 2)
#define ICR_RXDMT0  (1u << 4)
#define ICR_RXO     (1u << 6)
#define ICR_RXT0    (1u << 7)

#define ICR_WANTED  (ICR_LSC | ICR_RXDMT0 | ICR_RXO | ICR_RXT0)

/* how many descriptors, and how big each buffer is. */
#define RX_COUNT 32
#define TX_COUNT 32
#define BUF_SIZE 2048

/* the layout the card reads. */
struct __attribute__((packed)) rx_desc {
    uint64_t addr;
    uint16_t length;
    uint16_t checksum;
    uint8_t  status;
    uint8_t  errors;
    uint16_t special;
};

struct __attribute__((packed)) tx_desc {
    uint64_t addr;
    uint16_t length;
    uint8_t  cso;
    uint8_t  cmd;
    uint8_t  status;
    uint8_t  css;
    uint16_t special;
};

static volatile uint8_t *mmio;
static bool present;
static uint8_t irq_line;
static const char *model = "none";
static struct mac my_mac;

static struct rx_desc *rx_ring;
static struct tx_desc *tx_ring;
static uint64_t rx_ring_phys, tx_ring_phys;
static uint8_t *rx_buf[RX_COUNT];
static uint8_t *tx_buf[TX_COUNT];

static size_t rx_next;          /* where the driver will look next */
static size_t tx_next;          /* where the driver will write next */

/*
 * who to wake when something arrives, and whether the card has ever
 * managed to tell the project anything
 */
static void (*on_arrival)(void);
static bool interrupts_arrived;

static struct e1000_stats stats;

/*
 * the rings are touched from a shell command and, later, from anything
 * that sends, one lock over the whole card, which is the right grain
 * for a machine with one of them
 */
static struct spinlock e1000_lock = SPINLOCK("e1000", LOCK_RANK_DEVICE);

static uint32_t reg_read(unsigned off)
{
    return *(volatile uint32_t *)(mmio + off);
}

static void reg_write(unsigned off, uint32_t v)
{
    *(volatile uint32_t *)(mmio + off) = v;
}

void e1000_on_arrival(void (*fn)(void))
{
    on_arrival = fn;
}

bool e1000_interrupts_working(void)
{
    return interrupts_arrived;
}

uint8_t e1000_irq_line(void)
{
    return irq_line;
}

/*
 * everything this is allowed to do is here, and the shortness is the
 * design rather than an accident. reading the cause register is what
 * *clears* the interrupt, a level-triggered line that is not
 * acknowledged re-fires forever, and on a shared line that means the
 * machine stops. so: read it, decide whether it was ours, wake somebody.
 *
 * it takes no lock at all. the rings belong to the poll thread and are
 * not touched here; the cause register is read-to-clear and nothing else
 * reads it. a handler that took the card's lock would be a handler that
 * could arrive while the poll thread held it, on the same core, with
 * interrupts off having been the only thing preventing exactly that
 */
static void e1000_irq(void)
{

    uint32_t cause = reg_read(REG_ICR);
    if ((cause & ICR_WANTED) == 0) {
        /* a pci line is shared. */
        stats.not_ours++;
        return;
    }

    stats.interrupts++;
    interrupts_arrived = true;

    if (on_arrival != NULL) {
        on_arrival();
    }
}



static const struct pci_device *find_card(void)
{
    for (size_t i = 0; i < pci_count(); i++) {
        const struct pci_device *d = pci_at(i);
        if (d->vendor != E1000_VENDOR) {
            continue;
        }
        for (size_t k = 0; k < sizeof known / sizeof known[0]; k++) {
            if (d->device == known[k].id) {
                model = known[k].name;
                return d;
            }
        }
        /* the right vendor and a device the kernel has not heard of. */
        if (d->class_code == CLASS_NETWORK && d->subclass == SUBCLASS_ETHER) {
            model = "unknown intel";
            return d;
        }
    }
    return NULL;
}

/* the card's own address. */
static bool read_mac(void)
{
    uint32_t low = reg_read(REG_RAL);
    uint32_t high = reg_read(REG_RAH);

    if ((high & RAH_VALID) == 0) {
        return false;
    }
    my_mac.b[0] = (uint8_t)low;
    my_mac.b[1] = (uint8_t)(low >> 8);
    my_mac.b[2] = (uint8_t)(low >> 16);
    my_mac.b[3] = (uint8_t)(low >> 24);
    my_mac.b[4] = (uint8_t)high;
    my_mac.b[5] = (uint8_t)(high >> 8);
    return true;
}

/*
 * one page holds 256 receive descriptors or 256 transmit ones, so a
 * single page is more than enough for both rings and the buffers get
 * pages of their own
 */
static bool build_rings(void)
{
    uint64_t rx_phys = pmm_alloc();
    uint64_t tx_phys = pmm_alloc();
    if (rx_phys == 0 || tx_phys == 0) {
        return false;
    }
    rx_ring_phys = rx_phys;
    tx_ring_phys = tx_phys;
    rx_ring = pmm_phys_to_virt(rx_phys);
    tx_ring = pmm_phys_to_virt(tx_phys);
    memset(rx_ring, 0, PAGE_SIZE);
    memset(tx_ring, 0, PAGE_SIZE);

    /* two buffers per page, since a page is 4096 and a buffer is 2048 */
    for (size_t i = 0; i < RX_COUNT; i += 2) {
        uint64_t p = pmm_alloc();
        if (p == 0) {
            return false;
        }
        rx_buf[i]     = pmm_phys_to_virt(p);
        rx_buf[i + 1] = pmm_phys_to_virt(p + BUF_SIZE);
        rx_ring[i].addr     = p;
        rx_ring[i + 1].addr = p + BUF_SIZE;
        rx_ring[i].status = 0;
        rx_ring[i + 1].status = 0;
    }
    for (size_t i = 0; i < TX_COUNT; i += 2) {
        uint64_t p = pmm_alloc();
        if (p == 0) {
            return false;
        }
        tx_buf[i]     = pmm_phys_to_virt(p);
        tx_buf[i + 1] = pmm_phys_to_virt(p + BUF_SIZE);
        tx_ring[i].addr     = p;
        tx_ring[i + 1].addr = p + BUF_SIZE;
        /*
         * a transmit descriptor starts *done*, because "done" is what
         * the driver checks before reusing one and nothing has been
         * sent yet
         */
        tx_ring[i].status     = TXD_STAT_DD;
        tx_ring[i + 1].status = TXD_STAT_DD;
    }
    return true;
}

bool e1000_init(void)
{
    present = false;

    const struct pci_device *d = find_card();
    if (d == NULL) {
        return false;
    }

    /* let the card reach main memory. */
    if (!pci_enable(d, PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER)) {
        kprintf("e1000      : the card will not take bus mastering. "
                "it cannot reach memory\n");
        return false;
    }

    struct pci_bar bar = pci_decode_bar(d->bar[0]);
    if (bar.is_io || bar.address == 0) {
        kprintf("e1000     : bar 0 is not a memory window. no card\n");
        return false;
    }

    /*
     * the card's registers are a device, so the mapping must not be
     * cached, a write that sits in a cache is a write the card never
     * sees, and this is the same reason the framebuffer is mapped
     * uncached in vmm_init
     */
    mmio = pmm_phys_to_virt(bar.address);
    if (!vmm_map_range(vmm_kernel_pml4(), (uint64_t)(uintptr_t)mmio,
                       bar.address, 128 * 1024,
                       PTE_PRESENT | PTE_WRITE | PTE_NO_CACHE | vmm_nx())) {
        kprintf("e1000     : could not map its registers\n");
        return false;
    }

    /* a reset, and then wait for it to finish. */
    reg_write(REG_CTRL, reg_read(REG_CTRL) | CTRL_RST);
    for (int i = 0; i < 1000000; i++) {
        if ((reg_read(REG_CTRL) & CTRL_RST) == 0) {
            break;
        }
    }

    reg_write(REG_IMC, 0xffffffff);     /* no interrupts: this polls */
    (void)reg_read(REG_ICR);            /* and reading clears whatever
                                         * was already pending */

    if (!read_mac()) {
        kprintf("e1000     : the card will not say what its address is\n");
        return false;
    }

    /* the multicast table filters by hash and starts as rubbish. */
    for (unsigned i = 0; i < 128; i++) {
        reg_write(REG_MTA + i * 4, 0);
    }

    if (!build_rings()) {
        kprintf("e1000     : no memory for its descriptor rings\n");
        return false;
    }

    /* receive: the ring, then head and tail. */
    reg_write(REG_RDBAL, (uint32_t)rx_ring_phys);
    reg_write(REG_RDBAH, (uint32_t)(rx_ring_phys >> 32));
    reg_write(REG_RDLEN, RX_COUNT * (uint32_t)sizeof(struct rx_desc));
    reg_write(REG_RDH, 0);
    reg_write(REG_RDT, RX_COUNT - 1);
    rx_next = 0;

    reg_write(REG_TDBAL, (uint32_t)tx_ring_phys);
    reg_write(REG_TDBAH, (uint32_t)(tx_ring_phys >> 32));
    reg_write(REG_TDLEN, TX_COUNT * (uint32_t)sizeof(struct tx_desc));
    reg_write(REG_TDH, 0);
    reg_write(REG_TDT, 0);
    tx_next = 0;

    reg_write(REG_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC | RCTL_BSIZE_2048
                        | RCTL_UPE | RCTL_MPE);
    reg_write(REG_TCTL, TCTL_EN | TCTL_PSP);

    /* and tell it there is a cable */
    reg_write(REG_CTRL, reg_read(REG_CTRL) | CTRL_SLU | CTRL_ASDE);

    /*
     * three things have to be true and each of them is switched off by
     * default in a different place, which is why this is the part that
     * takes the trying:
     *
     *   the pci command register's interrupt-disable bit must be clear.
     *   it exists so firmware can quieten a device it is not driving,
     *   and nothing in this kernel had ever needed to clear it because
     *   no pci device here has used an interrupt before.
     *
     *   the line the firmware assigned has to be unmasked at the
     *   controller, and it is a *legacy* line, pci interrupts arrive
     *   through the same sixteen wires as everything else on this
     *   machine, which is why irq_install takes the same numbers.
     *
     *   and the card's own mask has to admit the causes the project care about,
     *   which is the one bit of it that is actually about ethernet.
     *
     * if any of that does not take, the fallback is that the poll thread
     * carries on waking on its timer, slower, and working. `ifconfig`
     * says which is happening rather than leaving it to be guessed
     */
    pci_enable(d, PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
    pci_disable(d, PCI_COMMAND_INTX_DISABLE);

    /*
     * before arming, not after: the card starts raising these the
     * moment the mask is written, and a counter cleared afterwards
     * loses the first one, which on a quiet wire may be the only one
     * for a while
     */
    memset(&stats, 0, sizeof stats);

    irq_line = d->irq_line;
    if (irq_line == 0 || irq_line >= 16) {
        /* out of range counts as none. */
        irq_line = 0;
        kprintf("e1000      : the firmware gave it no interrupt line. "
                "the poll thread will have to notice on its own\n");
    } else {
        irq_install(irq_line, e1000_irq);
        reg_write(REG_IMS, ICR_WANTED);
        kprintf("e1000      : irq %u\n", irq_line);
    }

    present = true;
    return true;
}

bool e1000_present(void)
{
    return present;
}
const struct mac *e1000_mac(void)
{
    return &my_mac;
}
const char *e1000_model(void)
{
    return model;
}

void e1000_get_stats(struct e1000_stats *out)
{
    uint64_t flags = spin_lock_irq(&e1000_lock);
    *out = stats;
    spin_unlock_irq(&e1000_lock, flags);
}



bool e1000_send(const void *frame, size_t len)
{
    if (!present || len == 0 || len > BUF_SIZE) {
        return false;
    }

    uint64_t flags = spin_lock_irq(&e1000_lock);

    struct tx_desc *d = &tx_ring[tx_next];

    /* the card sets DD when it has finished with a descriptor. */
    if ((d->status & TXD_STAT_DD) == 0) {
        stats.send_dropped++;
        spin_unlock_irq(&e1000_lock, flags);
        return false;
    }

    /*
     * NOTE: len is bounded by the mtu before it arrives, which is what makes
     * a 16-bit descriptor length correct rather than a truncation.
     */
    memcpy(tx_buf[tx_next], frame, len);
    d->length = (uint16_t)len;
    d->cmd    = TXD_CMD_EOP | TXD_CMD_IFCS | TXD_CMD_RS;
    d->status = 0;              /* the card will set DD when it has gone */

    tx_next = (tx_next + 1) % TX_COUNT;

    /* and only now move the tail. */
    reg_write(REG_TDT, (uint32_t)tx_next);

    stats.sent++;
    spin_unlock_irq(&e1000_lock, flags);
    return true;
}



size_t e1000_receive(void *out, size_t max)
{
    if (!present) {
        return 0;
    }

    uint64_t flags = spin_lock_irq(&e1000_lock);
    size_t got = 0;

    struct rx_desc *d = &rx_ring[rx_next];
    if ((d->status & RXD_STAT_DD) != 0) {
        size_t len = d->length;

        /*
         * FIXME: a frame larger than one buffer arrives as several
         * descriptors and only the last of them has EOP. the fragments
         * without it are dropped one at a time just below, which is
         * right, but nothing remembers that a frame is in progress, so
         * the final fragment is then delivered on its own as though it
         * were a whole frame, with a length read from that fragment
         * alone. the card is in promiscuous mode and hears jumbo frames
         * from whoever sends them, so this is reachable on a lan. drop
         * everything until EOP once a fragment without it has been
         * seen.
         */
        /* a frame spread over more than one descriptor. */
        if ((d->status & RXD_STAT_EOP) == 0 || len > max) {
            stats.receive_dropped++;
        } else {
            memcpy(out, rx_buf[rx_next], len);
            stats.received++;
            got = len;
        }

        d->status = 0;          /* the driver is finished with it */

        /*
         * the tail says "everything up to here is yours again", so it
         * trails one behind the descriptor about to be used
         */
        size_t tail = rx_next;
        rx_next = (rx_next + 1) % RX_COUNT;
        reg_write(REG_RDT, (uint32_t)tail);
    }

    spin_unlock_irq(&e1000_lock, flags);
    return got;
}
