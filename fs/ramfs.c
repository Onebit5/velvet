// SPDX-License-Identifier: GPL-2.0-only
/*
 * fs/ramfs.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * a filesystem kept in memory: what the archive the loader hands over is
 * unpacked into, and what any directory a program wants forgotten at
 * shutdown can be. it is a tree of nodes and the pages of each file, and
 * it forgets all of it when the machine stops, which is what it is for.
 */

#include <velvet/ramfs.h>
#include <velvet/epoch.h>
#include <velvet/kmalloc.h>
#include <velvet/spinlock.h>
#include <velvet/string.h>
#include <uapi/velvet/errno.h>
#include <uapi/velvet/stat.h>

#define RAMFS_PAGE   4096
#define LINKS_MAX    8          /* links followed on one walk */

/*
 * every node of every one of them is in one table, and is known by its
 * number: the low bits are where it is in the table and the high bits
 * how many nodes that place has held. an open file, or a lookup on one
 * core while another unlinks, holds a number rather than a pointer, so a
 * node that has gone is one whose number finds nothing, never memory
 * that has been handed to something else
 */
#define INDEX_BITS   13
#define NODES_MAX    (1u << INDEX_BITS)
#define INDEX_MASK   (NODES_MAX - 1)
#define GEN_MASK     ((1u << (32 - INDEX_BITS)) - 1)

struct node {
    char     arr_name[RAMFS_NAME_MAX];
    uint32_t u_ino;
    uint32_t u_mode;
    uint32_t u_uid, u_gid;
    uint32_t u_rdev;
    uint64_t u_size;
    uint64_t u_mtime;
    size_t   sz_fs;
    uint32_t u_opens;           /* open files on it */
    bool     b_orphan;          /* unlinked while open, gone at the last close */

    struct node *p_parent;      /* NULL for a root, and for an orphan */
    struct node *p_child;       /* a directory's first entry */
    struct node *p_next;        /* the entry after this one in its directory */

    uint8_t **pp_pages;         /* a file's bytes by the page, NULL for zeroes */
    size_t    sz_pages;         /* how many pp_pages has room for */
    char     *str_link;         /* a link's target */
};

static struct node *s_arr_nodes[NODES_MAX];
static uint32_t     s_arr_gens[NODES_MAX];
static struct node *s_arr_roots[RAMFS_MAX];

/*
 * one lock for all of it. it is held for the length of a walk or of one
 * page's copy at most, and takes the heap's under it
 */
static struct spinlock s_lock = SPINLOCK("ramfs", LOCK_RANK_DEVICE);

static bool is_type(uint32_t u_mode, uint32_t u_type)
{
    return (u_mode & S_IFMT) == u_type;
}

static bool is_dir(const struct node *p_node)
{
    return is_type(p_node->u_mode, S_IFDIR);
}

static bool is_link(const struct node *p_node)
{
    return is_type(p_node->u_mode, S_IFLNK);
}

static bool is_file(const struct node *p_node)
{
    return is_type(p_node->u_mode, S_IFREG);
}

static struct node *by_ino(uint32_t u_ino)
{
    struct node *p_node = s_arr_nodes[u_ino & INDEX_MASK];
    return (p_node != NULL && p_node->u_ino == u_ino) ? p_node : NULL;
}

static struct node *node_new(size_t sz_fs, uint32_t u_mode, uint32_t u_uid,
                             uint32_t u_gid)
{
    size_t sz_free = 0;
    for (size_t sz_at = 1; sz_at < NODES_MAX && sz_free == 0; sz_at++) {
        if (s_arr_nodes[sz_at] == NULL) {
            sz_free = sz_at;
        }
    }
    struct node *p_node = (sz_free != 0) ? kmalloc(sizeof *p_node) : NULL;
    if (p_node == NULL) {
        return NULL;
    }
    memset(p_node, 0, sizeof *p_node);
    s_arr_gens[sz_free] = (s_arr_gens[sz_free] + 1) & GEN_MASK;
    if (s_arr_gens[sz_free] == 0) {
        s_arr_gens[sz_free] = 1;
    }
    p_node->u_ino = s_arr_gens[sz_free] << INDEX_BITS | (uint32_t)sz_free;
    p_node->u_mode = u_mode;
    p_node->u_uid = u_uid;
    p_node->u_gid = u_gid;
    p_node->u_mtime = epoch_now();
    p_node->sz_fs = sz_fs;
    s_arr_nodes[sz_free] = p_node;
    return p_node;
}

