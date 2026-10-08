// SPDX-License-Identifier: GPL-2.0-only
/*
 * arch/x86_64/syscall.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the door between ring 3 and here.
 */

#ifndef ARCH_X86_64_SYSCALL_H
#define ARCH_X86_64_SYSCALL_H

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
 * this layout is duplicated in velvetOS's userland/syscall.h, which is
 * what an abi is: two sides agreeing on where the fields sit, with
 * nothing to enforce it but the fact that they were written together.
 * the padding is explicit so that neither side's compiler gets to
 * decide it
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

#define SYSCALL_COUNT 53

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

/*
 * everything ring 3 was holding when it made the call, written down by
 * the entry stub in the order it pushes them.
 *
 * this exists for fork and for nothing else. a forked child has to come
 * back from a syscall it never made, holding exactly what its parent
 * held, and the callee-saved half of that is in the cpu at the moment
 * of the call and gone a moment later, buried under some C prologue.
 *
 * the layout is the stack layout. changing either without the other is
 * a program that resumes with its registers shuffled, which is the kind
 * of bug that looks like the compiler being wrong
 */
struct user_regs {
    uint64_t r15, r14, r13, r12, rbx, rbp;
    uint64_t r9, r8, r10, rdx, rsi, rdi;
    uint64_t r11;       /* the user's rflags, courtesy of syscall */
    uint64_t rcx;       /* the user's rip, likewise */
    uint64_t rsp;
};

/*
 * fifteen registers, fifteen pushes. this catches a field appearing or
 * going away and cannot catch a reordering, for that the only real
 * check is reading the two exit paths in
 * `objdump -d bin/velvet --disassemble=syscall_entry` and
 * `--disassemble=fork_return` and seeing the same order twice
 */
_Static_assert(sizeof(struct user_regs) == 15 * 8,
               "user_regs and the pushes in syscall.asm have drifted apart");

/*
 * leave for ring 3 through a frame rather than through a call. rax is
 * zeroed on the way out, because the only caller is a forked child and
 * that is how it finds out it is the child
 */
void fork_return(struct user_regs *frame);


/* wire up STAR/LSTAR/SFMASK and turn on EFER.SCE */
void syscall_init(void);

/*
 * how many times each has been asked for, and what to call it. the
 * numbers are the cheapest possible picture of what a program actually
 * does, one line of arithmetic per call, and afterwards you can say
 * with certainty which door gets used
 */
uint64_t syscall_times_called(unsigned nr);
const char *syscall_name(unsigned nr);

/*
 * which kernel stack `syscall` should land on. the scheduler keeps this
 * pointed at the running thread, exactly like the tss rsp0
 */
/*
 * where this core's kernel stack is, for the entry stub to stand on.
 * per core, not global: the scheduler on one core must not be able to
 * rewrite the stack another core is about to use
 */
void syscall_set_kernel_rsp(uint64_t rsp);

#endif
