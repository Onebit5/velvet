; interrupt stubs, all 256 of them, macro generated because life is short.
;
; some exceptions push an error code on the stack and most dont, so the
; ones that dont get a fake 0 pushed. that way every vector lands in
; isr_common with the exact same frame layout and C only needs one
; struct to describe it (struct interrupt_frame, keep in sync!)

bits 64
section .text

extern interrupt_dispatch

%macro ISR_STUB 1
isr_stub_%1:
%if %1 == 8 || %1 == 10 || %1 == 11 || %1 == 12 || %1 == 13 || %1 == 14 || %1 == 17 || %1 == 21 || %1 == 29 || %1 == 30
    ; cpu already pushed an error code for this one
%else
    push qword 0
%endif
    push qword %1
    jmp isr_common
%endmacro

%assign v 0
%rep 256
ISR_STUB v
%assign v v+1
%endrep

isr_common:
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

    cld                     ; sysv wants DF clear before calling C
    mov rdi, rsp            ; first arg = pointer to the frame I just built
    call interrupt_dispatch

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    add rsp, 16             ; drop vector + error code
    iretq

section .rodata
global isr_stub_table
isr_stub_table:
%assign v 0
%rep 256
    dq isr_stub_%+v
%assign v v+1
%endrep

; tell the linker I dont want an executable stack, thanks
section .note.GNU-stack noalloc noexec nowrite progbits
