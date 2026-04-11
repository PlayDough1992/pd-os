; PD-Bootloader Stage 1 - 512 byte MBR

[BITS 16]
[ORG 0x7C00]

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti
    
    call clear_screen
    
    mov si, banner_top
    call print_string
    mov si, banner_title
    call print_string
    mov si, banner_bottom
    call print_string
    
    mov si, msg_load
    call print_string

    ; ---- Load Stage 2 from disk (sectors 2..5, 4 sectors = 2048 bytes) ----
    mov ax, 0x0000
    mov es, ax                 ; load buffer segment
    mov bx, 0x8000             ; load buffer offset  → linear 0x8000
    mov ah, 0x02               ; BIOS: read sectors
    mov al, 5                  ; number of sectors to read (5 * 512 = 2560 bytes)
    mov ch, 0                  ; cylinder 0
    mov cl, 2                  ; starting sector 2 (sector 1 = this MBR)
    mov dh, 0                  ; head 0
    ; DL already holds the boot drive number (set by BIOS before handing off)
    int 0x13
    jc  .disk_error

    jmp 0x0000:0x8000          ; jump to Stage 2

.disk_error:
    mov si, msg_disk_err
    call print_string
    jmp halt

clear_screen:
    pusha
    mov ah, 0x00
    mov al, 0x03
    int 0x10
    popa
    ret

print_string:
    pusha
    mov ah, 0x0E
.loop:
    lodsb
    cmp al, 0
    je .done
    int 0x10
    jmp .loop
.done:
    popa
    ret

halt:
    cli
    hlt
    jmp halt

; Data
banner_top:    db 0x0D, 0x0A, 0x0A
               db '               ==========================================', 0x0D, 0x0A, 0
banner_title:  db '                                                        ', 0x0D, 0x0A
               db '                      P D - B O O T L O A D E R         ', 0x0D, 0x0A
               db '                            Version 0.1                 ', 0x0D, 0x0A, 0
banner_bottom: db '               ==========================================', 0x0D, 0x0A, 0x0A, 0
msg_load:      db '                       Loading Stage 2...', 0x0D, 0x0A, 0x0A, 0
msg_disk_err:  db '               [ERROR] Failed to read Stage 2 from disk!', 0x0D, 0x0A, 0

times 510-($-$$) db 0
dw 0xAA55