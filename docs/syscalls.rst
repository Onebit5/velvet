system calls
============

the abi between ring 3 and the kernel: what a program can ask for, what
it passes, and what it gets back. it is the one every x86_64 program
and every libc for the machine already speaks, so a program is built
with an ordinary toolchain and an ordinary libc, and runs. the selftests
are built that way, against musl, which has never heard of this kernel.

the convention
--------------

the number goes in ``rax`` and the arguments in ``rdi``, ``rsi``, ``rdx``,
``r10``, ``r8`` and ``r9``, and ``syscall`` goes in. the answer comes back
in ``rax``: what was asked for, or minus an errno, a number from -4095 to
-1 that no successful call returns. ``syscall`` itself destroys ``rcx``
and ``r11``; every other register comes back as it went in.

a number with nothing behind it is ``-ENOSYS``, which every libc expects
from a kernel older than the call, and falls back from where it can.

the numbers are in ``include/uapi/velvet/unistd.h``, the errnos in
``errno.h`` beside it, and what the calls take in the rest of that
directory: ``stat.h``, ``fcntl.h``, ``signal.h``, ``termios.h``,
``socket.h`` and so on, each laid out byte for byte as programs expect.
that directory is the part of the kernel a program compiles against, and
it includes nothing else of the kernel's.

the table
---------

``where`` is the file the call is answered in. the calls in the arch file
are the old ones a newer call does the work of, with the newer one in the
notes, and the few that take the saved registers apart: fork, clone,
execve, rt_sigreturn and arch_prctl.

