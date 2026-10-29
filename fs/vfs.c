// SPDX-License-Identifier: GPL-2.0-only
/*
 * fs/vfs.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the virtual filesystem: names, descriptors, and the mounts under them.
 */

#include <velvet/vfs.h>
#include <velvet/file.h>
#include <uapi/velvet/errno.h>
#include <uapi/velvet/fcntl.h>
#include <uapi/velvet/stat.h>

#include <velvet/ramdisk.h>
#include <velvet/ramfs.h>
#include <velvet/disk.h>
#include <velvet/epoch.h>
#include <velvet/kmalloc.h>
#include <velvet/process.h>
#include <velvet/proc.h>
#include <velvet/dev.h>
#include <velvet/spinlock.h>
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
 * names under it are the mount's below
 */
#define VFS_MOUNTS_MAX 16

struct mount_point {
    char arr_at[PATH_MAX];
    char arr_source[VFS_SOURCE_MAX];    /* what mount was told it is */
    enum vfs_kind e_kind;
    size_t sz_slot;             /* which disk mount or ramfs, for those */
    bool b_used;
    uint32_t u_busy;            /* calls in the middle of a name under it */
};

/*
 * the root first: every name is under it, it stands in nothing, and it
 * is never taken down, only exchanged for another by pivot_root. it is
 * filled in at boot, and mount adds the rest in the first place free
 */
static struct mount_point s_arr_mounts[VFS_MOUNTS_MAX] = {
    { .arr_at = "/" },
};

/*
 * the table changes while names are being looked up on other cores, so
 * it is read and written under this, and a mount in use by a call that
 * has let go of the lock is counted in u_busy, so it is not taken down
 * underneath that call
 */
static struct spinlock s_lock = SPINLOCK("mounts", LOCK_RANK_MOUNT);

