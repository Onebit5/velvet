// SPDX-License-Identifier: GPL-2.0-only
/*
 * include/velvet/path.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * working out what a name means.
 */

/* the design notes for path.h are in docs/subsystems/mm.rst */

#ifndef VELVET_PATH_H
#define VELVET_PATH_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define PATH_MAX 256

bool path_resolve(const char *cwd, const char *path, char *out, size_t size);

bool path_split(const char *path, char *dir, size_t dir_size,
                char *name, size_t name_size);

bool path_is_root(const char *path);

#endif
