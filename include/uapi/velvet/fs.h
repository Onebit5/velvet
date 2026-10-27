// SPDX-License-Identifier: GPL-2.0-only
/*
 * include/uapi/velvet/fs.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * what a block device is asked: its size, its sectors, and to write out
 * what is held for it.
 */

#ifndef UAPI_VELVET_FS_H
#define UAPI_VELVET_FS_H

#define BLKGETSIZE   0x1260         /* how many 512-byte sectors, a long */
#define BLKFLSBUF    0x1261         /* what is held for it, onto it */
#define BLKSSZGET    0x1268         /* the sector the drive moves, an int */
#define BLKPBSZGET   0x127b         /* and the one it stores, an int */
#define BLKGETSIZE64 0x80081272     /* how many bytes, a 64-bit count */

#endif
