// SPDX-License-Identifier: GPL-2.0-only
/*
 * include/uapi/velvet/uio.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * a buffer in pieces, as readv and writev take one.
 */

#ifndef UAPI_VELVET_UIO_H
#define UAPI_VELVET_UIO_H

#include <stdint.h>

struct iovec {
    uint64_t iov_base;
    uint64_t iov_len;
};

#define IOV_MAX 1024

#endif
