// SPDX-License-Identifier: GPL-2.0-only
/*
 * sched/usermode.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * what a user thread needs to know before it stops being a kernel one.
 */

#include <velvet/usermode.h>
#include <velvet/sched.h>
#include <velvet/thread.h>
#include <asm/context.h>
#include <velvet/input.h>
#include <velvet/tty.h>
#include <velvet/elf.h>
#include <velvet/vfs.h>
#include <velvet/kprintf.h>
#include <velvet/string.h>
#include <velvet/addrspace.h>
#include <velvet/kmalloc.h>
#include <velvet/pmm.h>
#include <velvet/vmm.h>
#include <velvet/process.h>
#include <velvet/pit.h>
#include <velvet/pipe.h>
#include <velvet/file.h>
#include <velvet/netif.h>
#include <velvet/signal.h>
#include <asm/irq.h>

/* what a user thread needs to know before it stops being a kernel one. */
struct user_start {
    uint64_t entry;
    uint64_t stack_top;
    uint64_t argc;
    uint64_t argv;      /* a user address, inside that same stack */
};

static void user_thread_start(void *arg)
{
    struct user_start *u = arg;
    uint64_t entry = u->entry;
    uint64_t stack_top = u->stack_top;
    uint64_t argc = u->argc;
    uint64_t argv = u->argv;
    kfree(u);

    /*
     * the scheduler loaded its address space and pointed the tss rsp0
     * and the syscall stack at its kernel stack when it switched the kernel in,
     * so a trap from ring 3 lands somewhere the kernel owns. everything below
     * this line is one way
     */
    context_enter_user(entry, stack_top, argc, argv);
}

/* collect *everything* that has finished, whether anybody was going to ask about it or not. */
static void reap_abandoned(void)
{
    /*
     * collecting one renumbers the walk under the kernel, so finish and start
     * over rather than trying to carry on from where the kernel was
     */
    bool collected_one = true;
    while (collected_one) {
        collected_one = false;
        for (size_t i = 0; ; i++) {
            const struct process *p = process_at(i);
            if (p == NULL) {
                break;
            }
            if (p->exited) {
                /*
                 * before the slot goes, since afterwards there is
                 * nothing left to ask which pipes it was holding
                 */
                process_fds_release(p->pid);
                net_socket_close_all(p->pid);
                net_tcp_close_all(p->pid);
                process_collect(p->pid, NULL);
                collected_one = true;
                break;
            }
        }
    }
}

/*
 * lay the arguments out on the program's own stack, top downwards:
 * first the strings, then an array of pointers to them, then the stack
 * pointer it will start on.
 *
 * the kernel is writing into a stack that belongs to an address space nobody
 * has loaded yet, so every store goes through the direct map while
 * every *pointer* has to be the address the program will see. the two
 * run in lockstep, which is what user_addr() keeps straight.
 */
struct argblock {
    uint64_t stack_top;     /* where rsp starts, 16-aligned */
    uint64_t argv;          /* user address of the pointer array */
    uint64_t argc;
};

/* the arguments go on the pages that exist before the program does. */
static bool build_args(uint64_t stack_phys, int argc, const char *const argv[],
                       struct argblock *out)
{
    uint8_t *base_k = pmm_phys_to_virt(stack_phys);
    uint8_t *top_k  = base_k + USER_STACK_EAGER * PAGE_SIZE;

    /* the kernel address of a given user address inside this stack */
    #define user_addr(va) (top_k - (USER_STACK_TOP - (va)))

    uint64_t sp = USER_STACK_TOP;
    uint64_t str_va[MAX_ARGS];

    if (argc > MAX_ARGS) {
        argc = MAX_ARGS;
    }

    /* the strings themselves, backwards so argv[0] ends up lowest */
    for (int i = argc - 1; i >= 0; i--) {
        uint64_t len = strlen(argv[i]) + 1;
        if (sp - len <= USER_STACK_TOP - USER_STACK_EAGER * PAGE_SIZE + 256) {
            return false;       /* it has to fit in what is mapped so far */
        }
        sp -= len;
        memcpy(user_addr(sp), argv[i], len);
        str_va[i] = sp;
    }

    /* then the array of pointers to them, aligned */
    sp &= ~15ull;
    sp -= (uint64_t)(argc + 1) * 8;
    uint64_t *arr = (uint64_t *)user_addr(sp);
    for (int i = 0; i < argc; i++) {
        arr[i] = str_va[i];
    }
    arr[argc] = 0;              /* the NULL every argv ends with */

    out->argv = sp;
    out->argc = (uint64_t)argc;
    out->stack_top = sp & ~15ull;   /* sysv wants rsp 16-aligned at entry */

    #undef user_addr
    return true;
}

/*
 * a program made ready to run, short of anything to run it: its own
 * address space with the image and a stack in it, where it starts, and
 * its arguments already on that stack. spawn puts a new process around
 * one, and exec swaps one in for the caller's
 */
struct user_image {
    struct addrspace *p_space;
    uint64_t u_entry;
    struct argblock args;
};

