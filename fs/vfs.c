// SPDX-License-Identifier: GPL-2.0-only
/*
 * fs/vfs.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the virtual filesystem: names, descriptors, and two disks.
 */

#include <velvet/vfs.h>
#include <velvet/file.h>
#include <uapi/velvet/errno.h>
#include <uapi/velvet/fcntl.h>

#include <velvet/ramdisk.h>
#include <velvet/disk.h>
#include <velvet/kmalloc.h>
#include <velvet/proc.h>
#include <velvet/dev.h>
#include <velvet/string.h>

/* what a file on the disk is allowed to be. */
#define DISK_MODE 0644

static void copy_name(char *dst, const char *src)
{
    size_t i = 0;
    while (src[i] != '\0' && i < VFS_NAME_MAX - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}



/*
 * the namespace is a table of mounts: where each one hangs, and what is
 * there. a name belongs to the mount whose point is the longest one it
 * starts with, at a slash, and what the filesystem under that mount calls
 * it is the rest, from that filesystem's own root. that is the whole of
 * the routing, and it lives here rather than in fs/disk.c on purpose:
 * deciding that a name belongs to a mount at all is the namespace's job,
 * and a filesystem's job is to be one once somebody has decided.
 *
 * a disk mount with no filesystem behind it is not there at all, so the
 * names under it are the mount's below. the root is the one exception:
 * it is always there, and until the archive is the root, a name the root
 * filesystem does not have is looked for in the archive too
 */
#define VFS_MOUNTS_MAX 8

struct mount_point {
    char arr_at[PATH_MAX];
    enum vfs_kind e_kind;
    size_t sz_slot;             /* which disk mount, for VFS_DISK */
    bool b_union;               /* names it lacks are the archive's */
};

/* the root first: every name is under it, and it stands in nothing */
static struct mount_point s_arr_mounts[VFS_MOUNTS_MAX] = {
    { "/",          VFS_DISK,    DISK_ROOT, true  },
    { VFS_BOOT,     VFS_RAMDISK, 0,         false },
    { PROC_AT,      VFS_PROC,    0,         false },
    { DEV_AT,       VFS_DEV,     0,         false },
    { DISK_WORK_AT, VFS_DISK,    DISK_WORK, false },
};
static size_t s_sz_mounts = 5;

static bool is_present(const struct mount_point *p_mount)
{
    return p_mount->b_union || p_mount->e_kind != VFS_DISK
           || disk_ready(p_mount->sz_slot);
}

static const char *past_slashes(const char *str_at)
{
    while (*str_at == '/') {
        str_at++;
    }
    return str_at;
}

/* "/boot", "/boot/" and "/boot/x" are under /boot, and "/bootleg" is not */
static bool is_under(const char *str_path, const char *str_at, size_t sz_len)
{
    if (sz_len == 1) {
        return str_path[0] == '/';
    }
    return strncmp(str_path, str_at, sz_len) == 0
           && (str_path[sz_len] == '\0' || str_path[sz_len] == '/');
}

/*
 * the mount an absolute name is under, and the name from that mount's
 * root, which keeps its leading slash, so every filesystem answers the
 * same shape of question; the mount point itself is "/"
 */
static const struct mount_point *route(const char *str_path,
                                       const char **p_rest)
{
    const struct mount_point *p_best = &s_arr_mounts[0];
    size_t sz_best = 1;
    for (size_t sz_at = 1; sz_at < s_sz_mounts; sz_at++) {
        const struct mount_point *p_mount = &s_arr_mounts[sz_at];
        size_t sz_len = strlen(p_mount->arr_at);
        if (sz_len > sz_best && is_present(p_mount)
            && is_under(str_path, p_mount->arr_at, sz_len)) {
            p_best = p_mount;
            sz_best = sz_len;
        }
    }
    const char *str_rest = (sz_best > 1) ? str_path + sz_best : str_path;
    *p_rest = (str_rest[0] == '\0') ? "/" : str_rest;
    return p_best;
}

/* a name made absolute, a bare one being the root's, into str_out */
static const char *absolute(const char *str_path, char *str_out)
{
    if (str_path[0] == '/') {
        return str_path;
    }
    str_out[0] = '/';
    copy_name(str_out + 1, str_path);
    return str_out;
}

/* a name something is mounted at, and stays mounted at */
bool vfs_is_mount_point(const char *str_path)
{
    for (size_t sz_at = 0; sz_at < s_sz_mounts; sz_at++) {
        if (strcmp(s_arr_mounts[sz_at].arr_at, str_path) == 0) {
            return true;
        }
    }
    return false;
}

static void from_ramdisk(struct vfs_file *out, const struct ramdisk_file *f)
{
    memset(out, 0, sizeof *out);
    out->kind = VFS_RAMDISK;

    const char *name = f->name;
    if (name[0] == '.' && name[1] == '/') {
        name += 2;
    }
    copy_name(out->name, name);

    out->size = f->size;
    out->mode = f->mode;
    out->data = f->data;

    size_t n = strlen(out->name);
    out->is_dir = (n > 0 && out->name[n - 1] == '/');
}

static void from_disk(struct vfs_file *out, const struct disk_entry *e,
                      size_t which)
{
    memset(out, 0, sizeof *out);
    out->kind = VFS_DISK;
    out->mount = which;
    copy_name(out->name, e->name);
    out->size = e->size;
    out->is_dir = e->is_dir;
    out->mode = e->mode;
    out->cluster = e->cluster;
    out->entry_sector = e->entry_sector;
    out->entry_offset = e->entry_offset;
    out->written = e->written;
    out->uid = e->uid;
    out->gid = e->gid;
    out->is_symlink = e->is_symlink;
    out->u_ino = e->ino;
}

/* one filesystem's copy of a name, if it is mounted and has one */
static bool try_disk(size_t which, const char *path, bool b_follow,
                     struct vfs_file *out)
{
    if (!disk_ready(which)) {
        return false;
    }
    struct disk_entry e;
    bool b_found = b_follow ? disk_lookup(which, path, &e)
                            : disk_lookup_nofollow(which, path, &e);
    if (!b_found) {
        return false;
    }
    from_disk(out, &e, which);
    return true;
}

/* whether some name in the archive is under `str_dir`/ */
static bool ramdisk_has_under(const char *str_dir, size_t sz_len)
{
    struct ramdisk_file f;
    for (size_t sz_at = 0; ramdisk_stat(sz_at, &f); sz_at++) {
        const char *str_have = f.name;
        if (str_have[0] == '.' && str_have[1] == '/') {
            str_have += 2;
        }
        if (strncmp(str_have, str_dir, sz_len) == 0
            && str_have[sz_len] == '/') {
            return true;
        }
    }
    return false;
}

/*
 * a name in the archive: a file, or a directory, which tar names with a
 * slash on the end when it records one at all, and which is there just
 * the same when it does not, for being where files are
 */
static bool try_ramdisk(const char *path, struct vfs_file *out)
{
    struct ramdisk_file f;
    if (ramdisk_open(path, &f)) {
        from_ramdisk(out, &f);
        return true;
    }
    char arr_dir[VFS_NAME_MAX + 2];
    size_t sz_len = strlen(path);
    if (sz_len == 0 || sz_len + 2 > sizeof arr_dir) {
        return false;
    }
    memcpy(arr_dir, path, sz_len);
    arr_dir[sz_len] = '/';
    arr_dir[sz_len + 1] = '\0';
    if (ramdisk_open(arr_dir, &f)) {
        from_ramdisk(out, &f);
        return true;
    }
    if (!ramdisk_has_under(path, sz_len)) {
        return false;
    }
    memset(out, 0, sizeof *out);
    out->kind = VFS_RAMDISK;
    copy_name(out->name, arr_dir);
    out->is_dir = true;
    out->mode = 0555;
    return true;
}

/*
 * the place a mount hangs from, which is the root of what is mounted
 * there and on no filesystem of its own, and something has to be able
 * to say it is a directory or nobody can stand in it. its permissions
 * are the layer's rather than a filesystem's: anyone may look, nobody
 * may write to it
 */
static void mount_root(const struct mount_point *p_mount,
                       struct vfs_file *p_out)
{
    const char *str_name = strrchr(p_mount->arr_at, '/') + 1;

    memset(p_out, 0, sizeof *p_out);
    p_out->kind = p_mount->e_kind;
    p_out->mount = p_mount->sz_slot;
    copy_name(p_out->name, (str_name[0] != '\0') ? str_name : "/");
    p_out->is_dir = true;
    p_out->mode = 0555;
}

static bool lookup_in(const struct mount_point *p_mount, const char *str_rest,
                      bool b_follow, struct vfs_file *p_out)
{
    const char *str_name = past_slashes(str_rest);
    if (str_name[0] == '\0') {
        mount_root(p_mount, p_out);
        return true;
    }
    switch (p_mount->e_kind) {
    case VFS_DISK:
        if (try_disk(p_mount->sz_slot, str_rest, b_follow, p_out)) {
            return true;
        }
        return p_mount->b_union && try_ramdisk(str_name, p_out);
    case VFS_RAMDISK:
        return try_ramdisk(str_name, p_out);
    case VFS_PROC:
        memset(p_out, 0, sizeof *p_out);
        p_out->kind = VFS_PROC;
        if (!proc_find(str_name, &p_out->sz_proc)) {
            return false;
        }
        /*
         * none has a size until it is read, so it says 0 and a reader
         * reads to the end like any other file. who may read it is the
         * file's own say
         */
        copy_name(p_out->name, str_name);
        p_out->mode = proc_mode(p_out->sz_proc);
        return true;
    case VFS_DEV:
        memset(p_out, 0, sizeof *p_out);
        p_out->kind = VFS_DEV;
        if (!dev_find(str_name, &p_out->sz_dev)) {
            return false;
        }
        copy_name(p_out->name, str_name);
        p_out->mode = dev_mode(p_out->sz_dev);
        return true;
    default:
        return false;
    }
}

static bool open_as(const char *path, bool b_follow, struct vfs_file *out)
{
    char arr_absolute[VFS_NAME_MAX + 1];

    if (path == NULL || path[0] == '\0') {
        return false;
    }
    const char *rest;
    const struct mount_point *p_mount = route(absolute(path, arr_absolute),
                                              &rest);
    return lookup_in(p_mount, rest, b_follow, out);
}

/*
 * a bare name, from inside the kernel, which has no working directory of
 * its own, is the root's, so a disk can supply a newer copy of something
 * the archive also has
 */
bool vfs_open(const char *path, struct vfs_file *out)
{
    return open_as(path, true, out);
}

/* the same, with a link at the end the link rather than where it points */
bool vfs_open_nofollow(const char *path, struct vfs_file *out)
{
    return open_as(path, false, out);
}



/*
 * both archives are flat, their names contain slashes rather than
 * living in directories, so listing one of their subdirectories means
 * picking the names that begin with that prefix and showing only what
 * follows. that is what makes /boot/bin a real place even though
 * nothing in the tar says it is one, and it is the same sentence twice,
 * which is why it is one function.
 *
 * it rewrites the name in place, to what the file is called *here*. a
 * name that does not belong under this prefix comes back false and is
 * left alone.
 */
static bool listed_under(struct vfs_file *v, const char *rest,
                         size_t prefix)
{
    if (v->name[0] == '\0' || v->is_dir) {
        return false;           /* tar's directory records lead nowhere */
    }
    if (prefix == 0) {
        return true;
    }

    size_t k = 0;
    while (k < prefix && v->name[k] == rest[k]) {
        k++;
    }
    if (k != prefix || v->name[prefix] != '/') {
        return false;           /* somewhere else entirely */
    }

    size_t w = 0;
    for (size_t r = prefix + 1; v->name[r] != '\0'; r++) {
        v->name[w++] = v->name[r];
    }
    v->name[w] = '\0';
    return w > 0;
}

/*
 * entry `index` of what the filesystem under a mount holds in a
 * directory, or false past the end of it
 */
static bool readdir_in(const struct mount_point *p_mount,
                       const char *str_rest, size_t sz_index,
                       struct vfs_file *p_out)
{
    const char *str_name = past_slashes(str_rest);
    switch (p_mount->e_kind) {
    case VFS_DISK: {
        struct disk_entry e;
        if (!disk_ready(p_mount->sz_slot)
            || !disk_readdir(p_mount->sz_slot, str_rest, sz_index, &e)) {
            return false;
        }
        from_disk(p_out, &e, p_mount->sz_slot);
        return true;
    }
    case VFS_RAMDISK: {
        size_t prefix = strlen(str_name);
        struct ramdisk_file f;
        size_t seen = 0;
        for (size_t i = 0; ramdisk_stat(i, &f); i++) {
            struct vfs_file v;
            from_ramdisk(&v, &f);
            if (!listed_under(&v, str_name, prefix) || seen++ != sz_index) {
                continue;
            }
            *p_out = v;
            return true;
        }
        return false;
    }
    case VFS_PROC: {
        const char *name = proc_name(sz_index);
        if (str_name[0] != '\0' || name == NULL) {
            return false;
        }
        memset(p_out, 0, sizeof *p_out);
        p_out->kind = VFS_PROC;
        p_out->sz_proc = sz_index;
        p_out->mode = proc_mode(sz_index);
        copy_name(p_out->name, name);
        return true;
    }
    case VFS_DEV: {
        const char *name = dev_name(sz_index);
        if (str_name[0] != '\0' || name == NULL) {
            return false;
        }
        memset(p_out, 0, sizeof *p_out);
        p_out->kind = VFS_DEV;
        p_out->sz_dev = sz_index;
        p_out->mode = dev_mode(sz_index);
        copy_name(p_out->name, name);
        return true;
    }
    default:
        return false;
    }
}

/* the directory a mount point stands in, "/" for one hung from the root */
static bool stands_in(const struct mount_point *p_mount, const char *str_dir)
{
    const char *str_last = strrchr(p_mount->arr_at, '/');
    size_t sz_parent = (size_t)(str_last - p_mount->arr_at);
    size_t sz_dir = strlen(str_dir);

    /* "/" and "//" are the root, and a trailing slash changes nothing */
    while (sz_dir > 1 && str_dir[sz_dir - 1] == '/') {
        sz_dir--;
    }
    if (sz_parent == 0) {
        return past_slashes(str_dir)[0] == '\0';
    }
    return sz_dir == sz_parent
           && strncmp(str_dir, p_mount->arr_at, sz_parent) == 0;
}

/*
 * past the end of what a directory holds, the mounts standing in it,
 * which belong to no filesystem and have to be added by hand. one with
 * nothing mounted is not listed: a directory in a listing that cannot be
 * entered is worse than one that is not there, because the second is a
 * fact about the machine and the first looks like a fault
 */
static bool mount_in(const char *str_dir, size_t sz_index,
                     struct vfs_file *p_out)
{
    for (size_t sz_at = 1; sz_at < s_sz_mounts; sz_at++) {
        const struct mount_point *p_mount = &s_arr_mounts[sz_at];
        if (!is_present(p_mount) || !stands_in(p_mount, str_dir)) {
            continue;
        }
        if (sz_index-- == 0) {
            mount_root(p_mount, p_out);
            return true;
        }
    }
    return false;
}

bool vfs_readdir(const char *path, size_t index, struct vfs_file *out)
{
    char arr_absolute[VFS_NAME_MAX + 1];

    if (path == NULL || path[0] == '\0') {
        path = "/";
    }
    /* a relative directory is relative to the root, the same as a relative filename is. */
    path = absolute(path, arr_absolute);

    const char *rest;
    const struct mount_point *p_mount = route(path, &rest);
    if (readdir_in(p_mount, rest, index, out)) {
        return true;
    }
    size_t sz_own = 0;
    struct vfs_file ignored;
    while (readdir_in(p_mount, rest, sz_own, &ignored)) {
        sz_own++;
    }
    return index >= sz_own && mount_in(path, index - sz_own, out);
}



/*
 * the disk mount a name is on and what its filesystem calls it, for
 * everything that changes a filesystem: false for a name on read-only
 * memory, in /proc or /dev, or under a mount with nothing on it. a bare
 * name is made absolute in str_buf, which *p_rest points into after
 */
static bool on_disk(const char *path, char *str_buf, size_t *p_which,
                    const char **p_rest)
{
    if (path == NULL || path[0] == '\0') {
        return false;
    }
    const struct mount_point *p_mount = route(absolute(path, str_buf),
                                              p_rest);
    if (p_mount->e_kind != VFS_DISK || !disk_ready(p_mount->sz_slot)) {
        return false;
    }
    *p_which = p_mount->sz_slot;
    return true;
}

bool vfs_create(const char *path, uint32_t u_mode, uint32_t u_uid,
                uint32_t u_gid, struct vfs_file *out)
{
    char arr_buf[VFS_NAME_MAX + 1];
    size_t which;
    const char *rest;
    if (!on_disk(path, arr_buf, &which, &rest)) {
        return false;       /* read-only memory. there is nowhere to put it */
    }

    struct disk_entry e;
    if (!disk_create(which, rest, u_mode, u_uid, u_gid, &e)) {
        return false;
    }
    from_disk(out, &e, which);
    return true;
}

/*
 * the three-line ones all ask the same two questions first, is this
 * name on a filesystem at all, and which, so they ask them the same
 * way. `rest` is what the filesystem underneath calls it
 */
#define ROUTED(path, which, rest) on_disk((path), arr_buf, &(which), &(rest))

bool vfs_mkdir(const char *path, uint32_t u_mode, uint32_t u_uid,
               uint32_t u_gid)
{
    char arr_buf[VFS_NAME_MAX + 1];
    size_t which;
    const char *rest;
    /* read-only memory has no room for a new name */
    return ROUTED(path, which, rest)
           && disk_mkdir(which, rest, u_mode, u_uid, u_gid);
}

bool vfs_rmdir(const char *path)
{
    char arr_buf[VFS_NAME_MAX + 1];
    size_t which;
    const char *rest;
    return ROUTED(path, which, rest) && disk_rmdir(which, rest);
}

bool vfs_chmod(const char *path, uint32_t mode)
{
    char arr_buf[VFS_NAME_MAX + 1];
    size_t which;
    const char *rest;
    /* a tar in read-only memory has no opinions */
    return ROUTED(path, which, rest) && disk_chmod(which, rest, mode);
}

bool vfs_chown(const char *path, uint32_t uid, uint32_t gid)
{
    char arr_buf[VFS_NAME_MAX + 1];
    size_t which;
    const char *rest;
    return ROUTED(path, which, rest) && disk_chown(which, rest, uid, gid);
}

bool vfs_symlink(const char *path, const char *target, uint32_t u_uid,
                 uint32_t u_gid)
{
    char arr_buf[VFS_NAME_MAX + 1];
    size_t which;
    const char *rest;
    return target != NULL && ROUTED(path, which, rest)
        && disk_symlink(which, rest, target, u_uid, u_gid);
}

bool vfs_readlink(const char *path, char *out, size_t size)
{
    char arr_buf[VFS_NAME_MAX + 1];
    size_t which;
    const char *rest;
    return ROUTED(path, which, rest) && disk_readlink(which, rest, out, size);
}

bool vfs_unlink(const char *path)
{
    char arr_buf[VFS_NAME_MAX + 1];
    size_t which;
    const char *rest;
    /* the ramdisk is memory the kernel may not write */
    return ROUTED(path, which, rest) && disk_unlink(which, rest);
}

bool vfs_rename(const char *from, const char *to)
{
    char arr_from[VFS_NAME_MAX + 1];
    char arr_to[VFS_NAME_MAX + 1];
    size_t a, b;
    const char *rest_a, *rest_b;
    if (!on_disk(from, arr_from, &a, &rest_a)
        || !on_disk(to, arr_to, &b, &rest_b)) {
        /*
         * either end under /boot makes this a copy, and a rename that
         * quietly copies is a rename that silently costs a disk's worth
         * of time on a big file. so: no
         */
        return false;
    }
    if (a != b) {
        /*
         * and the same again across two filesystems: a rename moves a
         * name and nothing else, which is a sentence that only means
         * anything inside one filesystem. between two it would be a
         * copy and a delete wearing a rename's name
         */
        return false;
    }
    return disk_rename(a, rest_a, rest_b);
}

int64_t vfs_read(const struct vfs_file *f, uint64_t offset, void *buf,
                 uint64_t len)
{
    if (f->is_dir) {
        return -1;
    }
    if (f->kind == VFS_PROC) {
        return proc_read(f->sz_proc, offset, buf, len);
    }
    if (offset >= f->size) {
        return 0;
    }
    if (offset + len > f->size) {
        len = f->size - offset;
    }

    if (f->kind == VFS_RAMDISK) {
        memcpy(buf, (const uint8_t *)f->data + offset, len);
        return (int64_t)len;
    }
    if (f->kind == VFS_DISK) {
        return disk_read(f->mount, f->cluster, f->size, offset, buf, len);
    }
    return -1;
}

int64_t vfs_write(struct vfs_file *f, uint64_t offset, const void *buf,
                  uint64_t len)
{
    if (f->kind != VFS_DISK || f->is_dir) {
        return -1;
    }

    struct disk_entry e;
    memset(&e, 0, sizeof e);
    e.size = f->size;
    e.cluster = f->cluster;
    e.is_dir = false;
    e.entry_sector = f->entry_sector;
    e.entry_offset = f->entry_offset;

    int64_t n = disk_write_at(f->mount, &e, offset, buf, len);
    if (n > 0) {
        f->size = e.size;
        f->cluster = e.cluster;
    }
    return n;
}

bool vfs_truncate(struct vfs_file *p_node, uint64_t u_size)
{
    if (p_node->kind != VFS_DISK || p_node->is_dir) {
        return false;
    }
    struct disk_entry entry;
    memset(&entry, 0, sizeof entry);
    entry.size = p_node->size;
    entry.cluster = p_node->cluster;
    entry.entry_sector = p_node->entry_sector;
    entry.entry_offset = p_node->entry_offset;
    if (!disk_truncate(p_node->mount, &entry, u_size)) {
        return false;
    }
    p_node->size = entry.size;
    p_node->cluster = entry.cluster;
    return true;
}

bool vfs_may_read(const struct vfs_file *f, int uid)
{
    if (uid == 0) {
        return true;        /* the master may read anything */
    }
    if ((uint32_t)uid == f->uid) {
        return (f->mode & 0400) != 0;
    }
    return (f->mode & 0004) != 0;

    /* the owner half of that is what an inode answers. */
}

bool vfs_writable(const struct vfs_file *f)
{
    return f->kind == VFS_DISK;
}



bool vfs_slurp(const char *path, const void **data, uint64_t *size,
               bool *owned)
{
    struct vfs_file f;
    if (!vfs_open(path, &f) || f.is_dir) {
        return false;
    }

    /* already in memory, so hand it over where it lies. */
    if (f.kind == VFS_RAMDISK) {
        *data = f.data;
        *size = f.size;
        *owned = false;
        return true;
    }

    if (f.size == 0) {
        return false;
    }
    void *buf = kmalloc(f.size);
    if (buf == NULL) {
        return false;
    }
    if (vfs_read(&f, 0, buf, f.size) != (int64_t)f.size) {
        kfree(buf);
        return false;
    }

    *data = buf;
    *size = f.size;
    *owned = true;
    return true;
}

void vfs_release(const void *data, bool owned)
{
    if (owned) {
        kfree((void *)data);
    }
}



size_t vfs_mount_count(void)
{
    return 4;
}

bool vfs_mount_at(size_t index, struct vfs_mount *out)
{
    if (index == 0) {
        out->at = "/";
        /* whichever one answered. */
        out->what = disk_kind_name(DISK_ROOT);
        out->where = disk_ready(DISK_ROOT) ? disk_model()
                                           : "nothing, no disk found";
        out->writable = true;
        out->present = disk_ready(DISK_ROOT);
        return true;
    }
    if (index == 1) {
        /* the second filesystem, when the drive holds one. */
        out->at = DISK_WORK_AT;
        out->what = disk_kind_name(DISK_WORK);
        out->where = disk_ready(DISK_WORK)
                   ? disk_model()
                   : "nothing, the drive holds one filesystem";
        out->writable = true;
        out->present = disk_ready(DISK_WORK);
        return true;
    }
    if (index == 2) {
        out->at = VFS_BOOT;
        out->what = "ustar";
        out->where = "a module the bootloader handed the kernel";
        out->writable = false;
        out->present = ramdisk_present();
        return true;
    }
    return false;
}

/*
 * a file on the ramdisk, on a disk or in /proc. vfs_read and vfs_write
 * already know which of those the node is, so one set of operations
 * serves all three, and the position is the one thing added here.
 *
 * TODO: two processes sharing one file after a fork, reading it at once
 * on two cores, both start from the same position: nothing holds it for
 * the length of a read, and the disk is too slow to hold a spinlock over
 */
static int64_t node_read(struct file *p_file, void *p_buf, size_t sz_len)
{
    int64_t i_got = vfs_read(&p_file->node, p_file->u_pos, p_buf, sz_len);
    if (i_got > 0) {
        p_file->u_pos += (uint64_t)i_got;
    }
    return (i_got < 0) ? -EIO : i_got;
}

/*
 * the node is the file's own, so a write that grows the file leaves its
 * new size and first cluster where the next write, through any
 * descriptor on it, will find them
 */
static int64_t node_write(struct file *p_file, const void *p_buf,
                          size_t sz_len)
{
    if (p_file->node.kind != VFS_DISK) {
        return -EROFS;
    }
    if (p_file->u_flags & O_APPEND) {
        p_file->u_pos = p_file->node.size;
    }
    /*
     * past the end, the gap is made first and made of zeroes: fat has no
     * holes, and a cluster off the free list holds what it last held
     */
    if (p_file->u_pos > p_file->node.size
        && !vfs_truncate(&p_file->node, p_file->u_pos)) {
        return -ENOSPC;
    }
    int64_t i_put = vfs_write(&p_file->node, p_file->u_pos, p_buf, sz_len);
    if (i_put > 0) {
        p_file->u_pos += (uint64_t)i_put;
    }
    return (i_put < 0) ? -EIO : i_put;
}

/*
 * anywhere from the start on, the end and past it included: a write out
 * there makes the gap. /proc's files are written afresh on every read
 * and have no size to measure the end by, so they are read from the
 * start or not at all
 */
static int64_t node_seek(struct file *p_file, int64_t i_offset, int i_whence)
{
    if (p_file->node.kind == VFS_PROC) {
        return -ESPIPE;
    }
    int64_t i_from = (i_whence == SEEK_SET) ? 0
                   : (i_whence == SEEK_CUR) ? (int64_t)p_file->u_pos
                   : (i_whence == SEEK_END) ? (int64_t)p_file->node.size
                   : -1;
    if (i_from < 0) {
        return -EINVAL;
    }
    int64_t i_to = i_from + i_offset;
    if (i_to < 0) {
        return -EINVAL;
    }
    p_file->u_pos = (uint64_t)i_to;
    return i_to;
}

static const struct file_operations node_ops = {
    .read = node_read,
    .write = node_write,
    .seek = node_seek,
};

/*
 * a directory is read with getdents64, which counts entries in the
 * position, so the only seek it has is back to the start, which is what
 * rewinddir asks for
 */
static int64_t dir_read(struct file *p_file, void *p_buf, size_t sz_len)
{
    (void)p_file;
    (void)p_buf;
    (void)sz_len;
    return -EISDIR;
}

static int64_t dir_seek(struct file *p_file, int64_t i_offset, int i_whence)
{
    if (i_whence != SEEK_SET || i_offset < 0) {
        return -EINVAL;
    }
    p_file->u_pos = (uint64_t)i_offset;
    return i_offset;
}

static const struct file_operations dir_ops = {
    .read = dir_read,
    .seek = dir_seek,
};

struct file *vfs_open_file(const struct vfs_file *p_node, uint32_t u_mode)
{
    if (p_node->kind == VFS_DEV && !p_node->is_dir) {
        return dev_open(p_node->sz_dev, u_mode);
    }
    if (p_node->is_dir) {
        struct file *p_dir = file_alloc(&dir_ops, u_mode & FILE_READ,
                                        FILE_TYPE_DIR);
        if (p_dir != NULL) {
            p_dir->node = *p_node;
        }
        return p_dir;
    }
    struct file *p_file = file_alloc(&node_ops, u_mode, FILE_TYPE_REGULAR);
    if (p_file != NULL) {
        p_file->node = *p_node;
    }
    return p_file;
}
