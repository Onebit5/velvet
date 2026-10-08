// SPDX-License-Identifier: GPL-2.0-only
/*
 * sched/usermode.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * what a user thread needs to know before it stops being a kernel one.
 */

#include "sched/usermode.h"
#include "sched/sched.h"
#include "sched/thread.h"
#include "arch/context.h"
#include "drivers/input.h"
#include "drivers/tty.h"
#include "fs/elf.h"
#include "fs/vfs.h"
#include "lib/kprintf.h"
#include "lib/string.h"
#include "mm/addrspace.h"
#include "mm/kmalloc.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "sched/process.h"
#include "drivers/pit.h"
#include "fs/pipe.h"
#include "net/netif.h"

const char *const USER_RUN_NO_SUCH_FILE = "no such file";

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
                pipe_release_for(p->pid);
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

/* put a file where the console or the keyboard would have been. */
static bool redirect(int pid, int fd, const char *path, bool writing,
                     bool append, const char **error)
{
    struct vfs_file f;
    int uid = process_uid(pid);

    if (!writing) {
        if (!vfs_open(path, &f) || f.is_dir) {
            *error = "no such file to read from";
            return false;
        }
        /* the same question `open` asks, asked in the same way. */
        if (!vfs_may_read(&f, uid)) {
            *error = "that file is not yours to read";
            return false;
        }
    } else {
        if (uid != 0) {
            *error = "only the master may write files";
            return false;
        }
        if (!append) {
            /* truncate by removing it first. */
            struct vfs_file existing;
            if (vfs_open(path, &existing)) {
                if (existing.is_dir) {
                    *error = "that is a directory";
                    return false;
                }
                vfs_unlink(path);
            }
        }
        if (!vfs_create(path, &f)) {
            *error = "cannot write there, only the disk takes new files";
            return false;
        }
    }

    struct fd slot;
    memset(&slot, 0, sizeof slot);
    slot.size = f.size;
    slot.writing = writing;
    slot.pos = (writing && append) ? f.size : 0;

    if (f.kind == VFS_DISK) {
        slot.kind = FD_DISK;
        slot.mount = f.mount;
        slot.cluster = f.cluster;
        slot.entry_sector = f.entry_sector;
        slot.entry_offset = f.entry_offset;
    } else if (f.kind == VFS_SOURCE) {
        slot.kind = FD_SOURCE;
        slot.medium_at = f.medium_at;
    } else {
        slot.kind = FD_MEMORY;
        slot.data = f.data;
    }

    return process_fd_install(pid, fd, &slot);
}

static bool install_pipe(int pid, int fd, struct pipe *p, bool writing)
{
    struct fd slot;
    memset(&slot, 0, sizeof slot);
    slot.kind = FD_PIPE;
    slot.pipe = p;
    slot.writing = writing;
    return process_fd_install(pid, fd, &slot);
}

/* what a spawned program is born holding, when the caller has one to give. */
static const struct spawn_env *pending_env;

void user_spawn_env(const struct spawn_env *env)
{
    pending_env = env;
}

