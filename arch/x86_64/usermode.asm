; the one-way trip into ring 3.
;
; there is no instruction for "start running in user mode": you fake a
; return from an interrupt that never happened. push the five things
; iretq expects to pop, with user selectors and the user's flags, and
; let it deliver me somewhere I can never simply walk back from.

bits 64
section .text

global enter_usermode

; enter_usermode(entry, user_stack_top, user_cs, user_ss)
;                rdi    rsi              rdx      rcx
enter_usermode:
    ; the segment registers are not covered by iretq and would otherwise
    ; still hold kernel selectors in ring 3.
    ;
    ; gs is deliberately not among them. in 64-bit mode its base does not
    ; come from the descriptor table at all: it comes from an msr, and
    ; loading any real selector into it overwrites that base with the
    ; descriptor's, which is zero. this kernel keeps the per-core pointer
    ; there, so one `mov gs, ax` on the way into ring 3 quietly unnames
    ; the core, and the program's very first system call writes through a
    ; base of zero and faults inside the kernel.
    ;
    ; leaving it alone costs nothing: nothing in ring 3 reads gs, and
    ; iretq nulls the selector on its own when dropping privilege, a
    ; null selector being the one load that leaves the msr base intact
    mov ax, cx
    mov ds, ax
    mov es, ax
    mov fs, ax

    push rcx                ; ss
    push rsi                ; rsp
    push qword 0x202        ; rflags: IF set, and bit 1 which is always 1
    push rdx                ; cs
    push rdi                ; rip

    ; nothing is inherited: the arguments are on the stack, below rsp,
    ; and rdx is zero, which _start takes as no function for atexit.
    ; whatever else I happened to be holding is the kernel's business,
    ; not the program's
    xor rdi, rdi
    xor rsi, rsi
    xor rax, rax
    xor rbx, rbx
    xor rcx, rcx
    xor rdx, rdx
    xor rbp, rbp
    xor r8, r8
    xor r9, r9
    xor r10, r10
    xor r11, r11
    xor r12, r12
    xor r13, r13
    xor r14, r14
    xor r15, r15

    iretq

section .note.GNU-stack noalloc noexec nowrite progbits
