; philemon -- the bootloader, all of it.
;
; named for the one who grants the power and then steps back. he does not
; fight anything and he is not there for the rest of it. that is the whole
; job description of a bootloader.
;
; this is one program, in one file, at one address. the only reason there
; is a line drawn across the middle of it is that the bios reads exactly
; one sector -- 512 bytes, ending in 0x55 0xaa -- drops it at 0x7c00 and
; jumps to it. that is the entire contract, and it is not negotiable. so
; the first 512 bytes here do nothing but pull in the rest of this same
; file, to the address immediately after themselves, and carry on. after
; that the line is invisible: it is all one image at 0x7c00, and nothing
; below cares where the sector ended.
;
; the 64-bit half is a separate file only because it is C.

bits 16
org 0x7c00

%include "boot/philemon.inc"

LOADER_SECTORS equ 31       ; up to lba 32, where the disk table sits

; the part the bios reads

    jmp 0x0000:begin        ; some firmware arrives with cs set oddly

begin:
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7c00          ; growing down, away from everything
    cld

    ; dl is the drive I was read from, and the only way to know which
    mov [boot_drive], dl

    call serial_init
    mov si, msg_hello
    call print

    ; reading past the first 8 GiB needs the extended calls, and a bios
    ; without them is older than anything this kernel could run on
    mov ah, 0x41
    mov bx, 0x55aa
    mov dl, [boot_drive]
    int 0x13
    jc .too_old
    cmp bx, 0xaa55
    jne .too_old

    ; pull in the rest of myself, straight after this sector
    mov word [dap_count], LOADER_SECTORS
    mov word [dap_offset], 0x7e00
    mov word [dap_segment], 0
    mov dword [dap_lba], 1
    mov dword [dap_lba + 4], 0

    mov si, dap
    mov ah, 0x42
    mov dl, [boot_drive]
    int 0x13
    jc .unreadable

    jmp the_rest_of_me

.too_old:
    mov si, msg_too_old
    call print
    jmp halt
.unreadable:
    mov si, msg_unreadable
    call print
    jmp halt

;
; a bootloader that fails silently is one nobody can fix, and the two
; ways it can fail before it has read anything else are both up here.

serial_init:
    push dx
    push ax
    mov dx, 0x3fb           ; divisor latch on
    mov al, 0x80
    out dx, al
    mov dx, 0x3f8           ; divisor 1, which is 115200
    mov al, 0x01
    out dx, al
    mov dx, 0x3f9
    xor al, al
    out dx, al
    mov dx, 0x3fb           ; 8 bits, no parity, one stop
    mov al, 0x03
    out dx, al
    mov dx, 0x3fa           ; fifos on and cleared
    mov al, 0xc7
    out dx, al
    mov dx, 0x3fc
    mov al, 0x0b
    out dx, al
    pop ax
    pop dx
    ret

serial_putc:                ; al = the character
    push dx
    push ax
    mov dx, 0x3fd
    ; XXX: this poll has no bound. a machine whose com1 never reports
    ; THR-empty, an absent device whose status port reads zero rather
    ; than floating high, spins here forever, inside the first sector,
    ; before any of the messages further down can be printed. the 64-bit
    ; half bounds the same wait, in say_char; do the same here.
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

print:                      ; si = a zero-terminated string
    push ax
    push bx
.loop:
    lodsb
    test al, al
    jz .done
    cmp al, 10
    jne .send
    mov al, 13              ; a bare newline leaves a terminal halfway
    call serial_putc
    mov ah, 0x0e
    mov bx, 0x0007
    mov al, 13
    int 0x10
    mov al, 10
.send:
    call serial_putc
    push ax
    mov ah, 0x0e
    mov bx, 0x0007
    int 0x10
    pop ax
    jmp .loop
.done:
    pop bx
    pop ax
    ret

halt:
    cli
.forever:
    hlt
    jmp .forever

; eax, as eight hex digits, on both the serial port and the screen
print_hex32:
    push eax
    push ebx
    push cx
    mov cx, 8
