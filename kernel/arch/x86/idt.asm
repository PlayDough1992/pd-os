; ============================================================================
;  PD-Kernel  —  IDT interrupt stubs (256 entries)
;
;  Each stub:
;    - Pushes a dummy error code (0) or not (CPU already pushes for some)
;    - Pushes the interrupt number
;    - Jumps to the common handler
;
;  The common handler:
;    - Saves all GP registers (pusha)
;    - Saves DS
;    - Loads kernel data segment
;    - Calls C interrupt_dispatch(interrupt_frame_t *)
;    - Restores and returns
; ============================================================================

[BITS 32]

section .note.GNU-stack noalloc noexec nowrite progbits

section .text

[EXTERN interrupt_dispatch]

; ---------------------------------------------------------------------------
;  Macro: stub with no CPU error code (CPU does not push one)
; ---------------------------------------------------------------------------
%macro ISR_NOERR 1
[GLOBAL isr%1]
isr%1:
    push dword 0          ; dummy error code
    push dword %1         ; interrupt number
    jmp  isr_common
%endmacro

; ---------------------------------------------------------------------------
;  Macro: stub where CPU already pushes an error code
; ---------------------------------------------------------------------------
%macro ISR_ERR 1
[GLOBAL isr%1]
isr%1:
    push dword %1         ; interrupt number (error code already on stack)
    jmp  isr_common
%endmacro

; ---------------------------------------------------------------------------
;  Macro: IRQ stub (hardware interrupts, no error code)
; ---------------------------------------------------------------------------
%macro IRQ 2
[GLOBAL irq%1]
irq%1:
    push dword 0          ; dummy error code
    push dword %2         ; interrupt number (IRQ offset into IDT)
    jmp  isr_common
%endmacro

; ---------------------------------------------------------------------------
;  CPU Exception stubs  (INT 0 – 31)
;  See Intel SDM Vol.3 Table 6-1 for which ones push an error code
; ---------------------------------------------------------------------------
ISR_NOERR  0    ; Divide by zero
ISR_NOERR  1    ; Debug
ISR_NOERR  2    ; NMI
ISR_NOERR  3    ; Breakpoint
ISR_NOERR  4    ; Overflow
ISR_NOERR  5    ; Bound range exceeded
ISR_NOERR  6    ; Invalid opcode
ISR_NOERR  7    ; Device not available
ISR_ERR    8    ; Double fault (error code = 0 always)
ISR_NOERR  9    ; Coprocessor segment overrun (legacy)
ISR_ERR   10    ; Invalid TSS
ISR_ERR   11    ; Segment not present
ISR_ERR   12    ; Stack-segment fault
ISR_ERR   13    ; General protection fault
ISR_ERR   14    ; Page fault
ISR_NOERR 15    ; Reserved
ISR_NOERR 16    ; x87 FPU error
ISR_ERR   17    ; Alignment check
ISR_NOERR 18    ; Machine check
ISR_NOERR 19    ; SIMD FP exception
ISR_NOERR 20    ; Virtualisation exception
ISR_ERR   21    ; Control protection exception
ISR_NOERR 22    ; Reserved
ISR_NOERR 23    ; Reserved
ISR_NOERR 24    ; Reserved
ISR_NOERR 25    ; Reserved
ISR_NOERR 26    ; Reserved
ISR_NOERR 27    ; Reserved
ISR_NOERR 28    ; Hypervisor injection exception
ISR_ERR   29    ; VMM communication exception
ISR_ERR   30    ; Security exception
ISR_NOERR 31    ; Reserved

; ---------------------------------------------------------------------------
;  Hardware IRQs (remapped to INT 0x20 – 0x2F by PIC)
; ---------------------------------------------------------------------------
IRQ  0, 0x20    ; PIT  timer
IRQ  1, 0x21    ; PS/2 keyboard
IRQ  2, 0x22    ; cascade (PIC2)
IRQ  3, 0x23    ; COM2
IRQ  4, 0x24    ; COM1
IRQ  5, 0x25    ; LPT2 / sound
IRQ  6, 0x26    ; floppy
IRQ  7, 0x27    ; LPT1 / spurious
IRQ  8, 0x28    ; CMOS RTC
IRQ  9, 0x29    ; free
IRQ 10, 0x2A    ; free
IRQ 11, 0x2B    ; free
IRQ 12, 0x2C    ; PS/2 mouse
IRQ 13, 0x2D    ; FPU
IRQ 14, 0x2E    ; primary ATA
IRQ 15, 0x2F    ; secondary ATA

; ---------------------------------------------------------------------------
;  Common handler: save state, call C, restore state, return
; ---------------------------------------------------------------------------
isr_common:
    pusha                   ; push eax,ecx,edx,ebx,esp,ebp,esi,edi

    mov  ax, ds
    push eax                ; save DS

    mov  ax, 0x10           ; kernel data segment
    mov  ds, ax
    mov  es, ax
    mov  fs, ax
    mov  gs, ax

    push esp                ; interrupt_frame_t *
    call interrupt_dispatch
    add  esp, 4

    pop  eax                ; restore DS
    mov  ds, ax
    mov  es, ax
    mov  fs, ax
    mov  gs, ax

    popa
    add  esp, 8             ; pop int_no and err_code
    iret
