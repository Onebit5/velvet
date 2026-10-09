// SPDX-License-Identifier: GPL-2.0-only
/*
 * drivers/ahci.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the sata controller: commands, waits, and the block device.
 */

#include <velvet/ahci.h>
#include <velvet/pci.h>
#include <velvet/pmm.h>
#include <velvet/vmm.h>
#include <velvet/kprintf.h>
#include <velvet/string.h>



struct hba_port {
    volatile uint32_t clb, clbu;    /* where the command list is */
    volatile uint32_t fb, fbu;      /* where the drive posts its replies */
    volatile uint32_t is, ie;
    volatile uint32_t cmd;
    volatile uint32_t reserved0;
    volatile uint32_t tfd;          /* what the drive's status register says */
    volatile uint32_t sig;          /* what kind of thing is attached */
    volatile uint32_t ssts, sctl, serr, sact;
    volatile uint32_t ci;           /* one bit per command slot */
    volatile uint32_t sntf, fbs;
    volatile uint32_t reserved1[11];
    volatile uint32_t vendor[4];
};

struct hba_mem {
    volatile uint32_t cap, ghc, is, pi, vs;
    volatile uint32_t ccc_ctl, ccc_pts, em_loc, em_ctl, cap2, bohc;
    volatile uint8_t  reserved[0xa0 - 0x2c];
    volatile uint8_t  vendor[0x100 - 0xa0];
    struct hba_port   ports[32];
};

struct cmd_header {
    uint8_t  cfl_flags;             /* low 5 bits: fis length in dwords */
    uint8_t  flags2;
    uint16_t prdtl;                 /* how many scatter entries follow */
    volatile uint32_t prdbc;        /* how many bytes actually moved */
    uint32_t ctba, ctbau;
    uint32_t reserved[4];
};

struct prdt_entry {
    uint32_t dba, dbau;             /* a physical address, for the controller */
    uint32_t reserved;
    uint32_t dbc;                   /* byte count minus one, and an irq bit */
};

struct cmd_table {
    uint8_t  cfis[64];
    uint8_t  acmd[16];
    uint8_t  reserved[48];
    struct prdt_entry prdt[8];
};

/* the frame the drive receives: a register write, host to device */
struct fis_h2d {
    uint8_t fis_type;               /* 0x27 */
    uint8_t pmport_c;               /* bit 7 says this is a command */
    uint8_t command;
    uint8_t featurel;
    uint8_t lba0, lba1, lba2, device;
    uint8_t lba3, lba4, lba5, featureh;
    uint8_t countl, counth, icc, control;
    uint8_t reserved[4];
};

#define FIS_TYPE_REG_H2D 0x27

#define ATA_READ_DMA_EX  0x25
#define ATA_WRITE_DMA_EX 0x35
#define ATA_IDENTIFY     0xec

/*
 * FIXME: there is no flush here, and none below this driver either.
 * ahci.h offers init, a way to pick a disk, read and write, and nothing
 * else, so the block layer has no way to ask for durability even if
 * somebody wanted to. a write command completes the moment the drive
 * has taken the data into its own volatile cache, which means the three
 * flushes jbd2 performs around a transaction end in that cache and come
 * back saying done having reached nothing that survives a power cut. a
 * disk with a cache and no flush is not crash safe, it only looks it.
 * the fix is ATA_FLUSH_CACHE_EXT (0xea) issued through this same
 * command path with no data, an entry point for it in ahci.h, and a way
 * for disk.c to reach it; or SET FEATURES to turn the cache off, which
 * costs throughput and buys back the guarantee.
 */

#define PORT_CMD_ST   (1u << 0)
#define PORT_CMD_FRE  (1u << 4)
#define PORT_CMD_FR   (1u << 14)
#define PORT_CMD_CR   (1u << 15)

#define TFD_ERR  (1u << 0)
#define TFD_DRQ  (1u << 3)
#define TFD_BSY  (1u << 7)

#define GHC_AE   (1u << 31)         /* ahci enable */
#define GHC_HR   (1u << 0)          /* controller reset */