.digit:
    rol eax, 4
    push eax
    and al, 0x0f
    add al, '0'
    cmp al, '9'
    jbe .emit
    add al, 39              ; past the punctuation, into 'a'
.emit:
    call serial_putc
    mov ah, 0x0e
    mov bx, 0x0007
    int 0x10
    pop eax
    dec cx
    jnz .digit
    pop cx
    pop ebx
    pop eax
    ret

boot_drive: db 0

align 4
dap:
    db 0x10                 ; the size of this packet
    db 0
dap_count:   dw 0
dap_offset:  dw 0
dap_segment: dw 0
dap_lba:     dq 0

msg_hello:      db 'philemon', 10, 0
msg_too_old:    db 'this bios cannot read a disk by lba', 10, 0
msg_unreadable: db 'the disk would not be read', 10, 0

; the boot signature, and the end of what the bios will do for me
times 510 - ($ - $$) db 0
dw 0xaa55

; the rest of the same program, read in by the code above

the_rest_of_me:
    call enable_a20
    call get_memory_map
    call load_everything
    call set_video_mode

    ; the video call reloaded segment registers with real-mode rules, so
    ; the wide limits are gone. get them back before anything else uses
    ; a 32-bit offset -- this is the single easiest way to break all of
    ; this, and it breaks it silently
    call go_unreal

    call fill_early
    call build_page_tables

    mov si, msg_long
    call print
    jmp enter_long_mode

;
; the twenty-first address line is still disabled at power on so that a
; machine from 1981 could wrap around at one megabyte. it has been forty
; years.

enable_a20:
    call a20_check
    jc .done

    mov ax, 0x2401          ; ask the bios first
    int 0x15
    call a20_check
    jc .done

    in al, 0x92             ; then reach for the port
    test al, 2
    jnz .already
    or al, 2
    and al, 0xfe            ; bit 0 would reset the machine
    out 0x92, al
.already:
    call a20_check
    jc .done

    mov si, msg_a20_fail
    call print
    jmp halt
.done:
    mov si, msg_a20
    call print
    ret

; carry set if the line works: write different values to two addresses a
; megabyte apart and see whether they stay different
a20_check:
    push ax
    push bx
    push ds
    push es
    xor ax, ax
    mov ds, ax
    mov ax, 0xffff
    mov es, ax

    mov bx, [ds:0x7dfe]
    mov word [ds:0x7dfe], 0x1234
    mov word [es:0x7e0e], 0x4321    ; the same byte if a20 is off
    mov ax, [ds:0x7dfe]
    mov word [ds:0x7dfe], bx

    cmp ax, 0x4321
    je .wrapped
    stc
    jmp .done
.wrapped:
    clc
.done:
    pop es
    pop ds
    pop bx
    pop ax
    ret


get_memory_map:
    mov di, E820_OFF
    mov ax, E820_SEG
    mov es, ax
    xor ebx, ebx
    xor bp, bp

.next:
    mov eax, 0xe820
    mov edx, 0x534d4150     ; 'SMAP', which the bios echoes back
    mov ecx, 24
    mov dword [es:di + 20], 1
    int 0x15
    jc .done                ; carry on the first call means unsupported
    cmp eax, 0x534d4150
    jne .failed

    jcxz .skip
    mov ecx, [es:di + 8]
    or ecx, [es:di + 12]
    jz .skip                ; a zero-length region is not a region

    inc bp
    add di, 24
    cmp bp, 128
    jae .done

.skip:
    test ebx, ebx           ; zero means that was the last
    jnz .next

.done:
    mov [e820_count], bp
    xor ax, ax
    mov es, ax
    mov si, msg_e820
    call print
    ret

.failed:
    mov si, msg_e820_fail
    call print
    jmp halt

