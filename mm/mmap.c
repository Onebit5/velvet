// SPDX-License-Identifier: GPL-2.0-only
/*
 * mm/mmap.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * a program asking for memory, giving it back, and changing what it may do.
 */

#include <velvet/syscalls.h>
#include <velvet/errno.h>
#include <velvet/addrspace.h>
#include <velvet/file.h>
#include <velvet/pmm.h>
#include <velvet/process.h>
#include <velvet/sched.h>
#include <velvet/shared.h>
#include <velvet/thread.h>
#include <velvet/vmm.h>
#include <asm/uaccess.h>
#include <uapi/velvet/mman.h>

/*
 * the most a program may ask for in one go. a limit rather than "as
 * much as the address space holds", because the pages are not made
 * until they are touched, so without one, a program could reserve
 * more than the machine has and only find out halfway through using it
 */
#define MMAP_MAX (64ull * 1024 * 1024)

#define PAGE_OFFSET(x) ((x) & (PAGE_SIZE - 1))

static struct addrspace *my_space(void)
{
    struct thread *p_me = sched_current();
    return (p_me != NULL) ? p_me->space : NULL;
}

/*
 * what a page under i_prot is. a region that allows nothing is a region
 * all the same, there to be in the way, and its pages are not the
 * program's at all. writable is readable here, and so is executable
 */
static uint64_t flags_for(int i_prot)
{
    if ((i_prot & (PROT_READ | PROT_WRITE | PROT_EXEC)) == 0) {
        return 0;
    }
    uint64_t u_flags = PTE_USER;
    if (i_prot & PROT_WRITE) {
        u_flags |= PTE_WRITE;
    }
    if (!(i_prot & PROT_EXEC)) {
        u_flags |= vmm_nx();
    }
    return u_flags;
}

int64_t sys_mmap(uint64_t u_addr, uint64_t u_len, int i_prot, int i_flags,
                 int i_fd, int64_t i_offset)
{
    struct addrspace *p_as = my_space();
    if (p_as == NULL) {
        return -ENOMEM;
    }
    if (u_len == 0 || PAGE_OFFSET((uint64_t)i_offset) != 0 || i_offset < 0
        || (i_prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC)) != 0) {
        return -EINVAL;
    }
    int i_type = i_flags & MAP_TYPE;
    if (i_type != MAP_PRIVATE && i_type != MAP_SHARED
        && i_type != MAP_SHARED_VALIDATE) {
        return -EINVAL;
    }
    if (u_len > MMAP_MAX) {
        return -ENOMEM;
    }
    bool b_fixed = (i_flags & MAP_FIXED) != 0;
    if (b_fixed && PAGE_OFFSET(u_addr) != 0) {
        return -EINVAL;
    }

    bool b_shared = (i_type != MAP_PRIVATE);
    struct file *p_file = NULL;
    if (!(i_flags & MAP_ANONYMOUS)) {
        p_file = process_fd_file(current_pid(), i_fd);
        if (p_file == NULL) {
            return -EBADF;
        }
        int64_t i_why = 0;
        if (!vfs_mappable(p_file)) {
            i_why = -ENODEV;
        } else if (!(p_file->u_mode & FILE_READ)
                   || (b_shared && (i_prot & PROT_WRITE)
                       && !(p_file->u_mode & FILE_WRITE))) {
            i_why = -EACCES;
        }
        if (i_why < 0) {
            file_put(p_file);
            return i_why;
        }
    }

    /*
     * a shared mapping is of the set of pages every mapping of the file
     * shares, or of a set of its own when there is no file, which a fork
     * shares too. a file with no inode has no set to be found by the next
     * mapping of it, and is mapped as a copy when that is all the same,
     * because it is never written
     */
    struct shared_pages *p_set = NULL;
    bool b_may_write = p_file == NULL || !b_shared
                       || (p_file->u_mode & FILE_WRITE);
    if (b_shared) {
        p_set = (p_file != NULL) ? shared_of_file(p_file) : shared_anon();
        if (p_set == NULL && b_may_write) {
            int64_t i_why = (p_file != NULL && p_file->node.u_ino == 0)
                            ? -ENODEV : -ENOMEM;
            file_put(p_file);
            return i_why;
        }
    }

    /*
     * a file's pages are read in as they are touched, by the region, which
     * holds the file open from here on whatever becomes of the descriptor
     */
    uint64_t u_flags = flags_for(i_prot);
    bool b_noreplace = (i_flags & MAP_FIXED_NOREPLACE) != 0;
    bool b_over = b_fixed && !b_noreplace;
    uint64_t u_at;
    if (p_set != NULL) {
        u_at = addrspace_map_shared(p_as, u_addr, u_len, u_flags, b_over,
                                    p_set, p_file, (uint64_t)i_offset,
                                    b_may_write);
    } else if (p_file != NULL) {
        u_at = addrspace_map_file(p_as, u_addr, u_len, u_flags, b_over,
                                  p_file, (uint64_t)i_offset, b_may_write);
    } else {
        u_at = addrspace_map_anon(p_as, u_addr, u_len, u_flags, b_over);
    }
    shared_put(p_set);
    file_put(p_file);
    if (b_noreplace && u_at != 0 && u_at != u_addr) {
        addrspace_unmap(p_as, u_at, u_len);
        return -EEXIST;
    }
    if (u_at == 0) {
        return b_fixed ? -EINVAL : -ENOMEM;
    }
    return (int64_t)u_at;
}