int user_spawn(const char *path, int argc, const char *const argv[],
               const char *cwd,
               int parent, int uid, bool announce,
               const struct spawn_io *io, const char **error)
{
    /*
     * a program off the ramdisk is already in memory and is used where
     * it lies; one off the disk has to be read in first, and `owned`
     * says which happened so it can be let go of afterwards
     */
    const void *image = NULL;
    uint64_t image_size = 0;
    bool owned = false;
    if (!vfs_slurp(path, &image, &image_size, &owned)) {
        *error = USER_RUN_NO_SUCH_FILE;
        return 0;
    }

    const char *bad = NULL;
    if (!elf_is_loadable(image, image_size, &bad)) {
        vfs_release(image, owned);
        *error = bad;
        return 0;
    }

    /*
     * its own memory. two programs can now link to the same addresses
     * and never meet, which is the whole point of this milestone
     */
    struct addrspace *space = addrspace_create(vmm_kernel_pml4());
    if (space == NULL) {
        vfs_release(image, owned);
        *error = "no memory for an address space";
        return 0;
    }

    /*
     * two ways in, and which one depends on a single question: will
     * this image still be here when the program runs?
     *
     * a program off the ramdisk is a stretch of a tar philemon handed
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
    bool lazy = false;

    if (!owned && elf_describe(image, image_size, segs, ELF_SEGMENTS_MAX,
                               &seg_count, &loaded.entry, &loaded.brk,
                               &why)) {
        lazy = true;
        for (size_t i = 0; i < seg_count; i++) {
            if (!addrspace_add_region(space, segs[i].vaddr, segs[i].end,
                                      segs[i].flags, VMA_FILE,
                                      (const uint8_t *)image,
                                      segs[i].offset, segs[i].file_end)) {
                lazy = false;   /* no room to describe it. copy it in */
                break;
            }
        }
        loaded.ok = lazy;
    }

    if (!lazy) {
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
        *error = (loaded.error != NULL) ? loaded.error : why;
        return 0;
    }

    /* a stack for ring 3: writable, never executable, mapped user. */
    uint64_t stack_flags = PTE_USER | PTE_WRITE | vmm_nx();
    uint64_t stack_base = USER_STACK_TOP - USER_STACK_PAGES * PAGE_SIZE;

    if (!addrspace_add_region(space, stack_base, USER_STACK_TOP,
                              stack_flags, VMA_ANON, NULL, 0, 0)) {
        addrspace_destroy(space);
        *error = "no room to describe a stack";
        return 0;
    }

    uint64_t eager_base = USER_STACK_TOP - USER_STACK_EAGER * PAGE_SIZE;
    uint64_t stack_phys = pmm_alloc_pages(USER_STACK_EAGER);
    if (stack_phys == 0) {
        addrspace_destroy(space);
        *error = "no memory for a user stack";
        return 0;
    }
    memset(pmm_phys_to_virt(stack_phys), 0, USER_STACK_EAGER * PAGE_SIZE);

    if (!vmm_map_range(space->pml4, eager_base, stack_phys,
                       USER_STACK_EAGER * PAGE_SIZE, stack_flags)) {
        pmm_free_pages(stack_phys, USER_STACK_EAGER);
        addrspace_destroy(space);
        *error = "could not map a user stack";
        return 0;
    }

    struct user_start *start = kmalloc(sizeof *start);
    if (start == NULL) {
        addrspace_destroy(space);   /* which owns the stack by now */
        *error = "no memory";
        return 0;
    }
    struct argblock args;
    if (!build_args(stack_phys, argc, argv, &args)) {
        kfree(start);
        addrspace_destroy(space);
        *error = "those arguments do not fit on a stack";
        return 0;
    }

    start->entry     = loaded.entry;
    start->stack_top = args.stack_top;
    start->argc      = args.argc;
    start->argv      = args.argv;

    /*
     * the process comes first, because it is what outlives the thread
     * and holds the exit code somebody will want to read
     */
    /*
     * a program cannot ask to be somebody else: it runs as whoever
     * started it, and only the shell decides what that is
     */
    int pid = process_create(path, parent, uid, announce, pit_uptime_ms());
    if (pid == 0) {
        /*
         * full. init collects orphans on its own schedule and a shell
         * collects its own jobs, so anything still sitting here is
         * something somebody may yet ask about, but a machine that
         * cannot start a program is worse than one that lost an exit
         * code, so this takes the lot and tries once more
         */
        reap_abandoned();
        pid = process_create(path, parent, uid, announce, pit_uptime_ms());
    }

    /* wherever whoever started it was standing. */
    if (pid != 0 && cwd != NULL) {
        process_set_cwd(pid, cwd);
    }

    /* the environment it was born holding. */
    if (pid != 0 && pending_env != NULL && pending_env->block != NULL) {
        process_set_env(pid, pending_env->block, pending_env->len);
    } else if (pid != 0 && parent != 0) {
        /*
         * spawned by a program rather than by the shell: it takes its
         * parent's, which is the same rule seen from the other side
         */
        static char inherited[PROC_ENV_MAX];
        size_t n = process_get_env(parent, inherited, sizeof inherited);
        process_set_env(pid, inherited, n);
    }
    /*
     * whatever was asked for goes in before the thread exists, so the
     * program has never seen anything else in those slots
     */
    if (pid != 0 && io != NULL) {
        const char *why = NULL;
        bool ok = true;
        if (io->in != NULL) {
            ok = install_pipe(pid, FD_STDIN, io->in, false);
        } else if (io->in_path != NULL) {
            ok = redirect(pid, FD_STDIN, io->in_path, false, false, &why);
        }
        if (ok && io->out != NULL) {
            ok = install_pipe(pid, FD_STDOUT, io->out, true);
        } else if (ok && io->out_path != NULL) {
            ok = redirect(pid, FD_STDOUT, io->out_path, true, io->append, &why);
        }
        if (!ok) {
            int ignored;
            process_exited(pid, PROCESS_KILLED, pit_uptime_ms());
            process_collect(pid, &ignored);
            kfree(start);
            addrspace_destroy(space);
            *error = (why != NULL) ? why : "could not redirect that";
            return 0;
        }
    }
    if (pid == 0) {
        kfree(start);
        addrspace_destroy(space);
        *error = "the process table is full";
        return 0;
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
                path, pid, (void *)loaded.entry);
    }
    return pid;
}

