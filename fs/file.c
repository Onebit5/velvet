// SPDX-License-Identifier: GPL-2.0-only
/*
 * fs/file.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * open files: made, shared and let go of.
 */

#include <velvet/file.h>
#include <velvet/slab.h>
#include <velvet/string.h>

static struct slab_cache file_cache;

struct file *file_alloc(const struct file_operations *p_ops, uint32_t u_mode,
                        enum file_type e_type)
{
    slab_cache_init(&file_cache, "file", sizeof(struct file));

    struct file *p_file = slab_alloc(&file_cache);
    if (p_file == NULL) {
        return NULL;
    }
    memset(p_file, 0, sizeof *p_file);
    p_file->p_ops = p_ops;
    p_file->u_refs = 1;
    p_file->u_mode = u_mode;
    p_file->e_type = e_type;
    return p_file;
}

void file_get(struct file *p_file)
{
    __atomic_add_fetch(&p_file->u_refs, 1, __ATOMIC_RELAXED);
}

void file_put(struct file *p_file)
{
    if (p_file == NULL
        || __atomic_sub_fetch(&p_file->u_refs, 1, __ATOMIC_ACQ_REL) != 0) {
        return;
    }
    if (p_file->p_ops->release != NULL) {
        p_file->p_ops->release(p_file);
    }
    slab_free(p_file);
}
