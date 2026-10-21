// SPDX-License-Identifier: GPL-2.0-only
/*
 * fs/vfs.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the virtual filesystem: names, descriptors, and two disks.
 */

#include <velvet/vfs.h>
#include <velvet/ramdisk.h>
#include <velvet/disk.h>
#include <velvet/kmalloc.h>
#include <velvet/proc.h>
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



/* "/boot", "/boot/", "/boot/anything", but not "/bootleg" */
static bool under(const char *path, const char *point, const char **rest)
{
    size_t n = strlen(point);
    for (size_t i = 0; i < n; i++) {
        if (path[i] != point[i]) {
            return false;
        }
    }
    if (path[n] != '\0' && path[n] != '/') {
        return false;
    }

    const char *tail = path + n;
    while (*tail == '/') {
        tail++;
    }
    *rest = tail;
    return true;
}

static bool under_boot(const char *path, const char **rest)
{
    return under(path, VFS_BOOT, rest);
}

/* /proc, which no disk or archive holds: the kernel writes it as it is read. */
static bool under_proc(const char *path, const char **rest)
{
    return under(path, PROC_AT, rest);
}

/*
 * a name under /work belongs to the second one and everything else to
 * the root, and that one sentence is the whole of the routing. it lives
 * here rather than in fs/disk.c on purpose: deciding that a name
 * belongs to a mount at all is the namespace's job, and disk.c's job is
 * to be a filesystem once somebody has decided.
 *
 * false means the name is not on a disk at all, it is under /boot, in
 * the ramdisk, which every caller below handles first anyway
 */
static bool disk_route(const char *path, size_t *which, const char **rest)
{
    const char *tail;
    if (under_boot(path, &tail)) {
        return false;
    }
    if (disk_ready(DISK_WORK) && under(path, DISK_WORK_AT, &tail)) {
        *which = DISK_WORK;
        /*
         * what the second filesystem calls it, which is an absolute
         * path *on that filesystem*, so it keeps its leading slash
         * and the mount point itself is that filesystem's root. every
         * mount answers the same shape of question or the two of them
         * disagree about what a path is
         */
        const char *below = path + strlen(DISK_WORK_AT);
        *rest = (below[0] == '\0') ? "/" : below;
        return true;
    }
    *which = DISK_ROOT;
    *rest = path;
    return true;
}

/*
 * and the mount points themselves, which stand in the root and are on
 * no filesystem: nothing on the disk is called "work", because /work
 * *is* the other disk
 */
