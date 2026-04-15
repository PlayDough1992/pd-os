/* ============================================================================
 * PD-Kernel  —  CPU exception handlers + interrupt dispatch
 * ============================================================================ */

#include "idt.h"
#include "pic.h"
#include "pit.h"
#include "keyboard.h"
#include "mouse.h"
#include "kernel.h"
#include "io.h"

static const char *exception_names[] = {
    "Divide By Zero",           /*  0 */
    "Debug",                    /*  1 */
    "Non-Maskable Interrupt",   /*  2 */
    "Breakpoint",               /*  3 */
    "Overflow",                 /*  4 */
    "Bound Range Exceeded",     /*  5 */
    "Invalid Opcode",           /*  6 */
    "Device Not Available",     /*  7 */
    "Double Fault",             /*  8 */
    "Coprocessor Segment Overrun", /* 9 */
    "Invalid TSS",              /* 10 */
    "Segment Not Present",      /* 11 */
    "Stack-Segment Fault",      /* 12 */
    "General Protection Fault", /* 13 */
    "Page Fault",               /* 14 */
    "Reserved",                 /* 15 */
    "x87 FPU Error",            /* 16 */
    "Alignment Check",          /* 17 */
    "Machine Check",            /* 18 */
    "SIMD FP Exception",        /* 19 */
    "Virtualisation Exception", /* 20 */
    "Control Protection",       /* 21 */
    "Reserved", "Reserved", "Reserved", "Reserved",
    "Reserved", "Reserved",
    "Hypervisor Injection",     /* 28 */
    "VMM Communication",        /* 29 */
    "Security Exception",       /* 30 */
    "Reserved",                 /* 31 */
};

/* Central dispatcher — called from isr_common in idt.asm */
void interrupt_dispatch(interrupt_frame_t *frame)
{
    uint32_t n = frame->int_no;

    if (n < 32) {
        /* CPU exception */
        kprintf("\n[EXCEPTION %u] %s\n", n,
                n < 32 ? exception_names[n] : "Unknown");
        kprintf("  EIP=0x%x  CS=0x%x  ERR=0x%x  EFLAGS=0x%x\n",
                frame->eip, frame->cs, frame->err_code, frame->eflags);
        kernel_panic("Unhandled CPU exception");
    }
    else if (n >= 0x20 && n <= 0x2F) {
        /* Hardware IRQ */
        uint8_t irq = (uint8_t)(n - 0x20);
        switch (irq) {
        /* IRQ_TIMER (0) is handled by irq0_preempt -> sched_irq directly */
        case IRQ_KEYBOARD: keyboard_handler(); break;
        case 12:           mouse_handler();    break;   /* PS/2 mouse */
        default: break;
        }
        pic_send_eoi(irq);
    }
}