#define SIG_SATA 0x00000101

#define CLASS_STORAGE  0x01
#define SUBCLASS_SATA  0x06
#define PROGIF_AHCI    0x01

/* every wait is bounded. */
#define SPIN_LIMIT 20000000



static struct hba_mem  *hba;
static struct hba_port *port;

static struct cmd_header *cmd_list;     /* 32 headers */
static struct cmd_table  *cmd_tab;
static uint8_t           *bounce;       /* one page, for the dma */

static uint64_t cmd_list_phys, cmd_tab_phys, fis_phys, bounce_phys;

static bool     present;
static bool     addr64;
static uint64_t sector_count;
static char     model[41];
static char     serial[21];

bool ahci_present(void)
{
    return present;
}
const char *ahci_model(void)
{
    return model;
}
const char *ahci_serial(void)
{
    return serial;
}
uint64_t ahci_sectors(void)
{
    return sector_count;
}



static bool stop_port(void)
{
    port->cmd &= ~PORT_CMD_ST;
    port->cmd &= ~PORT_CMD_FRE;

    /* the controller may be in the middle of something. */
    for (uint64_t spin = 0; spin < SPIN_LIMIT; spin++) {
        if (!(port->cmd & (PORT_CMD_CR | PORT_CMD_FR))) {
            return true;
        }
    }
    return false;
}

static bool start_port(void)
{
    for (uint64_t spin = 0; spin < SPIN_LIMIT; spin++) {
        if (!(port->cmd & PORT_CMD_CR)) {
            port->cmd |= PORT_CMD_FRE;
            port->cmd |= PORT_CMD_ST;
            return true;
        }
    }
    return false;
}

/* wait for the drive to stop being busy */
static bool wait_ready(void)
{
    for (uint64_t spin = 0; spin < SPIN_LIMIT; spin++) {
        if (!(port->tfd & (TFD_BSY | TFD_DRQ))) {
            return true;
        }
    }
    return false;
}



static bool run_command(uint8_t command, uint64_t lba, uint32_t count,
                        uint32_t bytes, bool writing)
{
    if (!wait_ready()) {
        return false;
    }

    port->is = (uint32_t)-1;        /* clear anything left from last time */

    struct cmd_header *header = &cmd_list[0];
    header->cfl_flags = (uint8_t)((sizeof(struct fis_h2d) / 4) & 0x1f);
    if (writing) {
        header->cfl_flags |= (1u << 6);     /* this one moves data outward */
    }
    header->flags2 = 0;
    header->prdtl = 1;
    header->prdbc = 0;
    header->ctba = (uint32_t)cmd_tab_phys;
    header->ctbau = (uint32_t)(cmd_tab_phys >> 32);

    memset(cmd_tab, 0, sizeof *cmd_tab);
    cmd_tab->prdt[0].dba = (uint32_t)bounce_phys;
    cmd_tab->prdt[0].dbau = (uint32_t)(bounce_phys >> 32);
    cmd_tab->prdt[0].dbc = bytes - 1;       /* the count is off by one, always */

    struct fis_h2d *fis = (struct fis_h2d *)cmd_tab->cfis;
    memset(fis, 0, sizeof *fis);
    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->pmport_c = 0x80;           /* this is a command, not an update */
    fis->command = command;
    fis->lba0 = (uint8_t)lba;
    fis->lba1 = (uint8_t)(lba >> 8);
    fis->lba2 = (uint8_t)(lba >> 16);
    fis->lba3 = (uint8_t)(lba >> 24);
    fis->lba4 = (uint8_t)(lba >> 32);
    fis->lba5 = (uint8_t)(lba >> 40);
    fis->device = (command == ATA_IDENTIFY) ? 0 : (1u << 6);   /* lba mode */
    fis->countl = (uint8_t)count;
    fis->counth = (uint8_t)(count >> 8);

    /*
     * and that is the whole of it: one bit, and the controller goes and
     * moves the data on its own
     */
    port->ci = 1u << 0;

    /*
     * XXX: success here is one bit. the controller writes how many bytes
     * it actually moved into prdbc, which this file zeroes before every
     * command and then never reads, and it records interface and data
     * errors in serr, which is cleared once at setup and never read at
     * all. a transfer that came up short therefore returns true, and the
     * caller hands its buffer on with whatever the command before it
     * left behind. compare prdbc against the byte count asked for, and
     * read serr, and treat a mismatch as the failure it is.
     */
    for (uint64_t spin = 0; spin < SPIN_LIMIT; spin++) {
        if (!(port->ci & 1u)) {
            return !(port->tfd & TFD_ERR);
        }
        if (port->is & (1u << 30)) {        /* task file error */
            return false;
        }
    }
    return false;       /* it never came back */
}