;
; the bios cannot write above one megabyte and the kernel does not fit
; below it. so: step into protected mode for exactly long enough to load
; one segment register with a descriptor whose limit is the whole address
; space, and step back out. returning to real mode does not reload the
; hidden half of a segment register, so the wide limit survives and
; 32-bit offsets keep working.
;
; it goes in fs, not es. coming back to real mode leaves a segment
; register alone -- but *writing* to one reloads it with real-mode rules,
; and every bios call is entitled to write to es. fs is the one nothing
; else touches.

go_unreal:
    cli
    push eax
    push bx

    o32 lgdt [gdt32_ptr]    ; o32, or only 24 bits of the base are loaded

    mov eax, cr0
    or al, 1
    mov cr0, eax
    jmp $+2                 ; flush whatever the prefetcher believed

    mov bx, FLAT_SEL
    mov fs, bx

    mov eax, cr0
    and al, 0xfe
    mov cr0, eax            ; back to real mode, wide limit still cached
    jmp $+2

    pop bx
    pop eax
    sti
    ret

; esi = source, edi = destination, ecx = dwords. both linear
copy_up:
    push eax
.loop:
    test ecx, ecx
    jz .done
    a32 mov eax, [fs:esi]
    a32 mov [fs:edi], eax
    add esi, 4
    add edi, 4
    dec ecx
    jmp .loop
.done:
    pop eax
    ret

;
; dx:ax = lba, cx = sectors, edi = where it really goes. the bios reads
; into a buffer down in the first megabyte and each chunk is copied up.

read_sectors:
    pusha
.chunk:
    test cx, cx
    jz .done

    mov bx, cx
    cmp bx, BOUNCE_SECTORS
    jbe .have
    mov bx, BOUNCE_SECTORS
.have:
    mov word [dap_count], bx
    mov word [dap_offset], BOUNCE_OFF
    mov word [dap_segment], BOUNCE_SEG
    mov [dap_lba], ax
    mov [dap_lba + 2], dx
    mov dword [dap_lba + 4], 0

    push cx
    push ax
    push dx
    mov si, dap
    mov ah, 0x42
    mov dl, [boot_drive]
    int 0x13
    pop dx
    pop ax
    pop cx
    jc .failed

    ;  the bios has had the machine. some implementations use fs for
    ; their own purposes, and a flat fs is the only thing holding this
    ; together, so take it back rather than assume 
    call go_unreal

    push cx
    push edi
    movzx ecx, bx
    shl ecx, 7              ; sectors * 512 / 4
    mov esi, BOUNCE_LINEAR
    call copy_up
    pop edi
    pop cx

    movzx esi, bx
    shl esi, 9
    add edi, esi            ; on to the next chunk
    sub cx, bx
    add ax, bx
    adc dx, 0
    jmp .chunk

.done:
    popa
    ret

.failed:
    mov si, msg_disk_fail
    call print
    jmp halt

;
; the bios hands over a drive number in dl and it is usually the right
; one. usually is not something a bootloader can afford, and this
; machine has two disks -- and on the other one, sector 32 is the first
; sector of a file allocation table, which is a perfectly good sector
; full of something that is not mine.
;
; so rather than trust the number, ask each drive whether it is carrying
; my table. the one that says yes is the one I am on.

; read one sector from drive bl at lba ax into the bounce buffer. carry
; set if the drive would not answer. unlike read_sectors this does not
; halt: it is asking a question, and no is a valid reply
probe_read:
    push ax
    push dx
    push si
    mov word [dap_count], 1
    mov word [dap_offset], BOUNCE_OFF
    mov word [dap_segment], BOUNCE_SEG
    mov [dap_lba], ax
    mov word [dap_lba + 2], 0
    mov dword [dap_lba + 4], 0
    mov si, dap
    mov ah, 0x42
    mov dl, bl
    int 0x13
    pop si                  ; pops leave the carry flag alone, which is
    pop dx                  ; the whole answer here
    pop ax
    ret

find_my_disk:
    mov bl, 0x80            ; the first hard disk, by long convention
