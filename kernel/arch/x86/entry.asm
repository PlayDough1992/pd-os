; ============================================================================
;  PD-Kernel  -  x86 32-bit Entry Point
;  Loaded by Stage 2 at physical 0x100000
;  Called from protected mode with segments already set up (CS=0x08, DS=0x10)
; ============================================================================

[BITS 32]

; Mark stack as non-executable (suppresses GNU-stack linker warning)
section .note.GNU-stack noalloc noexec nowrite progbits

section .text

[EXTERN kernel_main]
[GLOBAL _start]

_start:
    ; Set up kernel stack (just below BIOS data area at 0x9FC00)
    mov  esp, 0x9FC00

    ; Clear stack frame linkage
    xor  ebp, ebp

    ; Call the C kernel
    call kernel_main

    ; Should never return — hang if it does
.hang:
    cli
    hlt
    jmp  .hang
