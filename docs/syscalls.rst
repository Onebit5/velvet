the system calls

the abi between ring 3 and the kernel: what a program can ask for, what
it passes, and what it gets back. the numbers are the kernel's and are
deliberately few, since there is no libc out there to satisfy, only the
handful of things a program here could want.

the table
---------

======  ============  ================================================  ======================
number  name          arguments                                         returns
======  ============  ================================================  ======================
0       EXIT          (code)                                            never returns
1       WRITE         (fd, buf, len)                                    bytes written
2       READ          (fd, buf, len)                                    bytes read
3       UPTIME        ()                                                ms since boot
4       YIELD         ()                                                
5       SLEEP         (ms)                                              
6       OPEN          (path, len)                                       fd
7       CLOSE         (fd)                                              
8       GETPID        ()                                                pid
9       SPAWN         (path, len)                                       pid of the new one
10      WAIT          (pid, int \*code)                                 pid, blocks
11      READDIR       (n,buf,len,path,plen)                             name length, or -1
12      GETUID        ()                                                who this runs as
13      CREATE        (path, len)                                       fd, for writing
14      CHDIR         (path, len)                                       0, or -1
15      GETCWD        (buf, len)                                        length written
16      MKDIR         (path, len)                                       0, or -1
17      RMDIR         (path, len)                                       0, or -1
18      UNLINK        (path, len)                                       0, or -1
19      RENAME        (from,flen,to,tlen)                               0, or -1
20      STAT          (path, len, struct user_stat \*)                  0, or -1
21      GETKEY        ()                                                one key, no echo
22      SCREEN        (uint32 \*cols, uint32 \*rows)                    0
23      CURSOR        (col, row)                                        0
24      CLEAR         ()                                                0
25      FORK          ()                                                the child's pid, or 0 if you are it
26      MMAP          (len)                                             address, or 0
27      MUNMAP        (address)                                         0, or -1
28      CHMOD         (path, len, mode)                                 0, or -1
29      CHOWN         (path, len, uid, gid)                             0, or -1
30      SYMLINK       (path,len,target,tlen)                            0, or -1
31      READLINK      (path,len,buf,size)                               length, or -1
32      GETENV        (name,len,buf,size)                               length, or -1
33      SETENV        (name,len,value,vlen)                             0, or -1
34      SOCKET        (port)                                            handle, or -1
35      SENDTO        (h, ip, port, buf, len)                           sent, or -1
36      RECVFROM      (h, struct user_from \*, buf, len)                n or -1
37      RECVWAIT      (h, struct user_from \*, buf, len, timeout)       n
38      SELECT        (handles, count, timeout, ready)                  how many
39      CONNECT       (ip, port)                                        handle, or -1
40      LISTEN        (port)                                            handle, or -1
41      ACCEPT        (h, timeout)                                      handle, or -1
42      SEND          (h, buf, len)                                     sent, or -1
43      RECV          (h, buf, len, timeout)                            n, 0 at end, -1
44      SHUTDOWN      (h)                                               0, or -1
45      SIGNAL        (sig, handler, trampoline)                        0, or -1
46      SIGSEND       (pid, sig)                                        0, or -1
47      SIGRETURN     (frame)                                           : does not return
48      SIGMASK       (how, mask)                                       the previous mask
49      TTYMODE       (set, flags)                                      the previous flags
50      WINSIZE       (cols\*, rows\*)                                  0, or -1
51      NOW           ()                                                seconds since 1970, or 0 if unknown
52      ALARM         (seconds)                                         seconds left on the last one
53      SYSLOG        (type, buf, len)                                  bytes copied, the size, or -1
54      SYNC          ()                                                0, or -1
55      FSCK          (path, len, flags, user_fsck\*)                   0, or -1
56      RESIZE        (path, len, blocks, user_resize\*)                0, or -1
57      SOURCE        (flags, user_source\*)                            0, or -1
58      EXECVE        (path, len, argv)                                 does not return, or -1
======  ============  ================================================  ======================

the kernel side
---------------

the numbers, ``struct user_stat``, ``struct user_from``, ``struct
user_resize`` and ``struct user_source`` are in
``include/uapi/velvet/syscall.h``, with what the calls take beside it in
the same directory: the signal numbers in ``signal.h``, the keys
``getkey`` hands back in ``input.h``, ``TTYMODE``'s flags in ``tty.h``, and
what ``fsck`` found in ``fsck.h``.
that directory is the part of the kernel a program compiles against, and
it includes nothing else of the kernel's. the entry stub, the saved frame
and the rest of what follows are x86's, in ``<asm/syscall.h>``.

the door between ring 3 and here. numbers are the kernel's, deliberately small in number, there is no libc out there to satisfy, only the handful of things a program in this kernel could want

four calls here are for a program drawing whole screens, and a shell needs none of them: it prints a prompt, reads a line and prints an answer, and the console keeps the cursor where the printing left it. an editor cannot work that way. it paints the entire screen, puts the cursor somewhere in the middle of what it painted, and waits for one keystroke rather than a line. getkey is the interesting one, because it is *also* how raw mode arrives. there is no flag anywhere saying "this terminal is raw": asking for a line gets the line discipline with its echo and its backspace handling, and asking for a key gets the key. the two questions are different, so they are different calls, and nothing has to remember which mode anything is in