.next:
    mov ax, PH_TABLE_LBA
    call probe_read
    jc .skip

    call go_unreal          ; the bios has just had the machine

    mov esi, BOUNCE_LINEAR
    a32 mov eax, [fs:esi]
    cmp eax, PH_MAGIC_LO
    jne .skip
    a32 mov eax, [fs:esi + 4]
    cmp eax, PH_MAGIC_HI
    jne .skip

    mov [boot_drive], bl
    clc
    ret
.skip:
    inc bl
    cmp bl, 0x88           ; eight drives is more than anyone has here
    jb .next
    stc
    ret


load_everything:
    call go_unreal

    call find_my_disk
    jc .not_mine

    mov si, msg_found_disk
    call print
    movzx eax, byte [boot_drive]
    call print_hex32
    mov al, 10
    call serial_putc

    ; the table is already in the bounce buffer: finding it is what
    ; found the disk
    mov esi, BOUNCE_LINEAR

    ; somewhere the next read will not tread on
    mov edi, TABLE_LINEAR
    mov ecx, 32
    mov esi, BOUNCE_LINEAR
    call copy_up

    ; the 64-bit half of me
    a32 mov eax, [fs:TABLE_LINEAR + 8]
    mov [tmp_lba], eax
    a32 mov eax, [fs:TABLE_LINEAR + 16]
    mov [tmp_count], ax
    mov ax, [tmp_lba]
    mov dx, [tmp_lba + 2]
    mov cx, [tmp_count]
    mov edi, HANDOFF64_LINEAR
    call read_sectors

    ; the kernel
    a32 mov eax, [fs:TABLE_LINEAR + 24]
    mov [tmp_lba], eax
    a32 mov eax, [fs:TABLE_LINEAR + 32]
    mov [tmp_count], ax
    a32 mov eax, [fs:TABLE_LINEAR + 40]
    mov [kernel_size], eax
    mov ax, [tmp_lba]
    mov dx, [tmp_lba + 2]
    mov cx, [tmp_count]
    mov edi, KERNEL_LINEAR
    call read_sectors

    ; and the ramdisk, if there is one
    a32 mov eax, [fs:TABLE_LINEAR + 48]
    mov [tmp_lba], eax
    a32 mov eax, [fs:TABLE_LINEAR + 56]
    mov [tmp_count], ax
    a32 mov eax, [fs:TABLE_LINEAR + 64]
    mov [ramdisk_size], eax
    mov cx, [tmp_count]
    test cx, cx
    jz .no_ramdisk
    mov ax, [tmp_lba]
    mov dx, [tmp_lba + 2]
    mov edi, RAMDISK_LINEAR
    call read_sectors
.no_ramdisk:

    mov si, msg_loaded
    call print
    ret

.not_mine:
    mov si, msg_not_mine
    call print

    ; what was actually in that sector, and what I hoped for. a
    ; bootloader that cannot be stepped through has to hand over its
    ; evidence instead
    mov si, msg_saw
    call print
    mov esi, BOUNCE_LINEAR
    a32 mov eax, [fs:esi + 4]
    call print_hex32
    mov esi, BOUNCE_LINEAR
    a32 mov eax, [fs:esi]
    call print_hex32

    mov si, msg_wanted
    call print
    mov eax, PH_MAGIC_HI
    call print_hex32
    mov eax, PH_MAGIC_LO
    call print_hex32

    mov si, msg_at_lba
    call print
    mov eax, PH_TABLE_LBA
    call print_hex32
    mov al, 10
    call serial_putc
    jmp halt

;
; vbe is the only way to get a framebuffer without writing a driver for
; whatever card this is. a machine without it still boots -- it just has
; nothing but the serial port to say so on.

set_video_mode:
    mov word [fb_width], 0

    mov ax, 0x4f00
    mov di, VBE_INFO_OFF
    mov bx, VBE_SEG
    mov es, bx
    mov dword [es:di], 'VBE2'
    int 0x10
    cmp ax, 0x004f
    jne .none

    ; the mode list is a far pointer. turn it into a linear address and
    ; read it through fs, which is flat by now and is the one register
    ; the bios leaves alone -- no copying, and no second segment
    movzx eax, word [es:di + 16]
    shl eax, 4
    movzx ebx, word [es:di + 14]
    add eax, ebx
    mov [mode_ptr], eax

    xor ax, ax
    mov es, ax

    xor bp, bp              ; which size I am hoping for
