; =============================================================================
;  PD-Bootloader  -  Stage 2
;  Loaded by Stage 1 at 0x0000:0x8000
;
;  1. Draw a clean TUI boot menu using direct VGA memory writes (no BIOS font)
;  2. Arrow / number key selection with countdown timer
;  3. Boot PD-OS  -> A20 -> GDT -> protected mode -> VGA stub
;  4. Enter BIOS  -> keyboard-controller reset (pulse pin 0xFE on port 0x64)
;                    This is the only reliable cross-platform hard reset that
;                    actually takes you back to firmware / POST on real hw and
;                    QEMU alike. INT 19h only reloads the boot sector.
; =============================================================================

[BITS 16]
[ORG 0x8000]

; ---------------------------------------------------------------------------
;  Colour attributes (foreground | background<<4)
; ---------------------------------------------------------------------------
ATTR_NORMAL     equ 0x07    ; white on black
ATTR_BRIGHT     equ 0x0F    ; bright white on black
ATTR_HILITE     equ 0x70    ; black on light-grey  (selected row)
ATTR_ACCENT     equ 0x0B    ; cyan on black
ATTR_DIM        equ 0x08    ; dark grey on black
ATTR_WARN       equ 0x0E    ; yellow on black
ATTR_GREEN      equ 0x0A    ; bright green on black

MENU_TIMEOUT    equ 10

; ---------------------------------------------------------------------------
;  Entry
; ---------------------------------------------------------------------------
stage2_start:
    cli
    xor  ax, ax
    mov  ds, ax
    mov  es, ax
    mov  ss, ax
    mov  sp, 0x7C00
    mov  [boot_drive], dl      ; save BIOS boot drive before any modifications
    sti

    ; Mode 3: 80x25 colour text, clear screen
    mov  ah, 0x00
    mov  al, 0x03
    int  0x10

    ; Hide cursor
    mov  ah, 0x01
    mov  cx, 0x2000
    int  0x10

    call draw_screen

    mov  byte [selected],  0
    mov  byte [countdown], MENU_TIMEOUT

.loop:
    call redraw_items
    call redraw_timer

    call wait_tick             ; wait ~1 s or keypress (CF=1 on key)
    jc   .got_key

    dec  byte [countdown]
    cmp  byte [countdown], 0
    je   .run
    jmp  .loop

.got_key:
    mov  ah, 0x00
    int  0x16
    cmp  ah, 0x48              ; up arrow
    je   .up
    cmp  ah, 0x50              ; down arrow
    je   .down
    cmp  al, '1'
    je   .pick0
    cmp  al, '2'
    je   .pick1
    cmp  al, 0x0D              ; Enter
    je   .run
    jmp  .loop

.up:
    mov  byte [selected], 0
    jmp  .loop
.down:
    mov  byte [selected], 1
    jmp  .loop
.pick0:
    mov  byte [selected], 0
    jmp  .run
.pick1:
    mov  byte [selected], 1
    jmp  .run

.run:
    cmp  byte [selected], 1
    je   .do_bios
    jmp  .do_boot

; ---------------------------------------------------------------------------
;  BIOS option - keyboard controller hard reset (pin 0xFE on port 0x64)
;  This triggers a full CPU reset, returning to POST / firmware.
;  Works on real hardware AND QEMU. Far superior to INT 19h.
; ---------------------------------------------------------------------------
.do_bios:
    ; clear screen, show message
    mov  ah, 0x00
    mov  al, 0x03
    int  0x10
    call show_cursor

    mov  si, msg_bios
    call print_color_str

    ; small delay so user can read the message (~2 s)
    mov  cx, 2
.bios_wait:
    call wait_tick
    loop .bios_wait

    ; flush keyboard buffer
    call kbd_flush

    ; Pulse reset line via keyboard controller command 0xFE
    ; This is a hard CPU reset - takes us back to BIOS POST
.kbd_reset:
    in   al, 0x64
    test al, 0x02              ; wait for input buffer empty
    jnz  .kbd_reset
    mov  al, 0xFE              ; pulse output port (reset line)
    out  0x64, al

    ; If KBC reset didn't work (some VMs), fall back to triple fault
    ; by loading a null IDT and triggering an interrupt
    cli
    lidt [null_idtr]
    int  3                     ; triple fault -> full reset

    jmp  $                     ; should never reach here

