// SPDX-License-Identifier: GPL-2.0-only
/*
 * include/velvet/dev.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * /dev: the devices a program opens by name.
 */

#ifndef VELVET_DEV_H
#define VELVET_DEV_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define DEV_AT "/dev"

/* a drive or a partition is root's: a drive is every file on it, whoever's */
#define DEV_BLOCK_MODE 0600

struct file;

/* how many there are, and the name and permissions of each */
size_t dev_count(void);
const char *dev_name(size_t sz_index);
uint32_t dev_mode(size_t sz_index);

/* and its number, major << 8 | minor, as stat gives it */
uint32_t dev_rdev(size_t sz_index);

/* a drive or a partition, rather than a device read a character at a time */
bool dev_is_block(size_t sz_index);

/* which one is called str_name: true and its index, or false */
bool dev_find(const char *str_name, size_t *p_index);

/* the device opened for u_mode, with one reference, or NULL */
struct file *dev_open(size_t sz_index, uint32_t u_mode);

#endif
