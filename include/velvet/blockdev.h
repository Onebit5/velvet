// SPDX-License-Identifier: GPL-2.0-only
/*
 * include/velvet/blockdev.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * a drive or a partition, open.
 */

#ifndef VELVET_BLOCKDEV_H
#define VELVET_BLOCKDEV_H

#include <stdint.h>

struct file;

/*
 * the drive or partition numbered u_rdev, opened for u_mode, positioned
 * at its first byte, with one reference: NULL when there is no such one
 * or no memory
 */
struct file *blockdev_open(uint32_t u_rdev, uint32_t u_mode);

#endif