static bool image_build(const char *str_path, int i_argc,
                        const char *const pp_argv[],
                        struct user_image *p_out, const char **pp_error)
{
    /*
     * a program off the ramdisk is already in memory and is used where
     * it lies; one off the disk has to be read in first, and `owned`
     * says which happened so it can be let go of afterwards
     */
    const void *image = NULL;
    uint64_t image_size = 0;
    bool owned = false;
    if (!vfs_slurp(str_path, &image, &image_size, &owned)) {
        *pp_error = "no such file";
        return false;
    }

    const char *bad = NULL;
    if (!elf_is_loadable(image, image_size, &bad)) {
        vfs_release(image, owned);
        *pp_error = bad;
        return false;
    }

    /*
     * its own memory. two programs can now link to the same addresses
     * and never meet, which is the whole point of this milestone
     */
    struct addrspace *space = addrspace_create(vmm_kernel_pml4());
    if (space == NULL) {
        vfs_release(image, owned);
        *pp_error = "no memory for an address space";
        return false;
    }

    /*
     * two ways in, and which one depends on a single question: will
     * this image still be here when the program runs?
     *
     * a program off the ramdisk is a stretch of a tar the loader handed
     * over at boot. it never moves and is never freed, so the segments
     * can simply be *described* and each page fetched the first time it
     * is touched, the program starts without a byte of it having been
     * read.
     *
     * one off the disk is a copy on the heap that somebody has to free.
     * making it outlive the program, and every fork of the program,
     * needs a lifetime scheme that demand paging does not need in order
     * to be worth having. so that one is loaded the old way, and every
     * program in /boot/bin, which is all of them, gets the new one
     */
    struct elf_load_result loaded = { 0, 0, false, NULL };
    struct elf_segment segs[ELF_SEGMENTS_MAX];
    size_t seg_count = 0;
    const char *why = NULL;
    bool lazy = !owned;

    if (!elf_describe(image, image_size, segs, ELF_SEGMENTS_MAX, &seg_count,
                      &loaded.entry, &loaded.brk, &why)) {
        vfs_release(image, owned);
        addrspace_destroy(space);
        *pp_error = why;
        return false;
    }

    /*
     * every segment is a region either way, since a region is what says
     * what a page there may be: what a fault makes, and whether a page
     * shared after a fork may be copied to be written. one read in now
     * has its pages made already, and its regions are only that
     */
    for (size_t sz_at = 0; sz_at < seg_count; sz_at++) {
        if (!addrspace_add_region(space, segs[sz_at].vaddr, segs[sz_at].end,
                                  segs[sz_at].flags,
                                  lazy ? VMA_FILE : VMA_ANON,
                                  lazy ? (const uint8_t *)image : NULL,
                                  lazy ? segs[sz_at].offset : 0,
                                  lazy ? segs[sz_at].file_end : 0)) {
            vfs_release(image, owned);
            addrspace_destroy(space);
            *pp_error = "no room to describe the program";
            return false;
        }
    }

    if (lazy) {
        loaded.ok = true;
    } else {
        loaded = elf_load(image, image_size, space->pml4);
    }

    /*
     * whatever the kernel read the program out of is nobody's business now,
     * unless it is being read from as the program runs, which is
     * exactly what `lazy` means
     */
    if (!lazy) {
        vfs_release(image, owned);
    }

    if (!loaded.ok) {
        addrspace_destroy(space);
        *pp_error = (loaded.error != NULL) ? loaded.error : why;
        return false;
    }

    /* the break starts on the page after the last segment, and grows up */
    space->u_brk_start = (loaded.brk + PAGE_SIZE - 1)
                         & ~(uint64_t)(PAGE_SIZE - 1);
    space->u_brk = space->u_brk_start;

    /* a stack for ring 3: writable, never executable, mapped user. */
    uint64_t stack_flags = PTE_USER | PTE_WRITE | vmm_nx();
    uint64_t stack_base = USER_STACK_TOP - USER_STACK_PAGES * PAGE_SIZE;

    if (!addrspace_add_region(space, stack_base, USER_STACK_TOP,
                              stack_flags, VMA_ANON, NULL, 0, 0)) {
        addrspace_destroy(space);
        *pp_error = "no room to describe a stack";
        return false;
    }

    uint64_t eager_base = USER_STACK_TOP - USER_STACK_EAGER * PAGE_SIZE;
    uint64_t stack_phys = pmm_alloc_pages(USER_STACK_EAGER);
    if (stack_phys == 0) {
        addrspace_destroy(space);
        *pp_error = "no memory for a user stack";
        return false;
    }
    memset(pmm_phys_to_virt(stack_phys), 0, USER_STACK_EAGER * PAGE_SIZE);

    if (!vmm_map_range(space->pml4, eager_base, stack_phys,
                       USER_STACK_EAGER * PAGE_SIZE, stack_flags)) {
        pmm_free_pages(stack_phys, USER_STACK_EAGER);
        addrspace_destroy(space);
        *pp_error = "could not map a user stack";
        return false;
    }

