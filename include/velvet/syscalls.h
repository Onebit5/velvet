// SPDX-License-Identifier: GPL-2.0-only
/*
 * include/velvet/syscalls.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the system calls the portable kernel answers, as an architecture calls them.
 */

#ifndef VELVET_SYSCALLS_H
#define VELVET_SYSCALLS_H

#include <stdint.h>
#include <stdbool.h>

/*
 * each takes what the program passed, a pointer as the address ring 3
 * gave and never read where it lies, and answers with the result or
 * minus an errno. an architecture's table unpacks the registers into
 * these; the calls that need the registers themselves, fork, exec,
 * sigreturn and their kind, are the architecture's own.
 *
 * a blocking call interrupted by a signal answers -ERESTARTSYS when it
 * may be started again after a handler with SA_RESTART, or with no
 * handler at all, and -ERESTARTNOHAND when only the second; neither
 * ever reaches a program
 */

/* fs/read_write.c */
int64_t sys_read(int i_fd, uint64_t u_buf, uint64_t u_len);
int64_t sys_write(int i_fd, uint64_t u_buf, uint64_t u_len);
int64_t sys_pread64(int i_fd, uint64_t u_buf, uint64_t u_len,
                    int64_t i_offset);
int64_t sys_pwrite64(int i_fd, uint64_t u_buf, uint64_t u_len,
                     int64_t i_offset);
int64_t sys_readv(int i_fd, uint64_t u_iov, int i_count);
int64_t sys_writev(int i_fd, uint64_t u_iov, int i_count);
int64_t sys_lseek(int i_fd, int64_t i_offset, int i_whence);

/* fs/open.c */
int64_t sys_openat(int i_dirfd, uint64_t u_path, int i_flags,
                   uint32_t u_mode);
int64_t sys_close(int i_fd);
int64_t sys_faccessat(int i_dirfd, uint64_t u_path, int i_mode, int i_flags);
int64_t sys_chdir(uint64_t u_path);
int64_t sys_fchdir(int i_fd);
int64_t sys_getcwd(uint64_t u_buf, uint64_t u_size);
int64_t sys_fchmodat(int i_dirfd, uint64_t u_path, uint32_t u_mode,
                     int i_flags);
int64_t sys_fchmod(int i_fd, uint32_t u_mode);
int64_t sys_fchownat(int i_dirfd, uint64_t u_path, uint32_t u_uid,
                     uint32_t u_gid, int i_flags);
int64_t sys_fchown(int i_fd, uint32_t u_uid, uint32_t u_gid);
int64_t sys_truncate(uint64_t u_path, int64_t i_length);
int64_t sys_ftruncate(int i_fd, int64_t i_length);
int64_t sys_umask(uint32_t u_mask);
int64_t sys_sync(void);
int64_t sys_fsync(int i_fd);

/* fs/namei.c */
int64_t sys_mkdirat(int i_dirfd, uint64_t u_path, uint32_t u_mode);
int64_t sys_unlinkat(int i_dirfd, uint64_t u_path, int i_flags);
int64_t sys_renameat2(int i_olddir, uint64_t u_old, int i_newdir,
                      uint64_t u_new, uint32_t u_flags);
int64_t sys_symlinkat(uint64_t u_target, int i_dirfd, uint64_t u_path);
int64_t sys_readlinkat(int i_dirfd, uint64_t u_path, uint64_t u_buf,
                       uint64_t u_size);
int64_t sys_linkat(int i_olddir, uint64_t u_old, int i_newdir,
                   uint64_t u_new, int i_flags);

/* fs/stat.c */
int64_t sys_newfstatat(int i_dirfd, uint64_t u_path, uint64_t u_stat,
                       int i_flags);
int64_t sys_fstat(int i_fd, uint64_t u_stat);
int64_t sys_getdents64(int i_fd, uint64_t u_buf, uint64_t u_len);

/* fs/fcntl.c */
int64_t sys_dup(int i_fd);
int64_t sys_dup3(int i_from, int i_to, int i_flags);
int64_t sys_dup2(int i_from, int i_to);
int64_t sys_fcntl(int i_fd, int i_cmd, uint64_t u_arg);
int64_t sys_pipe2(uint64_t u_fds, int i_flags);
int64_t sys_ioctl(int i_fd, uint32_t u_cmd, uint64_t u_arg);