; ---------------------------------------------------------------------------
;  Boot PD-OS
; ---------------------------------------------------------------------------
.do_boot:
    mov  ah, 0x00
    mov  al, 0x03
    int  0x10
    call show_cursor

    mov  si, msg_boot_start
    call print_color_str

    call enable_a20
    jc   .a20_err

    mov  si, msg_a20_ok
    call print_color_str

    ; ------------------------------------------------------------------
    ; Load kernel using standard CHS INT 13h (same method Stage 1 uses)
    ; Target: 0x1000:0x0000 = physical 0x10000 (safe low memory)
    ; kernel will be copied to 0x100000 in protected mode
    ; Sector 7 = LBA 6.  12 sectors = 6 KB, stays within track 0.
    ; ------------------------------------------------------------------
    mov  si, msg_kernel_load
    call print_color_str

    mov  ax, 0x1000
    mov  es, ax
    xor  bx, bx             ; ES:BX = 0x1000:0x0000 = physical 0x10000
    mov  ah, 0x02           ; BIOS: read sectors
    mov  al, 32             ; 32 sectors = 16 KB
    mov  ch, 0              ; cylinder 0
    mov  cl, 7              ; sector 7 = LBA 6
    mov  dh, 0              ; head 0
    mov  dl, [boot_drive]
    int  0x13
    jc   .kernel_err

    ; Restore ES=0 before protected-mode transition
    xor  ax, ax
    mov  es, ax

    mov  si, msg_kernel_ok
    call print_color_str
    ; ------------------------------------------------------------------

    lgdt [gdt_descriptor]

    mov  si, msg_gdt_ok
    call print_color_str

    cli
    mov  eax, cr0
    or   eax, 1
    mov  cr0, eax
    jmp  0x08:pm_entry

.a20_err:
    mov  si, msg_a20_fail
    call print_color_str
    jmp  hang

.kernel_err:
    mov  si, msg_kernel_fail
    call print_color_str
    jmp  hang

; ===========================================================================
;  32-bit protected mode — copy kernel from 0x10000 to 0x100000, then jump
; ===========================================================================
[BITS 32]
pm_entry:
    mov  ax, 0x10
    mov  ds, ax
    mov  es, ax
    mov  fs, ax
    mov  gs, ax
    mov  ss, ax
    mov  esp, 0x9FC00           ; kernel stack (below BIOS data area)

    ; Copy kernel: 0x10000 -> 0x100000  (32 sectors = 16384 bytes = 4096 dwords)
    mov  esi, 0x10000
    mov  edi, 0x100000
    mov  ecx, 4096
    rep  movsd

    ; Jump to kernel entry point
    mov  eax, 0x100000
    jmp  eax

; ===========================================================================
;  Back to 16-bit
; ===========================================================================
[BITS 16]

; ---------------------------------------------------------------------------
;  enable_a20  -  three methods, CF set on total failure
; ---------------------------------------------------------------------------
enable_a20:
    call .check
    jnz  .ok

    ; Method 1: BIOS INT 15h 2401
    mov  ax, 0x2401
    int  0x15
    call .check
    jnz  .ok

    ; Method 2: Keyboard controller
    call .kbd_method
    call .check
    jnz  .ok

    ; Method 3: Fast A20 port 0x92
    in   al, 0x92
    or   al, 0x02
    and  al, 0xFE
    out  0x92, al
    call .check
    jnz  .ok

    stc
    ret
.ok:
    clc
    ret

.check:
    push es
    push di
    xor  ax, ax
    mov  es, ax
    mov  di, 0x0500
    push word 0xFFFF
    pop  es
    mov  byte [es:0x0510], 0x00
    mov  byte [di],        0xFF
    cmp  byte [es:0x0510], 0xFF   ; if equal A20 is off (aliasing)
    pop  di
    pop  es
    ret

.kbd_method:
    cli
.w1: in al, 0x64
    test al, 0x02
    jnz  .w1
    mov  al, 0xAD
    out  0x64, al
.w2: in al, 0x64
    test al, 0x02
    jnz  .w2
    mov  al, 0xD0
    out  0x64, al
.w3: in al, 0x64
    test al, 0x01
    jz   .w3
    in   al, 0x60
    push ax
.w4: in al, 0x64
    test al, 0x02
    jnz  .w4
    mov  al, 0xD1
    out  0x64, al
.w5: in al, 0x64
    test al, 0x02
    jnz  .w5
    pop  ax
    or   al, 0x02
    out  0x60, al
.w6: in al, 0x64
    test al, 0x02
    jnz  .w6
    mov  al, 0xAE
    out  0x64, al
.w7: in al, 0x64
    test al, 0x02
    jnz  .w7
    sti
    ret

