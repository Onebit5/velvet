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
#include <velvet/kmalloc.h>
#include <velvet/pmm.h>
#include <velvet/process.h>
#include <velvet/sched.h>
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

/*
 * a file's bytes from i_offset, into the u_len just mapped at u_at, and
 * zeroes past its end. read now rather than as each page is touched:
 * the copy is private, so the one difference is when the cost is paid
 */
static int64_t fill_from(struct file *p_file, uint64_t u_at, uint64_t u_len,
                         int64_t i_offset)
{
    char *p_buf = kmalloc(PAGE_SIZE);
    if (p_buf == NULL) {
        return -ENOMEM;
    }
    uint64_t u_was = p_file->u_pos;
    p_file->u_pos = (uint64_t)i_offset;
    int64_t i_result = 0;
    for (uint64_t u_done = 0; u_done < u_len; u_done += PAGE_SIZE) {
        int64_t i_got = p_file->p_ops->read(p_file, p_buf, PAGE_SIZE);
        if (i_got < 0) {
            i_result = i_got;
            break;
        }
        if (i_got == 0) {
            break;
        }
        if (copy_to_user(u_at + u_done, p_buf, (size_t)i_got) != 0) {
            i_result = -EFAULT;
            break;
        }
    }
    p_file->u_pos = u_was;
    kfree(p_buf);
    return i_result;
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
        if (p_file->e_type != FILE_TYPE_REGULAR
            || p_file->p_ops->read == NULL) {
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
     * nothing here shares a page between two spaces except for a fork's
     * moment before the first write, so a mapping that promises writes
     * will be seen elsewhere cannot be kept. one that is never written
     * reads the same either way
     */
    if (b_shared && (i_prot & PROT_WRITE)) {
        file_put(p_file);
        return -ENODEV;
    }

    /*
     * a file's bytes are put in through a writable mapping, and the
     * protection asked for comes after
     */
    uint64_t u_flags = flags_for(i_prot);
    uint64_t u_first = (p_file != NULL) ? (PTE_USER | PTE_WRITE | vmm_nx())
                                        : u_flags;
    uint64_t u_at;
    if (i_flags & MAP_FIXED_NOREPLACE) {
        u_at = addrspace_map_anon(p_as, u_addr, u_len, u_first, false);
        if (u_at != 0 && u_at != u_addr) {
            addrspace_unmap(p_as, u_at, u_len);
            file_put(p_file);
            return -EEXIST;
        }
    } else {
        u_at = addrspace_map_anon(p_as, u_addr, u_len, u_first, b_fixed);
    }
    if (u_at == 0) {
        file_put(p_file);
        return b_fixed ? -EINVAL : -ENOMEM;
    }
    if (p_file != NULL) {
        int64_t i_why = fill_from(p_file, u_at, u_len, i_offset);
        file_put(p_file);
        if (i_why == 0 && u_flags != u_first
            && !addrspace_protect(p_as, u_at, u_len, u_flags)) {
            i_why = -ENOMEM;
        }
        if (i_why < 0) {
            addrspace_unmap(p_as, u_at, u_len);
            return i_why;
        }
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
    return addrspace_protect(p_as, u_addr, u_len, flags_for(i_prot))
           ? 0 : -ENOMEM;
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