/* wait for a pid, however it ends. */
bool user_wait(int pid, int *code)
{
    struct thread *self = sched_current();
    int me = (self != NULL) ? self->pid : 0;

    for (;;) {
        const struct process *p = process_find(pid);
        if (p == NULL) {
            return false;       /* gone, or somebody else collected it */
        }
        if (p->exited) {
            break;
        }

        /* waiting is a blocking call like any other, and a signal is a third reason for one to end. */
        if (me > 0 && process_take_interrupt(me)) {
            return false;
        }
        sleep_ms(20);
    }
    return process_collect(pid, code);
}

/* let a spawned process actually begin. */
void user_start(int pid)
{
    const struct process *p = process_find(pid);
    if (p != NULL && p->thread_id != 0) {
        sched_wake_thread(p->thread_id);
    }
}

/*
 * everything one typed line started, held together by a group number so
 * the terminal can talk to all of it at once. the number is the pid of
 * the first process, which is arbitrary and traditional and means the
 * group needs nothing allocated to name it
 */

bool user_job_alive(const struct job *j)
{
    for (int i = 0; i < j->count; i++) {
        if (j->pids[i] == 0) {
            continue;
        }
        const struct process *p = process_find(j->pids[i]);
        if (p != NULL && !p->exited) {
            return true;
        }
    }
    return false;
}

void user_job_collect(struct job *j)
{
    for (int i = 0; i < j->count; i++) {
        if (j->pids[i] == 0) {
            continue;
        }
        int code = 0;
        if (process_collect(j->pids[i], &code)) {
            /* the last one's status is the job's. */
            if (i == j->count - 1) {
                j->status = code;
            }
            /* being killed is worth saying, since somebody asked for it. */
            if (code == PROCESS_KILLED && i == j->count - 1) {
                kprintf("[kernel] pid %d was killed\n", j->pids[i]);
            }
            j->pids[i] = 0;
        }
    }
}

bool user_job_wait(struct job *j)
{
    /*
     * waiting by id rather than by pointer is deliberate: the reaper
     * may free a thread the moment it dies, and an id cannot dangle.
     * this polls every 20ms, which no human will notice
     */
    for (;;) {
        if (!user_job_alive(j)) {
            j->stopped = false;
            break;
        }

        /* the other way waiting can end. */
        int which = 0;
        if (tty_take_stopped(&which) && which == j->pgid) {
            j->stopped = true;
            tty_set_foreground(TTY_SHELL);
            return false;
        }

        sleep_ms(20);
    }

    tty_set_foreground(TTY_SHELL);
    user_job_collect(j);
    return true;
}