int64_t sys_munmap(uint64_t u_addr, uint64_t u_len)
{
    struct addrspace *p_as = my_space();
    if (p_as == NULL || u_len == 0 || PAGE_OFFSET(u_addr) != 0
        || !access_ok(u_addr, u_len)) {
        return -EINVAL;
    }
    return addrspace_unmap(p_as, u_addr, u_len) ? 0 : -ENOMEM;
}

int64_t sys_mprotect(uint64_t u_addr, uint64_t u_len, int i_prot)
{
    struct addrspace *p_as = my_space();
    if (p_as == NULL || PAGE_OFFSET(u_addr) != 0
        || (i_prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC)) != 0
        || !access_ok(u_addr, u_len)) {
        return -EINVAL;
    }
    if (u_len == 0) {
        return 0;
    }
    if ((i_prot & PROT_WRITE) && !addrspace_may_write(p_as, u_addr, u_len)) {
        return -EACCES;
    }
    return addrspace_protect(p_as, u_addr, u_len, flags_for(i_prot))
           ? 0 : -ENOMEM;
}

/*
 * what was stored to files mapped shared in the range, written to them.
 * the write is made before the call returns whichever is asked for:
 * MS_ASYNC only promises it will be, and a read of the file already says
 * what the mapping has, which is all the promise is for. MS_INVALIDATE
 * asks for what is already so, every mapping of a file having its pages
 */
int64_t sys_msync(uint64_t u_addr, uint64_t u_len, int i_flags)
{
    struct addrspace *p_as = my_space();
    if (p_as == NULL || PAGE_OFFSET(u_addr) != 0
        || (i_flags & ~(MS_ASYNC | MS_INVALIDATE | MS_SYNC)) != 0
        || ((i_flags & MS_ASYNC) && (i_flags & MS_SYNC))
        || !access_ok(u_addr, u_len)) {
        return -EINVAL;
    }
    if (u_len == 0) {
        return 0;
    }
    return addrspace_sync(p_as, u_addr, u_len);
}

/* the break where it ended up: where it was asked to go, or where it was */
int64_t sys_brk(uint64_t u_want)
{
    struct addrspace *p_as = my_space();
    if (p_as == NULL) {
        return 0;
    }
    return (int64_t)addrspace_brk(p_as, u_want,
                                  PTE_USER | PTE_WRITE | vmm_nx());
}

/*
 * advice is taken or ignored, both being allowed, except for the one
 * kind a program relies on: pages it no longer needs, which are given
 * back and come back as they first came, zeroes or the file's bytes
 */
int64_t sys_madvise(uint64_t u_addr, uint64_t u_len, int i_advice)
{
    struct addrspace *p_as = my_space();
    if (p_as == NULL || PAGE_OFFSET(u_addr) != 0
        || !access_ok(u_addr, u_len)) {
        return -EINVAL;
    }
    switch (i_advice) {
    case MADV_NORMAL:
    case MADV_RANDOM:
    case MADV_SEQUENTIAL:
    case MADV_WILLNEED:
        return 0;
    case MADV_DONTNEED:
    case MADV_FREE:
        if (u_len == 0) {
            return 0;
        }
        return addrspace_discard(p_as, u_addr, u_len) ? 0 : -ENOMEM;
    default:
        return -EINVAL;
    }
}

/*
 * smaller in place, or the same. growing would mean finding the room or
 * moving the pages, and a libc that is told there is no room does the
 * copy itself
 */
int64_t sys_mremap(uint64_t u_old, uint64_t u_old_len, uint64_t u_new_len,
                   int i_flags, uint64_t u_new)
{
    struct addrspace *p_as = my_space();
    (void)u_new;
    if (p_as == NULL || PAGE_OFFSET(u_old) != 0 || u_new_len == 0
        || (i_flags & ~(MREMAP_MAYMOVE | MREMAP_FIXED)) != 0
        || ((i_flags & MREMAP_FIXED) && !(i_flags & MREMAP_MAYMOVE))
        || !access_ok(u_old, u_old_len)) {
        return -EINVAL;
    }
    uint64_t u_old_top = (u_old_len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    uint64_t u_new_top = (u_new_len + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if (i_flags & MREMAP_FIXED) {
        return -ENOMEM;
    }
    if (u_new_top > u_old_top) {
        return -ENOMEM;
    }
    if (u_new_top < u_old_top
        && !addrspace_unmap(p_as, u_old + u_new_top, u_old_top - u_new_top)) {
        return -ENOMEM;
    }
    return (int64_t)u_old;
}
