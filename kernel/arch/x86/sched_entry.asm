; ============================================================================
;  PD-Kernel  —  Dedicated IRQ0 (PIT timer) entry for preemptive scheduling
;
;  This stub bypasses isr_common entirely and handles context switching
;  directly.  Stack layout on entry (from irq0_preempt):
;
;    (cpu saves nothing — no ring change)
;    push 0        ; dummy err_code
;    push 0x20     ; int_no
;    pusha         ; eax,ecx,edx,ebx,esp,ebp,esi,edi
;    push ds       ; saved DS   <- saved_esp points HERE
;
;  sched_irq(current_esp) is called with eax = address of that DS slot.
;  It returns the ESP to switch to (same if no switch, different if preempt).
;  After mov esp, eax the restoration sequence (pop ds; popa; add esp,8;
;  iret) runs on whichever task's saved frame is now on the stack.
; ============================================================================

[BITS 32]

section .note.GNU-stack noalloc noexec nowrite progbits

section .text

[EXTERN sched_irq]
[GLOBAL irq0_preempt]

irq0_preempt:
    push dword 0        ; dummy error code
    push dword 0x20     ; interrupt number

    pusha               ; save eax,ecx,edx,ebx,esp,ebp,esi,edi

    mov  ax, ds
    push eax            ; save DS  <- saved_esp = ESP right here

    mov  ax, 0x10       ; kernel data segment
    mov  ds, ax
    mov  es, ax
    mov  fs, ax
    mov  gs, ax

    push esp            ; arg: current_esp (address of saved DS slot)
    call sched_irq      ; C  : ACK PIC, tick, maybe pick new task
    add  esp, 4         ; pop arg

    mov  esp, eax       ; switch to returned ESP (same or new task's frame)

    pop  eax            ; restore DS
    mov  ds, ax
    mov  es, ax
    mov  fs, ax
    mov  gs, ax

    popa                ; restore GPRs (edi,esi,ebp,skip-esp,ebx,edx,ecx,eax)
    add  esp, 8         ; skip int_no + err_code
    iret
