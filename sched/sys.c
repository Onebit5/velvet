// SPDX-License-Identifier: GPL-2.0-only
/*
 * sched/sys.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * who a process is, what it may use, and what the machine is.
 */

#include <velvet/syscalls.h>
#include <velvet/errno.h>
#include <velvet/process.h>
#include <velvet/sched.h>
#include <velvet/thread.h>
#include <velvet/init.h>
#include <velvet/kprintf.h>
#include <velvet/kmalloc.h>
#include <velvet/pmm.h>
#include <velvet/pit.h>
#include <velvet/random.h>
#include <velvet/string.h>
#include <asm/cpu.h>
#include <asm/uaccess.h>
#include <uapi/velvet/prctl.h>
#include <uapi/velvet/random.h>
#include <uapi/velvet/reboot.h>
#include <uapi/velvet/resource.h>
#include <uapi/velvet/sysinfo.h>
#include <uapi/velvet/syslog.h>
#include <uapi/velvet/time.h>
#include <uapi/velvet/utsname.h>
#include <generated/version.h>

/* ids and groups. one uid stands for the real, effective and saved ones */

int64_t sys_getpid(void)
{
    return current_pid();
}

/* one thread to a process, so its thread is the process */
int64_t sys_gettid(void)
{
    return current_pid();
}

int64_t sys_getppid(void)
{
    const struct process *p = process_find(current_pid());
    return (p != NULL) ? p->parent : 0;
}

int64_t sys_getuid(void)
{
    return process_uid(current_pid());
}

int64_t sys_getgid(void)
{
    const struct process *p = process_find(current_pid());
    return (p != NULL) ? p->i_gid : 0;
}

/*
 * who the caller becomes: anybody, for root, and otherwise only who it
 * already is. -1 leaves it as it is
 */
static int64_t become(int i_uid, int i_gid)
{
    uint64_t u_flags;
    struct process *p = process_lock_slot(current_pid(), &u_flags);
    int64_t i_result = -EPERM;
    if (p != NULL && (p->uid == 0 || ((i_uid == -1 || i_uid == p->uid)
                                      && (i_gid == -1 || i_gid == p->i_gid)))) {
        if (i_uid != -1) {
            p->uid = i_uid;
        }
        if (i_gid != -1) {
            p->i_gid = i_gid;
        }
        i_result = 0;
    }
    process_unlock_slot(u_flags);
    return i_result;
}

/* of a real and an effective id, the one a single id has to be */
static int one_of(uint32_t u_real, uint32_t u_effective)
{
    return (int)((u_effective != (uint32_t)-1) ? u_effective : u_real);
}

int64_t sys_setuid(uint32_t u_uid)
{
    return ((int)u_uid < 0) ? -EINVAL : become((int)u_uid, -1);
}

int64_t sys_setgid(uint32_t u_gid)
{
    return ((int)u_gid < 0) ? -EINVAL : become(-1, (int)u_gid);
}

int64_t sys_setreuid(uint32_t u_ruid, uint32_t u_euid)
{
    return become(one_of(u_ruid, u_euid), -1);
}

int64_t sys_setregid(uint32_t u_rgid, uint32_t u_egid)
{
    return become(-1, one_of(u_rgid, u_egid));
}

int64_t sys_setresuid(uint32_t u_ruid, uint32_t u_euid, uint32_t u_suid)
{
    (void)u_suid;
    return become(one_of(u_ruid, u_euid), -1);
}

int64_t sys_setresgid(uint32_t u_rgid, uint32_t u_egid, uint32_t u_sgid)
{
    (void)u_sgid;
    return become(-1, one_of(u_rgid, u_egid));
}

static int64_t put_three(uint64_t u_a, uint64_t u_b, uint64_t u_c,
                         int i_value)
{
    if (put_user_i32(u_a, i_value) != 0 || put_user_i32(u_b, i_value) != 0
        || put_user_i32(u_c, i_value) != 0) {
        return -EFAULT;
    }
    return 0;
}

