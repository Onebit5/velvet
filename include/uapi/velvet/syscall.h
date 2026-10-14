// SPDX-License-Identifier: GPL-2.0-only
/*
 * include/uapi/velvet/syscall.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the numbers ring 3 calls the kernel by, and what the calls hand back.
 */

#ifndef UAPI_VELVET_SYSCALL_H
#define UAPI_VELVET_SYSCALL_H

#include <stdint.h>

/*
 * the door between ring 3 and here. numbers are the kernel's, deliberately
 * small in number, there is no libc out there to satisfy, only the
 * handful of things a program in this kernel could want
 */

#define SYS_EXIT   0
#define SYS_WRITE  1
#define SYS_READ   2
#define SYS_UPTIME 3
#define SYS_YIELD  4
#define SYS_SLEEP  5
#define SYS_OPEN   6
#define SYS_CLOSE  7
#define SYS_GETPID 8
#define SYS_SPAWN  9
#define SYS_WAIT   10
#define SYS_READDIR 11
#define SYS_GETUID 12
#define SYS_CREATE 13
#define SYS_CHDIR  14
#define SYS_GETCWD 15
#define SYS_MKDIR  16
#define SYS_RMDIR  17
#define SYS_UNLINK 18
#define SYS_RENAME 19
#define SYS_STAT   20

/*
 * a shell needs none of these: it prints a prompt, reads a line and
 * prints an answer, and the console keeps the cursor where the printing
 * left it. an editor cannot work that way. it paints the entire screen,
 * puts the cursor somewhere in the middle of what it painted, and waits
 * for one keystroke rather than a line.
 *
 * getkey is the interesting one, because it is *also* how raw mode
 * arrives. there is no flag anywhere saying "this terminal is raw":
 * asking for a line gets the line discipline with its echo and its
 * backspace handling, and asking for a key gets the key. the two
 * questions are different, so they are different calls, and nothing has
 * to remember which mode anything is in
 */
#define SYS_GETKEY 21
#define SYS_SCREEN 22
#define SYS_CURSOR 23
#define SYS_CLEAR  24

/*
 * what a file is, for anyone who wants to know without reading it.
 *
 * a program sees it under its posix name, struct stat, and velvetOS's
 * userland/syscall.h holds that one against this one field by field
 * at compile time, so the two cannot drift apart without the build
 * saying so. the padding is explicit so that neither side's compiler
 * gets to decide it
 */
struct user_stat {
    uint64_t size;
    uint32_t mode;
    uint32_t is_dir;
    uint32_t uid, gid;
    uint32_t is_symlink;
    uint16_t year;
    uint8_t  month, day, hour, minute, second;
    uint8_t  pad;
};

#define SYS_FORK   25
#define SYS_MMAP   26
#define SYS_MUNMAP 27
#define SYS_CHMOD  28
#define SYS_CHOWN  29
#define SYS_SYMLINK 30
#define SYS_READLINK 31
#define SYS_GETENV 32
#define SYS_SETENV 33

/*
 * three calls, which is the smallest set that lets a program use udp:
 * claim a port, send to somebody, take what arrived.
 *
 * there is no `connect` and no `accept`, because udp has neither, a
 * socket here hears from everybody and says who each datagram came
 * from, which is what makes a udp server one port rather than a
 * connection per talker.
 *
 * recvfrom does not block. it says "nothing yet" and the program sleeps
 * and asks again, which is a poll rather than a wait, honest for a
 * kernel with no way yet to park a thread on a socket, and it keeps the
 * receive path out of the business of waking anybody
 */
#define SYS_SOCKET   34
#define SYS_SENDTO   35
#define SYS_RECVFROM 36

/*
 * waiting, rather than asking again in a loop. every one of
 * these takes a timeout in milliseconds, 0 to look and move on, which
 * is exactly what recvfrom did before, and negative to wait as long as
 * it takes
 */
#define SYS_RECVWAIT 37
#define SYS_SELECT   38
#define SYS_CONNECT  39
#define SYS_LISTEN   40
#define SYS_ACCEPT   41
#define SYS_SEND     42
#define SYS_RECV     43
#define SYS_SHUTDOWN 44

/* signals */
#define SYS_SIGNAL    45
#define SYS_SIGSEND   46
#define SYS_SIGRETURN 47
#define SYS_SIGMASK   48

/* the terminal */
#define SYS_TTYMODE   49
#define SYS_WINSIZE   50

/* a clock that means something outside this boot */
#define SYS_NOW       51
#define SYS_ALARM     52

