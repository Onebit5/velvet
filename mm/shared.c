// SPDX-License-Identifier: GPL-2.0-only
/*
 * mm/shared.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * pages more than one space maps: a file's, mapped shared, or memory
 * mapped shared with no file behind it.
 */

#include <velvet/shared.h>
#include <velvet/errno.h>
#include <velvet/file.h>
#include <velvet/kmalloc.h>
#include <velvet/pmm.h>
#include <velvet/spinlock.h>
#include <velvet/string.h>
#include <velvet/vfs.h>

#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif

struct shared_page {
    uint64_t u_index;           /* which page of the file, or the memory */
    uint64_t u_frame;
    /*
     * stored to by some space. a page once stored to stays so: nothing
     * here can tell when a space stores to it again short of taking the
     * write away from every mapping of it, so each msync writes it out
     * again, which costs a write and loses nothing
     */
    bool b_dirty;
};

struct shared_pages {
    struct shared_pages *p_next;    /* the next file's, for one to be found */
    uint32_t u_refs;                /* the regions holding it */
    /*
     * the file, held, or NULL for memory of no file. its node is the
     * set's own, kept as long as the file is by every write and
     * truncation of it, which is what writing back is held to
     */
    struct file *p_file;
    size_t sz_pages;
    size_t sz_room;
    struct shared_page *arr_pages;
};

/*
 * every set and its pages, and a file's bytes while a page of it is read
 * in or a write to it lands, so neither sees the other half done
 */
static struct spinlock s_lock = SPINLOCK("shared", LOCK_RANK_SHARED);

/* the sets of files, for a second mapping of one to find the first's */
static struct shared_pages *s_p_files;

uint64_t shared_lock(void)
{
    return spin_lock_irq(&s_lock);
}

void shared_unlock(uint64_t u_flags)
{
    spin_unlock_irq(&s_lock, u_flags);
}

/* one file, however it was opened, is one filesystem's one inode */
static bool same_file(const struct vfs_file *p_a, const struct vfs_file *p_b)
{
    return p_a->kind == p_b->kind && p_a->mount == p_b->mount
           && p_a->u_ino == p_b->u_ino;
}

/* the set of the file p_node is, or NULL. the lock is held */
static struct shared_pages *set_of(const struct vfs_file *p_node)
{
    if (p_node->u_ino == 0) {
        return NULL;
    }
    for (struct shared_pages *p_set = s_p_files; p_set != NULL;
         p_set = p_set->p_next) {
        if (same_file(&p_set->p_file->node, p_node)) {
            return p_set;
        }
    }
    return NULL;
}

/* the set's page u_index, if it has it. the lock is held */
static struct shared_page *page_of(struct shared_pages *p_set,
                                   uint64_t u_index)
{
    for (size_t sz_at = 0; sz_at < p_set->sz_pages; sz_at++) {
        if (p_set->arr_pages[sz_at].u_index == u_index) {
            return &p_set->arr_pages[sz_at];
        }
    }
    return NULL;
}

/*
 * page u_index made, zeroes or the file's bytes up to where it ends, or
 * NULL when there is no memory or the file would not be read. the lock
 * is held, which ranks below the heap's, the frames' and the file's
 */
static struct shared_page *page_add(struct shared_pages *p_set,
                                    uint64_t u_index)
{
    if (p_set->sz_pages == p_set->sz_room) {
        size_t sz_room = (p_set->sz_room == 0) ? 16 : p_set->sz_room * 2;
        struct shared_page *p_more = kmalloc(sz_room * sizeof *p_more);
        if (p_more == NULL) {
            return NULL;
        }
        if (p_set->sz_pages > 0) {
            memcpy(p_more, p_set->arr_pages,
                   p_set->sz_pages * sizeof *p_more);
        }
        kfree(p_set->arr_pages);
        p_set->arr_pages = p_more;
        p_set->sz_room = sz_room;
    }
    uint64_t u_frame = pmm_alloc_pages(1);
    if (u_frame == 0) {
        return NULL;
    }
    uint8_t *p_mem = pmm_phys_to_virt(u_frame);
    memset(p_mem, 0, PAGE_SIZE);
    if (p_set->p_file != NULL
        && vfs_read_store(&p_set->p_file->node, u_index * PAGE_SIZE, p_mem,
                          PAGE_SIZE) < 0) {
        pmm_unref(u_frame);
        return NULL;
    }
    struct shared_page *p_page = &p_set->arr_pages[p_set->sz_pages++];
    p_page->u_index = u_index;
    p_page->u_frame = u_frame;
    p_page->b_dirty = false;
    return p_page;
}