int64_t sys_getresuid(uint64_t u_ruid, uint64_t u_euid, uint64_t u_suid)
{
    return put_three(u_ruid, u_euid, u_suid, (int)sys_getuid());
}

int64_t sys_getresgid(uint64_t u_rgid, uint64_t u_egid, uint64_t u_sgid)
{
    return put_three(u_rgid, u_egid, u_sgid, (int)sys_getgid());
}

int64_t sys_getgroups(int i_size, uint64_t u_list)
{
    int32_t arr_groups[NGROUPS_MAX];
    uint64_t u_flags;
    struct process *p = process_lock_slot(current_pid(), &u_flags);
    int i_count = (p != NULL) ? p->i_groups : 0;
    for (int i_at = 0; i_at < i_count; i_at++) {
        arr_groups[i_at] = p->arr_groups[i_at];
    }
    process_unlock_slot(u_flags);

    if (i_size == 0) {
        return i_count;
    }
    if (i_size < i_count) {
        return -EINVAL;
    }
    return (copy_to_user(u_list, arr_groups, (size_t)i_count * 4) == 0)
           ? i_count : -EFAULT;
}

int64_t sys_setgroups(uint64_t u_size, uint64_t u_list)
{
    int32_t arr_groups[NGROUPS_MAX];
    if (u_size > NGROUPS_MAX) {
        return -EINVAL;
    }
    if (copy_from_user(arr_groups, u_list, (size_t)u_size * 4) != 0) {
        return -EFAULT;
    }
    uint64_t u_flags;
    struct process *p = process_lock_slot(current_pid(), &u_flags);
    int64_t i_result = -EPERM;
    if (p != NULL && p->uid == 0) {
        for (uint64_t u_at = 0; u_at < u_size; u_at++) {
            p->arr_groups[u_at] = arr_groups[u_at];
        }
        p->i_groups = (int)u_size;
        i_result = 0;
    }
    process_unlock_slot(u_flags);
    return i_result;
}

/* groups and sessions */

/*
 * into a group: the caller, or a child of its own, into one named after
 * itself or one already in the caller's session. a session's leader
 * stays where it is, since its group is what names the session
 */
int64_t sys_setpgid(int i_pid, int i_pgid)
{
    int i_me = current_pid();
    i_pid = (i_pid == 0) ? i_me : i_pid;
    i_pgid = (i_pgid == 0) ? i_pid : i_pgid;
    if (i_pgid < 0) {
        return -EINVAL;
    }

    uint64_t u_flags;
    struct process *p_me = process_lock_slot(i_me, &u_flags);
    int i_sid = (p_me != NULL) ? p_me->i_sid : 0;
    process_unlock_slot(u_flags);

    int i_member = 0;
    bool b_group = i_pgid == i_pid
                   || (process_group_pids(i_pgid, &i_member, 1) == 1
                       && process_find(i_member) != NULL
                       && process_find(i_member)->i_sid == i_sid);

    struct process *p = process_lock_slot(i_pid, &u_flags);
    int64_t i_result;
    if (p == NULL || p->exited || (i_pid != i_me && p->parent != i_me)) {
        i_result = -ESRCH;
    } else if (p->i_sid != i_sid || p->i_sid == p->pid || !b_group) {
        i_result = -EPERM;
    } else {
        p->pgid = i_pgid;
        i_result = 0;
    }
    process_unlock_slot(u_flags);
    return i_result;
}

int64_t sys_getpgid(int i_pid)
{
    const struct process *p = process_find((i_pid == 0) ? current_pid()
                                                        : i_pid);
    return (p != NULL && !p->exited) ? p->pgid : -ESRCH;
}

int64_t sys_getpgrp(void)
{
    return sys_getpgid(0);
}

/*
 * a new session and a new group, both named after the caller, with no
 * terminal yet. a group's leader may not: its group would be left in a
 * session it is no longer in
 */
int64_t sys_setsid(void)
{
    int i_me = current_pid();
    int i_member = 0;
    if (process_group_pids(i_me, &i_member, 1) != 0) {
        return -EPERM;
    }
    uint64_t u_flags;
    struct process *p = process_lock_slot(i_me, &u_flags);
    if (p != NULL) {
        p->i_sid = i_me;
        p->pgid = i_me;
    }
    process_unlock_slot(u_flags);
    return i_me;
}

