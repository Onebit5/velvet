; the context switch. this is the whole magic trick of a scheduler and
; its 16 instructions long.
;
;   switch_context(uint64_t *save_rsp, uint64_t *load_rsp)
;   rdi = where to stash the outgoing thread's rsp
;   rsi = where to read the incoming thread's rsp from
;
; I only touch the callee-saved registers, because the sysv abi already
; says a function call may clobber the rest -- whoever called me has
; either saved rax/rcx/etc or doesnt care about them. that makes the
; parked state of a thread just: six registers and a return address,
; sitting on its own stack.
;
; rflags is deliberately NOT saved. threads that got preempted resume
; inside an irq handler and get their flags back from iretq; threads
; that yielded voluntarily get theirs back from irq_restore(); brand
; new threads sti for themselves in the bootstrap. see thread.c
;
; TODO: no xmm and no x87 state is saved here either, and nothing saves
; it on the syscall path. that is safe for exactly one reason: the kernel
; and the userland are both built -mno-sse -mno-80387, so no compiler in
; this build emits one. the flag is load bearing rather than an
; optimisation. a program that reaches for xmm anyway, through inline
; asm, shares those registers with every other thread on the core.
; saving them is what unlocks turning sse on at all

bits 64
section .text

global switch_context

switch_context:
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15

    mov [rdi], rsp      ; outgoing thread is now fully described by its rsp
    mov rsp, [rsi]      ; and here I become somebody else entirely

    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret                 ; returns wherever the incoming thread left off

section .note.GNU-stack noalloc noexec nowrite progbits