bool ahci_read(void *ctx, uint64_t lba, uint32_t count, void *buf)
{
    (void)ctx;
    if (!present || count == 0 || count > AHCI_MAX_SECTORS) {
        return false;
    }
    if (lba + count > sector_count) {
        return false;
    }
    if (!run_command(ATA_READ_DMA_EX, lba, count, count * AHCI_SECTOR, false)) {
        return false;
    }
    memcpy(buf, bounce, (size_t)count * AHCI_SECTOR);
    return true;
}

bool ahci_write(void *ctx, uint64_t lba, uint32_t count, const void *buf)
{
    (void)ctx;
    if (!present || count == 0 || count > AHCI_MAX_SECTORS) {
        return false;
    }
    if (lba + count > sector_count) {
        return false;
    }
    memcpy(bounce, buf, (size_t)count * AHCI_SECTOR);
    return run_command(ATA_WRITE_DMA_EX, lba, count, count * AHCI_SECTOR, true);
}



/*
 * ata strings are ascii with every pair of bytes the wrong way round, a
 * detail inherited from a sixteen-bit bus and never fixed
 */
static void ata_string(const uint16_t *words, size_t count, char *out)
{
    size_t n = 0;
    for (size_t i = 0; i < count; i++) {
        out[n++] = (char)(words[i] >> 8);
        out[n++] = (char)(words[i] & 0xff);
    }
    out[n] = '\0';
    while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == '\0')) {
        out[--n] = '\0';
    }
}

static bool identify(void)
{
    if (!run_command(ATA_IDENTIFY, 0, 1, AHCI_SECTOR, false)) {
        return false;
    }

    const uint16_t *words = (const uint16_t *)bounce;
    ata_string(&words[27], 20, model);
    ata_string(&words[10], 10, serial);

    /*
     * word 83 bit 10 says it understands 48-bit addressing, which is
     * what tells the kernel whether words 100..103 mean anything
     */
    if (words[83] & (1u << 10)) {
        sector_count = (uint64_t)words[100]
                     | ((uint64_t)words[101] << 16)
                     | ((uint64_t)words[102] << 32)
                     | ((uint64_t)words[103] << 48);
    } else {
        sector_count = (uint64_t)words[60] | ((uint64_t)words[61] << 16);
    }
    return sector_count > 0;
}



/* where in the kernel's address space the registers get mapped. */
#define AHCI_MMIO_VIRT 0xffffffffd0000000ull

static const struct pci_device *find_controller(void)
{
    for (size_t i = 0; i < pci_count(); i++) {
        const struct pci_device *d = pci_at(i);
        if (d->class_code == CLASS_STORAGE && d->subclass == SUBCLASS_SATA
            && d->prog_if == PROGIF_AHCI) {
            return d;
        }
    }
    return NULL;
}

/* every port with a real disk on it. */
static uint8_t usable[AHCI_MAX_DISKS];
static size_t usable_count;