int64_t sys_getsid(int i_pid)
{
    const struct process *p = process_find((i_pid == 0) ? current_pid()
                                                        : i_pid);
    return (p != NULL && !p->exited) ? p->i_sid : -ESRCH;
}

/* the machine's names */

#define NAME_MAX_LEN (UTS_LEN - 1)

/* what nobody has named yet, as every system has said it */
static char s_arr_hostname[UTS_LEN] = "(none)";
static char s_arr_domainname[UTS_LEN] = "(none)";

int64_t sys_uname(uint64_t u_buf)
{
    struct utsname name;
    memset(&name, 0, sizeof name);
    strncpy(name.sysname, "velvet", NAME_MAX_LEN);
    strncpy(name.nodename, s_arr_hostname, NAME_MAX_LEN);
    strncpy(name.release, VELVET_VERSION, NAME_MAX_LEN);
    strncpy(name.version, "#1 SMP", NAME_MAX_LEN);
    strncpy(name.machine, ARCH_MACHINE, NAME_MAX_LEN);
    strncpy(name.domainname, s_arr_domainname, NAME_MAX_LEN);
    return (copy_to_user(u_buf, &name, sizeof name) == 0) ? 0 : -EFAULT;
}

static int64_t set_name(char *p_name, uint64_t u_name, uint64_t u_len)
{
    char arr_new[UTS_LEN];
    if (process_uid(current_pid()) != 0) {
        return -EPERM;
    }
    if (u_len > NAME_MAX_LEN) {
        return -EINVAL;
    }
    if (copy_from_user(arr_new, u_name, (size_t)u_len) != 0) {
        return -EFAULT;
    }
    arr_new[u_len] = '\0';
    memcpy(p_name, arr_new, sizeof arr_new);
    return 0;
}

int64_t sys_sethostname(uint64_t u_name, uint64_t u_len)
{
    return set_name(s_arr_hostname, u_name, u_len);
}

int64_t sys_setdomainname(uint64_t u_name, uint64_t u_len)
{
    return set_name(s_arr_domainname, u_name, u_len);
}

int64_t sys_sysinfo(uint64_t u_info)
{
    struct sysinfo info;
    memset(&info, 0, sizeof info);
    info.uptime = (int64_t)(pit_uptime_ms() / 1000);
    info.totalram = pmm_total_bytes();
    info.freeram = pmm_free_bytes();
    info.procs = (uint16_t)process_count();
    info.mem_unit = 1;
    return (copy_to_user(u_info, &info, sizeof info) == 0) ? 0 : -EFAULT;
}

/* limits */

/*
 * a limit read and set at once. the caller's own, or another's when the
 * caller is root. the soft limit stays under the hard one, and only root
 * raises a hard one; a descriptor table has room for so many and no more
 */
int64_t sys_prlimit64(int i_pid, int i_resource, uint64_t u_new,
                      uint64_t u_old)
{
    struct rlimit limit;
    if (i_resource < 0 || i_resource >= RLIM_NLIMITS) {
        return -EINVAL;
    }
    if (u_new != 0) {
        if (copy_from_user(&limit, u_new, sizeof limit) != 0) {
            return -EFAULT;
        }
        if (limit.rlim_cur > limit.rlim_max) {
            return -EINVAL;
        }
    }
    int i_me = current_pid();
    bool b_root = process_uid(i_me) == 0;
    i_pid = (i_pid == 0) ? i_me : i_pid;
    if (i_pid != i_me && !b_root) {
        return -EPERM;
    }

    uint64_t u_flags;
    struct process *p = process_lock_slot(i_pid, &u_flags);
    int64_t i_result = 0;
    struct rlimit was = { 0, 0 };
    if (p == NULL || p->exited) {
        i_result = -ESRCH;
    } else {
        was = p->arr_limits[i_resource];
        if (u_new != 0) {
            if ((limit.rlim_max > was.rlim_max && !b_root)
                || (i_resource == RLIMIT_NOFILE
                    && limit.rlim_max > MAX_FDS)) {
                i_result = -EPERM;
            } else {
                p->arr_limits[i_resource] = limit;
            }
        }
    }
    process_unlock_slot(u_flags);
    if (i_result == 0 && u_old != 0
        && copy_to_user(u_old, &was, sizeof was) != 0) {
        i_result = -EFAULT;
    }
    return i_result;
}

