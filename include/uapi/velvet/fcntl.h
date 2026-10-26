// SPDX-License-Identifier: GPL-2.0-only
/*
 * include/uapi/velvet/fcntl.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * how a file is opened, and what can be asked of a descriptor.
 */

#ifndef UAPI_VELVET_FCNTL_H
#define UAPI_VELVET_FCNTL_H

#define O_ACCMODE   00000003
#define O_RDONLY    00000000
#define O_WRONLY    00000001
#define O_RDWR      00000002
#define O_CREAT     00000100
#define O_EXCL      00000200
#define O_NOCTTY    00000400
#define O_TRUNC     00001000
#define O_APPEND    00002000
#define O_NONBLOCK  00004000
#define O_DSYNC     00010000
#define O_DIRECT    00040000
#define O_LARGEFILE 00100000
#define O_DIRECTORY 00200000
#define O_NOFOLLOW  00400000
#define O_NOATIME   01000000
#define O_CLOEXEC   02000000
#define O_SYNC      04010000
#define O_PATH      010000000

/* fcntl's commands */
#define F_DUPFD         0
#define F_GETFD         1
#define F_SETFD         2
#define F_GETFL         3
#define F_SETFL         4
#define F_GETLK         5
#define F_SETLK         6
#define F_SETLKW        7
#define F_DUPFD_CLOEXEC 1030

#define FD_CLOEXEC 1

/* the *at calls: a directory to start from, and what to do at the end */
#define AT_FDCWD            (-100)
#define AT_SYMLINK_NOFOLLOW 0x100
#define AT_REMOVEDIR        0x200
#define AT_EACCESS          0x200
#define AT_SYMLINK_FOLLOW   0x400
#define AT_EMPTY_PATH       0x1000

/* lseek's whence */
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

/* access */
#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4

#endif
