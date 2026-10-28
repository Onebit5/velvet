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
#include <velvet/random.h>
#include <velvet/errno.h>
#include <asm/irq.h>
#include <asm/uaccess.h>
#include <uapi/velvet/auxvec.h>

/* what a user thread needs to know before it stops being a kernel one. */
struct user_start {
    uint64_t entry;
    uint64_t stack_top;
};

static void user_thread_start(void *arg)
{
    struct user_start *u = arg;
    uint64_t entry = u->entry;
    uint64_t stack_top = u->stack_top;
    kfree(u);

    /*
     * the scheduler loaded its address space and pointed the tss rsp0
     * and the syscall stack at its kernel stack when it switched the kernel in,
     * so a trap from ring 3 lands somewhere the kernel owns. everything below
     * this line is one way
     */
    context_enter_user(entry, stack_top);
}

/*
 * one vector of strings out of ring 3, appended to what is in p_buf: how
 * many there were, or why not. the room counts a pointer for each as
 * well as its bytes, as both will lie on the new program's stack
 */
static int64_t copy_vector(uint64_t u_vec, char *p_buf, size_t *p_used,
                           size_t *p_count, size_t sz_counted)
{
    if (u_vec == 0) {
        return 0;
    }
    for (size_t sz_at = 0;; sz_at++) {
        uint64_t u_str = 0;
        if (get_user_u64(&u_str, u_vec + sz_at * 8) != 0) {
            return -EFAULT;
        }
        if (u_str == 0) {
            return 0;
        }
        size_t sz_pointers = (sz_counted + *p_count + 1) * 8;
        if (*p_used + sz_pointers >= EXEC_ARGS_MAX) {
            return -E2BIG;
        }
        size_t sz_room = EXEC_ARGS_MAX - sz_pointers - *p_used;
        int64_t i_len = strncpy_from_user(p_buf + *p_used, u_str, sz_room);
        if (i_len < 0) {
            return (i_len == -ENAMETOOLONG) ? -E2BIG : i_len;
        }
        *p_used += (size_t)i_len + 1;
        (*p_count)++;
    }
}

/* pointers to the count strings lying one after another from p_from */
static void point_at(const char **pp_out, char *p_from, size_t sz_count)
{
    for (size_t sz_at = 0; sz_at < sz_count; sz_at++) {
        pp_out[sz_at] = p_from;
        p_from += strlen(p_from) + 1;
    }
    pp_out[sz_count] = NULL;
}

int64_t exec_args_copy(uint64_t u_argv, uint64_t u_envp,
                       struct exec_args *p_out)
{
    memset(p_out, 0, sizeof *p_out);
    char *p_buf = kmalloc(EXEC_ARGS_MAX);
    if (p_buf == NULL) {
        return -ENOMEM;
    }
    size_t sz_used = 0;
    size_t sz_argc = 0;
    size_t sz_envc = 0;
    int64_t i_why = copy_vector(u_argv, p_buf, &sz_used, &sz_argc, 0);
    if (i_why == 0) {
        i_why = copy_vector(u_envp, p_buf, &sz_used, &sz_envc, sz_argc);
    }
    const char **pp_vec = (i_why == 0)
                          ? kmalloc((sz_argc + sz_envc + 2) * sizeof *pp_vec)
                          : NULL;
    if (i_why == 0 && pp_vec == NULL) {
        i_why = -ENOMEM;
    }
    if (i_why < 0) {
        kfree(p_buf);
        return i_why;
    }
    point_at(pp_vec, p_buf, sz_argc);
    char *p_env = p_buf;
    for (size_t sz_at = 0; sz_at < sz_argc; sz_at++) {
        p_env += strlen(p_env) + 1;
    }
    point_at(pp_vec + sz_argc + 1, p_env, sz_envc);

    p_out->pp_argv = pp_vec;
    p_out->sz_argc = sz_argc;
    p_out->pp_envp = pp_vec + sz_argc + 1;
    p_out->sz_envc = sz_envc;
    p_out->p_strings = p_buf;
    return 0;
}

void exec_args_free(struct exec_args *p_args)
{
    kfree(p_args->p_strings);
    kfree((void *)p_args->pp_argv);
    memset(p_args, 0, sizeof *p_args);
}

/* the auxiliary vector's entries, AT_NULL's included */
#define AUX_PAIRS 18

/* what the auxiliary vector says about the program and who runs it */
struct aux_facts {
    uint64_t u_entry;
    uint64_t u_phdr;
    uint64_t u_phnum;
    int i_uid;
    int i_gid;
};

/* the bytes a stack laid out for p_args needs, all of it */
static size_t stack_need(const struct exec_args *p_args, const char *str_path)
{
    size_t sz_bytes = strlen(str_path) + 1 + 16;
    for (size_t sz_at = 0; sz_at < p_args->sz_argc; sz_at++) {
        sz_bytes += strlen(p_args->pp_argv[sz_at]) + 1;
    }
    for (size_t sz_at = 0; sz_at < p_args->sz_envc; sz_at++) {
        sz_bytes += strlen(p_args->pp_envp[sz_at]) + 1;
    }
    sz_bytes += (1 + p_args->sz_argc + 1 + p_args->sz_envc + 1
                 + 2 * AUX_PAIRS) * 8;
    return sz_bytes + 32;       /* the two alignments */
}