int64_t sys_getrlimit(int i_resource, uint64_t u_limit)
{
    return sys_prlimit64(0, i_resource, 0, u_limit);
}

int64_t sys_setrlimit(int i_resource, uint64_t u_limit)
{
    return sys_prlimit64(0, i_resource, u_limit, 0);
}

/* time used */

#define MS_PER_TICK (1000 / CLOCK_TICK_HZ)

/* the ticks the caller's own thread has run, and its collected children's */
static void ticks_of(uint64_t *p_own, uint64_t *p_children)
{
    struct thread *p_me = sched_current();
    const struct process *p = process_find(current_pid());
    *p_own = (p_me != NULL) ? p_me->cpu_ticks : 0;
    *p_children = (p != NULL) ? p->u_child_ticks : 0;
}

static struct rusage_time as_timeval(uint64_t u_ticks)
{
    uint64_t u_ms = u_ticks * MS_PER_TICK;
    struct rusage_time tv = {
        .tv_sec = (int64_t)(u_ms / 1000),
        .tv_usec = (int64_t)(u_ms % 1000) * 1000,
    };
    return tv;
}

int64_t sys_getrusage(int i_who, uint64_t u_usage)
{
    struct rusage usage;
    uint64_t u_own;
    uint64_t u_children;
    if (i_who != RUSAGE_SELF && i_who != RUSAGE_CHILDREN
        && i_who != RUSAGE_THREAD) {
        return -EINVAL;
    }
    ticks_of(&u_own, &u_children);
    memset(&usage, 0, sizeof usage);
    usage.ru_utime = as_timeval((i_who == RUSAGE_CHILDREN) ? u_children
                                                            : u_own);
    return (copy_to_user(u_usage, &usage, sizeof usage) == 0) ? 0 : -EFAULT;
}

/* the clock ticks since boot, at the rate AT_CLKTCK says */
int64_t sys_times(uint64_t u_buf)
{
    uint64_t u_own;
    uint64_t u_children;
    ticks_of(&u_own, &u_children);
    struct tms counts = {
        .tms_utime = (int64_t)(u_own * MS_PER_TICK / 10),
        .tms_cutime = (int64_t)(u_children * MS_PER_TICK / 10),
    };
    if (u_buf != 0 && copy_to_user(u_buf, &counts, sizeof counts) != 0) {
        return -EFAULT;
    }
    return (int64_t)(pit_uptime_ms() / 10);
}

/* the machine itself */

/*
 * the magic numbers are there so that a stray call cannot take the
 * machine down, and only root may make a real one
 */
int64_t sys_reboot(uint32_t u_magic1, uint32_t u_magic2, uint32_t u_cmd,
                   uint64_t u_arg)
{
    (void)u_arg;
    if (u_magic1 != REBOOT_MAGIC1
        || (u_magic2 != REBOOT_MAGIC2 && u_magic2 != REBOOT_MAGIC2A
            && u_magic2 != REBOOT_MAGIC2B && u_magic2 != REBOOT_MAGIC2C)) {
        return -EINVAL;
    }
    if (process_uid(current_pid()) != 0) {
        return -EPERM;
    }
    switch (u_cmd) {
    case REBOOT_CMD_RESTART:
        init_stop_machine(INIT_REBOOT);
    case REBOOT_CMD_HALT:
    case REBOOT_CMD_POWER_OFF:
        init_stop_machine(INIT_POWEROFF);
    case REBOOT_CMD_CAD_ON:
    case REBOOT_CMD_CAD_OFF:
        /* ctrl+alt+del is not wired to anything, either way */
        return 0;
    default:
        return -EINVAL;
    }
}

