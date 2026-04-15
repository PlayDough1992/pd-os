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

; E820 memory map output addresses (must match e820.h in the kernel)
E820_COUNT   equ 0x5000      ; dword: number of valid entries
E820_ENTRIES equ 0x5004      ; 24-byte entries (base8 + len8 + type4 + acpi4)
E820_MAX     equ 32

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
    ; ------------------------------------------------------------------
    ; Load kernel using INT 13h Extended Read (AH=42h, LBA mode)
    ; 5 reads of 128 sectors each = 640 sectors = 320 KB max
    ;   Chunk 1: LBA 6   -> 0x1000:0x0000 (physical 0x10000)
    ;   Chunk 2: LBA 134 -> 0x2000:0x0000 (physical 0x20000)
    ;   Chunk 3: LBA 262 -> 0x3000:0x0000 (physical 0x30000)
    ;   Chunk 4: LBA 390 -> 0x4000:0x0000 (physical 0x40000)
    ;   Chunk 5: LBA 518 -> 0x5000:0x0000 (physical 0x50000)
    ; PDFS starts at LBA 1024 (well past last chunk at LBA 645)
    ; ------------------------------------------------------------------
    mov  si, msg_kernel_load
    call print_color_str

    ; --- Chunk 1: 128 sectors at LBA 7 ---
    mov  word  [dap_count],   128    ; 128 sectors = 64 KB (fixed for all chunks)
    mov  word  [dap_offset],  0x0000 ; offset always 0
    mov  word  [dap_segment], 0x1000
    mov  dword [dap_lba_lo],  7
    mov  dword [dap_lba_hi],  0
    mov  ah, 0x42
    mov  dl, [boot_drive]
    mov  si, dap
    int  0x13
    jc   .kernel_err

    ; --- Chunk 2: 128 sectors at LBA 135 ---
    mov  word  [dap_segment], 0x2000
    mov  dword [dap_lba_lo],  135
    mov  ah, 0x42
    mov  dl, [boot_drive]
    mov  si, dap
    int  0x13
    jc   .kernel_err

    ; --- Chunk 3: 128 sectors at LBA 263 ---
    mov  word  [dap_segment], 0x3000
    mov  dword [dap_lba_lo],  263
    mov  ah, 0x42
    mov  dl, [boot_drive]
    mov  si, dap
    int  0x13
    jc   .kernel_err

    ; --- Chunk 4: 128 sectors at LBA 391 ---
    mov  word  [dap_segment], 0x4000
    mov  dword [dap_lba_lo],  391
    mov  ah, 0x42
    mov  dl, [boot_drive]
    mov  si, dap
    int  0x13
    jc   .kernel_err

    ; --- Chunk 5: 128 sectors at LBA 519 ---
    mov  word  [dap_segment], 0x5000
    mov  dword [dap_lba_lo],  519
    mov  ah, 0x42
    mov  dl, [boot_drive]
    mov  si, dap
    int  0x13
    jc   .kernel_err

    mov  si, msg_kernel_ok
    call print_color_str

    ; -----------------------------------------------------------------
    ; Probe BIOS memory map (E820) before entering protected mode.
    ; Results are written to E820_COUNT/E820_ENTRIES and read by the
    ; kernel from those physical addresses after the switch.
    ; -----------------------------------------------------------------
    call do_e820
    cmp  dword [E820_COUNT], 0
    je   .e820_warn
    mov  si, msg_e820_ok
    call print_color_str
    jmp  .e820_cont
.e820_warn:
    mov  si, msg_e820_warn
    call print_color_str