/* a file's pages from the first one at or past sz_from */
static void drop_pages(struct node *p_node, size_t sz_from)
{
    for (size_t sz_at = sz_from; sz_at < p_node->sz_pages; sz_at++) {
        kfree(p_node->pp_pages[sz_at]);
        p_node->pp_pages[sz_at] = NULL;
    }
}

static void node_free(struct node *p_node)
{
    drop_pages(p_node, 0);
    kfree(p_node->pp_pages);
    kfree(p_node->str_link);
    s_arr_nodes[p_node->u_ino & INDEX_MASK] = NULL;
    kfree(p_node);
}

/* out of its directory, the last entry following the one before it */
static void detach(struct node *p_node)
{
    struct node *p_dir = p_node->p_parent;
    if (p_dir == NULL) {
        return;
    }
    struct node **pp_at = &p_dir->p_child;
    while (*pp_at != NULL && *pp_at != p_node) {
        pp_at = &(*pp_at)->p_next;
    }
    if (*pp_at == p_node) {
        *pp_at = p_node->p_next;
    }
    p_node->p_parent = NULL;
    p_node->p_next = NULL;
    p_dir->u_mtime = epoch_now();
}

/* into a directory, after everything already there */
static void attach(struct node *p_dir, struct node *p_node)
{
    struct node **pp_at = &p_dir->p_child;
    while (*pp_at != NULL) {
        pp_at = &(*pp_at)->p_next;
    }
    *pp_at = p_node;
    p_node->p_parent = p_dir;
    p_node->p_next = NULL;
    p_dir->u_mtime = epoch_now();
}

/* a name gone: now, or at the last close of a file still open */
static void drop(struct node *p_node)
{
    detach(p_node);
    if (p_node->u_opens > 0) {
        p_node->b_orphan = true;
    } else {
        node_free(p_node);
    }
}

static struct node *child(const struct node *p_dir, const char *str_name,
                          size_t sz_len)
{
    for (struct node *p_at = p_dir->p_child; p_at != NULL;
         p_at = p_at->p_next) {
        if (strlen(p_at->arr_name) == sz_len
            && strncmp(p_at->arr_name, str_name, sz_len) == 0) {
            return p_at;
        }
    }
    return NULL;
}

/*
 * the node a path leads to, from p_from or, for one with a leading
 * slash, from the root, following the links on the way and the one at
 * the end when b_follow. a link's target is read from the directory the
 * link is in, as a name in that directory would be. NULL when nothing
 * is there
 */
static struct node *walk(struct node *p_root, struct node *p_from,
                         const char *str_path, bool b_follow, int i_links)
{
    struct node *p_at = (str_path[0] == '/') ? p_root : p_from;
    const char *p_name = str_path;

    while (p_at != NULL) {
        while (*p_name == '/') {
            p_name++;
        }
        if (*p_name == '\0') {
            return p_at;
        }
        if (!is_dir(p_at)) {
            return NULL;
        }
        size_t sz_len = 0;
        while (p_name[sz_len] != '\0' && p_name[sz_len] != '/') {
            sz_len++;
        }
        const char *p_rest = p_name + sz_len;
        const char *p_peek = p_rest;
        while (*p_peek == '/') {
            p_peek++;
        }
        bool b_last = *p_peek == '\0';

        if (sz_len == 1 && p_name[0] == '.') {
            p_name = p_rest;
            continue;
        }
        if (sz_len == 2 && p_name[0] == '.' && p_name[1] == '.') {
            if (p_at->p_parent != NULL) {
                p_at = p_at->p_parent;
            }
            p_name = p_rest;
            continue;
        }
        struct node *p_found = child(p_at, p_name, sz_len);
        if (p_found != NULL && is_link(p_found) && (!b_last || b_follow)) {
            if (i_links >= LINKS_MAX) {
                return NULL;
            }
            p_found = walk(p_root, p_at, p_found->str_link, true,
                           i_links + 1);
        }
        p_at = p_found;
        p_name = p_rest;
    }
    return NULL;
}

static struct node *find(size_t sz_fs, const char *str_path, bool b_follow)
{
    if (sz_fs >= RAMFS_MAX || s_arr_roots[sz_fs] == NULL) {
        return NULL;
    }
    struct node *p_root = s_arr_roots[sz_fs];
    return walk(p_root, p_root, str_path, b_follow, 0);
}

