// SPDX-License-Identifier: GPL-2.0-only
/*
 * include/uapi/velvet/ext4.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * what ioctl asks of a mounted ext4 filesystem.
 */

#ifndef UAPI_VELVET_EXT4_H
#define UAPI_VELVET_EXT4_H

/* grow the filesystem to the count of its own blocks the argument points at */
#define EXT4_IOC_RESIZE_FS 0x40086610

#endif
