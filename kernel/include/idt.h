#pragma once

/* ============================================================================
 * PD-Kernel  —  IDT (Interrupt Descriptor Table)
 * ============================================================================ */

#include "kernel.h"

/* An IDT gate descriptor (8 bytes) */
typedef struct __attribute__((packed)) {
    uint16_t base_lo;       /* handler address bits 0-15  */
    uint16_t selector;      /* kernel code segment selector */
    uint8_t  zero;          /* always 0 */
    uint8_t  flags;         /* gate type + DPL + present bit */
    uint16_t base_hi;       /* handler address bits 16-31 */
} idt_entry_t;

/* IDTR register value */
typedef struct __attribute__((packed)) {
    uint16_t limit;
    uint32_t base;
} idt_ptr_t;

/* Interrupt frame pushed by CPU + our stubs */
typedef struct __attribute__((packed)) {
    uint32_t ds;
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax; /* pusha */
    uint32_t int_no, err_code;
    uint32_t eip, cs, eflags, useresp, ss;            /* pushed by CPU */
} interrupt_frame_t;

void idt_init(void);
void idt_set_gate(uint8_t num, uint32_t base, uint16_t sel, uint8_t flags);