/*
 * the directory a new name would go in, and the name, into str_name:
 * NULL for a path with no last name, or one that is "." or "..", or a
 * directory that is not there
 */
static struct node *parent_of(size_t sz_fs, const char *str_path,
                              char *str_name)
{
    size_t sz_end = strlen(str_path);
    while (sz_end > 0 && str_path[sz_end - 1] == '/') {
        sz_end--;
    }
    size_t sz_start = sz_end;
    while (sz_start > 0 && str_path[sz_start - 1] != '/') {
        sz_start--;
    }
    size_t sz_name = sz_end - sz_start;
    if (sz_name == 0 || sz_name >= RAMFS_NAME_MAX
        || sz_start >= RAMFS_PATH_MAX) {
        return NULL;
    }
    memcpy(str_name, str_path + sz_start, sz_name);
    str_name[sz_name] = '\0';
    if (strcmp(str_name, ".") == 0 || strcmp(str_name, "..") == 0) {
        return NULL;
    }
    char arr_dir[RAMFS_PATH_MAX];
    memcpy(arr_dir, str_path, sz_start);
    arr_dir[sz_start] = '\0';
    struct node *p_dir = find(sz_fs, (sz_start > 0) ? arr_dir : "/", true);
    return (p_dir != NULL && is_dir(p_dir)) ? p_dir : NULL;
}

static void fill(const struct node *p_node, struct ramfs_entry *p_out)
{
    memset(p_out, 0, sizeof *p_out);
    strncpy(p_out->arr_name, p_node->arr_name, RAMFS_NAME_MAX - 1);
    p_out->u_ino = p_node->u_ino;
    p_out->u_mode = p_node->u_mode;
    p_out->u_uid = p_node->u_uid;
    p_out->u_gid = p_node->u_gid;
    p_out->u_rdev = p_node->u_rdev;
    p_out->u_size = p_node->u_size;
    p_out->u_mtime = p_node->u_mtime;
}



int ramfs_new(uint32_t u_mode)
{
    uint64_t u_flags = spin_lock_irq(&s_lock);
    int i_fs = -1;
    for (size_t sz_at = 0; sz_at < RAMFS_MAX && i_fs < 0; sz_at++) {
        if (s_arr_roots[sz_at] != NULL) {
            continue;
        }
        struct node *p_root = node_new(sz_at, S_IFDIR | (u_mode & 07777),
                                       0, 0);
        if (p_root != NULL) {
            strcpy(p_root->arr_name, "/");
            s_arr_roots[sz_at] = p_root;
            i_fs = (int)sz_at;
        }
        break;
    }
    spin_unlock_irq(&s_lock, u_flags);
    return i_fs;
}

static bool busy_locked(size_t sz_fs)
{
    for (size_t sz_at = 1; sz_at < NODES_MAX; sz_at++) {
        const struct node *p_node = s_arr_nodes[sz_at];
        if (p_node != NULL && p_node->sz_fs == sz_fs && p_node->u_opens > 0) {
            return true;
        }
    }
    return false;
}

bool ramfs_busy(size_t sz_fs)
{
    uint64_t u_flags = spin_lock_irq(&s_lock);
    bool b_busy = busy_locked(sz_fs);
    spin_unlock_irq(&s_lock, u_flags);
    return b_busy;
}

bool ramfs_free(size_t sz_fs)
{
    if (sz_fs >= RAMFS_MAX) {
        return false;
    }
    uint64_t u_flags = spin_lock_irq(&s_lock);
    bool b_ok = s_arr_roots[sz_fs] != NULL && !busy_locked(sz_fs);
    if (b_ok) {
        for (size_t sz_at = 1; sz_at < NODES_MAX; sz_at++) {
            struct node *p_node = s_arr_nodes[sz_at];
            if (p_node != NULL && p_node->sz_fs == sz_fs) {
                node_free(p_node);
            }
        }
        s_arr_roots[sz_fs] = NULL;
    }
    spin_unlock_irq(&s_lock, u_flags);
    return b_ok;
}

bool ramfs_lookup(size_t sz_fs, const char *str_path, bool b_follow,
                  struct ramfs_entry *p_out)
{
    uint64_t u_flags = spin_lock_irq(&s_lock);
    struct node *p_node = find(sz_fs, str_path, b_follow);
    if (p_node != NULL) {
        fill(p_node, p_out);
    }
    spin_unlock_irq(&s_lock, u_flags);
    return p_node != NULL;
}