struct shared_pages *shared_of_file(struct file *p_file)
{
    if (p_file == NULL || p_file->node.u_ino == 0) {
        return NULL;
    }
    struct shared_pages *p_new = kmalloc(sizeof *p_new);
    if (p_new == NULL) {
        return NULL;
    }
    memset(p_new, 0, sizeof *p_new);

    uint64_t u_flags = spin_lock_irq(&s_lock);
    struct shared_pages *p_set = set_of(&p_file->node);
    if (p_set == NULL) {
        p_set = p_new;
        p_new = NULL;
        file_get(p_file);
        p_set->p_file = p_file;
        p_set->p_next = s_p_files;
        s_p_files = p_set;
    }
    p_set->u_refs++;
    spin_unlock_irq(&s_lock, u_flags);
    kfree(p_new);
    return p_set;
}

struct shared_pages *shared_anon(void)
{
    struct shared_pages *p_set = kmalloc(sizeof *p_set);
    if (p_set != NULL) {
        memset(p_set, 0, sizeof *p_set);
        p_set->u_refs = 1;
    }
    return p_set;
}

void shared_get(struct shared_pages *p_set)
{
    uint64_t u_flags = spin_lock_irq(&s_lock);
    p_set->u_refs++;
    spin_unlock_irq(&s_lock, u_flags);
}

/*
 * the pages from u_first to u_last that were stored to, written to the
 * file as far as it goes and never further: a store past the end of a
 * file is in the mapping and not in the file, which mapping it did not
 * make any longer. the lock is held
 */
static int64_t write_back(struct shared_pages *p_set, uint64_t u_first,
                          uint64_t u_last)
{
    if (p_set->p_file == NULL) {
        return 0;
    }
    struct vfs_file *p_node = &p_set->p_file->node;
    int64_t i_why = 0;
    for (size_t sz_at = 0; sz_at < p_set->sz_pages; sz_at++) {
        const struct shared_page *p_page = &p_set->arr_pages[sz_at];
        uint64_t u_offset = p_page->u_index * PAGE_SIZE;
        if (!p_page->b_dirty || p_page->u_index < u_first
            || p_page->u_index > u_last || u_offset >= p_node->size) {
            continue;
        }
        uint64_t u_len = p_node->size - u_offset;
        if (u_len > PAGE_SIZE) {
            u_len = PAGE_SIZE;
        }
        if (vfs_write_store(p_node, u_offset,
                            pmm_phys_to_virt(p_page->u_frame), u_len)
            != (int64_t)u_len) {
            i_why = -EIO;
        }
    }
    return i_why;
}

void shared_put(struct shared_pages *p_set)
{
    if (p_set == NULL) {
        return;
    }
    uint64_t u_flags = spin_lock_irq(&s_lock);
    bool b_last = --p_set->u_refs == 0;
    if (b_last && p_set->p_file != NULL) {
        write_back(p_set, 0, ~0ull);
        struct shared_pages **pp_at = &s_p_files;
        while (*pp_at != p_set) {
            pp_at = &(*pp_at)->p_next;
        }
        *pp_at = p_set->p_next;
    }
    spin_unlock_irq(&s_lock, u_flags);
    if (!b_last) {
        return;
    }
    for (size_t sz_at = 0; sz_at < p_set->sz_pages; sz_at++) {
        pmm_unref(p_set->arr_pages[sz_at].u_frame);
    }
    file_put(p_set->p_file);
    kfree(p_set->arr_pages);
    kfree(p_set);
}

bool shared_frame(struct shared_pages *p_set, uint64_t u_index, bool b_write,
                  uint64_t *p_frame)
{
    uint64_t u_flags = spin_lock_irq(&s_lock);
    struct shared_page *p_page = page_of(p_set, u_index);
    if (p_page == NULL) {
        p_page = page_add(p_set, u_index);
    }
    if (p_page != NULL) {
        p_page->b_dirty = p_page->b_dirty || b_write;
        pmm_ref(p_page->u_frame);
        *p_frame = p_page->u_frame;
    }
    spin_unlock_irq(&s_lock, u_flags);
    return p_page != NULL;
}