======  ===============  =====================  =========================================
number  name             where                  notes
======  ===============  =====================  =========================================
0       read             fs/read_write.c
1       write            fs/read_write.c
2       open             arch/x86_64/syscall.c  openat from the working directory
3       close            fs/open.c
4       stat             arch/x86_64/syscall.c  newfstatat, following a link
5       fstat            fs/stat.c
6       lstat            arch/x86_64/syscall.c  newfstatat, not following one
7       poll             fs/select.c            every 10 ms
8       lseek            fs/read_write.c
9       mmap             mm/mmap.c              anonymous, or a private copy of a file
10      mprotect         mm/mmap.c
11      munmap           mm/mmap.c
12      brk              mm/mmap.c
13      rt_sigaction     sched/sigcall.c
14      rt_sigprocmask   sched/sigcall.c
15      rt_sigreturn     arch/x86_64/syscall.c  from the frame a handler was entered with
16      ioctl            fs/fcntl.c             terminals, sockets, ext4 resize
17      pread64          fs/read_write.c
18      pwrite64         fs/read_write.c
19      readv            fs/read_write.c
20      writev           fs/read_write.c
21      access           arch/x86_64/syscall.c  faccessat from the working directory
22      pipe             arch/x86_64/syscall.c  pipe2 with no flags
23      select           fs/select.c            every 10 ms
24      sched_yield      sched/sys.c
25      mremap           mm/mmap.c              smaller in place; larger is -ENOMEM
28      madvise          mm/mmap.c              MADV_DONTNEED gives pages back
32      dup              fs/fcntl.c
33      dup2             fs/fcntl.c
34      pause            sched/sigcall.c
35      nanosleep        sched/time.c
36      getitimer        sched/sigcall.c        ITIMER_REAL
37      alarm            sched/sigcall.c
38      setitimer        sched/sigcall.c        ITIMER_REAL
39      getpid           sched/sys.c
41      socket           net/sockets.c          AF_INET, stream and datagram
42      connect          net/sockets.c
43      accept           arch/x86_64/syscall.c  accept4 with no flags
44      sendto           net/sockets.c
45      recvfrom         net/sockets.c
46      sendmsg          net/sockets.c
47      recvmsg          net/sockets.c
48      shutdown         net/sockets.c
49      bind             net/sockets.c
50      listen           net/sockets.c
51      getsockname      net/sockets.c
52      getpeername      net/sockets.c
54      setsockopt       net/sockets.c
55      getsockopt       net/sockets.c
56      clone            arch/x86_64/syscall.c  a new process only, see below
57      fork             arch/x86_64/syscall.c  clone with SIGCHLD
58      vfork            arch/x86_64/syscall.c  the parent waits for exec or exit
59      execve           arch/x86_64/syscall.c  see the stack below
60      exit             sched/exit.c
61      wait4            sched/exit.c           with the child's rusage
62      kill             sched/sigcall.c
63      uname            sched/sys.c            velvet, its version, x86_64
72      fcntl            fs/fcntl.c
74      fsync            fs/open.c
75      fdatasync        arch/x86_64/syscall.c  fsync
76      truncate         fs/open.c
77      ftruncate        fs/open.c
79      getcwd           fs/open.c
80      chdir            fs/open.c
81      fchdir           fs/open.c
82      rename           arch/x86_64/syscall.c  renameat2
83      mkdir            arch/x86_64/syscall.c  mkdirat
84      rmdir            arch/x86_64/syscall.c  unlinkat with AT_REMOVEDIR
85      creat            arch/x86_64/syscall.c  open with O_CREAT, O_WRONLY, O_TRUNC
86      link             arch/x86_64/syscall.c  linkat: -EPERM, files have one name
87      unlink           arch/x86_64/syscall.c  unlinkat
88      symlink          arch/x86_64/syscall.c  symlinkat
89      readlink         arch/x86_64/syscall.c  readlinkat
90      chmod            arch/x86_64/syscall.c  fchmodat
91      fchmod           fs/open.c
92      chown            arch/x86_64/syscall.c  fchownat
93      fchown           fs/open.c
94      lchown           arch/x86_64/syscall.c  fchownat, not following
95      umask            fs/open.c
96      gettimeofday     sched/time.c
97      getrlimit        sched/sys.c
98      getrusage        sched/sys.c
99      sysinfo          sched/sys.c
100     times            sched/sys.c
102     getuid           sched/sys.c
103     syslog           sched/sys.c            actions 0, 1, 3 and 10
104     getgid           sched/sys.c
105     setuid           sched/sys.c
106     setgid           sched/sys.c
107     geteuid          arch/x86_64/syscall.c  the uid: there is one per process
108     getegid          arch/x86_64/syscall.c  the gid, likewise
109     setpgid          sched/sys.c
110     getppid          sched/sys.c
111     getpgrp          sched/sys.c
112     setsid           sched/sys.c
113     setreuid         sched/sys.c
114     setregid         sched/sys.c
115     getgroups        sched/sys.c
116     setgroups        sched/sys.c
117     setresuid        sched/sys.c
118     getresuid        sched/sys.c
119     setresgid        sched/sys.c
120     getresgid        sched/sys.c
121     getpgid          sched/sys.c
124     getsid           sched/sys.c
127     rt_sigpending    sched/sigcall.c
128     rt_sigtimedwait  sched/sigcall.c
130     rt_sigsuspend    sched/sigcall.c
131     sigaltstack      sched/sigcall.c
157     prctl            sched/sys.c            PR_SET_NAME, PR_GET_NAME
158     arch_prctl       arch/x86_64/syscall.c  ARCH_SET_FS and ARCH_GET_FS
160     setrlimit        sched/sys.c
162     sync             fs/open.c
164     settimeofday     sched/time.c
169     reboot           sched/sys.c            root; restart or power off
170     sethostname      sched/sys.c            root; uname tells it
171     setdomainname    sched/sys.c            root, likewise
186     gettid           sched/sys.c
200     tkill            sched/sigcall.c
201     time             sched/time.c
217     getdents64       fs/stat.c
218     set_tid_address  sched/exit.c
227     clock_settime    sched/time.c
228     clock_gettime    sched/time.c
229     clock_getres     sched/time.c
230     clock_nanosleep  sched/time.c           not on a cpu-time clock
231     exit_group       sched/exit.c
234     tgkill           sched/sigcall.c
257     openat           fs/open.c
258     mkdirat          fs/namei.c
260     fchownat         fs/open.c
262     newfstatat       fs/stat.c
263     unlinkat         fs/namei.c
264     renameat         arch/x86_64/syscall.c  renameat2 with no flags
265     linkat           fs/namei.c             -EPERM, files have one name
266     symlinkat        fs/namei.c
267     readlinkat       fs/namei.c
268     fchmodat         fs/open.c
269     faccessat        fs/open.c
270     pselect6         fs/select.c            under its own mask
271     ppoll            fs/select.c            under its own mask
273     set_robust_list  sched/sys.c            accepted, for threads to come
288     accept4          net/sockets.c
292     dup3             fs/fcntl.c
293     pipe2            fs/fcntl.c
302     prlimit64        sched/sys.c
306     syncfs           arch/x86_64/syscall.c  sync
316     renameat2        fs/namei.c
318     getrandom        sched/sys.c            chacha20, never blocks
439     faccessat2       arch/x86_64/syscall.c  faccessat, with flags
======  ===============  =====================  =========================================

a call a signal cuts short
--------------------------

a call that waits, a read with nothing to read, a wait4 with no child
done, a sleep, answers inside the kernel with one of three numbers no
program sees, and the way back to ring 3 decides what it becomes:

``ERESTARTSYS``
   made again when no handler ran, or after a handler installed with
   ``SA_RESTART``; ``-EINTR`` after any other handler.

``ERESTARTNOHAND``
   made again only when no handler ran. sigsuspend, pause, poll and
   select are this kind, since they wait *for* the handler.

