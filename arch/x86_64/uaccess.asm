; the only instructions in the kernel that touch ring 3's memory.
;
; a pointer a program hands over is a claim, not a promise: the page may
; never have been touched, may belong to nobody, or may be read only. the
; first two the page fault handler settles the way it would for the
; program itself, making the page or making the copy, and the instruction
; runs again. the last is a mistake the program made, and it must not
; become the kernel's: so each instruction below that can fault has an
; entry in __ex_table naming where to go instead, and the fault handler,
; finding the faulting rip there, sends it on rather than panicking.
;
; each routine answers how much it did not do, which is 0 when nothing
; went wrong. the C half in uaccess.c turns that into -EFAULT.

bits 64
section .text

; uint64_t uaccess_copy(void *to, const void *from, uint64_t len)
; either side may be ring 3's. returns the bytes left uncopied
global uaccess_copy
uaccess_copy:
    mov rcx, rdx
.move:
    rep movsb               ; a fault leaves rcx counting what is left
    xor eax, eax
    ret
.fault:
    mov rax, rcx
    ret

; uint64_t uaccess_clear(void *to, uint64_t len)
; returns the bytes left unzeroed
global uaccess_clear
uaccess_clear:
    mov rcx, rsi
    xor eax, eax
.store:
    rep stosb
    xor eax, eax
    ret
.fault:
    mov rax, rcx
    ret

; int64_t uaccess_strncpy(char *to, const char *from, uint64_t max)
; copies up to and including the NUL. returns the length before it, max
; when there was none within max bytes, or -1 for a fault
global uaccess_strncpy
uaccess_strncpy:
    xor eax, eax
.next:
    cmp rax, rdx
    je .done
.load:
    mov cl, [rsi + rax]
    mov [rdi + rax], cl
    test cl, cl
    jz .done
    inc rax
    jmp .next
.done:
    ret
.fault:
    mov rax, -1
    ret

; the instruction that may fault, and where it goes when it does
section __ex_table progbits alloc noexec nowrite align=8
    dq uaccess_copy.move, uaccess_copy.fault
    dq uaccess_clear.store, uaccess_clear.fault
    dq uaccess_strncpy.load, uaccess_strncpy.fault

section .note.GNU-stack noalloc noexec nowrite progbits