what a file is, for anyone who wants to know without reading it. a program sees it under its posix name, struct stat, and velvetOS's userland/syscall.h holds that one against this one field by field at compile time, so the two cannot drift apart without the build saying so. the padding is explicit so that neither side's compiler gets to decide it

three calls are the smallest set that lets a program use udp: claim a port, send to somebody, take what arrived. there is no `connect` and no `accept`, because udp has neither, a socket here hears from everybody and says who each datagram came from, which is what makes a udp server one port rather than a connection per talker. recvfrom does not block. it says "nothing yet" and the program sleeps and asks again, which is a poll rather than a wait, honest for a kernel with no way yet to park a thread on a socket, and it keeps the receive path out of the business of waking anybody

waiting, rather than asking again in a loop. every one of these takes a timeout in milliseconds, 0 to look and move on, which is exactly what recvfrom did before, and negative to wait as long as it takes

the kernel's log, what dmesg reads. the first argument says what to do with it: SYSLOG_SIZE_BUFFER answers how big a buffer the whole log needs, and SYSLOG_READ_ALL copies the newest len bytes of it, oldest first, and leaves it as it was. anyone may read it, as anyone at the shell may run dmesg. the numbers are the ones every dmesg already passes

everything the kernel is holding for the disk, written to it. 0 once the disk holds what the kernel believes, which it already does when nothing was waiting or there is no disk, and -1 when the drive refused part of it

check the filesystem mounted at a path, and with FSCK_MEND put right what has exactly one right answer, which only root may ask. what it found goes in a struct user_fsck, from <velvet/fsck.h>: 0 when the check was made, whatever it found, and -1 when it could not be, with the reason in its arr_why

grow or shrink the ext4 filesystem mounted at a path to a number of its own blocks, 0 for as far as its partition and its descriptor table both allow. root's alone. what happened goes in a struct user_resize: 0 when it is that size now, which it may already have been, and -1 when it is not

what SYS_RESIZE did, in blocks of u_block_size: the partition's room, the most the descriptor table can describe, and the filesystem before and after. a shrink that would lose something refuses and counts what was in the way. when the call returns -1, arr_why says why

the source tree this kernel was built from, out on the medium it booted from, described in a struct user_source. with SOURCE_HASH the archive on the medium is read and hashed as well, so a program can say whether it is the tree this kernel was built from. -1 when the medium carries no source, or stopped answering part way through the hash

what SYS_SOURCE says: the archive on the medium, its files, bytes and first sector, then the size and sha1 of the archive the kernel was built with, and with SOURCE_HASH the sha1 of the one on the medium. both digests are hex. the build's is filled in even when the call returns -1, since the kernel knows it whatever the medium says

become another program: the one at path, with argv, a NULL-ended array of strings, as its arguments, or with only its own path when argv is NULL. the pid, the descriptors, the working directory, the uid and the environment stay; caught signals go back to their defaults, since the code that caught them is gone. it does not return, unless it failed, and then it returns -1 with nothing changed. an argv longer than MAX_ARGS is refused rather than cut short

who a datagram came from, filled in by recvfrom. a struct rather than two more registers because the syscall abi has six and recvfrom would otherwise want seven

everything ring 3 was holding when it made the call, written down by the entry stub in the order it pushes them. this exists for fork and for nothing else. a forked child has to come back from a syscall it never made, holding exactly what its parent held, and the callee-saved half of that is in the cpu at the moment of the call and gone a moment later, buried under some C prologue. the layout is the stack layout. changing either without the other is a program that resumes with its registers shuffled, which is the kind of bug that looks like the compiler being wrong

fifteen registers, fifteen pushes. this catches a field appearing or going away and cannot catch a reordering, for that the only real check is reading the two exit paths in `objdump -d bin/velvet --disassemble=syscall_entry` and `--disassemble=fork_return` and seeing the same order twice

leave for ring 3 through a frame rather than through a call. rax is zeroed on the way out, because the only caller is a forked child and that is how it finds out it is the child

how many times each has been asked for, and what to call it. the numbers are the cheapest possible picture of what a program actually does, one line of arithmetic per call, and afterwards you can say with certainty which door gets used

which kernel stack `syscall` should land on. the scheduler keeps this pointed at the running thread, exactly like the tss rsp0

where this core's kernel stack is, for the entry stub to stand on. per core, not global: the scheduler on one core must not be able to rewrite the stack another core is about to use

the libc side
-------------

the other side of the door is not in this tree. velvetOS, the system built
on this kernel, keeps it in ``userland/syscall.h``: each of these wrapped in
a function, so a program calls ``write(1, buf, len)`` rather than the
number. the wrappers are one instruction and a return value, and the shape
of each is the argument list above.

the numbers are written down once. ``userland/syscall.h`` includes the
headers in ``include/uapi/velvet/`` from the release it clones into
``obj/velvet/`` rather than keeping a copy, so the two sides cannot
disagree about which number is which. ``struct stat`` keeps its posix
name there, and is checked against ``struct user_stat`` with
``_Static_assert``\ s, size and every offset, so a layout that drifts
fails to compile. a call added on this side is usable there once a
release has it, velvetOS's ``kernel-release.mk`` names that release, and
``syscall.h`` has its wrapper, which is why a new call is still a change
to both projects, in that order.

