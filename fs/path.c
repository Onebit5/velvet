// SPDX-License-Identifier: GPL-2.0-only
/*
 * fs/path.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * resolving a name into a path.
 */

#include "fs/path.h"

/* the components of a path, as offsets into the answer being built. */
#define MAX_PARTS 32

bool path_resolve(const char *cwd, const char *path, char *out, size_t size)
{
    if (path == NULL || out == NULL || size < 2) {
        return false;
    }

    char parts[MAX_PARTS][64];
    size_t depth = 0;

    /*
     * a name beginning with a slash says where it is from, so the
     * working directory has nothing to do with it
     */
    const char *sources[2] = { NULL, path };
    if (path[0] != '/') {
        sources[0] = (cwd != NULL && cwd[0] != '\0') ? cwd : "/";
    }

    for (int which = 0; which < 2; which++) {
        const char *p = sources[which];
        if (p == NULL) {
            continue;
        }

        while (*p != '\0') {
            while (*p == '/') {
                p++;           /* one slash or five, it is the same slash */
            }
            if (*p == '\0') {
                break;
            }

            /*
             * XXX: sixty-four bytes is not absurd. ext4 allows 255 and fat
             * 255, and this refuses at 63, so a name on a disk the kernel
             * did not make can be listed by readdir and never opened:
             * every call that has to resolve it comes back false with
             * nothing to say. the depth limit below does the same to a
             * path of 33 parts. either allow what the filesystems below
             * allow, or refuse the name where it is listed rather than
             * where it is used.
             */
            size_t n = 0;
            char component[64];
            while (*p != '\0' && *p != '/') {
                if (n + 1 >= sizeof component) {
                    return false;       /* one component, absurdly long */
                }
                component[n++] = *p++;
            }
            component[n] = '\0';

            if (n == 1 && component[0] == '.') {
                continue;               /* here, which is where the kernel is */
            }
            if (n == 2 && component[0] == '.' && component[1] == '.') {
                /* back one, and from the root, back one is the root. */
                if (depth > 0) {
                    depth--;
                }
                continue;
            }

            if (depth >= MAX_PARTS) {
                return false;
            }
            for (size_t i = 0; i <= n; i++) {
                parts[depth][i] = component[i];
            }
            depth++;
        }
    }

    /*
     * and back into one string, always absolute, never with a trailing
     * slash unless there is nothing else to say
     */
    size_t at = 0;
    out[at++] = '/';

    for (size_t i = 0; i < depth; i++) {
        if (i > 0) {
            if (at + 1 >= size) {
                return false;
            }
            out[at++] = '/';
        }
        for (size_t k = 0; parts[i][k] != '\0'; k++) {
            if (at + 1 >= size) {
                return false;
            }
            out[at++] = parts[i][k];
        }
    }

    out[at] = '\0';
    return true;
}

bool path_split(const char *path, char *dir, size_t dir_size,
                char *name, size_t name_size)
{
    if (path == NULL || path[0] != '/') {
        return false;
    }

    /* the last slash is the seam. */
    size_t len = 0;
    size_t cut = 0;
    for (size_t i = 0; path[i] != '\0'; i++) {
        if (path[i] == '/') {
            cut = i;
        }
        len = i + 1;
    }

    if (name != NULL) {
        size_t n = len - cut - 1;
        if (n + 1 > name_size) {
            return false;
        }
        for (size_t i = 0; i < n; i++) {
            name[i] = path[cut + 1 + i];
        }
        name[n] = '\0';
    }

    if (dir != NULL) {
        /* the root is its own parent, and is spelled "/" rather than "" */
        size_t n = (cut == 0) ? 1 : cut;
        if (n + 1 > dir_size) {
            return false;
        }
        for (size_t i = 0; i < n; i++) {
            dir[i] = path[i];
        }
        dir[n] = '\0';
        if (cut == 0) {
            dir[0] = '/';
            dir[1] = '\0';
        }
    }
    return true;
}

bool path_is_root(const char *path)
{
    if (path == NULL) {
        return false;
    }
    for (size_t i = 0; path[i] != '\0'; i++) {
        if (path[i] != '/') {
            return false;
        }
    }
    return path[0] == '/';
}