static bool is_mount_point(const char *path, size_t *which)
{
    const char *rest;
    if (disk_ready(DISK_WORK) && under(path, DISK_WORK_AT, &rest)
        && rest[0] == '\0') {
        *which = DISK_WORK;
        return true;
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
}

/* one filesystem's copy of a name, if it is mounted and has one */
static bool try_disk(size_t which, const char *path, struct vfs_file *out)
{
    if (!disk_ready(which)) {
        return false;
    }
    struct disk_entry e;
    if (!disk_lookup(which, path, &e)) {
        return false;
    }
    from_disk(out, &e, which);
    return true;
}

static bool try_ramdisk(const char *path, struct vfs_file *out)
{
    struct ramdisk_file f;
    if (!ramdisk_open(path, &f)) {
        return false;
    }
    from_ramdisk(out, &f);
    return true;
}

bool vfs_open(const char *path, struct vfs_file *out)
{
    if (path == NULL || path[0] == '\0') {
        return false;
    }

    /*
     * the root belongs to no filesystem either, it is the place the
     * mounts hang from, and something has to be able to say it is a
     * directory or nobody can stand in it
     */
    if (path[0] == '/' && path[1] == '\0') {
        memset(out, 0, sizeof *out);
        out->kind = VFS_DISK;
        copy_name(out->name, "/");
        out->is_dir = true;
        /*
         * the root is the place mounts hang from rather than a file on
         * any of them, so its permissions are the layer's rather than a
         * filesystem's: anyone may look, nobody may write to it
         */
        out->mode = 0555;
        return true;
    }

    const char *rest;
    if (under_proc(path, &rest)) {
        memset(out, 0, sizeof *out);
        out->kind = VFS_PROC;
        if (rest[0] == '\0') {
            copy_name(out->name, PROC_AT + 1);
            out->is_dir = true;
            out->mode = 0555;
            return true;
        }
        if (!proc_find(rest, &out->sz_proc)) {
            return false;
        }
        /*
         * none has a size until it is read, so it says 0 and a reader
         * reads to the end like any other file. who may read it is the
         * file's own say
         */
        copy_name(out->name, rest);
        out->mode = proc_mode(out->sz_proc);
        return true;
    }
    if (under_boot(path, &rest)) {
        /* the mount point itself. */
        if (rest[0] == '\0') {
            memset(out, 0, sizeof *out);
            out->kind = VFS_RAMDISK;
            copy_name(out->name, VFS_BOOT + 1);
            out->is_dir = true;
            out->mode = 0555;
            return true;
        }
        return try_ramdisk(rest, out);
    }

    /*
     * /work itself, which is on no filesystem for the same reason
     * /boot is not: it is the place one hangs from
     */
    size_t which;
    if (is_mount_point(path, &which)) {
        memset(out, 0, sizeof *out);
        out->kind = VFS_DISK;
        out->mount = which;
        copy_name(out->name, DISK_WORK_AT + 1);
        out->is_dir = true;
        out->mode = 0555;
        return true;
    }

    if (path[0] == '/') {
        if (!disk_route(path, &which, &rest)) {
            return false;
        }
        if (which != DISK_ROOT) {
            /* a name under another mount is on that filesystem or it is nowhere. */
            return try_disk(which, rest, out);
        }

        /* the disk first, then the ramdisk with the leading slash taken off. */
        if (try_disk(DISK_ROOT, path, out)) {
            return true;
        }
        return try_ramdisk(path + 1, out);
    }

    /*
     * a bare name from inside the kernel, which has no working
     * directory of its own: the disk first, so a disk can supply a
     * newer copy of something, then the ramdisk
     */
    char absolute[VFS_NAME_MAX + 1];
    absolute[0] = '/';
    copy_name(absolute + 1, path);
    if (try_disk(DISK_ROOT, absolute, out)) {
        return true;
    }
    return try_ramdisk(path, out);
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
 * "/" has the root filesystem in it and also the mount points standing
 * on it, which belong to no filesystem and have to be added by hand.
 *
 * /work is listed only when something is mounted there. a directory in
 * a listing that cannot be entered is worse than one that is not there,
 * because the second is a fact about the machine and the first looks
 * like a fault
 */
static bool root_extra(size_t index, struct vfs_file *out)
{
    memset(out, 0, sizeof *out);
    out->is_dir = true;
    out->mode = 0555;

    if (index == 0) {
        out->kind = VFS_RAMDISK;
        copy_name(out->name, VFS_BOOT + 1);     /* past the slash */
        return true;
    }
    if (index == 1) {
        out->kind = VFS_PROC;
        copy_name(out->name, PROC_AT + 1);
        return true;
    }
    if (index == 2 && disk_ready(DISK_WORK)) {
        out->kind = VFS_DISK;
        out->mount = DISK_WORK;
        copy_name(out->name, DISK_WORK_AT + 1);
        return true;
    }
    return false;
}

bool vfs_readdir(const char *path, size_t index, struct vfs_file *out)
{
    if (path == NULL || path[0] == '\0') {
        path = "/";
    }

    /* a relative directory is relative to the root, the same as a relative filename is. */
    char absolute[VFS_NAME_MAX + 1];
    if (path[0] != '/') {
        absolute[0] = '/';
        copy_name(absolute + 1, path);
        path = absolute;
    }

    const char *rest;
    if (under_proc(path, &rest)) {
        const char *name = proc_name(index);
        if (rest[0] != '\0' || name == NULL) {
            return false;
        }
        memset(out, 0, sizeof *out);
        out->kind = VFS_PROC;
        out->sz_proc = index;
        out->mode = proc_mode(index);
        copy_name(out->name, name);
        return true;
    }
    if (under_boot(path, &rest)) {
        size_t prefix = strlen(rest);

        struct ramdisk_file f;
        size_t seen = 0;
        for (size_t i = 0; ramdisk_stat(i, &f); i++) {
            struct vfs_file v;
            from_ramdisk(&v, &f);
            if (!listed_under(&v, rest, prefix) || seen++ != index) {
                continue;
            }
            *out = v;
            return true;
        }
        return false;
    }

    /*
     * "/" and "//" and "/////" are all the root, and a trailing slash
     * never changes which directory is meant
     */
    bool is_root = true;
    for (const char *p = path; *p != '\0'; p++) {
        if (*p != '/') {
            is_root = false;
            break;
        }
    }

    size_t which;
    if (!disk_route(path, &which, &rest)) {
        return false;
    }
    if (is_mount_point(path, &which)) {
        rest = "/";             /* the other filesystem's own root */
    }

    if (disk_ready(which)) {
        struct disk_entry e;
        if (disk_readdir(which, rest, index, &e)) {
            from_disk(out, &e, which);
            return true;
        }
        /* past the end of the root filesystem, the mounts standing on it */
        if (is_root) {
            size_t count = 0;
            struct disk_entry ignored;
            while (disk_readdir(which, rest, count, &ignored)) {
                count++;
            }
            return root_extra(index - count, out);
        }
        return false;
    }

    /* no disk at all: the root holds nothing but the mount points */
    return is_root && root_extra(index, out);
}



bool vfs_create(const char *path, struct vfs_file *out)
{
    if (path == NULL || path[0] == '\0') {
        return false;
    }

    char absolute[VFS_NAME_MAX + 1];
    if (path[0] != '/') {
        absolute[0] = '/';
        copy_name(absolute + 1, path);
        path = absolute;
    }

    size_t which;
    const char *rest;
    if (!disk_route(path, &which, &rest) || !disk_ready(which)) {
        return false;       /* read-only memory. there is nowhere to put it */
    }

    struct disk_entry e;
    if (!disk_create(which, rest, &e)) {
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
#define ROUTED(path, which, rest) \
    ((path) != NULL && (path)[0] != '\0' \
     && disk_route((path), &(which), &(rest)) && disk_ready(which))

bool vfs_mkdir(const char *path)
{
    size_t which;
    const char *rest;
    /* read-only memory has no room for a new name */
    return ROUTED(path, which, rest) && disk_mkdir(which, rest);
}

bool vfs_rmdir(const char *path)
{
    size_t which;
    const char *rest;
    return ROUTED(path, which, rest) && disk_rmdir(which, rest);
}

bool vfs_chmod(const char *path, uint32_t mode)
{
    size_t which;
    const char *rest;
    /* a tar in read-only memory has no opinions */
    return ROUTED(path, which, rest) && disk_chmod(which, rest, mode);
}

bool vfs_chown(const char *path, uint32_t uid, uint32_t gid)
{
    size_t which;
    const char *rest;
    return ROUTED(path, which, rest) && disk_chown(which, rest, uid, gid);
}

bool vfs_symlink(const char *path, const char *target)
{
    size_t which;
    const char *rest;
    return target != NULL && ROUTED(path, which, rest)
        && disk_symlink(which, rest, target);
}

bool vfs_readlink(const char *path, char *out, size_t size)
{
    size_t which;
    const char *rest;
    return ROUTED(path, which, rest) && disk_readlink(which, rest, out, size);
}

bool vfs_open_nofollow(const char *path, struct vfs_file *out)
{
    size_t which;
    const char *rest;
    if (ROUTED(path, which, rest)) {
        struct disk_entry e;
        if (disk_lookup_nofollow(which, rest, &e)) {
            from_disk(out, &e, which);
            return true;
        }
    }
    /* not on the disk, or not a disk that has symlinks. */
    return vfs_open(path, out);
}

bool vfs_unlink(const char *path)
{
    size_t which;
    const char *rest;
    /* the ramdisk is memory the kernel may not write */
    return ROUTED(path, which, rest) && disk_unlink(which, rest);
}

bool vfs_rename(const char *from, const char *to)
{
    size_t a, b;
    const char *rest_a, *rest_b;
    if (!ROUTED(from, a, rest_a) || !ROUTED(to, b, rest_b)) {
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
