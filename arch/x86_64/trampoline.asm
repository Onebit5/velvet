; where a second cpu begins.
;
; a core that has never run holds itself in reset until its own local
; apic tells it otherwise. when it finally starts, it starts the way
; every x86 has started since 1978: in real mode, sixteen bits wide,
; with no page tables, no stack, and no idea that any of the last forty
; years happened. the vector in the startup message is a *page* number,
; so it wakes up at the top of one page below a megabyte -- and this is
; what has to be sitting there.
;
; its whole job is to climb back out into the present and jump into C.
; philemon does the same climb at boot; this is the same walk in a
; quarter of the space, for a core that is not the first.
;
; assembled flat and copied into place at runtime, so the addresses here
; have to be the addresses it really runs at.

bits 16
org 0x8000

TRAMP_BASE  equ 0x8000

; the bootstrap processor writes these in before waking anybody. they
; are past the code and never overwritten by the copy, and cpu/smp.c has
; the same numbers -- keep them together
ARG_CR3     equ TRAMP_BASE + 0x0f00     ; page tables with a low identity map
ARG_STACK   equ TRAMP_BASE + 0x0f08     ; this core's own stack, already mapped
ARG_ENTRY   equ TRAMP_BASE + 0x0f10     ; the C function to land in
ARG_CPU     equ TRAMP_BASE + 0x0f18     ; which core this is, passed in rdi
ARG_EFER    equ TRAMP_BASE + 0x0f20     ; the feature bits the first core runs with
ARG_LOUD    equ TRAMP_BASE + 0x0f28     ; whether to say where I have got to

; the core arrives with cs set to the vector and ip zero, so the very
; first thing has to be a jump that agrees with the org above
entry:
    jmp 0x0000:start

start:
    cli
    cld
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7c00          ; the loader's old stack, long since dead

    ; a woken core has no idt. any fault it takes has no handler, so it
    ; triple faults and the machine resets with nothing said, which is
    ; indistinguishable from every other way this can go wrong. so it can
    ; say where it has got to, one letter at a time, out of the port the
    ; first core already configured.
    ;
    ; it only does that when asked. a core that comes up is not
    ; interesting and should not clutter the boot; a core that does not
    ; is asked again with this turned on, and then the last letter it
    ; managed is the whole diagnosis
    mov al, 'A'
    call putc

    o32 lgdt [gdt_ptr]      ; o32, or only 24 bits of the base get loaded

    mov eax, cr4
    or eax, 1 << 5          ; physical address extension
    mov cr4, eax

    mov al, 'B'
    call putc

    ; the page tables the bootstrap processor prepared. they are a copy
    ; of the kernel's, plus an identity map of the first two megabytes --
    ; without which the instruction after paging comes on is unreachable,
    ; since this code is running down here and the kernel maps none of it
    mov eax, [ARG_CR3]
    mov cr3, eax

    mov al, 'C'
    call putc

    ; the same feature bits the bootstrap processor is running with, not
    ; just long mode. it matters more than it looks: that core enabled
    ; no-execute, so the kernel's page tables have bit 63 set on every
    ; page that is not code. on a core where no-execute is *off* that bit
    ; is reserved, and a reserved bit in a page table entry faults on
    ; every access to it -- so this core would climb all the way into
    ; long mode and then die on the first push to its own stack
    mov ecx, 0xc0000080
    rdmsr
    or eax, [ARG_EFER]
    wrmsr

    mov al, 'D'
    call putc

    mov eax, cr0
    or eax, (1 << 31) | 1   ; paging and protection at once
    mov cr0, eax

    jmp dword 0x08:long_mode

; al = the character. the serial port is already configured, so this is
; one status read and one write, with a bounded wait
putc:
    cmp byte [ARG_LOUD], 0
    jne .say
    ret
.say:
    push dx
    push ax
    mov dx, 0x3fd
.wait:
    in al, dx
    test al, 0x20
    jnz .send
    jmp .wait
.send:
    pop ax
    push ax
    mov dx, 0x3f8
    out dx, al
    pop ax
    pop dx
    ret

bits 64
long_mode:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    ; long mode reached. saying so needs its own copy of putc, since the
    ; 16-bit one is unreachable from a 64-bit code segment
    cmp byte [ARG_LOUD], 0
    je .quiet_e
    mov dx, 0x3fd
.wait64:
    in al, dx
    test al, 0x20
    jz .wait64
    mov dx, 0x3f8
    mov al, 'E'
    out dx, al
.quiet_e:

    mov rsp, [ARG_STACK]
    xor rbp, rbp

    cmp byte [ARG_LOUD], 0
    je .quiet_f
    mov dx, 0x3fd
.wait64b:
    in al, dx
    test al, 0x20
    jz .wait64b
    mov dx, 0x3f8
    mov al, 'F'
    out dx, al
.quiet_f:

    ; and into C, which switches to the kernel's own page tables as its
    ; first act -- it cannot be done here, because the moment the
    ; identity map goes the next instruction fetch would fault
    mov rdi, [ARG_CPU]
    mov rax, [ARG_ENTRY]
    jmp rax

bits 16

align 8
gdt:
    dq 0
    dq 0x00af9a000000ffff   ; 64-bit code
    dq 0x00af92000000ffff   ; data, which long mode barely reads
gdt_end:
gdt_ptr:
    dw gdt_end - gdt - 1
    dd gdt