; ---------------------------------------------------------------------------
;  kbd_flush  -  drain the keyboard output buffer
; ---------------------------------------------------------------------------
kbd_flush:
    in   al, 0x64
    test al, 0x01
    jz   .done
    in   al, 0x60
    jmp  kbd_flush
.done:
    ret

; ---------------------------------------------------------------------------
;  wait_tick  -  wait ~1 second OR until a key is in the buffer
;  Returns: CF=1 key waiting (NOT consumed), CF=0 timeout
; ---------------------------------------------------------------------------
wait_tick:
    push ax
    push dx
    push es
    xor  ax, ax
    mov  es, ax
    mov  dx, [es:0x046C]     ; current tick count
    add  dx, 18              ; +18 ticks ~ 1 second

.poll:
    mov  ah, 0x01            ; peek (does not remove key)
    int  0x16
    jnz  .has_key

    mov  ax, [es:0x046C]
    cmp  ax, dx
    jae  .timeout
    jmp  .poll

.has_key:
    stc
    pop  es
    pop  dx
    pop  ax
    ret

.timeout:
    clc
    pop  es
    pop  dx
    pop  ax
    ret

; ---------------------------------------------------------------------------
;  show_cursor / hang
; ---------------------------------------------------------------------------
show_cursor:
    mov  ah, 0x01
    mov  cx, 0x0607
    int  0x10
    ret

hang:
    cli
.h: hlt
    jmp .h

; ===========================================================================
;  VGA direct-write screen drawing
;  We draw everything by poking 0xB8000 in real mode (segment 0xB800).
;  This avoids BIOS teletype quirks completely.
; ===========================================================================

; vga_putchar: write AL (char) with attribute BL at (DH=row, DL=col)
vga_putchar:
    push ax
    push bx
    push di
    push es
    mov  bx, 0xB800
    mov  es, bx
    ; offset = (row*80 + col) * 2
    movzx di, dh
    imul di, di, 80
    movzx bx, dl
    add  di, bx
    shl  di, 1
    mov  ah, [cur_attr]
    mov  [es:di], ax
    pop  es
    pop  di
    pop  bx
    pop  ax
    ret

; vga_str: print NUL-terminated string at SI, attribute [cur_attr],
;          starting at (DH=row, DL=col). Advances DL each char.
vga_str:
    push ax
.vs_loop:
    lodsb
    test al, al
    jz   .vs_done
    call vga_putchar
    inc  dl
    jmp  .vs_loop
.vs_done:
    pop ax
    ret

; vga_fill_row: fill columns DL..CL on row DH with char AL, attr [cur_attr]
vga_fill:
    ; AL=char, DH=row, DL=start_col, CL=end_col
    push ax
    push cx
.vf:
    call vga_putchar
    inc  dl
    cmp  dl, cl
    jle  .vf
    pop  cx
    pop  ax
    ret

; ---------------------------------------------------------------------------
;  draw_hline: draw a horizontal box line
;  DH=row  DL=start  CL=end  AL=left_cap  AH=fill  BL=right_cap  [cur_attr]
; ---------------------------------------------------------------------------
draw_hline:
    push ax
    push si
    push cx
    push dx
    ; left cap
    xchg al, ah              ; AL=fill, AH=left_cap (swap trick)
    xchg al, ah              ; back: AL=left_cap
    call vga_putchar
    inc  dl
    mov  al, ah              ; fill char
    ; fill middle
.dhl:
    cmp  dl, cl
    jge  .dhl_right
    call vga_putchar
    inc  dl
    jmp  .dhl
.dhl_right:
    mov  al, bl              ; right cap
    call vga_putchar
    pop  dx
    pop  cx
    pop  si
    pop  ax
    ret

; ---------------------------------------------------------------------------
;  draw_screen  -  paint the entire TUI
;  Uses VGA direct writes so characters look exactly right.
; ---------------------------------------------------------------------------

; CP437 box drawing chars available in BIOS VGA font:
;  0xC9 = top-left double  0xBB = top-right double
;  0xCC = left-T double    0xB9 = right-T double
;  0xC8 = bot-left double  0xBC = bot-right double
;  0xCD = horiz double line
;  0xBA = vert double line

BOX_TL  equ 0xC9
BOX_TR  equ 0xBB
BOX_BL  equ 0xC8
BOX_BR  equ 0xBC
BOX_H   equ 0xCD
BOX_V   equ 0xBA
BOX_LT  equ 0xCC   ; left side T-junction
BOX_RT  equ 0xB9   ; right side T-junction

