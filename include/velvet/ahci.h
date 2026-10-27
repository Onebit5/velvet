// SPDX-License-Identifier: GPL-2.0-only
/*
 * include/velvet/ahci.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the sata controller, which is how a real disk is reached.
 */

#ifndef VELVET_AHCI_H
#define VELVET_AHCI_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* the sata controller, which is how a real disk is reached. */

#define AHCI_SECTOR 512

#define AHCI_MAX_DISKS 8

/*
 * find a controller on the pci bus and start every drive on it: whether
 * there is at least one
 */
bool ahci_init(void);

size_t ahci_disk_count(void);

/*
 * the ctx a read or a write for drive `which` is given, or NULL if there
 * is no such drive: the drives are numbered from 0 in the order their
 * ports are
 */
void *ahci_drive(size_t which);

/* a drive, as it describes itself. empty strings if unknown */
const char *ahci_model(size_t which);
const char *ahci_serial(size_t which);

/* how many 512-byte sectors it has */
uint64_t ahci_sectors(size_t which);

/*
 * move sectors to and from the drive ctx names. the caller holds the
 * disk layer's lock: there is one buffer every transfer goes through
 */
bool ahci_read(void *ctx, uint64_t lba, uint32_t count, void *buf);
bool ahci_write(void *ctx, uint64_t lba, uint32_t count, const void *buf);

#define AHCI_MAX_SECTORS 8

#endif
