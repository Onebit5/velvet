// SPDX-License-Identifier: GPL-2.0-only
/*
 * include/uapi/velvet/utsname.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * what uname says the machine and its kernel are.
 */

#ifndef UAPI_VELVET_UTSNAME_H
#define UAPI_VELVET_UTSNAME_H

#define UTS_LEN 65

struct utsname {
    char sysname[UTS_LEN];
    char nodename[UTS_LEN];
    char release[UTS_LEN];
    char version[UTS_LEN];
    char machine[UTS_LEN];
    char domainname[UTS_LEN];
};

#endif