/*
 * the stack a program starts on, as every x86_64 program expects to find
 * it. from rsp up: argc, the argv pointers and a NULL, the envp pointers
 * and a NULL, and the auxiliary vector ending in AT_NULL; above all of
 * that the strings they point at, the path the program was run by, and
 * sixteen bytes nobody could predict, for a libc's stack protector.
 *
 * written through the direct map into pages the new space does not have
 * loaded yet, while every pointer is the address the program will see,
 * which is what user_addr keeps straight. the stack pointer it starts
 * on, 16-aligned
 */
static uint64_t build_stack(uint8_t *p_top_k, const struct exec_args *p_args,
                            const char *str_path,
                            const struct aux_facts *p_facts,
                            struct addrspace *p_space)
{
    #define user_addr(va) (p_top_k - (USER_STACK_TOP - (va)))

    uint64_t u_sp = USER_STACK_TOP;

    size_t sz_len = strlen(str_path) + 1;
    u_sp -= sz_len;
    memcpy(user_addr(u_sp), str_path, sz_len);
    uint64_t u_execfn = u_sp;

    u_sp -= 16;
    random_bytes(user_addr(u_sp), 16);
    uint64_t u_random = u_sp;

    /*
     * the environment's strings and then the arguments', each set last
     * first, so that from argv[0] up they lie in the order they are
     * numbered, and the pointers can be worked out by walking them
     */
    p_space->u_env_end = u_sp;
    for (size_t sz_at = p_args->sz_envc; sz_at-- > 0; ) {
        sz_len = strlen(p_args->pp_envp[sz_at]) + 1;
        u_sp -= sz_len;
        memcpy(user_addr(u_sp), p_args->pp_envp[sz_at], sz_len);
    }
    p_space->u_env_start = u_sp;
    for (size_t sz_at = p_args->sz_argc; sz_at-- > 0; ) {
        sz_len = strlen(p_args->pp_argv[sz_at]) + 1;
        u_sp -= sz_len;
        memcpy(user_addr(u_sp), p_args->pp_argv[sz_at], sz_len);
    }
    uint64_t u_string = u_sp;

    uint64_t arr_aux[AUX_PAIRS * 2] = {
        AT_PHDR,   p_facts->u_phdr,
        AT_PHENT,  ELF_PHDR_SIZE,
        AT_PHNUM,  p_facts->u_phnum,
        AT_PAGESZ, PAGE_SIZE,
        AT_BASE,   0,
        AT_FLAGS,  0,
        AT_ENTRY,  p_facts->u_entry,
        AT_UID,    (uint64_t)p_facts->i_uid,
        AT_EUID,   (uint64_t)p_facts->i_uid,
        AT_GID,    (uint64_t)p_facts->i_gid,
        AT_EGID,   (uint64_t)p_facts->i_gid,
        AT_SECURE, 0,
        AT_CLKTCK, 100,
        AT_HWCAP,  0,
        AT_RANDOM, u_random,
        AT_EXECFN, u_execfn,
        AT_IGNORE, 0,
        AT_NULL,   0,
    };

    /*
     * everything below the strings is words, and argc has to land on a
     * sixteen byte boundary, so the gap goes above the words
     */
    size_t sz_words = 1 + p_args->sz_argc + 1 + p_args->sz_envc + 1
                      + AUX_PAIRS * 2;
    u_sp &= ~15ull;
    if (sz_words % 2 != 0) {
        u_sp -= 8;
    }
    u_sp -= sz_words * 8;

    uint64_t *p_word = (uint64_t *)user_addr(u_sp);
    *p_word++ = p_args->sz_argc;
    for (size_t sz_at = 0; sz_at < p_args->sz_argc; sz_at++) {
        *p_word++ = u_string;
        u_string += strlen(p_args->pp_argv[sz_at]) + 1;
    }
    *p_word++ = 0;
    for (size_t sz_at = 0; sz_at < p_args->sz_envc; sz_at++) {
        *p_word++ = u_string;
        u_string += strlen(p_args->pp_envp[sz_at]) + 1;
    }
    *p_word++ = 0;
    memcpy(p_word, arr_aux, sizeof arr_aux);

    #undef user_addr
    return u_sp;
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
    uint64_t u_stack_top;
};