    if (!build_args(stack_phys, i_argc, pp_argv, &p_out->args)) {
        addrspace_destroy(space);   /* which owns the stack by now */
        *pp_error = "those arguments do not fit on a stack";
        return false;
    }
    p_out->p_space = space;
    p_out->u_entry = loaded.entry;
    return true;
}

int user_spawn(const char *path, int argc, const char *const argv[],
               const char *cwd, int parent, int uid, bool announce,
               const char **error)
{
    struct user_image img;
    if (!image_build(path, argc, argv, &img, error)) {
        return 0;
    }
    struct addrspace *space = img.p_space;
    struct user_start *start = kmalloc(sizeof *start);
    if (start == NULL) {
        addrspace_destroy(space);
        *error = "no memory";
        return 0;
    }

    start->entry     = img.u_entry;
    start->stack_top = img.args.stack_top;
    start->argc      = img.args.argc;
    start->argv      = img.args.argv;

    /*
     * the process comes first, because it is what outlives the thread
     * and holds the exit code somebody will want to read
     */
    /* a program cannot ask to be somebody else: it runs as who started it */
    int pid = process_create(path, parent, uid, announce, pit_uptime_ms());
    if (pid == 0) {
        /*
         * full. init collects orphans on its own schedule and a parent
         * its own children, so anything still sitting here is something
         * somebody may yet ask about, but a machine that cannot start a
         * program is worse than one that lost an exit code, so this
         * takes the lot and tries once more
         */
        reap_abandoned();
        pid = process_create(path, parent, uid, announce, pit_uptime_ms());
    }

    /* wherever whoever started it was standing. */
    if (pid != 0 && cwd != NULL) {
        process_set_cwd(pid, cwd);
    }

    /* the environment it was born holding, which is its parent's. */
    if (pid != 0 && parent != 0) {
        static char inherited[PROC_ENV_MAX];
        size_t n = process_get_env(parent, inherited, sizeof inherited);
        process_set_env(pid, inherited, n);
    }
    if (pid == 0) {
        kfree(start);
        addrspace_destroy(space);
        *error = "the process table is full";
        return 0;
    }

    /* a program the kernel starts reads and writes the console */
    for (int i_fd = FD_STDIN; i_fd <= FD_STDERR; i_fd++) {
        struct file *p_old = NULL;
        process_fd_replace(pid, i_fd, tty_console_file(), false, &p_old);
    }

    /*
     * parked, not running.
     *
     * a thread that starts the instant it is created can print before
     * anybody has said which job it belongs to or who holds the
     * terminal, so its first line of output goes to whichever answer
     * those questions happened to have a moment ago. that is a race
     * with a very quiet failure: some output, sometimes, missing.
     *
     * so it goes into the ring asleep and whoever spawned it says when.
     * user_start is that.
     */
    struct thread *t = thread_create_parked(path, user_thread_start, start);
    if (t == NULL) {
        int ignored;
        process_exited(pid, PROCESS_KILLED, pit_uptime_ms());
        process_fds_release(pid);
        process_collect(pid, &ignored);
        kfree(start);
        addrspace_destroy(space);
        *error = "no memory for a thread";
        return 0;
    }
    t->space = space;
    t->pid   = pid;
    process_set_thread(pid, t->id);

    if (announce) {
        kprintf("[kernel] %s is pid %d, ring 3 at %p\n",
                path, pid, (void *)img.u_entry);
    }
    return pid;
}

bool user_exec(const char *str_path, int i_argc, const char *const pp_argv[],
               struct user_entry *p_out, const char **pp_error)
{
    struct thread *p_me = sched_current();
    if (p_me == NULL || p_me->pid == 0 || p_me->space == NULL) {
        *pp_error = "only a program can become another one";
        return false;
    }

    /* everything that can fail, done while the old program is still whole */
    struct user_image img;
    if (!image_build(str_path, i_argc, pp_argv, &img, pp_error)) {
        return false;
    }

    /*
     * interrupts off across the swap, so the scheduler never finds this
     * thread naming one space while the cpu walks the other's tables
     */
    struct addrspace *p_old = p_me->space;
    uint64_t u_flags = irq_save();
    p_me->space = img.p_space;
    addrspace_switch(img.p_space);
    irq_restore(u_flags);
    addrspace_destroy(p_old);

    signal_exec(process_signal_state(p_me->pid));
    process_set_name(p_me->pid, str_path);
    thread_set_name(p_me, str_path);

    p_out->u_entry = img.u_entry;
    p_out->u_stack_top = img.args.stack_top;
    p_out->u_argc = img.args.argc;
    p_out->u_argv = img.args.argv;
    return true;
}

/* wait for a pid, however it ends. */
/*
 * collect a process that has ended, letting go of its pipes first. a
 * killed one leaves them for the reaper, and a parent that collects it
 * before the reaper comes round would otherwise take the slot, and the
 * only record of those ends, with it
 */
bool user_collect(int i_pid, int *p_code)
{
    process_fds_release(i_pid);
    return process_collect(i_pid, p_code);
}

/* let a spawned process actually begin. */
void user_start(int pid)
{
    const struct process *p = process_find(pid);
    if (p != NULL && p->thread_id != 0) {
        sched_wake_thread(p->thread_id);
    }
}