bool ramfs_readdir(size_t sz_fs, const char *str_path, size_t sz_index,
                   struct ramfs_entry *p_out)
{
    uint64_t u_flags = spin_lock_irq(&s_lock);
    struct node *p_dir = find(sz_fs, str_path, true);
    struct node *p_at = (p_dir != NULL && is_dir(p_dir)) ? p_dir->p_child
                                                         : NULL;
    for (; p_at != NULL && sz_index > 0; sz_index--) {
        p_at = p_at->p_next;
    }
    if (p_at != NULL) {
        fill(p_at, p_out);
    }
    spin_unlock_irq(&s_lock, u_flags);
    return p_at != NULL;
}

bool ramfs_make(size_t sz_fs, const char *str_path, uint32_t u_mode,
                uint32_t u_uid, uint32_t u_gid, uint32_t u_rdev,
                const char *str_target, struct ramfs_entry *p_out)
{
    char arr_name[RAMFS_NAME_MAX];
    bool b_ok = false;
    uint64_t u_flags = spin_lock_irq(&s_lock);

    struct node *p_dir = parent_of(sz_fs, str_path, arr_name);
    if (p_dir != NULL && child(p_dir, arr_name, strlen(arr_name)) == NULL
        && (!is_type(u_mode, S_IFLNK) || str_target != NULL)) {
        struct node *p_node = node_new(sz_fs, u_mode, u_uid, u_gid);
        if (p_node != NULL && is_type(u_mode, S_IFLNK)) {
            size_t sz_len = strlen(str_target);
            p_node->str_link = kmalloc(sz_len + 1);
            if (p_node->str_link == NULL) {
                node_free(p_node);
                p_node = NULL;
            } else {
                memcpy(p_node->str_link, str_target, sz_len + 1);
                p_node->u_size = sz_len;
            }
        }
        if (p_node != NULL) {
            strcpy(p_node->arr_name, arr_name);
            p_node->u_rdev = u_rdev;
            attach(p_dir, p_node);
            if (p_out != NULL) {
                fill(p_node, p_out);
            }
            b_ok = true;
        }
    }
    spin_unlock_irq(&s_lock, u_flags);
    return b_ok;
}

static bool remove_name(size_t sz_fs, const char *str_path, bool b_dir)
{
    uint64_t u_flags = spin_lock_irq(&s_lock);
    struct node *p_node = find(sz_fs, str_path, false);
    bool b_ok = p_node != NULL && p_node->p_parent != NULL
                && is_dir(p_node) == b_dir
                && (!b_dir || p_node->p_child == NULL);
    if (b_ok) {
        drop(p_node);
    }
    spin_unlock_irq(&s_lock, u_flags);
    return b_ok;
}

bool ramfs_unlink(size_t sz_fs, const char *str_path)
{
    return remove_name(sz_fs, str_path, false);
}

bool ramfs_rmdir(size_t sz_fs, const char *str_path)
{
    return remove_name(sz_fs, str_path, true);
}

/* whether p_node is p_dir or anything inside it */
static bool is_within(const struct node *p_node, const struct node *p_dir)
{
    for (const struct node *p_at = p_node; p_at != NULL;
         p_at = p_at->p_parent) {
        if (p_at == p_dir) {
            return true;
        }
    }
    return false;
}

bool ramfs_rename(size_t sz_fs, const char *str_from, const char *str_to)
{
    char arr_name[RAMFS_NAME_MAX];
    bool b_ok = false;
    uint64_t u_flags = spin_lock_irq(&s_lock);

    struct node *p_node = find(sz_fs, str_from, false);
    struct node *p_dir = parent_of(sz_fs, str_to, arr_name);
    if (p_node != NULL && p_node->p_parent != NULL && p_dir != NULL
        && !is_within(p_dir, p_node)) {
        struct node *p_old = child(p_dir, arr_name, strlen(arr_name));
        b_ok = p_old == NULL || p_old == p_node
               || (is_dir(p_old) == is_dir(p_node)
                   && (!is_dir(p_old) || p_old->p_child == NULL));
        if (b_ok && p_old != p_node) {
            if (p_old != NULL) {
                drop(p_old);
            }
            detach(p_node);
            strcpy(p_node->arr_name, arr_name);
            attach(p_dir, p_node);
        }
    }
    spin_unlock_irq(&s_lock, u_flags);
    return b_ok;
}