static void find_ports(void)
{
    uint32_t implemented = hba->pi;
    usable_count = 0;

    for (int i = 0; i < 32 && usable_count < AHCI_MAX_DISKS; i++) {
        if (!(implemented & (1u << i))) {
            continue;
        }
        struct hba_port *p = &hba->ports[i];

        /* det == 3 means a device is there and communicating; ipm == 1 that it is awake. */
        uint32_t det = p->ssts & 0x0f;
        uint32_t ipm = (p->ssts >> 8) & 0x0f;
        if (det != 3 || ipm != 1) {
            continue;
        }
        if (p->sig != SIG_SATA) {
            continue;   /* an atapi drive or a port multiplier, not a disk */
        }
        usable[usable_count++] = (uint8_t)i;
    }
}

size_t ahci_disk_count(void)
{
    return usable_count;
}

/* point the one set of command structures at a different port. */
bool ahci_use_disk(size_t which)
{
    if (which >= usable_count) {
        return false;
    }
    present = false;
    port = &hba->ports[usable[which]];

    memset(cmd_list, 0, PAGE_SIZE);
    memset(cmd_tab, 0, PAGE_SIZE);
    memset(pmm_phys_to_virt(fis_phys), 0, PAGE_SIZE);

    if (!stop_port()) {
        return false;
    }

    port->clb  = (uint32_t)cmd_list_phys;
    port->clbu = (uint32_t)(cmd_list_phys >> 32);
    port->fb   = (uint32_t)fis_phys;
    port->fbu  = (uint32_t)(fis_phys >> 32);
    port->serr = (uint32_t)-1;      /* write ones to clear */
    port->is   = (uint32_t)-1;
    port->ie   = 0;                 /* the kernel polls; no interrupts wanted */

    if (!start_port()) {
        return false;
    }

    present = true;
    if (!identify()) {
        present = false;
        return false;
    }
    return true;
}

bool ahci_init(void)
{
    present = false;

    const struct pci_device *dev = find_controller();
    if (dev == NULL) {
        return false;
    }

    /*
     * FIXME: nothing here turns bus mastering on. the controller cannot
     * reach memory to do dma without bit 2 of the pci command register,
     * and this driver never sets it and never checks it, so it works
     * only because the firmware that booted the machine happened to
     * leave it set. the network driver next door enables it and says
     * why, and pci.h calls that bit the one that matters. the memory
     * space bit wants setting for the same reason on the way past,
     * since every register below is reached through a bar.
     */
    /* bar 5 is the one that points at the registers */
    struct pci_bar bar = pci_decode_bar(dev->bar[5]);
    if (bar.is_io || bar.address == 0) {
        return false;
    }

    if (!vmm_map_range(vmm_kernel_pml4(), AHCI_MMIO_VIRT, bar.address,
                       2 * PAGE_SIZE, PTE_WRITE | PTE_NO_CACHE | vmm_nx())) {
        return false;
    }
    hba = (struct hba_mem *)AHCI_MMIO_VIRT;

    hba->ghc |= GHC_AE;             /* talk ahci, not legacy ide */
    addr64 = (hba->cap & (1u << 31)) != 0;

    find_ports();
    if (usable_count == 0) {
        return false;               /* a controller, but nothing plugged in */
    }

    /* three pages: the command list, the frames the drive sends back, and one command table. */
    cmd_list_phys = pmm_alloc();
    fis_phys      = pmm_alloc();
    cmd_tab_phys  = pmm_alloc();
    bounce_phys   = pmm_alloc();
    if (cmd_list_phys == 0 || fis_phys == 0
        || cmd_tab_phys == 0 || bounce_phys == 0) {
        return false;
    }

    /*
     * a controller that cannot address memory above 4 GiB must not be
     * handed a buffer up there. rather than fight the allocator, say so
     * and carry on without a disk
     */
    if (!addr64 && (cmd_list_phys >> 32 || fis_phys >> 32
                    || cmd_tab_phys >> 32 || bounce_phys >> 32)) {
        kprintf("ahci       : controller is 32-bit only and its buffers are not\n");
        return false;
    }

    cmd_list = pmm_phys_to_virt(cmd_list_phys);
    cmd_tab  = pmm_phys_to_virt(cmd_tab_phys);
    bounce   = pmm_phys_to_virt(bounce_phys);

    /* the ports are found but none is chosen. */
    return true;
}