``ERESTARTNOINTR``
   made again whatever happened.

making a call again is ``rip`` put back on the ``syscall`` instruction and
the number back in ``rax``, so the program runs it again having never
noticed. a sleep is told how much was left through its second argument.

signals
-------

a handler is entered on the frame every x86_64 program expects: the
return address, a ``ucontext`` holding the interrupted registers, the
mask they ran under and a pointer to the saved fpu state, then the
``siginfo``, all on the program's stack below the 128 byte red zone, or on
the alternate stack when ``SA_ONSTACK`` asks and ``sigaltstack`` gave one.
the return address is the ``sa_restorer`` the program installed, since
the kernel puts no code in a program's memory. a handler with no
restorer, or a frame with nowhere writable to go, cannot be entered, and
the program is ended by ``SIGSEGV`` the way a fault ends it.
``rt_sigreturn`` puts everything back from the frame, including what the
handler changed in it.

a fault in ring 3 is a signal with ``si_code`` and ``si_addr`` saying what
happened: a page that is not mapped (``SEGV_MAPERR``) or not the
program's to touch that way (``SEGV_ACCERR``), an instruction the cpu
will not run (``SIGILL``), a division by zero (``SIGFPE``). a program
with no handler for it ends by it, and its parent sees that in wait4.

pid 1 is not ended by a signal it has not asked for: one with its
default action is dropped on the way in, so init cannot be taken down by
a stray ``kill``.

a new program
-------------

execve replaces the caller's memory with the program at the path and
enters it with the stack every psABI program starts from: ``argc``, then
``argv`` and a NULL, ``envp`` and a NULL, then the auxiliary vector,
pairs ending in ``AT_NULL``: ``AT_PHDR``, ``AT_PHENT``, ``AT_PHNUM``,
``AT_PAGESZ``, ``AT_BASE``, ``AT_FLAGS``, ``AT_ENTRY``, the four ids,
``AT_SECURE``, ``AT_CLKTCK``, ``AT_HWCAP``, ``AT_RANDOM`` (16 bytes from
the kernel's generator) and ``AT_EXECFN``. every other register is zero.
the arguments and the environment together may be 128 KiB.

the environment is the program's and nowhere else: the kernel copies it
across an exec and keeps nothing, and ``/proc/environ`` is the reader's
own, read out of its own stack.

the kernel starts ``/init`` with ``argv`` of ``/init`` and an environment
of ``HOME=/`` and a ``TERM`` naming the terminal type its console speaks,
on descriptors 0, 1 and 2 open on the console.

pointers
--------

a pointer a program hands over is never read or written where it lies.
it goes through ``copy_from_user``, ``copy_to_user`` or
``strncpy_from_user`` in ``<asm/uaccess.h>``, which check that the range is
below the end of ring 3 and copy. a page not touched yet, or one a forked
child still shares, is made or copied by the fault the copy takes, the way
a store from the program would. one that is not the program's at all, or
not its to write, is a fault in an instruction listed in ``__ex_table``,
and the fault handler sends it to its fixup: the call fails with
``-EFAULT``, and the machine carries on.

what is not there yet
---------------------

these answer, but less than they will:

- clone makes a new process, never a thread: ``CLONE_VM``, ``CLONE_FILES``
  and the rest of the sharing flags are ``-EINVAL``, and so is a signal
  other than ``SIGCHLD``. ``CLONE_VFORK``, ``CLONE_SETTLS``,
  ``CLONE_PARENT_SETTID``, ``CLONE_CHILD_SETTID`` and
  ``CLONE_CHILD_CLEARTID`` work. there is no futex.
- a file is mapped as a private copy. ``MAP_SHARED`` over a file is
  refused with ``-ENODEV`` when it could be written, since nothing would
  carry the writes back.
- mremap only makes a mapping smaller.
- poll and select look again every 10 ms rather than being woken.
- the interval timers count real time only; ``ITIMER_VIRTUAL`` and
  ``ITIMER_PROF`` are ``-EINVAL``.
- sockets are ``AF_INET`` only, a stream or a datagram, and the network
  is configured by nothing yet, so until it is, connecting anywhere is
  ``-ENETUNREACH``.
- a file has one name: link and linkat are ``-EPERM``.

and these are ``-ENOSYS`` until they exist: mount and the rest of the
mount table, futex, statx, utimensat, the ``*xattr`` calls, inotify,
epoll, eventfd and timerfd.

the kernel side
---------------

the arch file's table maps each number to a small function that takes
the saved frame apart into typed arguments and calls the ``sys_`` function
in the subsystem the call belongs to, declared in
``include/velvet/syscalls.h``. nothing below the table knows registers
exist. the entry stub, the saved frame and the rest of what follows
are x86's, in ``<asm/syscall.h>``.