/*
 * the kernel's log, what dmesg reads. the first argument says what to
 * do with it: SYSLOG_SIZE_BUFFER answers how big a buffer the whole log
 * needs, and SYSLOG_READ_ALL copies the newest len bytes of it, oldest
 * first, and leaves it as it was. the numbers are the ones every dmesg
 * already passes
 */
#define SYS_SYSLOG    53

#define SYSLOG_READ_ALL    3
#define SYSLOG_SIZE_BUFFER 10

/*
 * everything the kernel is holding for the disk, written to it. 0 once
 * the disk holds what the kernel believes, which it already does when
 * nothing was waiting or there is no disk, and -1 when the drive
 * refused part of it
 */
#define SYS_SYNC      54

/*
 * check the filesystem mounted at a path, and with FSCK_MEND put right
 * what has exactly one right answer, which only root may ask. what it
 * found goes in a struct user_fsck, from <velvet/fsck.h>: 0 when the
 * check was made, whatever it found, and -1 when it could not be
 */
#define SYS_FSCK      55

/*
 * grow or shrink the ext4 filesystem mounted at a path to a number of
 * its own blocks, 0 for as far as its partition and its descriptor
 * table both allow. root's alone. what happened goes in a struct
 * user_resize: 0 when it is that size now, which it may already have
 * been, and -1 when it is not
 */
#define SYS_RESIZE    56

/*
 * the source tree this kernel was built from, out on the medium it
 * booted from, described in a struct user_source. with SOURCE_HASH the
 * archive on the medium is read and hashed as well, so a program can say
 * whether it is the tree this kernel was built from. -1 when the medium
 * carries no source, or stopped answering part way through the hash
 */
#define SYS_SOURCE    57

#define SOURCE_HASH 1

/*
 * become another program: the one at path, with argv, a NULL-ended
 * array of strings, as its arguments, or with only its own path when
 * argv is NULL. the pid, the descriptors, the working directory, the uid
 * and the environment stay; caught signals go back to their defaults,
 * since the code that caught them is gone. it does not return, unless
 * it failed, and then it returns -1 with nothing changed
 */
#define SYS_EXECVE    58

/*
 * a pipe the caller holds both ends of: the reading end's descriptor in
 * fds[0] and the writing end's in fds[1], two ints. reads wait for
 * something to arrive and return 0 once every writing end is closed
 */
#define SYS_PIPE      59

/*
 * make descriptor `to` another handle on whatever `from` is, closing
 * what `to` held first: `to`, or -1 when `from` is not open. a copy of
 * a pipe end is one more holder of it, and a copy of a file keeps a
 * position of its own
 */
#define SYS_DUP2      60

/*
 * put a process in a group: the caller or a child of its own, into a
 * group named after it or one that already has a member of the same
 * user. 0 for the pid means the caller and 0 for the group means the
 * pid's own. sigsend with minus a pgid signals the whole group
 */
#define SYS_SETPGID   61

#define SYSCALL_COUNT 62

/*
 * what SYS_SOURCE says: the archive on the medium, its files, bytes and
 * first sector, then the size and sha1 of the archive the kernel was
 * built with, and with SOURCE_HASH the sha1 of the one on the medium.
 * both digests are hex. the build's is filled in even when the call
 * returns -1, since the kernel knows it whatever the medium says
 */
struct user_source {
    uint64_t u_files, u_bytes, u_lba;
    uint64_t u_built_bytes;
    char     arr_built[41];
    char     arr_medium[41];
    char     arr_pad[6];
};

#define RESIZE_WHY_MAX 128

/*
 * what SYS_RESIZE did, in blocks of u_block_size: the partition's room,
 * the most the descriptor table can describe, and the filesystem before
 * and after. a shrink that would lose something refuses and counts what
 * was in the way. when the call returns -1, arr_why says why
 */
struct user_resize {
    uint64_t u_room, u_ceiling;
    uint64_t u_blocks_in_way, u_inodes_in_way;
    uint32_t u_block_size;
    uint32_t u_blocks_before, u_blocks_after;
    uint32_t u_groups_before, u_groups_after;
    uint32_t u_inodes_before, u_inodes_after;
    uint32_t u_pad;
    char     arr_why[RESIZE_WHY_MAX];
};

/*
 * who a datagram came from, filled in by recvfrom. a struct rather than
 * two more registers because the syscall abi has six and recvfrom would
 * otherwise want seven
 */
struct user_from {
    uint32_t address;
    uint16_t port;
    uint16_t reserved;
};

#endif
