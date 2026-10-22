; where ring 3 comes in.
;
; `syscall` is fast because it does almost nothing: it puts the return
; address in rcx, the flags in r11, loads cs/ss from STAR and rip from
; LSTAR, and that is the whole of it. in particular it does NOT change
; rsp: I arrive on the *user's* stack, in ring 0, which is as
; alarming as it sounds. the first job is to get off it.
;
; the swap used to use two globals, which was safe because SFMASK clears
; IF: I arrive with interrupts off, and nothing could preempt me in the
; three instructions before the user's rsp was on a kernel stack.
;
; that reasoning was airtight and is now wrong. it says nothing about
; another core, and with four of them a global kernel rsp means a thread
; can `syscall` its way onto some other core's stack.
;
; so the two words are per core, found through gs, which can be read
; without clobbering a single register, and that matters here more than
; anywhere: rax holds the call number, rcx and r11 hold the return
; address and flags, everything else holds arguments, and there is no
; stack yet to save anything on.
;
;   gs:0   this core's kernel stack, kept current by the scheduler
;   gs:8   somewhere to park the user's stack for two instructions
;
; there is no swapgs here, and that is deliberate. the usual arrangement
; is one gs base for ring 3 and another for ring 0, exchanged on the way
; in and out, but the *parity* of those exchanges is then per thread,
; while the bases are per core. a thread preempted inside a syscall and
; resumed on another core does its exit swap on a core that never did
; the entry one, which leaves that core's two bases the wrong way round
; and the next syscall on it reading through a base of zero. that is not
; a hypothetical: it is what this stub did on its second call.
;
; so gs simply names the current core, in both rings, always. nothing to
; keep in step and nothing to get wrong across a migration.
;
; the price is that anything loading a real selector into gs zeroes the
; base, and the next system call then writes through zero and faults in
; the kernel. that is not hypothetical either: the trip into ring 3 used
; to do exactly that, one instruction at a time, on every program that
; ever ran. see usermode.asm. every program here is one I compiled and
; none of them touch gs, so what is left is a program able to crash the
; machine rather than escape it: a real edge, worth naming.
;
; FIXME: that "crash rather than escape" holds only while address 0 is
; unmapped, and nothing makes it so. fs/elf.c refuses a segment above the
; higher half and never refuses one at the bottom, so a program can have
; a writable page at 0, and then this stub loads rsp out of it, in ring
; 0, and the kernel runs on a stack the program chose. even without that,
; one legal `mov gs, <user selector>` from ring 3 is a panic: [gs:0]
; becomes address 0, ring 0 faults, and the fault handler panics. the
; base has to come from somewhere ring 3 cannot clear.

bits 64
section .text

extern syscall_from_user
extern interrupt_return

%define USER_CS 0x23                        ; GDT_USER_CODE3, asm/gdt.h
%define USER_SS 0x1b                        ; GDT_USER_DATA3

global syscall_entry

; the frame built here is a struct pt_regs, the same one an interrupt
; leaves, so that everything after this point, a signal, a fork, a
; program that faults inside a call, has one layout to read and one way
; out. what the cpu would have pushed for an interrupt from ring 3 is
; pushed by hand: the user's stack and flags and where it was, with the
; selectors it was running under.
;
; the way out is `iretq` rather than `sysret`. sysret puts rcx and r11
; back from wherever the user's rip and flags are, so a frame a signal
; handler or sigreturn rewrote could only ever come back with those two
; registers wrong; iretq gives back every register the frame says. it
; costs a few hundred cycles a call, which nothing here will notice.
syscall_entry:
    mov [gs:8], rsp                         ; park the user stack briefly
    mov rsp, [gs:0]                         ; and stand on my own

    push qword USER_SS                      ; ss
    push qword [gs:8]                       ; rsp, now per-thread
    push r11                                ; rflags, courtesy of syscall
    push qword USER_CS                      ; cs
    push rcx                                ; rip, likewise
    push qword 0                            ; error code
    push rax                                ; vector: the call's number

    ; and then exactly what isr_common pushes, in its order. rcx and r11
    ; hold the rip and flags syscall put there, which is what a program
    ; finds in them after a call either way
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    sti                                     ; safe now, I am on my own stack
    cld
    mov rdi, rsp                            ; the frame is the one argument
    call syscall_from_user                  ; which writes the answer into it

    cli                                     ; nothing may interrupt the unwind
    jmp interrupt_return

;
; the child is a thread that has never been in ring 3, so it leaves
; through a copy of its parent's frame, already holding the zero that
; tells it which of the two it is.
;
; fork_return(struct pt_regs *frame): never returns
global fork_return
fork_return:
    cli
    mov rsp, rdi                            ; stand on the copy
    jmp interrupt_return

section .note.GNU-stack noalloc noexec nowrite progbits