bool ramfs_chmod(size_t sz_fs, const char *str_path, uint32_t u_mode)
{
    uint64_t u_flags = spin_lock_irq(&s_lock);
    struct node *p_node = find(sz_fs, str_path, true);
    if (p_node != NULL) {
        p_node->u_mode = (p_node->u_mode & ~07777u) | (u_mode & 07777);
    }
    spin_unlock_irq(&s_lock, u_flags);
    return p_node != NULL;
}

bool ramfs_chown(size_t sz_fs, const char *str_path, uint32_t u_uid,
                 uint32_t u_gid)
{
    uint64_t u_flags = spin_lock_irq(&s_lock);
    struct node *p_node = find(sz_fs, str_path, true);
    if (p_node != NULL) {
        p_node->u_uid = u_uid;
        p_node->u_gid = u_gid;
    }
    spin_unlock_irq(&s_lock, u_flags);
    return p_node != NULL;
}

bool ramfs_readlink(size_t sz_fs, const char *str_path, char *str_out,
                    size_t sz_size)
{
    uint64_t u_flags = spin_lock_irq(&s_lock);
    struct node *p_node = find(sz_fs, str_path, false);
    bool b_ok = p_node != NULL && is_link(p_node) && sz_size > 0;
    if (b_ok) {
        strncpy(str_out, p_node->str_link, sz_size - 1);
        str_out[sz_size - 1] = '\0';
    }
    spin_unlock_irq(&s_lock, u_flags);
    return b_ok;
}



/* room for a file's first sz_count pages: whether there is */
static bool grow_pages(struct node *p_node, size_t sz_count)
{
    if (sz_count <= p_node->sz_pages) {
        return true;
    }
    size_t sz_room = (p_node->sz_pages > 0) ? p_node->sz_pages : 4;
    while (sz_room < sz_count) {
        sz_room *= 2;
    }
    uint8_t **pp_new = kmalloc(sz_room * sizeof *pp_new);
    if (pp_new == NULL) {
        return false;
    }
    memset(pp_new, 0, sz_room * sizeof *pp_new);
    if (p_node->pp_pages != NULL) {
        memcpy(pp_new, p_node->pp_pages, p_node->sz_pages * sizeof *pp_new);
        kfree(p_node->pp_pages);
    }
    p_node->pp_pages = pp_new;
    p_node->sz_pages = sz_room;
    return true;
}

int64_t ramfs_read(uint32_t u_ino, uint64_t u_offset, void *p_buf,
                   uint64_t u_len)
{
    uint8_t *p_out = p_buf;
    uint64_t u_done = 0;
    uint64_t u_flags = spin_lock_irq(&s_lock);

    struct node *p_node = by_ino(u_ino);
    if (p_node == NULL || !is_file(p_node)) {
        spin_unlock_irq(&s_lock, u_flags);
        return -EINVAL;
    }
    if (u_offset < p_node->u_size) {
        if (u_len > p_node->u_size - u_offset) {
            u_len = p_node->u_size - u_offset;
        }
        while (u_done < u_len) {
            uint64_t u_at = u_offset + u_done;
            size_t sz_page = (size_t)(u_at / RAMFS_PAGE);
            uint64_t u_in = u_at % RAMFS_PAGE;
            uint64_t u_step = RAMFS_PAGE - u_in;
            if (u_step > u_len - u_done) {
                u_step = u_len - u_done;
            }
            const uint8_t *p_page = (sz_page < p_node->sz_pages)
                                    ? p_node->pp_pages[sz_page] : NULL;
            if (p_page != NULL) {
                memcpy(p_out + u_done, p_page + u_in, u_step);
            } else {
                memset(p_out + u_done, 0, u_step);
            }
            u_done += u_step;
        }
    }
    spin_unlock_irq(&s_lock, u_flags);
    return (int64_t)u_done;
}

