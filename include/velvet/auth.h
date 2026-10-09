// SPDX-License-Identifier: GPL-2.0-only
/*
 * include/velvet/auth.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * who is allowed in, read out of a file in the ramdisk.
 */

/* the design notes for auth.h are in docs/subsystems/mm.rst */

#ifndef VELVET_AUTH_H
#define VELVET_AUTH_H

#include <stddef.h>
#include <stdbool.h>

#define AUTH_MAX_ACCOUNTS 8
#define AUTH_NAME_MAX     24
#define AUTH_DESC_MAX     48

struct account {
    char name[AUTH_NAME_MAX];
    char password[AUTH_NAME_MAX];
    int  uid;
    char description[AUTH_DESC_MAX];
};

void auth_init(void);

void auth_load(const char *text, size_t len);

int auth_login(const char *name, const char *password);

const struct account *auth_find(const char *name);
const char *auth_name_for(int uid);
size_t auth_count(void);

#endif
