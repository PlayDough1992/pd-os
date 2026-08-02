/* ============================================================================
 * PD-Kernel  —  Kernel panic handler
 * ============================================================================ */

#include "kernel.h"
#include "vga.h"
#include "io.h"

void kernel_panic(const char *msg)
{
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_RED);
    kprintf("\n\n");
    kprintf("  ************************************************************\n");
    kprintf("  *                    *** KERNEL PANIC ***                  *\n");
    kprintf("  ************************************************************\n");
    kprintf("\n  %s\n", msg);
    kprintf("\n  System halted. Please restart.\n");

    for (;;)
        __asm__ volatile ("cli; hlt");
}