int64_t ramfs_write(uint32_t u_ino, uint64_t u_offset, const void *p_buf,
                    uint64_t u_len)
{
    const uint8_t *p_in = p_buf;
    uint64_t u_done = 0;
    if (u_len > UINT64_MAX - u_offset) {
        return -EINVAL;
    }
    uint64_t u_flags = spin_lock_irq(&s_lock);

    struct node *p_node = by_ino(u_ino);
    if (p_node == NULL || !is_file(p_node)) {
        spin_unlock_irq(&s_lock, u_flags);
        return -EINVAL;
    }
    uint64_t u_end = u_offset + u_len;
    bool b_room = grow_pages(p_node,
                             (size_t)((u_end + RAMFS_PAGE - 1) / RAMFS_PAGE));
    while (b_room && u_done < u_len) {
        uint64_t u_at = u_offset + u_done;
        size_t sz_page = (size_t)(u_at / RAMFS_PAGE);
        uint64_t u_in = u_at % RAMFS_PAGE;
        uint64_t u_step = RAMFS_PAGE - u_in;
        if (u_step > u_len - u_done) {
            u_step = u_len - u_done;
        }
        if (p_node->pp_pages[sz_page] == NULL) {
            p_node->pp_pages[sz_page] = kmalloc(RAMFS_PAGE);
            if (p_node->pp_pages[sz_page] == NULL) {
                break;
            }
            memset(p_node->pp_pages[sz_page], 0, RAMFS_PAGE);
        }
        memcpy(p_node->pp_pages[sz_page] + u_in, p_in + u_done, u_step);
        u_done += u_step;
    }
    if (u_offset + u_done > p_node->u_size) {
        p_node->u_size = u_offset + u_done;
    }
    if (u_done > 0) {
        p_node->u_mtime = epoch_now();
    }
    spin_unlock_irq(&s_lock, u_flags);
    return (u_done > 0 || u_len == 0) ? (int64_t)u_done : -ENOSPC;
}

bool ramfs_truncate(uint32_t u_ino, uint64_t u_size)
{
    uint64_t u_flags = spin_lock_irq(&s_lock);
    struct node *p_node = by_ino(u_ino);
    bool b_ok = p_node != NULL && is_file(p_node);
    if (b_ok && u_size < p_node->u_size) {
        /*
         * the pages past the new end go, and the end of the last one is
         * zeroed, so a file grown again later reads as zeroes there
         * rather than as what it held before it was cut
         */
        size_t sz_keep = (size_t)((u_size + RAMFS_PAGE - 1) / RAMFS_PAGE);
        drop_pages(p_node, sz_keep);
        uint64_t u_in = u_size % RAMFS_PAGE;
        if (u_in != 0 && sz_keep - 1 < p_node->sz_pages
            && p_node->pp_pages[sz_keep - 1] != NULL) {
            memset(p_node->pp_pages[sz_keep - 1] + u_in, 0,
                   RAMFS_PAGE - u_in);
        }
    }
    if (b_ok) {
        p_node->u_size = u_size;
        p_node->u_mtime = epoch_now();
    }
    spin_unlock_irq(&s_lock, u_flags);
    return b_ok;
}

void ramfs_hold(uint32_t u_ino)
{
    uint64_t u_flags = spin_lock_irq(&s_lock);
    struct node *p_node = by_ino(u_ino);
    if (p_node != NULL) {
        p_node->u_opens++;
    }
    spin_unlock_irq(&s_lock, u_flags);
}

void ramfs_let_go(uint32_t u_ino)
{
    uint64_t u_flags = spin_lock_irq(&s_lock);
    struct node *p_node = by_ino(u_ino);
    if (p_node != NULL && p_node->u_opens > 0 && --p_node->u_opens == 0
        && p_node->b_orphan) {
        node_free(p_node);
    }
    spin_unlock_irq(&s_lock, u_flags);
}

void ramfs_usage(size_t sz_fs, uint64_t *p_bytes, uint64_t *p_nodes)
{
    uint64_t u_flags = spin_lock_irq(&s_lock);
    *p_bytes = 0;
    *p_nodes = 0;
    for (size_t sz_at = 1; sz_at < NODES_MAX; sz_at++) {
        const struct node *p_node = s_arr_nodes[sz_at];
        if (p_node == NULL || p_node->sz_fs != sz_fs) {
            continue;
        }
        (*p_nodes)++;
        for (size_t sz_page = 0; sz_page < p_node->sz_pages; sz_page++) {
            if (p_node->pp_pages[sz_page] != NULL) {
                *p_bytes += RAMFS_PAGE;
            }
        }
    }
    spin_unlock_irq(&s_lock, u_flags);
}