/*
 * the kernel's log, read whole or asked its size. it is copied into a
 * buffer of the kernel's under the print lock, and only then to the
 * caller: a write to a user page can fault, and a fault is not something
 * to take with every core's printing stopped
 */
int64_t sys_syslog(int i_type, uint64_t u_buf, int i_len)
{
    switch (i_type) {
    case SYSLOG_ACTION_CLOSE:
    case SYSLOG_ACTION_OPEN:
        return 0;
    case SYSLOG_ACTION_SIZE_BUFFER:
        return KLOG_SIZE;
    case SYSLOG_ACTION_READ_ALL:
        break;
    default:
        return -EINVAL;
    }
    if (i_len < 0) {
        return -EINVAL;
    }
    char *str_log = kmalloc(KLOG_SIZE);
    if (str_log == NULL) {
        return -ENOMEM;
    }
    size_t sz_want = ((size_t)i_len < KLOG_SIZE) ? (size_t)i_len : KLOG_SIZE;
    size_t sz_got = klog_copy(str_log, sz_want);
    int64_t i_copied = copy_to_user(u_buf, str_log, sz_got);
    kfree(str_log);
    return (i_copied == 0) ? (int64_t)sz_got : -EFAULT;
}

/*
 * bytes from the generator, which is seeded before anything runs and
 * never runs dry, so no call has to wait and none is ever told to
 */
int64_t sys_getrandom(uint64_t u_buf, uint64_t u_len, uint32_t u_flags)
{
    uint8_t arr_chunk[256];
    if (u_flags & ~(uint32_t)(GRND_NONBLOCK | GRND_RANDOM | GRND_INSECURE)) {
        return -EINVAL;
    }
    if (u_len > 33554431) {
        u_len = 33554431;
    }
    for (uint64_t u_done = 0; u_done < u_len; ) {
        size_t sz_take = (u_len - u_done < sizeof arr_chunk)
                         ? (size_t)(u_len - u_done) : sizeof arr_chunk;
        random_bytes(arr_chunk, sz_take);
        if (copy_to_user(u_buf + u_done, arr_chunk, sz_take) != 0) {
            return (u_done > 0) ? (int64_t)u_done : -EFAULT;
        }
        u_done += sz_take;
    }
    return (int64_t)u_len;
}

int64_t sys_prctl(int i_option, uint64_t u_arg2, uint64_t u_arg3,
                  uint64_t u_arg4, uint64_t u_arg5)
{
    char arr_name[TASK_COMM_LEN];
    (void)u_arg3;
    (void)u_arg4;
    (void)u_arg5;

    switch (i_option) {
    case PR_SET_NAME: {
        int64_t i_len = strncpy_from_user(arr_name, u_arg2, sizeof arr_name);
        if (i_len == -EFAULT) {
            return -EFAULT;
        }
        arr_name[TASK_COMM_LEN - 1] = '\0';
        process_set_name(current_pid(), arr_name);
        struct thread *p_me = sched_current();
        if (p_me != NULL) {
            thread_set_name(p_me, arr_name);
        }
        return 0;
    }
    case PR_GET_NAME:
        memset(arr_name, 0, sizeof arr_name);
        strncpy(arr_name, process_name(current_pid()), TASK_COMM_LEN - 1);
        return (copy_to_user(u_arg2, arr_name, sizeof arr_name) == 0)
               ? 0 : -EFAULT;
    default:
        return -EINVAL;
    }
}

int64_t sys_sched_yield(void)
{
    sched_yield();
    return 0;
}

/*
 * where a thread keeps the robust mutexes it holds, for the kernel to
 * release if it dies holding one. with one thread to a process, a death
 * takes every other user of such a mutex in the process with it, so
 * there is nobody left to release one for: the list is taken as given
 */
int64_t sys_set_robust_list(uint64_t u_head, uint64_t u_len)
{
    (void)u_head;
    return (u_len == 24) ? 0 : -EINVAL;
}