draw_screen:
    ; --- setup segment ---
    push es
    mov  ax, 0xB800
    mov  es, ax

    ; ---- top border  row 0 ----
    mov  byte [cur_attr], ATTR_BRIGHT
    mov  dh, 0
    mov  dl, 1
    mov  al, BOX_TL
    call vga_putchar
    inc  dl
    mov  al, BOX_H
    mov  cl, 77
.top_h:
    call vga_putchar
    inc  dl
    cmp  dl, cl
    jl   .top_h
    mov  al, BOX_TR
    call vga_putchar

    ; ---- vertical sides rows 1-21 ----
    mov  dh, 1
.sides:
    mov  dl, 1
    mov  al, BOX_V
    call vga_putchar
    mov  dl, 77
    call vga_putchar
    inc  dh
    cmp  dh, 21
    jle  .sides

    ; ---- bottom border row 22 ----
    mov  dh, 22
    mov  dl, 1
    mov  al, BOX_BL
    call vga_putchar
    mov  dl, 2
    mov  al, BOX_H
    mov  cl, 77
.bot_h:
    call vga_putchar
    inc  dl
    cmp  dl, cl
    jl   .bot_h
    mov  al, BOX_BR
    call vga_putchar

    ; ---- separator below logo, row 8 ----
    ; replace vert sides with T-junctions
    mov  dh, 8
    mov  dl, 1
    mov  al, BOX_LT
    mov  byte [cur_attr], ATTR_DIM
    call vga_putchar
    mov  dl, 2
    mov  al, BOX_H
    mov  cl, 77
.sep_h:
    call vga_putchar
    inc  dl
    cmp  dl, cl
    jl   .sep_h
    mov  al, BOX_RT
    call vga_putchar

    ; ---- logo  rows 2-7 (centred in 76-char wide box) ----
    mov  byte [cur_attr], ATTR_BRIGHT
    mov  dh, 2
    mov  dl, 3
    mov  si, logo_r1
    call vga_str

    mov  dh, 3
    mov  dl, 3
    mov  si, logo_r2
    call vga_str

    mov  dh, 4
    mov  dl, 3
    mov  si, logo_r3
    call vga_str

    mov  dh, 5
    mov  dl, 3
    mov  si, logo_r4
    call vga_str

    mov  dh, 6
    mov  dl, 3
    mov  byte [cur_attr], ATTR_ACCENT
    mov  si, logo_r5
    call vga_str

    mov  dh, 7
    mov  dl, 3
    mov  byte [cur_attr], ATTR_DIM
    mov  si, logo_r6
    call vga_str

    ; ---- "Select a boot option:" row 9 ----
    mov  dh, 9
    mov  dl, 4
    mov  byte [cur_attr], ATTR_WARN
    mov  si, menu_title
    call vga_str

    ; ---- hint rows 19-20 ----
    mov  dh, 19
    mov  dl, 4
    mov  byte [cur_attr], ATTR_DIM
    mov  si, hint1
    call vga_str

    mov  dh, 20
    mov  dl, 4
    mov  si, hint2
    call vga_str

    ; ---- version row 23 ----
    mov  dh, 23
    mov  dl, 2
    mov  byte [cur_attr], ATTR_DIM
    mov  si, ver_str
    call vga_str

    pop  es
    ret

; ---------------------------------------------------------------------------
;  redraw_items  -  repaint the two menu rows only
; ---------------------------------------------------------------------------
redraw_items:
    push es
    mov  ax, 0xB800
    mov  es, ax

    ; item 1 row 12
    mov  dh, 12
    mov  dl, 4
    cmp  byte [selected], 0
    je   .i1_sel
    mov  byte [cur_attr], ATTR_NORMAL
    mov  si, item1_off
    call vga_str
    jmp  .item2
.i1_sel:
    mov  byte [cur_attr], ATTR_HILITE
    mov  si, item1_on
    call vga_str

.item2:
    ; item 2 row 14
    mov  dh, 14
    mov  dl, 4
    cmp  byte [selected], 1
    je   .i2_sel
    mov  byte [cur_attr], ATTR_NORMAL
    mov  si, item2_off
    call vga_str
    jmp  .done
.i2_sel:
    mov  byte [cur_attr], ATTR_HILITE
    mov  si, item2_on
    call vga_str

.done:
    pop  es
    ret

