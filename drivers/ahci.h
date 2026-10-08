// SPDX-License-Identifier: GPL-2.0-only
/*
 * drivers/ahci.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the sata controller, which is how a real disk is reached.
 */

#ifndef DRIVERS_AHCI_H
#define DRIVERS_AHCI_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* the sata controller, which is how a real disk is reached. */

#define AHCI_SECTOR 512

#define AHCI_MAX_DISKS 8

/* find a controller on the pci bus and list the drives on it. */
bool ahci_init(void);

size_t ahci_disk_count(void);

/* choose the drive every read and write below will go to. */
bool ahci_use_disk(size_t which);

bool ahci_present(void);

/* the drive, as it describes itself. empty strings if unknown */
const char *ahci_model(void);
const char *ahci_serial(void);

/* how many 512-byte sectors it has */
uint64_t ahci_sectors(void);

/* move sectors to and from the disk. */
bool ahci_read(void *ctx, uint64_t lba, uint32_t count, void *buf);
bool ahci_write(void *ctx, uint64_t lba, uint32_t count, const void *buf);

#define AHCI_MAX_SECTORS 8

#endif