.want_next:
    cmp bp, want_count
    jae .none

    mov si, bp
    shl si, 2
    mov ax, [want_list + si]
    mov [want_w], ax
    mov ax, [want_list + si + 2]
    mov [want_h], ax

    mov esi, [mode_ptr]
    xor bx, bx              ; how far along the list I have got
    ; XXX: this walk reads the mode list through fs, and fs is flat only
    ; because go_unreal set a wide limit on it. the int 0x10 calls above,
    ; and the 4f01 inside this loop, are firmware under no obligation to
    ; leave fs alone, and the caller already believes that a video call
    ; resets segment registers, because it calls go_unreal again the
    ; moment set_video_mode returns. one of the two believes something
    ; false. the failure mode is silent: a machine that finds no mode it
    ; likes and reports that the card has none. re-establish the wide
    ; limit before reading through fs.
.mode_next:
    cmp bx, 128
    jae .want_done
    a32 mov cx, [fs:esi]
    add esi, 2
    inc bx
    cmp cx, 0xffff
    je .want_done

    push bx
    push bp
    push esi
    push cx
    mov ax, 0x4f01
    mov di, VBE_MODE_OFF
    mov ax, VBE_SEG
    mov es, ax
    mov ax, 0x4f01
    int 0x10
    pop cx
    pop esi
    pop bp
    pop bx
    cmp ax, 0x004f
    jne .mode_next

    mov ax, VBE_SEG
    mov es, ax
    mov di, VBE_MODE_OFF

    mov ax, [es:di]         ; attributes
    test ax, 0x80           ; a linear framebuffer, not a window
    jz .mode_next
    test ax, 0x10           ; graphics, not text
    jz .mode_next

    mov al, [es:di + 25]    ; bits per pixel
    cmp al, 32
    jne .mode_next

    mov ax, [es:di + 18]
    cmp ax, [want_w]
    jne .mode_next
    mov ax, [es:di + 20]
    cmp ax, [want_h]
    jne .mode_next

    ; this one will do. everything about it has to be read now, because
    ; setting a mode does not tell you any of this afterwards
    mov ax, [es:di + 16]
    mov [fb_pitch], ax
    mov eax, [es:di + 40]
    mov [fb_addr], eax
    mov al, [es:di + 31]
    mov [fb_red_size], al
    mov al, [es:di + 32]
    mov [fb_red_shift], al
    mov al, [es:di + 33]
    mov [fb_green_size], al
    mov al, [es:di + 34]
    mov [fb_green_shift], al
    mov al, [es:di + 35]
    mov [fb_blue_size], al
    mov al, [es:di + 36]
    mov [fb_blue_shift], al

    mov ax, 0x4f02
    mov bx, cx
    or bx, 0x4000           ; with the linear framebuffer, please
    int 0x10
    cmp ax, 0x004f
    jne .none

    mov ax, [want_w]
    mov [fb_width], ax
    mov ax, [want_h]
    mov [fb_height], ax

    xor ax, ax
    mov es, ax
    mov si, msg_video
    call print
    ret

.want_done:
    inc bp
    jmp .want_next

.none:
    xor ax, ax
    mov es, ax
    mov word [fb_width], 0
    mov si, msg_no_video
    call print
    ret