; ---------------------------------------------------------------------------
;  redraw_timer  -  update countdown row 17
; ---------------------------------------------------------------------------
redraw_timer:
    push es
    mov  ax, 0xB800
    mov  es, ax

    mov  dh, 17
    mov  dl, 4
    mov  byte [cur_attr], ATTR_DIM
    mov  si, timer_pre
    call vga_str

    ; single digit
    mov  al, [countdown]
    add  al, '0'
    call vga_putchar
    inc  dl

    mov  si, timer_post
    call vga_str

    pop  es
    ret

; ---------------------------------------------------------------------------
;  print_color_str  -  teletype print of SI, color = [cur_attr] (for post-menu)
; ---------------------------------------------------------------------------
print_color_str:
    pusha
.pcs:
    lodsb
    test al, al
    jz   .pcs_done
    mov  ah, 0x0E
    int  0x10
    jmp  .pcs
.pcs_done:
    popa
    ret

; ===========================================================================
;  GDT
; ===========================================================================
align 4
gdt_start:
    dq 0                       ; null
    ; code: base=0 limit=4G 32-bit ring0 exec+read
    dw 0xFFFF, 0x0000
    db 0x00, 10011010b, 11001111b, 0x00
    ; data: base=0 limit=4G 32-bit ring0 read+write
    dw 0xFFFF, 0x0000
    db 0x00, 10010010b, 11001111b, 0x00
gdt_end:

gdt_descriptor:
    dw gdt_end - gdt_start - 1
    dd gdt_start

; null IDT for triple-fault reset fallback
null_idtr:
    dw 0
    dd 0

; ===========================================================================
;  Data
; ===========================================================================
selected    db 0
countdown   db MENU_TIMEOUT
cur_attr    db ATTR_NORMAL
boot_drive  db 0x80            ; BIOS drive number saved at entry

; Disk Address Packet (DAP) for INT 13h Extended Read (AH=0x42)
align 2
dap:
    db  0x10        ; size of DAP = 16 bytes
    db  0x00        ; reserved
dap_count:
    dw  0           ; number of sectors (filled at runtime)
dap_offset:
    dw  0           ; transfer buffer offset (filled at runtime)
dap_segment:
    dw  0           ; transfer buffer segment (filled at runtime)
dap_lba_lo:
    dd  0           ; LBA low 32 bits (filled at runtime)
dap_lba_hi:
    dd  0           ; LBA high 32 bits (always 0 for us)

; Logo - pure ASCII, no backslash art, fits 74 chars
;        PD-OS spelled with simple block letters
logo_r1  db '     ____  ____        ___  ____', 0
logo_r2  db '    |  _ \|  _ \      / _ \/ ___|', 0
logo_r3  db '    | |_) | | | |    | | | \___ \', 0
logo_r4  db '    |  __/| |_| |    | |_| |___) |', 0
logo_r5  db '    PD-OS Custom Bootloader  --  Stage 2  v0.1', 0
logo_r6  db '    Open Source OS Project  |  github.com/PlayDough1992/pd-os', 0

menu_title   db 'Select a boot option:', 0

item1_off  db '  [ 1 ]   Boot PD-OS                                        ', 0
item1_on   db '  [ 1 ]   Boot PD-OS                                     ***', 0
item2_off  db '  [ 2 ]   Enter BIOS Setup                                   ', 0
item2_on   db '  [ 2 ]   Enter BIOS Setup                                ***', 0

hint1   db 'Use UP / DOWN arrows or press  1  or  2  to select.', 0
hint2   db 'Press ENTER to confirm.', 0

timer_pre   db 'Booting default in  ', 0
timer_post  db '  second(s) ...', 0

ver_str     db 'PD-OS Bootloader v0.1  (c) PlayDough1992', 0

msg_boot_start  db 0x0D, 0x0A
                db ' >> Booting PD-OS...', 0x0D, 0x0A, 0
msg_a20_ok      db ' >> A20 enabled.', 0x0D, 0x0A, 0
msg_a20_fail    db ' [ERROR] A20 enable failed. System halted.', 0x0D, 0x0A, 0
msg_gdt_ok      db ' >> GDT loaded. Entering protected mode...', 0x0D, 0x0A, 0
msg_kernel_load db ' >> Loading kernel...', 0x0D, 0x0A, 0
msg_kernel_ok   db ' >> Kernel loaded at 0x100000.', 0x0D, 0x0A, 0
msg_kernel_fail db ' [ERROR] Failed to load kernel from disk!', 0x0D, 0x0A, 0
msg_bios        db 0x0D, 0x0A
                db ' >> Resetting system to BIOS/firmware...', 0x0D, 0x0A
                db ' >> Please press your BIOS key (Del / F2 / F12) when POST starts.', 0x0D, 0x0A, 0
