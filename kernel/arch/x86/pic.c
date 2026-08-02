/* ============================================================================
 * PD-Kernel  —  8259A PIC (Programmable Interrupt Controller) driver
 * ============================================================================ */

#include "pic.h"
#include "kernel.h"

/* ICW = Initialization Command Word */
#define PIC_EOI     0x20    /* End-of-interrupt command */
#define ICW1_ICW4   0x01    /* ICW4 needed */
#define ICW1_INIT   0x10    /* initialization */
#define ICW4_8086   0x01    /* 8086/88 mode */

static inline void outb(uint16_t port, uint8_t val)
{
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void io_wait(void)
{
    /* Brief delay by writing to an unused port */
    outb(0x80, 0);
}

void pic_init(void)
{
    /* Save current masks */
    uint8_t mask1 = inb(PIC1_DATA);
    uint8_t mask2 = inb(PIC2_DATA);

    /* Start initialization sequence (cascade mode) */
    outb(PIC1_CMD,  ICW1_INIT | ICW1_ICW4); io_wait();
    outb(PIC2_CMD,  ICW1_INIT | ICW1_ICW4); io_wait();

    /* ICW2: vector offsets */
    outb(PIC1_DATA, PIC1_OFFSET); io_wait();  /* IRQ0-7  -> INT 0x20-0x27 */
    outb(PIC2_DATA, PIC2_OFFSET); io_wait();  /* IRQ8-15 -> INT 0x28-0x2F */

    /* ICW3: cascade identity */
    outb(PIC1_DATA, 0x04); io_wait();  /* PIC1: slave on IRQ2 */
    outb(PIC2_DATA, 0x02); io_wait();  /* PIC2: cascade identity = 2 */

    /* ICW4: 8086 mode */
    outb(PIC1_DATA, ICW4_8086); io_wait();
    outb(PIC2_DATA, ICW4_8086); io_wait();

    /* Restore masks (mask everything initially; drivers unmask their own IRQ) */
    outb(PIC1_DATA, mask1);
    outb(PIC2_DATA, mask2);
}

void pic_send_eoi(uint8_t irq)
{
    if (irq >= 8)
        outb(PIC2_CMD, PIC_EOI);
    outb(PIC1_CMD, PIC_EOI);
}

void pic_mask_irq(uint8_t irq)
{
    uint16_t port;
    uint8_t  val;
    if (irq < 8) {
        port = PIC1_DATA;
    } else {
        port = PIC2_DATA;
        irq -= 8;
    }
    val = inb(port) | (uint8_t)(1 << irq);
    outb(port, val);
}

void pic_unmask_irq(uint8_t irq)
{
    uint16_t port;
    uint8_t  val;
    if (irq < 8) {
        port = PIC1_DATA;
    } else {
        port = PIC2_DATA;
        irq -= 8;
    }
    val = inb(port) & (uint8_t)~(1 << irq);
    outb(port, val);
}