fill_early:
    mov edi, EARLY_LINEAR
    a32 mov dword [fs:edi + 0], PH_MAGIC_LO
    a32 mov dword [fs:edi + 4], PH_MAGIC_HI

    a32 mov dword [fs:edi + 8], KERNEL_LINEAR
    a32 mov dword [fs:edi + 12], 0
    mov eax, [kernel_size]
    a32 mov [fs:edi + 16], eax
    a32 mov dword [fs:edi + 20], 0

    a32 mov dword [fs:edi + 24], RAMDISK_LINEAR
    a32 mov dword [fs:edi + 28], 0
    mov eax, [ramdisk_size]
    a32 mov [fs:edi + 32], eax
    a32 mov dword [fs:edi + 36], 0

    a32 mov dword [fs:edi + 40], E820_LINEAR
    a32 mov dword [fs:edi + 44], 0
    movzx eax, word [e820_count]
    a32 mov [fs:edi + 48], eax
    a32 mov dword [fs:edi + 52], 0

    mov eax, [fb_addr]
    a32 mov [fs:edi + 56], eax
    a32 mov dword [fs:edi + 60], 0
    movzx eax, word [fb_pitch]
    a32 mov [fs:edi + 64], eax
    a32 mov dword [fs:edi + 68], 0
    movzx eax, word [fb_width]
    a32 mov [fs:edi + 72], eax
    movzx eax, word [fb_height]
    a32 mov [fs:edi + 76], eax
    movzx eax, byte [fb_bpp]
    a32 mov [fs:edi + 80], eax

    movzx eax, byte [fb_red_shift]
    a32 mov [fs:edi + 84], eax
    movzx eax, byte [fb_green_shift]
    a32 mov [fs:edi + 88], eax
    movzx eax, byte [fb_blue_shift]
    a32 mov [fs:edi + 92], eax
    movzx eax, byte [fb_red_size]
    a32 mov [fs:edi + 96], eax
    movzx eax, byte [fb_green_size]
    a32 mov [fs:edi + 100], eax
    movzx eax, byte [fb_blue_size]
    a32 mov [fs:edi + 104], eax

    ; FIXME: this stores the drive number at offset 108, which is the
    ; padding before boot_drive, and then zeroes the low half of the
    ; field itself at 112. struct ph_early puts boot_drive at 112, so the
    ; value goes nowhere and the field is never set. nothing reads it
    ; today, which is the only reason this has not been a bug on the
    ; machine. store the value at 112 as a qword, or move the field to
    ; where the value actually lands.
    movzx eax, byte [boot_drive]
    a32 mov [fs:edi + 108], eax
    a32 mov dword [fs:edi + 112], 0
    ret

;
; long mode will not start without them. three windows: everything
; identity mapped so this code keeps working, the same memory again in
; the higher half where the kernel expects a direct map, and the kernel's
; own window at the very top.
;
; two-megabyte pages throughout, because four-kilobyte ones covering the
; same four gigabytes would be four megabytes of tables by themselves.

build_page_tables:
    mov edi, PT_BASE
    mov ecx, (PT_TOTAL_BYTES / 4)
    xor eax, eax
.clear:
    a32 mov [fs:edi], eax   ; a stray bit in an unused entry is a fault
    add edi, 4              ; with no obvious cause
    dec ecx
    jnz .clear

    mov edi, PT_PML4
    mov eax, PT_PDPT_LOW | 0x03
    a32 mov [fs:edi + 0], eax
    mov eax, PT_PDPT_HHDM | 0x03
    a32 mov [fs:edi + 256 * 8], eax
    mov eax, PT_PDPT_HIGH | 0x03
    a32 mov [fs:edi + 511 * 8], eax

    ; four directories, one per gigabyte, shared between the identity
    ; window and the direct map -- they describe the same memory, so
    ; there is no reason to build it twice
    mov edi, PT_PDPT_LOW
    mov esi, PT_PDPT_HHDM
    mov eax, PT_PD0 | 0x03
    mov ecx, 4
.pdpt:
    a32 mov [fs:edi], eax
    a32 mov [fs:esi], eax
    add edi, 8
    add esi, 8
    add eax, 0x1000
    dec ecx
    jnz .pdpt

    mov edi, PT_PD0
    mov eax, 0x83           ; present, writable, and a large page
    mov ecx, 2048