.e820_cont:

    ; ---- Copy VGA BIOS 8x16 font to 0x3000 (before switching video mode) ---
    mov  ax, 0x1130
    mov  bh, 0x06           ; 8x16 font pointer -> ES:BP
    int  0x10
    push es
    push ds
    push si
    push di
    push cx
    push ax
    mov  ax, es             ; source segment = ES (from INT 10h result)
    mov  ds, ax
    mov  si, bp             ; source offset  = BP
    xor  ax, ax
    mov  es, ax             ; destination segment = 0
    mov  di, 0x3000         ; destination = physical 0x3000
    mov  cx, 2048           ; 256 chars * 8 bytes per row (we'll treat as 8x8 pairs)
                            ; actually copy 4096 bytes (256 * 16) for full 8x16 font
    mov  cx, 4096
    rep  movsb
    pop  ax
    pop  cx
    pop  di
    pop  si
    pop  ds
    pop  es

    ; ---- Set VBE graphics mode 0x119 (1024x768x32bpp LFB) -----------------
    call do_vbe

    ; Load GDT from the safe copy at 0x0618 (written by do_vbe after all BIOS calls).
    ; The original gdt_descriptor in stage2 at 0x8690 may have been corrupted by
    ; the VESA BIOS, which is known to overwrite memory in the 0x8000 range.
    ; IMPORTANT: no BIOS calls (INT instructions) must occur between lgdt and
    ; the far jump into protected mode — BIOS handlers corrupt low memory.
    lgdt [0x0618]

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
;  32-bit protected mode — copy kernel from staging buffers to above 1 MB
;  0x10000..0x50000 (5 x 64 KB) -> 0x100000..0x150000, then jump
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

    ; Copy 5 x 128-sector chunks (65536 bytes = 16384 dwords each)
    mov  esi, 0x10000
    mov  edi, 0x100000
    mov  ecx, 16384
    rep  movsd

    mov  esi, 0x20000
    mov  edi, 0x110000
    mov  ecx, 16384
    rep  movsd

    mov  esi, 0x30000
    mov  edi, 0x120000
    mov  ecx, 16384
    rep  movsd

    mov  esi, 0x40000
    mov  edi, 0x130000
    mov  ecx, 16384
    rep  movsd

    mov  esi, 0x50000
    mov  edi, 0x140000
    mov  ecx, 16384
    rep  movsd

    ; Jump to kernel entry point
    mov  eax, 0x100000
    jmp  eax

; ===========================================================================
;  Back to 16-bit
; ===========================================================================
[BITS 16]

; ---------------------------------------------------------------------------
;  do_e820  -  build physical memory map via BIOS INT 15h/E820
;
;  Writes to flat memory (ES=0):
;    [E820_COUNT]   = uint32: number of valid entries stored
;    [E820_ENTRIES] = array of 24-byte entries:
;                     base(8) + length(8) + type(4) + acpi_ext(4)
;
;  ACPI 3.0 extended field is pre-set to 1 ("entry valid") so
;  entries are usable even when the BIOS returns only 20 bytes.
;
;  Clobbers nothing (full pushad/pop es around entire routine).
; ---------------------------------------------------------------------------
do_e820:
    pushad
    push es

    xor  ax, ax
    mov  es, ax               ; ES = segment 0 (flat low memory)

    mov  dword [E820_COUNT], 0
    xor  ebx, ebx             ; continuation = 0 on first call
    mov  di,  E820_ENTRIES    ; ES:DI = 0x0000:0x5004
    xor  bp,  bp              ; entry counter (16-bit, max E820_MAX)

.e820_loop:
    ; Pre-set ACPI 3.0 extended attribute byte to 1 ("entry is valid").
    ; If the BIOS returns only 20 bytes this dword is not overwritten
    ; and stays 1, which is the correct default.
    mov  dword [es:di+20], 1

    mov  eax, 0xE820          ; function code
    mov  ecx, 24              ; ask for 24-byte (ACPI 3.0) entries
    mov  edx, 0x534D4150      ; 'SMAP' signature required by BIOS
    int  0x15

    jc   .e820_done           ; CF=1: list exhausted or unsupported
    cmp  eax, 0x534D4150      ; BIOS must echo 'SMAP' back in EAX
    jne  .e820_done
    cmp  ecx, 20              ; must have filled at least 20 bytes
    jl   .e820_skip

    ; Skip zero-length regions (invalid / firmware artefacts)
    mov  eax, dword [es:di+8]   ; length_low
    or   eax, dword [es:di+12]  ; length_high
    jz   .e820_skip

    inc  bp                   ; count this entry
    add  di,  24              ; advance write pointer
    cmp  bp,  E820_MAX        ; safety cap
    jae  .e820_done

.e820_skip:
    test ebx, ebx             ; EBX=0 means this was the final entry
    jnz  .e820_loop

.e820_done:
    ; Store count as dword (zero-extend from bp)
    xor  eax, eax
    mov  ax,  bp
    mov  dword [E820_COUNT], eax

    pop  es
    popad
    ret

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

; ---------------------------------------------------------------------------
;  do_vbe  -  set VBE 1024×768×32bpp LFB mode, query real pitch/fbaddr,
;             write boot_info to 0x4000, then write GDT to 0x0600.
;
;  Memory layout used:
;    0x3000  VGA BIOS 8×16 font  (4096 bytes, copied before call)
;    0x4000  boot_info_t         (64 bytes, written here)
;    0x7000  VBE ModeInfoBlock   (256 bytes, mode query scratch)
;    0x7200  VBE ControllerInfo  (512 bytes, mode-list scan scratch)
;
;  Strategy:
;    Phase 1 — scan BIOS VBE mode list (INT 10h 4F00h / 4F01h / 4F02h)
;              Finds the correct 1024×768×32bpp LFB mode on any real
;              hardware by matching XRes/YRes/BPP/LFB in the mode table.
;    Phase 2 — Bochs VBE I/O-port fallback (QEMU only).
;              Sets mode via ports 0x01CE/0x01CF, then queries the now-
;              live mode via INT 10h 4F03h → 4F01h to get PhysBasePtr.
;    Phase 3 — hard-coded fallback (0xFD000000 / pitch=4096).
; ---------------------------------------------------------------------------
do_vbe:
    pushad
    push es
    push ds

    xor  ax, ax
    mov  es, ax
    mov  ds, ax

    ; Zero boot_info block at 0x4000 (64 bytes = 32 words)
    mov  di, 0x4000
    mov  cx, 32
    rep  stosw

    ; Mark font present (VGA BIOS 8×16 font copied to 0x3000 before this call)
    mov  byte [es:0x4011], 1

    ; ==================================================================
    ; Phase 1 — INT 10h VBE mode scan (real hardware + QEMU with BIOS)
    ;
    ; Write "VBE2" into the info block so the BIOS fills VBE 2.0 fields
    ; (including the full VideoModePtr and per-mode LFB capability bit).
    ; ==================================================================
    mov  dword [es:0x7200], 0x32454256   ; "VBE2"

    mov  ax, 0x4F00         ; Get VBE Controller Info
    mov  di, 0x7200         ; ES:DI = 0:0x7200 → ControllerInfoBlock
    int  0x10

    xor  bx, bx
    mov  es, bx
    mov  ds, bx

    cmp  ax, 0x004F
    jne  .phase2            ; VBE 2.0 not available → Bochs ports

    ; VideoModePtr (a real-mode far pointer) is at ControllerInfoBlock + 0x0E.
    ; Physical address = segment × 16 + offset.
    movzx esi, word [0x720E]            ; mode list offset
    movzx eax, word [0x7210]            ; mode list segment
    shl   eax, 4
    add   esi, eax                      ; esi = flat address of mode list

.scan_next:
    movzx ecx, word [esi]               ; next mode number (0xFFFF = end)
    cmp   cx, 0xFFFF
    je    .phase2                       ; end of list, no match
    add   esi, 2

    ; Query mode info — INT 10h may clobber ESI/ECX; save them.
    push  esi
    push  ecx

    xor  bx, bx
    mov  es, bx
    mov  ax, 0x4F01         ; cx = mode number (already set)
    mov  di, 0x7000         ; ModeInfoBlock at 0x7000
    int  0x10

    xor  bx, bx
    mov  es, bx
    mov  ds, bx

    pop  ecx                ; restore mode number
    pop  esi                ; restore list pointer

    cmp  ax, 0x004F
    jne  .scan_next

    ; Require: XResolution == 1024 (offset 0x12)
    cmp  word [0x7012], 1024
    jne  .scan_next

    ; Require: YResolution == 768 (offset 0x14)
    cmp  word [0x7014], 768
    jne  .scan_next

    ; Require: BitsPerPixel == 32 (offset 0x19)
    cmp  byte [0x7019], 32
    jne  .scan_next

    ; Require: ModeAttributes bit 7 set = LFB supported (offset 0x00)
    test word [0x7000], 0x0080
    jz   .scan_next

    ; ---- Match found.  Set the mode (ECX = mode number). ----
    push  ecx
    mov  ax, 0x4F02
    mov  bx, cx
    or   bx, 0x4000         ; set LFB enable flag
    int  0x10

    xor  bx, bx
    mov  es, bx
    mov  ds, bx

    pop  ecx
    cmp  ax, 0x004F
    jne  .scan_next         ; set failed → try next candidate mode

    ; Re-query mode info after mode is live (PhysBasePtr finalised).
    push  ecx
    mov  ax, 0x4F01
    mov  di, 0x7000
    ; cx = mode number (ecx low half)
    int  0x10

    xor  bx, bx
    mov  es, bx
    mov  ds, bx

    pop  ecx
    cmp  ax, 0x004F
    jne  .phase3

    mov  eax, dword [0x7028]  ; PhysBasePtr
    test eax, eax
    jnz  .got_pitch
    jmp  .phase3

    ; ==================================================================
    ; Phase 2 — Bochs VBE I/O-port fallback (QEMU / Bochs emulator)
    ;
    ; Programs the Bochs VBE device registers directly.  After enabling,
    ; INT 10h 4F03h returns the now-active mode number so we can query
    ; PhysBasePtr via INT 10h 4F01h without hardcoding an address.
    ; ==================================================================
.phase2:
    ; Disable VBE before reconfiguring
    mov  dx, 0x01CE
    mov  ax, 0x0004         ; INDEX_ENABLE
    out  dx, ax
    mov  dx, 0x01CF
    xor  ax, ax
    out  dx, ax

    ; Set XRES = 1024
    mov  dx, 0x01CE
    mov  ax, 0x0001
    out  dx, ax
    mov  dx, 0x01CF
    mov  ax, 1024
    out  dx, ax

    ; Set YRES = 768
    mov  dx, 0x01CE
    mov  ax, 0x0002
    out  dx, ax
    mov  dx, 0x01CF
    mov  ax, 768
    out  dx, ax

    ; Set BPP = 32
    mov  dx, 0x01CE
    mov  ax, 0x0003
    out  dx, ax
    mov  dx, 0x01CF
    mov  ax, 32
    out  dx, ax

    ; Enable with LFB: ENABLED(0x01) | LFB_ENABLED(0x40)
    mov  dx, 0x01CE
    mov  ax, 0x0004
    out  dx, ax
    mov  dx, 0x01CF
    mov  ax, 0x0041
    out  dx, ax

    ; Get current VBE mode number: INT 10h AX=4F03h → BX = mode
    mov  ax, 0x4F03
    int  0x10

    xor  dx, dx
    mov  es, dx
    mov  ds, dx

    cmp  ax, 0x004F
    jne  .phase3

    and  bx, 0x01FF         ; strip LFB / no-clear bits
    jz   .phase3            ; mode = 0 means not set

    ; Query live mode info → PhysBasePtr + pitch
    mov  cx, bx
    mov  ax, 0x4F01
    mov  di, 0x7000
    int  0x10

    xor  bx, bx
    mov  es, bx
    mov  ds, bx

    cmp  ax, 0x004F
    jne  .phase3

    mov  eax, dword [0x7028]  ; PhysBasePtr
    test eax, eax
    jz   .phase3

.got_pitch:
    movzx ecx, word [0x7010]  ; BytesPerScanLine
    test  ecx, ecx
    jz    .fix_pitch
    cmp   ecx, 65536          ; reject implausibly large values
    ja    .fix_pitch
    jmp   .have_fb

.fix_pitch:
    mov  ecx, 4096            ; default: 1024 pixels × 4 bytes

    ; ==================================================================
    ; Phase 3 — hard-coded fallback
    ; ==================================================================
.phase3:
    mov  eax, 0xFD000000      ; QEMU stdvga LFB default
    mov  ecx, 4096

    ; ==================================================================
    ; Write boot_info at physical 0x4000
    ; eax = framebuffer physical base address
    ; ecx = bytes per scan line (pitch)
    ; ==================================================================
.have_fb:
    xor  bx, bx
    mov  es, bx
    mov  dword [es:0x4000], 0xB0075E00   ; magic
    mov  dword [es:0x4004], eax          ; vbe_fb
    mov  word  [es:0x4008], 1024         ; vbe_width
    mov  word  [es:0x400A], 768          ; vbe_height
    mov  word  [es:0x400C], 32           ; vbe_bpp
    mov  word  [es:0x400E], cx           ; vbe_pitch
    mov  byte  [es:0x4010], 1            ; vbe_ok

    ; ==================================================================
    ; Write GDT to 0x0600 and gdtr to 0x0618 — must happen AFTER every
    ; BIOS call because the VESA BIOS corrupts the 0x8000 region where
    ; the GDT originally lives.  Written as immediates so there is no
    ; source buffer that can be corrupted.
    ; Layout:  null (0x00) | code 0x08 | data 0x10
    ; gdtr:    limit=23, base=0x00000600
    ; ==================================================================
    mov  dword [es:0x0600], 0x00000000   ; null descriptor (low)
    mov  dword [es:0x0604], 0x00000000   ;                 (high)
    mov  dword [es:0x0608], 0x0000FFFF   ; code: limit low
    mov  dword [es:0x060C], 0x00CF9A00   ; code: base/flags
    mov  dword [es:0x0610], 0x0000FFFF   ; data: limit low
    mov  dword [es:0x0614], 0x00CF9200   ; data: base/flags
    mov  word  [es:0x0618], 0x0017       ; gdtr limit = 23
    mov  dword [es:0x061A], 0x00000600   ; gdtr base

    pop  ds
    pop  es
    popad
    ret

; vbe_ok stays 0 only if stage2 never reaches the boot_info write above
; (both the INT 10h and Bochs-port paths are tried in sequence).


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

msg_e820_ok     db ' >> Memory map built.', 0x0D, 0x0A, 0
msg_e820_warn   db ' >> [WARN] E820 not supported; memory map empty.', 0x0D, 0x0A, 0
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