/* fs/select.c */
int64_t sys_ppoll(uint64_t u_fds, uint64_t u_count, uint64_t u_timeout,
                  uint64_t u_mask, uint64_t u_masksize);
int64_t sys_poll(uint64_t u_fds, uint64_t u_count, int i_timeout_ms);
int64_t sys_pselect6(int i_count, uint64_t u_read, uint64_t u_write,
                     uint64_t u_except, uint64_t u_timeout, uint64_t u_mask);
int64_t sys_select(int i_count, uint64_t u_read, uint64_t u_write,
                   uint64_t u_except, uint64_t u_timeout);

/* mm/mmap.c */
int64_t sys_mmap(uint64_t u_addr, uint64_t u_len, int i_prot, int i_flags,
                 int i_fd, int64_t i_offset);
int64_t sys_munmap(uint64_t u_addr, uint64_t u_len);
int64_t sys_mprotect(uint64_t u_addr, uint64_t u_len, int i_prot);
int64_t sys_brk(uint64_t u_want);
int64_t sys_madvise(uint64_t u_addr, uint64_t u_len, int i_advice);
int64_t sys_mremap(uint64_t u_old, uint64_t u_old_len, uint64_t u_new_len,
                   int i_flags, uint64_t u_new);

/* sched/exit.c */
int64_t sys_exit(int i_code) __attribute__((noreturn));
int64_t sys_exit_group(int i_code) __attribute__((noreturn));
int64_t sys_wait4(int i_pid, uint64_t u_status, int i_options,
                  uint64_t u_rusage);
int64_t sys_set_tid_address(uint64_t u_tid);

/* sched/sys.c */
int64_t sys_getpid(void);
int64_t sys_getppid(void);
int64_t sys_gettid(void);
int64_t sys_getuid(void);
int64_t sys_getgid(void);
int64_t sys_setuid(uint32_t u_uid);
int64_t sys_setgid(uint32_t u_gid);
int64_t sys_setreuid(uint32_t u_ruid, uint32_t u_euid);
int64_t sys_setregid(uint32_t u_rgid, uint32_t u_egid);
int64_t sys_setresuid(uint32_t u_ruid, uint32_t u_euid, uint32_t u_suid);
int64_t sys_setresgid(uint32_t u_rgid, uint32_t u_egid, uint32_t u_sgid);
int64_t sys_getresuid(uint64_t u_ruid, uint64_t u_euid, uint64_t u_suid);
int64_t sys_getresgid(uint64_t u_rgid, uint64_t u_egid, uint64_t u_sgid);
int64_t sys_getgroups(int i_size, uint64_t u_list);
int64_t sys_setgroups(uint64_t u_size, uint64_t u_list);
int64_t sys_setpgid(int i_pid, int i_pgid);
int64_t sys_getpgid(int i_pid);
int64_t sys_getpgrp(void);
int64_t sys_setsid(void);
int64_t sys_getsid(int i_pid);
int64_t sys_uname(uint64_t u_buf);
int64_t sys_sethostname(uint64_t u_name, uint64_t u_len);
int64_t sys_setdomainname(uint64_t u_name, uint64_t u_len);
int64_t sys_sysinfo(uint64_t u_info);
int64_t sys_prlimit64(int i_pid, int i_resource, uint64_t u_new,
                      uint64_t u_old);
int64_t sys_getrlimit(int i_resource, uint64_t u_limit);
int64_t sys_setrlimit(int i_resource, uint64_t u_limit);
int64_t sys_getrusage(int i_who, uint64_t u_usage);
int64_t sys_times(uint64_t u_buf);
int64_t sys_reboot(uint32_t u_magic1, uint32_t u_magic2, uint32_t u_cmd,
                   uint64_t u_arg);
int64_t sys_mount(uint64_t u_source, uint64_t u_target, uint64_t u_type,
                  uint64_t u_flags, uint64_t u_data);
