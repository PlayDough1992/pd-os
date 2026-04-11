/* ============================================================================
 * PD-Kernel  —  kernel_main()  (Phase 4: Foundation)
 * ============================================================================ */

#include "kernel.h"
#include "vga.h"
#include "io.h"

void kernel_main(void)
{
    vga_init();

    /* ---- Banner ---------------------------------------------------------- */
    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    kprintf("============================================================\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    kprintf("  PD-OS Kernel  v0.1  -  Phase 4: Foundation\n");
    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    kprintf("============================================================\n");

    /* ---- System info ----------------------------------------------------- */
    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    kprintf("\n  [OK] Kernel entry reached.\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    kprintf("  Architecture : x86  (32-bit Protected Mode)\n");
    kprintf("  VGA driver   : 80x25 text mode @ 0xB8000\n");
    kprintf("  Kernel base  : 0x100000\n");
    kprintf("  Stack        : 0x9FC00\n");

    /* ---- kprintf test ---------------------------------------------------- */
    vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
    kprintf("\n  --- kprintf test ---\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    kprintf("  decimal  : %d\n", 42);
    kprintf("  hex      : 0x%x\n", 0xDEADBEEF);
    kprintf("  string   : %s\n", "Hello from PD-OS!");

    /* ---- Done ------------------------------------------------------------ */
    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    kprintf("\n  PD-OS is running. System halted.\n");

    for (;;)
        __asm__ volatile ("hlt");
}