void user_job_continue(struct job *j, bool foreground)
{
    int ids[MAX_PROCESSES];
    size_t n = process_group_threads(j->pgid, ids, MAX_PROCESSES);
    for (size_t i = 0; i < n; i++) {
        sched_set_stopped(ids[i], false);
    }
    j->stopped = false;

    /* the terminal goes with it, or does not. */
    if (foreground) {
        tty_set_foreground(j->pgid);
    }
}

bool user_pipeline(const struct stage *stages, int count, const char *cwd,
                   int uid, bool background, struct job *out,
                   const char **error)
{
    memset(out, 0, sizeof *out);

    if (count < 1 || count > PIPELINE_MAX) {
        *error = "that is more commands than the kernel can join up";
        return false;
    }

    /* one pipe between each neighbouring pair, so count-1 of them. */
    struct pipe *pipes[PIPELINE_MAX - 1];
    for (int i = 0; i < count - 1; i++) {
        pipes[i] = pipe_create();
        if (pipes[i] == NULL) {
            for (int j = 0; j < i; j++) {
                pipe_close_read(pipes[j]);
                pipe_close_write(pipes[j]);
            }
            *error = "no memory for a pipe";
            return false;
        }
    }

    int started = 0;
    out->count = count;

    for (int i = 0; i < count; i++) {
        struct spawn_io io;
        memset(&io, 0, sizeof io);
        io.in  = (i > 0)         ? pipes[i - 1] : NULL;
        io.out = (i < count - 1) ? pipes[i]     : NULL;
        io.in_path  = stages[i].in_path;
        io.out_path = stages[i].out_path;
        io.append   = stages[i].append;

        const char *why = NULL;
        int pid = user_spawn(stages[i].path, stages[i].argc,
                             (const char *const *)stages[i].argv,
                             cwd, 0, uid, false, &io, &why);
        out->pids[i] = pid;
        if (pid != 0) {
            /* the first one to start names the group and everybody else joins it. */
            if (out->pgid == 0) {
                out->pgid = pid;
            }
            process_set_pgid(pid, out->pgid);
            started++;
            continue;
        }

        /*
         * it never started, so it will never close the ends it was
         * going to be given, and a pipe nobody closes is a neighbour
         * blocked forever. close them here instead, which the ones
         * either side see as end of file and a broken pipe, and they
         * finish by themselves
         */
        if (i == 0 && count == 1) {
            *error = why;
            return false;
        }
        kprintf("cannot run %s: %s\n", stages[i].path, why);
        pipe_close_read(io.in);
        pipe_close_write(io.out);

        if (i == 0) {
            *error = why;
        }
    }

    if (started == 0) {
        return false;
    }

    /* the terminal is the job's now, unless it was sent to the background. */
    if (!background) {
        tty_set_foreground(out->pgid);
    }

    for (int i = 0; i < count; i++) {
        if (out->pids[i] != 0) {
            user_start(out->pids[i]);
        }
    }

    if (background) {
        return true;
    }

    user_job_wait(out);
    return true;
}

bool user_run(const char *path, int argc, const char *const argv[],
              const char *cwd,
              int uid, bool background, bool announce, struct job *out,
              const char **error)
{
    memset(out, 0, sizeof *out);

    int pid = user_spawn(path, argc, argv, cwd, 0, uid, announce,
                         NULL, error);
    if (pid == 0) {
        return false;
    }

    /* a command on its own is a job of one, and its group is itself. */
    out->pgid = pid;
    out->pids[0] = pid;
    out->count = 1;

    if (!background) {
        tty_set_foreground(out->pgid);
    }
    user_start(pid);

    if (background) {
        /* nobody is waiting, so nobody will collect it. */
        if (announce) {
            kprintf("[kernel] pid %d runs in the background\n", pid);
        }
        return true;
    }

    user_job_wait(out);
    return true;
}