int64_t sys_umount2(uint64_t u_target, int i_flags);
int64_t sys_statfs(uint64_t u_path, uint64_t u_buf);
int64_t sys_fstatfs(int i_fd, uint64_t u_buf);
int64_t sys_pivot_root(uint64_t u_new, uint64_t u_old);
int64_t sys_mknodat(int i_dirfd, uint64_t u_path, uint32_t u_mode,
                    uint32_t u_dev);
int64_t sys_chroot(uint64_t u_path);
int64_t sys_syslog(int i_type, uint64_t u_buf, int i_len);
int64_t sys_getrandom(uint64_t u_buf, uint64_t u_len, uint32_t u_flags);
int64_t sys_prctl(int i_option, uint64_t u_arg2, uint64_t u_arg3,
                  uint64_t u_arg4, uint64_t u_arg5);
int64_t sys_sched_yield(void);
int64_t sys_set_robust_list(uint64_t u_head, uint64_t u_len);

/* sched/sigcall.c */
int64_t sys_rt_sigaction(int i_sig, uint64_t u_act, uint64_t u_old,
                         uint64_t u_setsize);
int64_t sys_rt_sigprocmask(int i_how, uint64_t u_set, uint64_t u_old,
                           uint64_t u_setsize);
int64_t sys_rt_sigpending(uint64_t u_set, uint64_t u_setsize);
int64_t sys_rt_sigsuspend(uint64_t u_set, uint64_t u_setsize);
int64_t sys_rt_sigtimedwait(uint64_t u_set, uint64_t u_info,
                            uint64_t u_timeout, uint64_t u_setsize);
int64_t sys_sigaltstack(uint64_t u_new, uint64_t u_old, uint64_t u_sp);
int64_t sys_kill(int i_pid, int i_sig);
int64_t sys_tgkill(int i_tgid, int i_tid, int i_sig);
int64_t sys_tkill(int i_tid, int i_sig);
int64_t sys_pause(void);
int64_t sys_alarm(uint32_t u_seconds);
int64_t sys_setitimer(int i_which, uint64_t u_new, uint64_t u_old);
int64_t sys_getitimer(int i_which, uint64_t u_value);

/* sched/time.c */
int64_t sys_clock_gettime(int i_clock, uint64_t u_ts);
int64_t sys_clock_settime(int i_clock, uint64_t u_ts);
int64_t sys_clock_getres(int i_clock, uint64_t u_ts);
int64_t sys_clock_nanosleep(int i_clock, int i_flags, uint64_t u_req,
                            uint64_t u_rem);
int64_t sys_nanosleep(uint64_t u_req, uint64_t u_rem);
int64_t sys_gettimeofday(uint64_t u_tv, uint64_t u_tz);
int64_t sys_settimeofday(uint64_t u_tv, uint64_t u_tz);
int64_t sys_time(uint64_t u_out);

/* net/sockets.c */
int64_t sys_socket(int i_family, int i_type, int i_protocol);
int64_t sys_bind(int i_fd, uint64_t u_addr, uint32_t u_len);
int64_t sys_connect(int i_fd, uint64_t u_addr, uint32_t u_len);
int64_t sys_listen(int i_fd, int i_backlog);
int64_t sys_accept4(int i_fd, uint64_t u_addr, uint64_t u_len, int i_flags);
int64_t sys_sendto(int i_fd, uint64_t u_buf, uint64_t u_len, int i_flags,
                   uint64_t u_addr, uint32_t u_addrlen);
int64_t sys_recvfrom(int i_fd, uint64_t u_buf, uint64_t u_len, int i_flags,
                     uint64_t u_addr, uint64_t u_addrlen);
int64_t sys_sendmsg(int i_fd, uint64_t u_msg, int i_flags);
int64_t sys_recvmsg(int i_fd, uint64_t u_msg, int i_flags);
int64_t sys_shutdown(int i_fd, int i_how);
int64_t sys_getsockname(int i_fd, uint64_t u_addr, uint64_t u_len);
int64_t sys_getpeername(int i_fd, uint64_t u_addr, uint64_t u_len);
int64_t sys_setsockopt(int i_fd, int i_level, int i_name, uint64_t u_value,
                       uint32_t u_len);
int64_t sys_getsockopt(int i_fd, int i_level, int i_name, uint64_t u_value,
                       uint64_t u_len);

#endif
