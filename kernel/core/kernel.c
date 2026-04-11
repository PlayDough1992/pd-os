/* ============================================================================
 * PD-Kernel  —  kernel_main()  (Phase 5: Interrupts, PIC, PIT, Keyboard)
 * ============================================================================ */

#include "kernel.h"
#include "vga.h"
#include "io.h"
#include "idt.h"
#include "pic.h"
#include "pit.h"
#include "keyboard.h"

void kernel_main(void)
{
    vga_init();

    /* ---- Banner ---------------------------------------------------------- */
    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    kprintf("============================================================\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    kprintf("         PD-Kernel  v0.1  -  Phase 5: Interrupts\n            ");
    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    kprintf("============================================================\n");

    /* ---- Subsystem init -------------------------------------------------- */
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);

    kprintf("  [..] Waking up IDT...");
    idt_init();
    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    kprintf(" OK\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);

    kprintf("  [..] Waking up PIC...");
    pic_init();
    /* Mask everything; PIT + keyboard init will unmask their own IRQs */
    pic_mask_irq(0); pic_mask_irq(1); pic_mask_irq(2); pic_mask_irq(3);
    pic_mask_irq(4); pic_mask_irq(5); pic_mask_irq(6); pic_mask_irq(7);
    pic_mask_irq(8); pic_mask_irq(9); pic_mask_irq(10); pic_mask_irq(11);
    pic_mask_irq(12); pic_mask_irq(13); pic_mask_irq(14); pic_mask_irq(15);
    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    kprintf(" OK\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);

    kprintf("  [..] Waking up PIT @ 100 Hz...");
    pit_init(100);
    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    kprintf(" OK\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);

    kprintf("  [..] Waking up keyboard...");
    keyboard_init();
    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    kprintf(" OK\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);

    /* ---- Enable interrupts ----------------------------------------------- */
    __asm__ volatile ("sti");
    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    kprintf("\n  Interrupts online.\n");

    /* ---- Keyboard echo loop ---------------------------------------------- */
    vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
    kprintf("\n  Type something (keyboard echo test):\n  > ");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);

    for (;;) {
        char c = keyboard_getchar();
        if (c == '\n' || c == '\r') {
            kprintf("\n  > ");
        } else {
            vga_putchar(c);
        }
    }
}
