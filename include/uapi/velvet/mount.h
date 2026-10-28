// SPDX-License-Identifier: GPL-2.0-only
/*
 * include/uapi/velvet/mount.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * what mount and umount2 are told, beyond where and what.
 */

#ifndef UAPI_VELVET_MOUNT_H
#define UAPI_VELVET_MOUNT_H

/*
 * the flags mount takes. the atime ones are taken and change nothing,
 * since no filesystem here keeps the time a file was last read; a flag
 * not named here is refused rather than taken and ignored
 */
#define MS_NOATIME      1024
#define MS_NODIRATIME   2048
#define MS_SILENT       32768
#define MS_RELATIME     (1 << 21)
#define MS_STRICTATIME  (1 << 24)

/* what an old program puts in the top half of the flags, and means nothing by */
#define MS_MGC_VAL      0xc0ed0000u
#define MS_MGC_MSK      0xffff0000u

/* the one flag umount2 takes: a link at the end is not followed */
#define UMOUNT_NOFOLLOW 8

#endif