void shared_dirty(struct shared_pages *p_set, uint64_t u_index)
{
    uint64_t u_flags = spin_lock_irq(&s_lock);
    struct shared_page *p_page = page_of(p_set, u_index);
    if (p_page != NULL) {
        p_page->b_dirty = true;
    }
    spin_unlock_irq(&s_lock, u_flags);
}

int64_t shared_sync(struct shared_pages *p_set, uint64_t u_first,
                    uint64_t u_last)
{
    uint64_t u_flags = spin_lock_irq(&s_lock);
    int64_t i_why = write_back(p_set, u_first, u_last);
    spin_unlock_irq(&s_lock, u_flags);
    return i_why;
}

int64_t shared_sync_all(void)
{
    int64_t i_why = 0;
    uint64_t u_flags = spin_lock_irq(&s_lock);
    for (struct shared_pages *p_set = s_p_files; p_set != NULL;
         p_set = p_set->p_next) {
        if (write_back(p_set, 0, ~0ull) < 0) {
            i_why = -EIO;
        }
    }
    spin_unlock_irq(&s_lock, u_flags);
    return i_why;
}

/*
 * where page p_page and [u_offset, u_offset + u_len) of the file meet:
 * how far into each, and how many bytes. false when they do not
 */
static bool overlap(const struct shared_page *p_page, uint64_t u_offset,
                    uint64_t u_len, uint64_t *p_in_page, uint64_t *p_in_buf,
                    uint64_t *p_count)
{
    uint64_t u_start = p_page->u_index * PAGE_SIZE;
    uint64_t u_from = (u_start > u_offset) ? u_start : u_offset;
    uint64_t u_to = (u_start + PAGE_SIZE < u_offset + u_len)
                    ? u_start + PAGE_SIZE : u_offset + u_len;
    if (u_from >= u_to) {
        return false;
    }
    *p_in_page = u_from - u_start;
    *p_in_buf = u_from - u_offset;
    *p_count = u_to - u_from;
    return true;
}

void shared_wrote(const struct vfs_file *p_node, uint64_t u_offset,
                  const void *p_buf, uint64_t u_len)
{
    struct shared_pages *p_set = set_of(p_node);
    if (p_set == NULL) {
        return;
    }
    p_set->p_file->node.size = p_node->size;
    p_set->p_file->node.cluster = p_node->cluster;
    for (size_t sz_at = 0; sz_at < p_set->sz_pages; sz_at++) {
        const struct shared_page *p_page = &p_set->arr_pages[sz_at];
        uint64_t u_in_page;
        uint64_t u_in_buf;
        uint64_t u_count;
        if (overlap(p_page, u_offset, u_len, &u_in_page, &u_in_buf,
                    &u_count)) {
            memcpy((uint8_t *)pmm_phys_to_virt(p_page->u_frame) + u_in_page,
                   (const uint8_t *)p_buf + u_in_buf, u_count);
        }
    }
}

void shared_overlay(const struct vfs_file *p_node, uint64_t u_offset,
                    void *p_buf, uint64_t u_len)
{
    struct shared_pages *p_set = set_of(p_node);
    if (p_set == NULL) {
        return;
    }
    for (size_t sz_at = 0; sz_at < p_set->sz_pages; sz_at++) {
        const struct shared_page *p_page = &p_set->arr_pages[sz_at];
        uint64_t u_in_page;
        uint64_t u_in_buf;
        uint64_t u_count;
        if (overlap(p_page, u_offset, u_len, &u_in_page, &u_in_buf,
                    &u_count)) {
            memcpy((uint8_t *)p_buf + u_in_buf,
                   (const uint8_t *)pmm_phys_to_virt(p_page->u_frame)
                       + u_in_page, u_count);
        }
    }
}

void shared_resized(const struct vfs_file *p_node)
{
    struct shared_pages *p_set = set_of(p_node);
    if (p_set == NULL) {
        return;
    }
    p_set->p_file->node.size = p_node->size;
    p_set->p_file->node.cluster = p_node->cluster;
    for (size_t sz_at = 0; sz_at < p_set->sz_pages; sz_at++) {
        const struct shared_page *p_page = &p_set->arr_pages[sz_at];
        uint64_t u_start = p_page->u_index * PAGE_SIZE;
        if (u_start + PAGE_SIZE <= p_node->size) {
            continue;
        }
        uint64_t u_keep = (p_node->size > u_start) ? p_node->size - u_start
                                                   : 0;
        memset((uint8_t *)pmm_phys_to_virt(p_page->u_frame) + u_keep, 0,
               PAGE_SIZE - u_keep);
    }
}