.pd:
    a32 mov [fs:edi], eax
    add edi, 8
    add eax, 0x200000
    dec ecx
    jnz .pd

    ; the kernel is linked at 0xffffffff80000000, which is entry 510 of
    ; the top directory pointer table
    mov edi, PT_PDPT_HIGH
    mov eax, PT_PD_KERNEL | 0x03
    a32 mov [fs:edi + 510 * 8], eax

    mov edi, PT_PD_KERNEL
    mov eax, KERNEL_PHYS | 0x83
    mov ecx, 32             ; sixty-four megabytes, far more than it needs
.pdk:
    a32 mov [fs:edi], eax
    add edi, 8
    add eax, 0x200000
    dec ecx
    jnz .pdk
    ret


enter_long_mode:
    cli
    o32 lgdt [gdt64_ptr]

    mov eax, cr4
    or eax, 1 << 5          ; physical address extension
    mov cr4, eax

    mov eax, PT_PML4
    mov cr3, eax

    mov ecx, 0xc0000080     ; the long mode enable bit lives in an msr
    rdmsr
    or eax, 1 << 8
    wrmsr

    mov eax, cr0
    or eax, (1 << 31) | 1   ; paging and protection at once, the only way
    mov cr0, eax

    jmp dword CODE64_SEL:long_start

bits 64
long_start:
    mov ax, DATA64_SEL
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    mov rsp, 0x00090000     ; free, and below the extended bios data area
    mov rax, HANDOFF64_LINEAR
    jmp rax

bits 16


align 8
gdt32:
    dq 0
    ; flat data: base 0, limit the whole address space. this is the one
    ; whose limit unreal mode borrows
    dw 0xffff, 0x0000
    db 0x00, 0x92, 0xcf, 0x00
gdt32_end:
gdt32_ptr:
    dw gdt32_end - gdt32 - 1
    dd gdt32

align 8
gdt64:
    dq 0
    dq 0x00af9a000000ffff   ; 64-bit code
    dq 0x00af92000000ffff   ; and data, which long mode ignores harder
gdt64_end:
gdt64_ptr:
    dw gdt64_end - gdt64 - 1
    dd gdt64

FLAT_SEL   equ 0x08
CODE64_SEL equ 0x08
DATA64_SEL equ 0x10


e820_count:   dw 0
kernel_size:  dd 0
ramdisk_size: dd 0
tmp_lba:      dd 0
tmp_count:    dw 0
want_w:       dw 0
want_h:       dw 0

fb_addr:        dd 0
fb_pitch:       dw 0
fb_width:       dw 0
fb_height:      dw 0
fb_bpp:         db 32
fb_red_size:    db 0
fb_red_shift:   db 0
fb_green_size:  db 0
fb_green_shift: db 0
fb_blue_size:   db 0
fb_blue_shift:  db 0

; what to ask the card for, in the order I would rather have it
want_list:
    dw 1280, 800
    dw 1024, 768
    dw 1280, 1024
    dw 800, 600
    dw 640, 480
want_count equ 5

mode_ptr: dd 0

msg_a20:        db '  a20 on', 10, 0
msg_a20_fail:   db '  a20 will not turn on', 10, 0
msg_e820:       db '  memory map read', 10, 0
msg_e820_fail:  db '  the bios will not describe memory', 10, 0
msg_loaded:     db '  kernel and ramdisk loaded', 10, 0
msg_not_mine:   db '  no disk here is carrying my table', 10, 0
msg_found_disk: db '  my disk is drive ', 0
msg_saw:        db '    last saw ', 0
msg_wanted:     db 10, '    wanted ', 0
; TODO: nothing prints this. either use it for the drive the table was
; found on, or delete it. the loader has a thirty-one sector budget and
; pays for every byte in it.
msg_from_drive: db 10, '    drive  ', 0
msg_at_lba:     db '  lba ', 0
msg_video:      db '  video mode set', 10, 0
msg_no_video:   db '  no vbe. serial only, which is enough to see by', 10, 0
msg_long:       db '  entering long mode', 10, 0
msg_disk_fail:  db '  the disk stopped answering', 10, 0
