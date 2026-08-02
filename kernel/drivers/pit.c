/* ============================================================================
 * PD-Kernel  —  8253/8254 PIT (Programmable Interval Timer) driver
 * ============================================================================ */

#include "pit.h"
#include "pic.h"
#include "kernel.h"

#define PIT_CHANNEL0  0x40  /* Channel 0 data port */
#define PIT_CMD       0x43  /* Mode/command register */
#define PIT_BASE_HZ   1193182

static volatile uint32_t ticks = 0;

static inline void outb(uint16_t port, uint8_t val)
{
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

void pit_init(uint32_t frequency_hz)
{
    uint32_t divisor = PIT_BASE_HZ / frequency_hz;

    /* Channel 0, lobyte/hibyte, rate generator (mode 2) */
    outb(PIT_CMD, 0x36);
    outb(PIT_CHANNEL0, (uint8_t)(divisor & 0xFF));
    outb(PIT_CHANNEL0, (uint8_t)((divisor >> 8) & 0xFF));

    pic_unmask_irq(IRQ_TIMER);
}

void pit_handler(void)
{
    ticks++;
}

uint32_t pit_get_ticks(void)
{
    return ticks;
}