static bool is_present(const struct mount_point *p_mount)
{
    return p_mount->b_used
           && (p_mount->e_kind != VFS_DISK || disk_ready(p_mount->sz_slot));
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
static struct mount_point *route(const char *str_path, const char **p_rest)
{
    struct mount_point *p_best = &s_arr_mounts[0];
    size_t sz_best = 1;
    for (size_t sz_at = 1; sz_at < VFS_MOUNTS_MAX; sz_at++) {
        struct mount_point *p_mount = &s_arr_mounts[sz_at];
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

/* the mount an absolute name is under, kept until mount_leave */
static struct mount_point *mount_enter(const char *str_path,
                                       const char **p_rest)
{
    uint64_t u_flags = spin_lock_irq(&s_lock);
    struct mount_point *p_mount = route(str_path, p_rest);
    p_mount->u_busy++;
    spin_unlock_irq(&s_lock, u_flags);
    return p_mount;
}

static void mount_leave(struct mount_point *p_mount)
{
    if (p_mount == NULL) {
        return;
    }
    uint64_t u_flags = spin_lock_irq(&s_lock);
    p_mount->u_busy--;
    spin_unlock_irq(&s_lock, u_flags);
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

static struct mount_point *mounted_at(const char *str_path)
{
    for (size_t sz_at = 0; sz_at < VFS_MOUNTS_MAX; sz_at++) {
        struct mount_point *p_mount = &s_arr_mounts[sz_at];
        if (p_mount->b_used && strcmp(p_mount->arr_at, str_path) == 0) {
            return p_mount;
        }
    }
    return NULL;
}

/* a name something is mounted at, and stays mounted at */
bool vfs_is_mount_point(const char *str_path)
{
    uint64_t u_flags = spin_lock_irq(&s_lock);
    bool b_is = mounted_at(str_path) != NULL;
    spin_unlock_irq(&s_lock, u_flags);
    return b_is;
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
    out->u_type = e->u_type;
    out->u_rdev = e->u_rdev;
}

/*
 * a ramfs node as the namespace describes one. the time goes the way a
 * fat one would, broken down, because that is what a node carries
 */
static void from_ramfs(struct vfs_file *p_out, const struct ramfs_entry *p_e,
                       size_t sz_fs)
{
    struct rtc_time when;

    memset(p_out, 0, sizeof *p_out);
    p_out->kind = VFS_RAMFS;
    p_out->mount = sz_fs;
    copy_name(p_out->name, p_e->arr_name);
    p_out->size = p_e->u_size;
    p_out->is_dir = (p_e->u_mode & S_IFMT) == S_IFDIR;
    p_out->is_symlink = (p_e->u_mode & S_IFMT) == S_IFLNK;
    p_out->u_type = p_e->u_mode & S_IFMT;
    p_out->u_rdev = p_e->u_rdev;
    p_out->mode = p_e->u_mode & 07777;
    p_out->uid = p_e->u_uid;
    p_out->gid = p_e->u_gid;
    p_out->u_ino = p_e->u_ino;
    epoch_to_date(p_e->u_mtime, &when);
    p_out->written.year = when.year;
    p_out->written.month = when.month;
    p_out->written.day = when.day;
    p_out->written.hour = when.hour;
    p_out->written.minute = when.minute;
    p_out->written.second = when.second;
}

static bool try_ramfs(size_t sz_fs, const char *str_path, bool b_follow,
                      struct vfs_file *p_out)
{
    struct ramfs_entry e;
    if (!ramfs_lookup(sz_fs, str_path, b_follow, &e)) {
        return false;
    }
    from_ramfs(p_out, &e, sz_fs);
    return true;
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
    p_out->is_dir = true;
    p_out->mode = 0555;
    /* one whose root is a node of its own says what that node is */
    if (p_mount->e_kind == VFS_RAMFS) {
        try_ramfs(p_mount->sz_slot, "/", true, p_out);
    }
    copy_name(p_out->name, (str_name[0] != '\0') ? str_name : "/");
}

/* a name on the filesystem a mount holds, and nowhere else */
static bool lookup_own(const struct mount_point *p_mount, const char *str_rest,
                       bool b_follow, struct vfs_file *p_out)
{
    const char *str_name = past_slashes(str_rest);
    switch (p_mount->e_kind) {
    case VFS_DISK:
        return try_disk(p_mount->sz_slot, str_rest, b_follow, p_out);
    case VFS_RAMFS:
        return try_ramfs(p_mount->sz_slot, str_rest, b_follow, p_out);
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

static bool lookup_in(const struct mount_point *p_mount, const char *str_rest,
                      bool b_follow, struct vfs_file *p_out)
{
    const char *str_name = past_slashes(str_rest);
    if (str_name[0] == '\0') {
        mount_root(p_mount, p_out);
        return true;
    }
    return lookup_own(p_mount, str_rest, b_follow, p_out);
}

static bool open_as(const char *path, bool b_follow, struct vfs_file *out)
{
    char arr_absolute[VFS_NAME_MAX + 1];

    if (path == NULL || path[0] == '\0') {
        return false;
    }
    const char *rest;
    struct mount_point *p_mount = mount_enter(absolute(path, arr_absolute),
                                              &rest);
    bool b_found = lookup_in(p_mount, rest, b_follow, out);
    mount_leave(p_mount);
    return b_found;
}

/*
 * a bare name, from inside the kernel, which has no working directory of
 * its own, is the root's
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
    case VFS_RAMFS: {
        struct ramfs_entry e;
        if (!ramfs_readdir(p_mount->sz_slot, str_rest, sz_index, &e)) {
            return false;
        }
        from_ramfs(p_out, &e, p_mount->sz_slot);
        return true;
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

/* the root of the nth mount standing in a directory, into p_out */
static bool nth_standing(const char *str_dir, size_t sz_nth,
                         struct vfs_file *p_out)
{
    bool b_found = false;
    uint64_t u_flags = spin_lock_irq(&s_lock);
    for (size_t sz_at = 1; sz_at < VFS_MOUNTS_MAX && !b_found; sz_at++) {
        const struct mount_point *p_mount = &s_arr_mounts[sz_at];
        if (is_present(p_mount) && stands_in(p_mount, str_dir)
            && sz_nth-- == 0) {
            mount_root(p_mount, p_out);
            b_found = true;
        }
    }
    spin_unlock_irq(&s_lock, u_flags);
    return b_found;
}

/*
 * past the end of what a directory holds, the mounts standing in it that
 * it does not hold a directory for, which belong to no filesystem and
 * have to be added by hand; one hung over a directory the filesystem has
 * is listed already, as that directory. one with nothing mounted is not
 * listed: a directory in a listing that cannot be entered is worse than
 * one that is not there, because the second is a fact about the machine
 * and the first looks like a fault
 */
static bool mount_in(const struct mount_point *p_dir, const char *str_rest,
                     const char *str_dir, size_t sz_index,
                     struct vfs_file *p_out)
{
    struct vfs_file root;
    struct vfs_file there;
    char arr_name[PATH_MAX];

    for (size_t sz_nth = 0; nth_standing(str_dir, sz_nth, &root); sz_nth++) {
        size_t sz_len = strlen(str_rest);
        if (sz_len + 1 + strlen(root.name) >= sizeof arr_name) {
            continue;
        }
        memcpy(arr_name, str_rest, sz_len);
        arr_name[sz_len] = '/';
        copy_name(arr_name + sz_len + 1, root.name);
        if (lookup_own(p_dir, arr_name, false, &there)) {
            continue;
        }
        if (sz_index-- == 0) {
            *p_out = root;
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
    struct mount_point *p_mount = mount_enter(path, &rest);
    bool b_found = readdir_in(p_mount, rest, index, out);
    if (!b_found) {
        size_t sz_own = 0;
        struct vfs_file ignored;
        while (readdir_in(p_mount, rest, sz_own, &ignored)) {
            sz_own++;
        }
        b_found = index >= sz_own
                  && mount_in(p_mount, rest, path, index - sz_own, out);
    }
    mount_leave(p_mount);
    return b_found;
}



/*
 * what can be done to a filesystem that keeps names of its own, a disk's
 * or one in memory. read-only memory, /proc and /dev keep none
 */
struct store_ops {
    bool (*create)(size_t which, const char *path, uint32_t u_mode,
                   uint32_t u_uid, uint32_t u_gid, struct vfs_file *out);
    bool (*mkdir)(size_t which, const char *path, uint32_t u_mode,
                  uint32_t u_uid, uint32_t u_gid);
    bool (*rmdir)(size_t which, const char *path);
    bool (*unlink)(size_t which, const char *path);
    bool (*rename)(size_t which, const char *from, const char *to);
    bool (*chmod)(size_t which, const char *path, uint32_t mode);
    bool (*chown)(size_t which, const char *path, uint32_t uid,
                  uint32_t gid);
    bool (*symlink)(size_t which, const char *path, const char *target,
                    uint32_t u_uid, uint32_t u_gid);
    bool (*readlink)(size_t which, const char *path, char *out,
                     size_t size);
    bool (*mknod)(size_t which, const char *path, uint32_t u_mode,
                  uint32_t u_uid, uint32_t u_gid, uint32_t u_rdev);
};

static bool disk_create_node(size_t which, const char *path, uint32_t u_mode,
                             uint32_t u_uid, uint32_t u_gid,
                             struct vfs_file *out)
{
    struct disk_entry e;
    if (!disk_create(which, path, u_mode, u_uid, u_gid, &e)) {
        return false;
    }
    from_disk(out, &e, which);
    return true;
}

static bool ramfs_create_node(size_t sz_fs, const char *str_path,
                              uint32_t u_mode, uint32_t u_uid, uint32_t u_gid,
                              struct vfs_file *p_out)
{
    struct ramfs_entry e;
    if (!ramfs_make(sz_fs, str_path, S_IFREG | (u_mode & 07777), u_uid,
                    u_gid, 0, NULL, &e)) {
        return false;
    }
    from_ramfs(p_out, &e, sz_fs);
    return true;
}

static bool ramfs_mkdir_node(size_t sz_fs, const char *str_path,
                             uint32_t u_mode, uint32_t u_uid, uint32_t u_gid)
{
    return ramfs_make(sz_fs, str_path, S_IFDIR | (u_mode & 07777), u_uid,
                      u_gid, 0, NULL, NULL);
}

static bool ramfs_symlink_node(size_t sz_fs, const char *str_path,
                               const char *str_target, uint32_t u_uid,
                               uint32_t u_gid)
{
    return ramfs_make(sz_fs, str_path, S_IFLNK | 0777, u_uid, u_gid, 0,
                      str_target, NULL);
}

static bool ramfs_mknod_node(size_t sz_fs, const char *str_path,
                             uint32_t u_mode, uint32_t u_uid, uint32_t u_gid,
                             uint32_t u_rdev)
{
    return ramfs_make(sz_fs, str_path, u_mode, u_uid, u_gid, u_rdev, NULL,
                      NULL);
}

static const struct store_ops s_disk_ops = {
    disk_create_node, disk_mkdir, disk_rmdir, disk_unlink, disk_rename,
    disk_chmod, disk_chown, disk_symlink, disk_readlink, disk_mknod,
};

static const struct store_ops s_ramfs_ops = {
    ramfs_create_node, ramfs_mkdir_node, ramfs_rmdir, ramfs_unlink,
    ramfs_rename, ramfs_chmod, ramfs_chown, ramfs_symlink_node,
    ramfs_readlink, ramfs_mknod_node,
};

/*
 * the mount a name is under, kept until mount_leave, when what is there
 * keeps names, with its operations in *pp_ops: NULL for a name on
 * read-only memory, in /proc or /dev, or under a mount with nothing on
 * it. a bare name is made absolute in str_buf, which *p_rest points
 * into after
 */
static struct mount_point *on_store(const char *path, char *str_buf,
                                    const char **p_rest,
                                    const struct store_ops **pp_ops)
{
    if (path == NULL || path[0] == '\0') {
        return NULL;
    }
    struct mount_point *p_mount = mount_enter(absolute(path, str_buf),
                                              p_rest);
    *pp_ops = (p_mount->e_kind == VFS_RAMFS) ? &s_ramfs_ops
            : (p_mount->e_kind == VFS_DISK && disk_ready(p_mount->sz_slot))
              ? &s_disk_ops
            : NULL;
    if (*pp_ops == NULL) {
        mount_leave(p_mount);
        return NULL;
    }
    return p_mount;
}

bool vfs_create(const char *path, uint32_t u_mode, uint32_t u_uid,
                uint32_t u_gid, struct vfs_file *out)
{
    char arr_buf[VFS_NAME_MAX + 1];
    const char *rest;
    const struct store_ops *p_ops;
    struct mount_point *p_mount = on_store(path, arr_buf, &rest, &p_ops);
    /* /proc and /dev keep no names, so there is nowhere to put it */
    bool b_ok = p_mount != NULL
                && p_ops->create(p_mount->sz_slot, rest, u_mode, u_uid, u_gid,
                                 out);
    mount_leave(p_mount);
    return b_ok;
}

bool vfs_mkdir(const char *path, uint32_t u_mode, uint32_t u_uid,
               uint32_t u_gid)
{
    char arr_buf[VFS_NAME_MAX + 1];
    const char *rest;
    const struct store_ops *p_ops;
    struct mount_point *p_mount = on_store(path, arr_buf, &rest, &p_ops);
    bool b_ok = p_mount != NULL
                && p_ops->mkdir(p_mount->sz_slot, rest, u_mode, u_uid, u_gid);
    mount_leave(p_mount);
    return b_ok;
}

bool vfs_rmdir(const char *path)
{
    char arr_buf[VFS_NAME_MAX + 1];
    const char *rest;
    const struct store_ops *p_ops;
    struct mount_point *p_mount = on_store(path, arr_buf, &rest, &p_ops);
    bool b_ok = p_mount != NULL && p_ops->rmdir(p_mount->sz_slot, rest);
    mount_leave(p_mount);
    return b_ok;
}

bool vfs_chmod(const char *path, uint32_t mode)
{
    char arr_buf[VFS_NAME_MAX + 1];
    const char *rest;
    const struct store_ops *p_ops;
    struct mount_point *p_mount = on_store(path, arr_buf, &rest, &p_ops);
    bool b_ok = p_mount != NULL && p_ops->chmod(p_mount->sz_slot, rest, mode);
    mount_leave(p_mount);
    return b_ok;
}

bool vfs_chown(const char *path, uint32_t uid, uint32_t gid)
{
    char arr_buf[VFS_NAME_MAX + 1];
    const char *rest;
    const struct store_ops *p_ops;
    struct mount_point *p_mount = on_store(path, arr_buf, &rest, &p_ops);
    bool b_ok = p_mount != NULL
                && p_ops->chown(p_mount->sz_slot, rest, uid, gid);
    mount_leave(p_mount);
    return b_ok;
}

bool vfs_symlink(const char *path, const char *target, uint32_t u_uid,
                 uint32_t u_gid)
{
    char arr_buf[VFS_NAME_MAX + 1];
    const char *rest;
    const struct store_ops *p_ops;
    if (target == NULL) {
        return false;
    }
    struct mount_point *p_mount = on_store(path, arr_buf, &rest, &p_ops);
    bool b_ok = p_mount != NULL
                && p_ops->symlink(p_mount->sz_slot, rest, target, u_uid,
                                  u_gid);
    mount_leave(p_mount);
    return b_ok;
}

bool vfs_mknod(const char *str_path, uint32_t u_mode, uint32_t u_uid,
               uint32_t u_gid, uint32_t u_rdev)
{
    char arr_buf[VFS_NAME_MAX + 1];
    const char *rest;
    const struct store_ops *p_ops;
    struct mount_point *p_mount = on_store(str_path, arr_buf, &rest, &p_ops);
    bool b_ok = p_mount != NULL
                && p_ops->mknod(p_mount->sz_slot, rest, u_mode, u_uid, u_gid,
                                u_rdev);
    mount_leave(p_mount);
    return b_ok;
}

bool vfs_readlink(const char *path, char *out, size_t size)
{
    char arr_buf[VFS_NAME_MAX + 1];
    const char *rest;
    const struct store_ops *p_ops;
    struct mount_point *p_mount = on_store(path, arr_buf, &rest, &p_ops);
    bool b_ok = p_mount != NULL
                && p_ops->readlink(p_mount->sz_slot, rest, out, size);
    mount_leave(p_mount);
    return b_ok;
}

bool vfs_unlink(const char *path)
{
    char arr_buf[VFS_NAME_MAX + 1];
    const char *rest;
    const struct store_ops *p_ops;
    struct mount_point *p_mount = on_store(path, arr_buf, &rest, &p_ops);
    bool b_ok = p_mount != NULL && p_ops->unlink(p_mount->sz_slot, rest);
    mount_leave(p_mount);
    return b_ok;
}

bool vfs_rename(const char *from, const char *to)
{
    char arr_from[VFS_NAME_MAX + 1];
    char arr_to[VFS_NAME_MAX + 1];
    const char *rest_a, *rest_b;
    const struct store_ops *p_ops;
    const struct store_ops *p_ops_b;
    struct mount_point *p_a = on_store(from, arr_from, &rest_a, &p_ops);
    struct mount_point *p_b = on_store(to, arr_to, &rest_b, &p_ops_b);
    /*
     * a rename moves a name and nothing else, which is a sentence that
     * only means anything inside one filesystem. between two it would be
     * a copy and a delete wearing a rename's name, a copy that quietly
     * costs a disk's worth of time on a big file. so: no
     */
    bool b_ok = p_a != NULL && p_a == p_b
                && p_ops->rename(p_a->sz_slot, rest_a, rest_b);
    mount_leave(p_a);
    mount_leave(p_b);
    return b_ok;
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
    /* a file in memory knows its own size, whoever else has it open */
    if (f->kind == VFS_RAMFS) {
        return ramfs_read(f->u_ino, offset, buf, len);
    }
    if (offset >= f->size) {
        return 0;
    }
    if (offset + len > f->size) {
        len = f->size - offset;
    }

    if (f->kind == VFS_DISK) {
        return disk_read(f->mount, f->cluster, f->size, offset, buf, len);
    }
    return -1;
}

int64_t vfs_write(struct vfs_file *f, uint64_t offset, const void *buf,
                  uint64_t len)
{
    if (f->kind == VFS_RAMFS && !f->is_dir) {
        int64_t i_put = ramfs_write(f->u_ino, offset, buf, len);
        if (i_put > 0 && offset + (uint64_t)i_put > f->size) {
            f->size = offset + (uint64_t)i_put;
        }
        return (i_put < 0) ? -1 : i_put;
    }
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
    if (p_node->kind == VFS_RAMFS && !p_node->is_dir) {
        bool b_ok = ramfs_truncate(p_node->u_ino, u_size);
        if (b_ok) {
            p_node->size = u_size;
        }
        return b_ok;
    }
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

bool vfs_is_device(const struct vfs_file *p_node)
{
    return (p_node->kind == VFS_DEV && !p_node->is_dir)
           || p_node->u_type == S_IFCHR || p_node->u_type == S_IFBLK
           || p_node->u_type == S_IFIFO || p_node->u_type == S_IFSOCK;
}

bool vfs_writable(const struct vfs_file *f)
{
    return f->kind == VFS_DISK || f->kind == VFS_RAMFS;
}

void vfs_refresh(struct vfs_file *p_node)
{
    uint64_t u_size;
    if (p_node->kind == VFS_RAMFS && ramfs_size(p_node->u_ino, &u_size)) {
        p_node->size = u_size;
    }
}



bool vfs_slurp(const char *path, const void **data, uint64_t *size,
               bool *owned)
{
    struct vfs_file f;
    if (!vfs_open(path, &f) || f.is_dir) {
        return false;
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



/* what kind of filesystem hangs at a mount, by the name mount knows it by */
static const char *type_of(const struct mount_point *p_mount)
{
    switch (p_mount->e_kind) {
    case VFS_DISK:    return disk_kind_name(p_mount->sz_slot);
    case VFS_RAMFS:   return "ramfs";
    case VFS_PROC:    return "proc";
    case VFS_DEV:     return "devtmpfs";
    default:          return "none";
    }
}

/* and what it is, when mount was not told */
static const char *source_of(const struct mount_point *p_mount)
{
    if (p_mount->arr_source[0] != '\0') {
        return p_mount->arr_source;
    }
    switch (p_mount->e_kind) {
    case VFS_DISK:
        return disk_ready(p_mount->sz_slot) ? "a drive" : "nothing";
    case VFS_PROC:    return "the kernel, as it is read";
    case VFS_DEV:     return "the kernel's devices";
    default:          return "memory";
    }
}

size_t vfs_mount_count(void)
{
    size_t sz_count = 0;
    uint64_t u_flags = spin_lock_irq(&s_lock);
    for (size_t sz_at = 0; sz_at < VFS_MOUNTS_MAX; sz_at++) {
        sz_count += s_arr_mounts[sz_at].b_used ? 1 : 0;
    }
    spin_unlock_irq(&s_lock, u_flags);
    return sz_count;
}

bool vfs_mount_at(size_t index, struct vfs_mount *out)
{
    bool b_found = false;
    uint64_t u_flags = spin_lock_irq(&s_lock);
    for (size_t sz_at = 0; sz_at < VFS_MOUNTS_MAX && !b_found; sz_at++) {
        const struct mount_point *p_mount = &s_arr_mounts[sz_at];
        if (!p_mount->b_used || index-- != 0) {
            continue;
        }
        memset(out, 0, sizeof *out);
        strncpy(out->arr_at, p_mount->arr_at, sizeof out->arr_at - 1);
        strncpy(out->arr_where, source_of(p_mount), sizeof out->arr_where - 1);
        out->what = type_of(p_mount);
        out->writable = p_mount->e_kind == VFS_DISK
                        || p_mount->e_kind == VFS_RAMFS;
        out->present = is_present(p_mount);
        out->e_kind = p_mount->e_kind;
        out->sz_slot = p_mount->sz_slot;
        b_found = true;
    }
    spin_unlock_irq(&s_lock, u_flags);
    return b_found;
}

bool vfs_where(enum vfs_kind e_kind, size_t sz_slot, char *str_out,
               size_t sz_size)
{
    bool b_found = false;
    uint64_t u_flags = spin_lock_irq(&s_lock);
    for (size_t sz_at = 0; sz_at < VFS_MOUNTS_MAX && !b_found; sz_at++) {
        const struct mount_point *p_mount = &s_arr_mounts[sz_at];
        if (is_present(p_mount) && p_mount->e_kind == e_kind
            && p_mount->sz_slot == sz_slot) {
            strncpy(str_out, p_mount->arr_at, sz_size - 1);
            str_out[sz_size - 1] = '\0';
            b_found = true;
        }
    }
    spin_unlock_irq(&s_lock, u_flags);
    if (!b_found && sz_size > 0) {
        strncpy(str_out, "?", sz_size);
    }
    return b_found;
}

int64_t vfs_mount_add(const char *str_at, enum vfs_kind e_kind,
                      size_t sz_slot, const char *str_source)
{
    if (strlen(str_at) >= PATH_MAX) {
        return -ENAMETOOLONG;
    }
    int64_t i_why = -ENOMEM;
    uint64_t u_flags = spin_lock_irq(&s_lock);
    if (mounted_at(str_at) != NULL) {
        i_why = -EBUSY;
    }
    for (size_t sz_at = 1; sz_at < VFS_MOUNTS_MAX && i_why == -ENOMEM;
         sz_at++) {
        struct mount_point *p_mount = &s_arr_mounts[sz_at];
        if (p_mount->b_used) {
            continue;
        }
        memset(p_mount, 0, sizeof *p_mount);
        strcpy(p_mount->arr_at, str_at);
        strncpy(p_mount->arr_source, (str_source != NULL) ? str_source : "",
                sizeof p_mount->arr_source - 1);
        p_mount->e_kind = e_kind;
        p_mount->sz_slot = sz_slot;
        p_mount->b_used = true;
        i_why = 0;
    }
    spin_unlock_irq(&s_lock, u_flags);
    return i_why;
}

bool vfs_root_init(size_t *p_files)
{
    int i_fs = ramfs_new(0755);
    if (i_fs < 0) {
        return false;
    }
    *p_files = ramdisk_present() ? ramdisk_unpack((size_t)i_fs) : 0;

    uint64_t u_flags = spin_lock_irq(&s_lock);
    struct mount_point *p_root = &s_arr_mounts[0];
    memset(p_root, 0, sizeof *p_root);
    strcpy(p_root->arr_at, "/");
    strcpy(p_root->arr_source, ramdisk_present()
                               ? "a module the bootloader handed the kernel"
                               : "memory");
    p_root->e_kind = VFS_RAMFS;
    p_root->sz_slot = (size_t)i_fs;
    p_root->b_used = true;
    spin_unlock_irq(&s_lock, u_flags);

    /* the devices, filled in by the kernel, where a program looks for them */
    ramfs_make((size_t)i_fs, DEV_AT, S_IFDIR | 0755, 0, 0, 0, NULL, NULL);
    return vfs_mount_add(DEV_AT, VFS_DEV, 0, "devtmpfs") == 0;
}

/*
 * how a path changes when mounts move, so that whatever it named before
 * it names after: moving a mount from one place to another, or pivoting
 * the root, where the old root goes to a place under the new one and
 * everything not under the new one goes with it
 */
struct path_map {
    const char *str_from;       /* the mount moved, or the new root */
    const char *str_to;         /* where it goes, or where the old root goes */
    bool        b_pivot;
};

/* str_in as it reads after the move, into str_out: false when too long */
static bool map_path(const struct path_map *p_map, const char *str_in,
                     char *str_out)
{
    size_t sz_from = strlen(p_map->str_from);
    const char *str_rest = str_in + sz_from;
    const char *str_head;

    if (is_under(str_in, p_map->str_from, sz_from)) {
        /* what is under the moved one, or under the new root */
        str_head = p_map->b_pivot ? "" : p_map->str_to;
        if (p_map->b_pivot && str_rest[0] == '\0') {
            str_rest = "/";
        }
    } else if (p_map->b_pivot) {
        /* everything else hangs under where the old root went */
        str_head = p_map->str_to;
        str_rest = (strcmp(str_in, "/") == 0) ? "" : str_in;
    } else {
        str_head = "";
        str_rest = str_in;
    }
    size_t sz_head = strlen(str_head);
    size_t sz_rest = strlen(str_rest);
    if (sz_head + sz_rest + 1 > PATH_MAX) {
        return false;
    }
    memmove(str_out + sz_head, str_rest, sz_rest + 1);
    memcpy(str_out, str_head, sz_head);
    return true;
}

/*
 * a working directory or a root, which pivoting moves to the new root
 * when it was the old one, and every other path the way a mount's does
 */
static void map_process_path(char *str_path, bool b_place, const void *p_ctx)
{
    const struct path_map *p_map = p_ctx;
    char arr_new[PATH_MAX];

    if (b_place && p_map->b_pivot && strcmp(str_path, "/") == 0) {
        return;
    }
    if (map_path(p_map, str_path, arr_new)) {
        strcpy(str_path, arr_new);
    }
}

/* every mount under p_map's str_from, in use by no call: whether so */
static bool idle_under(const char *str_at)
{
    for (size_t sz_at = 0; sz_at < VFS_MOUNTS_MAX; sz_at++) {
        const struct mount_point *p_mount = &s_arr_mounts[sz_at];
        if (p_mount->b_used && p_mount->u_busy > 0
            && is_under(p_mount->arr_at, str_at, strlen(str_at))) {
            return false;
        }
    }
    return true;
}

/*
 * every mount's point, in the table, as p_map has it. all or none: false
 * with nothing changed when one would be too long
 */
static bool map_mounts(const struct path_map *p_map, bool b_all)
{
    static char s_arr_new[VFS_MOUNTS_MAX][PATH_MAX];

    for (size_t sz_at = 0; sz_at < VFS_MOUNTS_MAX; sz_at++) {
        const struct mount_point *p_mount = &s_arr_mounts[sz_at];
        bool b_moves = b_all || is_under(p_mount->arr_at, p_map->str_from,
                                         strlen(p_map->str_from));
        if (p_mount->b_used && b_moves
            && !map_path(p_map, p_mount->arr_at, s_arr_new[sz_at])) {
            return false;
        }
    }
    for (size_t sz_at = 0; sz_at < VFS_MOUNTS_MAX; sz_at++) {
        struct mount_point *p_mount = &s_arr_mounts[sz_at];
        bool b_moves = b_all || is_under(p_mount->arr_at, p_map->str_from,
                                         strlen(p_map->str_from));
        if (p_mount->b_used && b_moves) {
            strcpy(p_mount->arr_at, s_arr_new[sz_at]);
        }
    }
    return true;
}

int64_t vfs_mount_move(const char *str_from, const char *str_to)
{
    struct path_map map = { str_from, str_to, false };
    int64_t i_why = 0;
    uint64_t u_flags = spin_lock_irq(&s_lock);

    struct mount_point *p_mount = mounted_at(str_from);
    if (p_mount == NULL || p_mount == &s_arr_mounts[0]
        || is_under(str_to, str_from, strlen(str_from))) {
        i_why = -EINVAL;
    } else if (mounted_at(str_to) != NULL || !idle_under(str_from)) {
        i_why = -EBUSY;
    } else if (!map_mounts(&map, false)) {
        i_why = -ENAMETOOLONG;
    } else {
        process_map_paths(map_process_path, &map);
    }
    spin_unlock_irq(&s_lock, u_flags);
    return i_why;
}

int64_t vfs_pivot(const char *str_new, const char *str_old)
{
    size_t sz_new = strlen(str_new);
    struct path_map map = { str_new, str_old + sz_new, true };
    int64_t i_why = 0;
    uint64_t u_flags = spin_lock_irq(&s_lock);

    struct mount_point *p_new = mounted_at(str_new);
    struct mount_point *p_old = mounted_at(str_old);
    if (p_new == NULL || p_new == &s_arr_mounts[0]
        || strlen(str_old) <= sz_new || !is_under(str_old, str_new, sz_new)) {
        i_why = -EINVAL;
    } else if (p_old != NULL || !idle_under("/")) {
        i_why = -EBUSY;
    } else if (!map_mounts(&map, true)) {
        i_why = -ENAMETOOLONG;
    } else {
        /* the new root goes first, where the root always is */
        struct mount_point swap = s_arr_mounts[0];
        s_arr_mounts[0] = *p_new;
        *p_new = swap;
        process_map_paths(map_process_path, &map);
    }
    spin_unlock_irq(&s_lock, u_flags);
    return i_why;
}

/*
 * what keeps a mount up: a file open on what is there, which a regular
 * file and a directory are, the ones opened by a name on it
 */
struct mount_ref {
    enum vfs_kind e_kind;
    size_t sz_slot;
    const char *str_at;
};

static const struct file_operations node_ops;
static const struct file_operations dir_ops;

static bool holds_file(const struct file *p_file, const void *p_ctx)
{
    const struct mount_ref *p_ref = p_ctx;
    return (p_file->p_ops == &node_ops || p_file->p_ops == &dir_ops)
           && p_file->node.kind == p_ref->e_kind
           && p_file->node.mount == p_ref->sz_slot;
}

/* and a working directory on it or below */
static bool holds_cwd(const char *str_cwd, const void *p_ctx)
{
    const struct mount_ref *p_ref = p_ctx;
    return is_under(str_cwd, p_ref->str_at, strlen(p_ref->str_at));
}

int64_t vfs_mount_remove(const char *str_at, enum vfs_kind *p_kind,
                         size_t *p_slot)
{
    int64_t i_why = 0;
    uint64_t u_flags = spin_lock_irq(&s_lock);
    struct mount_point *p_mount = mounted_at(str_at);
    if (p_mount == NULL) {
        i_why = -EINVAL;
    } else if (p_mount == &s_arr_mounts[0] || p_mount->u_busy > 0) {
        i_why = -EBUSY;
    }
    /* nothing else may stand on it, and nobody may be in it */
    for (size_t sz_at = 1; sz_at < VFS_MOUNTS_MAX && i_why == 0; sz_at++) {
        const struct mount_point *p_other = &s_arr_mounts[sz_at];
        if (p_other != p_mount && p_other->b_used
            && strlen(p_other->arr_at) > strlen(str_at)
            && is_under(p_other->arr_at, str_at, strlen(str_at))) {
            i_why = -EBUSY;
        }
    }
    if (i_why == 0) {
        struct mount_ref ref = { p_mount->e_kind, p_mount->sz_slot,
                                 p_mount->arr_at };
        if (process_any_file(holds_file, &ref)
            || process_any_cwd(holds_cwd, &ref)) {
            i_why = -EBUSY;
        }
    }
    if (i_why == 0) {
        *p_kind = p_mount->e_kind;
        *p_slot = p_mount->sz_slot;
        p_mount->b_used = false;
    }
    spin_unlock_irq(&s_lock, u_flags);
    return i_why;
}

/*
 * a file on a disk, in memory or in /proc. vfs_read and vfs_write
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
    if (!vfs_writable(&p_file->node)) {
        return -EROFS;
    }
    vfs_refresh(&p_file->node);
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
    vfs_refresh(&p_file->node);
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

/* a file in memory stays, unlinked or not, while it is open */
static void node_release(struct file *p_file)
{
    if (p_file->node.kind == VFS_RAMFS) {
        ramfs_let_go(p_file->node.u_ino);
    }
}

static const struct file_operations node_ops = {
    .read = node_read,
    .write = node_write,
    .seek = node_seek,
    .release = node_release,
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
    /*
     * a node naming a device by its number opens that device, wherever
     * the node is. a fifo or a socket opens as nothing here yet
     */
    if (p_node->u_type == S_IFCHR || p_node->u_type == S_IFBLK) {
        return dev_open_rdev(p_node->u_rdev, u_mode);
    }
    if (vfs_is_device(p_node)) {
        return NULL;
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
        if (p_node->kind == VFS_RAMFS) {
            ramfs_hold(p_node->u_ino);
        }
    }
    return p_file;
}
