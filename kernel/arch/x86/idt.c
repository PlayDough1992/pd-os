/* ============================================================================
 * PD-Kernel  —  IDT setup
 * ============================================================================ */

#include "idt.h"
#include "kernel.h"
#include "pic.h"
#include "io.h"

/* Forward declarations for all ISR/IRQ stubs (defined in idt.asm) */
extern void isr0(void);  extern void isr1(void);  extern void isr2(void);
extern void isr3(void);  extern void isr4(void);  extern void isr5(void);
extern void isr6(void);  extern void isr7(void);  extern void isr8(void);
extern void isr9(void);  extern void isr10(void); extern void isr11(void);
extern void isr12(void); extern void isr13(void); extern void isr14(void);
extern void isr15(void); extern void isr16(void); extern void isr17(void);
extern void isr18(void); extern void isr19(void); extern void isr20(void);
extern void isr21(void); extern void isr22(void); extern void isr23(void);
extern void isr24(void); extern void isr25(void); extern void isr26(void);
extern void isr27(void); extern void isr28(void); extern void isr29(void);
extern void isr30(void); extern void isr31(void);

extern void irq0_preempt(void);  /* dedicated scheduler IRQ0 stub */
extern void irq0(void);  extern void irq1(void);  extern void irq2(void);
extern void irq3(void);  extern void irq4(void);  extern void irq5(void);
extern void irq6(void);  extern void irq7(void);  extern void irq8(void);
extern void irq9(void);  extern void irq10(void); extern void irq11(void);
extern void irq12(void); extern void irq13(void); extern void irq14(void);
extern void irq15(void);

static idt_entry_t idt[256];
static idt_ptr_t   idt_ptr;

void idt_set_gate(uint8_t num, uint32_t base, uint16_t sel, uint8_t flags)
{
    idt[num].base_lo  = (uint16_t)(base & 0xFFFF);
    idt[num].base_hi  = (uint16_t)((base >> 16) & 0xFFFF);
    idt[num].selector = sel;
    idt[num].zero     = 0;
    idt[num].flags    = flags;
}

void idt_init(void)
{
    idt_ptr.limit = sizeof(idt) - 1;
    idt_ptr.base  = (uint32_t)&idt;

    /* Zero the whole table */
    uint32_t i;
    for (i = 0; i < 256; i++)
        idt_set_gate((uint8_t)i, 0, 0, 0);

    /* CPU exceptions — INT 0-31 */
    /* 0x8E = present, ring 0, 32-bit interrupt gate */
    idt_set_gate( 0, (uint32_t)isr0,  0x08, 0x8E);
    idt_set_gate( 1, (uint32_t)isr1,  0x08, 0x8E);
    idt_set_gate( 2, (uint32_t)isr2,  0x08, 0x8E);
    idt_set_gate( 3, (uint32_t)isr3,  0x08, 0x8E);
    idt_set_gate( 4, (uint32_t)isr4,  0x08, 0x8E);
    idt_set_gate( 5, (uint32_t)isr5,  0x08, 0x8E);
    idt_set_gate( 6, (uint32_t)isr6,  0x08, 0x8E);
    idt_set_gate( 7, (uint32_t)isr7,  0x08, 0x8E);
    idt_set_gate( 8, (uint32_t)isr8,  0x08, 0x8E);
    idt_set_gate( 9, (uint32_t)isr9,  0x08, 0x8E);
    idt_set_gate(10, (uint32_t)isr10, 0x08, 0x8E);
    idt_set_gate(11, (uint32_t)isr11, 0x08, 0x8E);
    idt_set_gate(12, (uint32_t)isr12, 0x08, 0x8E);
    idt_set_gate(13, (uint32_t)isr13, 0x08, 0x8E);
    idt_set_gate(14, (uint32_t)isr14, 0x08, 0x8E);
    idt_set_gate(15, (uint32_t)isr15, 0x08, 0x8E);
    idt_set_gate(16, (uint32_t)isr16, 0x08, 0x8E);
    idt_set_gate(17, (uint32_t)isr17, 0x08, 0x8E);
    idt_set_gate(18, (uint32_t)isr18, 0x08, 0x8E);
    idt_set_gate(19, (uint32_t)isr19, 0x08, 0x8E);
    idt_set_gate(20, (uint32_t)isr20, 0x08, 0x8E);
    idt_set_gate(21, (uint32_t)isr21, 0x08, 0x8E);
    idt_set_gate(22, (uint32_t)isr22, 0x08, 0x8E);
    idt_set_gate(23, (uint32_t)isr23, 0x08, 0x8E);
    idt_set_gate(24, (uint32_t)isr24, 0x08, 0x8E);
    idt_set_gate(25, (uint32_t)isr25, 0x08, 0x8E);
    idt_set_gate(26, (uint32_t)isr26, 0x08, 0x8E);
    idt_set_gate(27, (uint32_t)isr27, 0x08, 0x8E);
    idt_set_gate(28, (uint32_t)isr28, 0x08, 0x8E);
    idt_set_gate(29, (uint32_t)isr29, 0x08, 0x8E);
    idt_set_gate(30, (uint32_t)isr30, 0x08, 0x8E);
    idt_set_gate(31, (uint32_t)isr31, 0x08, 0x8E);

    /* Hardware IRQs — INT 0x20-0x2F */
    /* IRQ0 uses a dedicated stub that handles context switching directly */
    idt_set_gate(0x20, (uint32_t)irq0_preempt, 0x08, 0x8E);
    idt_set_gate(0x21, (uint32_t)irq1,  0x08, 0x8E);
    idt_set_gate(0x22, (uint32_t)irq2,  0x08, 0x8E);
    idt_set_gate(0x23, (uint32_t)irq3,  0x08, 0x8E);
    idt_set_gate(0x24, (uint32_t)irq4,  0x08, 0x8E);
    idt_set_gate(0x25, (uint32_t)irq5,  0x08, 0x8E);
    idt_set_gate(0x26, (uint32_t)irq6,  0x08, 0x8E);
    idt_set_gate(0x27, (uint32_t)irq7,  0x08, 0x8E);
    idt_set_gate(0x28, (uint32_t)irq8,  0x08, 0x8E);
    idt_set_gate(0x29, (uint32_t)irq9,  0x08, 0x8E);
    idt_set_gate(0x2A, (uint32_t)irq10, 0x08, 0x8E);
    idt_set_gate(0x2B, (uint32_t)irq11, 0x08, 0x8E);
    idt_set_gate(0x2C, (uint32_t)irq12, 0x08, 0x8E);
    idt_set_gate(0x2D, (uint32_t)irq13, 0x08, 0x8E);
    idt_set_gate(0x2E, (uint32_t)irq14, 0x08, 0x8E);
    idt_set_gate(0x2F, (uint32_t)irq15, 0x08, 0x8E);

    __asm__ volatile ("lidt %0" : : "m"(idt_ptr));
}
