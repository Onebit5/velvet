// SPDX-License-Identifier: GPL-2.0-only
/*
 * include/uapi/velvet/mman.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * what mmap and its relatives take.
 */

#ifndef UAPI_VELVET_MMAN_H
#define UAPI_VELVET_MMAN_H

#define PROT_NONE  0x0
#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define PROT_EXEC  0x4

#define MAP_SHARED          0x01
#define MAP_PRIVATE         0x02
#define MAP_SHARED_VALIDATE 0x03
#define MAP_TYPE            0x0f
#define MAP_FIXED           0x10
#define MAP_ANONYMOUS       0x20
#define MAP_GROWSDOWN       0x0100
#define MAP_DENYWRITE       0x0800
#define MAP_EXECUTABLE      0x1000
#define MAP_LOCKED          0x2000
#define MAP_NORESERVE       0x4000
#define MAP_POPULATE        0x8000
#define MAP_NONBLOCK        0x10000
#define MAP_STACK           0x20000
#define MAP_FIXED_NOREPLACE 0x100000

#define MADV_NORMAL     0
#define MADV_RANDOM     1
#define MADV_SEQUENTIAL 2
#define MADV_WILLNEED   3
#define MADV_DONTNEED   4
#define MADV_FREE       8

#define MREMAP_MAYMOVE 1
#define MREMAP_FIXED   2

#endif
