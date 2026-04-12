#pragma once

/* ============================================================================
 * PD-Kernel  —  PIC (8259A Programmable Interrupt Controller)
 * ============================================================================ */

#include "kernel.h"

/* PIC I/O ports */
#define PIC1_CMD    0x20
#define PIC1_DATA   0x21
#define PIC2_CMD    0xA0
#define PIC2_DATA   0xA1

/* IRQ base vectors after remapping */
#define PIC1_OFFSET 0x20    /* IRQ0-7  -> INT 0x20-0x27 */
#define PIC2_OFFSET 0x28    /* IRQ8-15 -> INT 0x28-0x2F */

/* IRQ numbers (before offset) */
#define IRQ_TIMER    0
#define IRQ_KEYBOARD 1

void pic_init(void);
void pic_send_eoi(uint8_t irq);
void pic_mask_irq(uint8_t irq);
void pic_unmask_irq(uint8_t irq);