static int64_t image_build(const char *str_path,
                           const struct exec_args *p_args, int i_uid,
                           int i_gid, struct user_image *p_out,
                           const char **pp_error)
{
    /*
     * the stack has to hold all of it before the program has run an
     * instruction, so what does not fit is refused before anything is
     * read
     */
    size_t sz_need = stack_need(p_args, str_path);
    size_t sz_eager = (sz_need + PAGE_SIZE - 1) / PAGE_SIZE + 1;
    if (sz_eager < USER_STACK_EAGER) {
        sz_eager = USER_STACK_EAGER;
    }
    if (sz_eager > USER_STACK_PAGES / 4) {
        *pp_error = "those arguments do not fit on a stack";
        return -E2BIG;
    }

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
        return -ENOENT;
    }

    const char *bad = NULL;
    if (!elf_is_loadable(image, image_size, &bad)) {
        vfs_release(image, owned);
        *pp_error = bad;
        return -ENOEXEC;
    }

    struct aux_facts facts = { .i_uid = i_uid, .i_gid = i_gid };
    elf_headers_at(image, image_size, &facts.u_phdr, &facts.u_phnum);

    /*
     * its own memory. two programs can now link to the same addresses
     * and never meet, which is the whole point of this milestone
     */
    struct addrspace *space = addrspace_create(vmm_kernel_pml4());
    if (space == NULL) {
        vfs_release(image, owned);
        *pp_error = "no memory for an address space";
        return -ENOMEM;
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
        return -ENOEXEC;
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
            return -ENOMEM;
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
        return -ENOEXEC;
    }
    facts.u_entry = loaded.entry;

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
        return -ENOMEM;
    }

    uint64_t eager_base = USER_STACK_TOP - sz_eager * PAGE_SIZE;
    uint64_t stack_phys = pmm_alloc_pages(sz_eager);
    if (stack_phys == 0) {
        addrspace_destroy(space);
        *pp_error = "no memory for a user stack";
        return -ENOMEM;
    }
    memset(pmm_phys_to_virt(stack_phys), 0, sz_eager * PAGE_SIZE);

    if (!vmm_map_range(space->pml4, eager_base, stack_phys,
                       sz_eager * PAGE_SIZE, stack_flags)) {
        pmm_free_pages(stack_phys, sz_eager);
        addrspace_destroy(space);
        *pp_error = "could not map a user stack";
        return -ENOMEM;
    }

    uint8_t *p_top_k = (uint8_t *)pmm_phys_to_virt(stack_phys)
                       + sz_eager * PAGE_SIZE;
    p_out->u_stack_top = build_stack(p_top_k, p_args, str_path, &facts,
                                     space);
    p_out->p_space = space;
    p_out->u_entry = loaded.entry;
    return 0;
}

int user_spawn(const char *path, const struct exec_args *p_args,
               const char *cwd, int parent, int uid, bool announce,
               const char **error)
{
    struct user_image img;
    if (image_build(path, p_args, uid, uid, &img, error) < 0) {
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
    start->stack_top = img.u_stack_top;

    /*
     * the process comes first, because it is what outlives the thread
     * and holds the exit code somebody will want to read
     */
    /* a program cannot ask to be somebody else: it runs as who started it */
    int pid = process_create(path, parent, uid, announce, pit_uptime_ms());

    /* wherever whoever started it was standing. */
    if (pid != 0 && cwd != NULL) {
        process_set_cwd(pid, cwd);
    }
    struct file *p_console = (pid != 0) ? tty_open(0, FILE_READ | FILE_WRITE)
                                        : NULL;
    if (p_console == NULL) {
        if (pid != 0) {
            process_exited(pid, PROCESS_KILLED, pit_uptime_ms());
            process_collect(pid, NULL);
        }
        kfree(start);
        addrspace_destroy(space);
        *error = (pid == 0) ? "the process table is full"
                            : "no memory for a terminal";
        return 0;
    }

    /*
     * a program the kernel starts reads and writes the first console,
     * one open of it on all three, as though it had opened it once and
     * duplicated that twice
     */
    for (int i_fd = FD_STDIN; i_fd <= FD_STDERR; i_fd++) {
        struct file *p_old = NULL;
        file_get(p_console);
        process_fd_replace(pid, i_fd, p_console, false, &p_old);
    }
    file_put(p_console);

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
        process_exited(pid, PROCESS_KILLED, pit_uptime_ms());
        process_fds_release(pid);
        process_collect(pid, NULL);
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

int64_t user_exec(const char *str_path, const struct exec_args *p_args,
                  struct user_entry *p_out)
{
    struct thread *p_me = sched_current();
    if (p_me == NULL || p_me->pid == 0 || p_me->space == NULL) {
        return -EPERM;
    }
    const struct process *p_proc = process_find(p_me->pid);
    int i_uid = (p_proc != NULL) ? p_proc->uid : 0;
    int i_gid = (p_proc != NULL) ? p_proc->i_gid : 0;

    /* everything that can fail, done while the old program is still whole */
    struct user_image img;
    const char *str_why = NULL;
    int64_t i_why = image_build(str_path, p_args, i_uid, i_gid, &img,
                                &str_why);
    if (i_why < 0) {
        return i_why;
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
    process_vfork_done(p_me->pid);

    /*
     * known by the last part of the path it was started by, the way a
     * program is everywhere, and not by where on the medium it happened
     * to be: every program under one long directory would otherwise have
     * that directory's name
     */
    const char *str_name = strrchr(str_path, '/');
    str_name = (str_name != NULL && str_name[1] != '\0') ? str_name + 1
                                                         : str_path;
    process_set_name(p_me->pid, str_name);
    thread_set_name(p_me, str_name);

    p_out->u_entry = img.u_entry;
    p_out->u_stack_top = img.u_stack_top;
    return 0;
}

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
