// SPDX-License-Identifier: GPL-2.0-only
/*
 * fs/blockdev.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * a drive or a partition, open: read and written by the byte from
 * wherever the position is, through the same cache a filesystem on it
 * goes through.
 */

#include <velvet/blockdev.h>
#include <velvet/disk.h>
#include <velvet/file.h>
#include <asm/uaccess.h>
#include <uapi/velvet/errno.h>
#include <uapi/velvet/fcntl.h>
#include <uapi/velvet/fs.h>

/*
 * which one an open file is, by its number, the one thing about it that
 * means the same in /dev, in a stat and on a filesystem holding a node
 * for it
 */
static const struct disk_block *block_of(const struct file *p_file)
{
    return disk_block_find(p_file->u_rdev);
}

static uint64_t bytes_of(const struct disk_block *p_block)
{
    return p_block->u_sectors * BCACHE_SECTOR;
}

static int64_t block_read(struct file *p_file, void *p_buf, size_t sz_len)
{
    const struct disk_block *p_block = block_of(p_file);
    if (p_block == NULL) {
        return -ENXIO;
    }
    int64_t i_got = disk_block_read(p_block, p_file->u_pos, p_buf, sz_len);
    if (i_got > 0) {
        p_file->u_pos += (uint64_t)i_got;
    }
    return i_got;
}

static int64_t block_write(struct file *p_file, const void *p_buf,
                           size_t sz_len)
{
    const struct disk_block *p_block = block_of(p_file);
    if (p_block == NULL) {
        return -ENXIO;
    }
    int64_t i_put = disk_block_write(p_block, p_file->u_pos, p_buf, sz_len);
    if (i_put > 0) {
        p_file->u_pos += (uint64_t)i_put;
    }
    return i_put;
}

/*
 * anywhere at all: a read past the end is the end, and a write there has
 * no room
 */
static int64_t block_seek(struct file *p_file, int64_t i_offset, int i_whence)
{
    const struct disk_block *p_block = block_of(p_file);
    if (p_block == NULL) {
        return -ENXIO;
    }
    int64_t i_from = (i_whence == SEEK_SET) ? 0
                   : (i_whence == SEEK_CUR) ? (int64_t)p_file->u_pos
                   : (i_whence == SEEK_END) ? (int64_t)bytes_of(p_block)
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

static int64_t block_ioctl(struct file *p_file, uint32_t u_cmd,
                           uint64_t u_arg)
{
    const struct disk_block *p_block = block_of(p_file);
    if (p_block == NULL) {
        return -ENXIO;
    }
    switch (u_cmd) {
    case BLKGETSIZE64:
        return (put_user_u64(u_arg, bytes_of(p_block)) == 0) ? 0 : -EFAULT;
    case BLKGETSIZE:
        return (put_user_u64(u_arg, p_block->u_sectors) == 0) ? 0 : -EFAULT;
    case BLKSSZGET:
    case BLKPBSZGET:
        return (put_user_i32(u_arg, BCACHE_SECTOR) == 0) ? 0 : -EFAULT;
    case BLKFLSBUF:
        return disk_sync() ? 0 : -EIO;
    default:
        return -ENOTTY;
    }
}

/*
 * the last close writes out what is held for the drive, so a program
 * that copied an image onto one and closed it has its bytes on the
 * drive, which is what it will go and boot from
 */
static void block_release(struct file *p_file)
{
    (void)p_file;
    (void)disk_sync();
}

static const struct file_operations block_ops = {
    .read = block_read,
    .write = block_write,
    .seek = block_seek,
    .ioctl = block_ioctl,
    .release = block_release,
};

struct file *blockdev_open(uint32_t u_rdev, uint32_t u_mode)
{
    if (disk_block_find(u_rdev) == NULL) {
        return NULL;
    }
    struct file *p_file = file_alloc(&block_ops, u_mode, FILE_TYPE_BLOCK);
    if (p_file != NULL) {
        p_file->u_rdev = u_rdev;
    }
    return p_file;
}
