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
    kprintf("         PD-Kernel  v0.1  -  Phase 5: Interrupts\n");
    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    kprintf("============================================================\n");

    /* ---- Subsystem init -------------------------------------------------- */
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);

    kprintf("  (0) Waking up IDT...");
    idt_init();
    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    kprintf("  (X) COMPLETE\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);

    kprintf("  (0) Waking up PIC...");
    pic_init();
    /* Mask everything; PIT + keyboard init will unmask their own IRQs */
    pic_mask_irq(0); pic_mask_irq(1); pic_mask_irq(2); pic_mask_irq(3);
    pic_mask_irq(4); pic_mask_irq(5); pic_mask_irq(6); pic_mask_irq(7);
    pic_mask_irq(8); pic_mask_irq(9); pic_mask_irq(10); pic_mask_irq(11);
    pic_mask_irq(12); pic_mask_irq(13); pic_mask_irq(14); pic_mask_irq(15);
    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    kprintf("  (X) COMPLETE\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);

    kprintf("  (0) Waking up PIT @ 100 Hz...");
    pit_init(100);
    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    kprintf("  (X) COMPLETE\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);

    kprintf("  (0) Waking up keyboard...");
    keyboard_init();
    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    kprintf("  (X) COMPLETE\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);

    /* ---- Enable interrupts ----------------------------------------------- */
    __asm__ volatile ("sti");
    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    kprintf("\n  Interrupts online.\n");

    /* ---- Keyboard echo loop ---------------------------------------------- */
    vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
    kprintf("\n  Type something (keyboard echo test):\n  > ");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);

    uint32_t prompt_col, prompt_row;  /* cursor position just after '> ' */

    /* Capture start position once after the initial prompt */
    prompt_col = vga_get_col();
    prompt_row = vga_get_row();

    for (;;) {
        char c = keyboard_getchar();
        if (c == '\n' || c == '\r') {
            kprintf("\n  > ");
            /* Update anchor for the new prompt */
            prompt_col = vga_get_col();
            prompt_row = vga_get_row();
        } else if (c == '\b') {
            /* Only backspace if cursor is strictly past the input start */
            if (vga_get_row() > prompt_row ||
                (vga_get_row() == prompt_row && vga_get_col() > prompt_col)) {
                vga_backspace();
            }
        } else if (c == KEY_LEFT) {
            /* Would wrap to end of previous row if at col 0 */
            uint8_t new_col = vga_get_col() > 0 ? vga_get_col() - 1 : VGA_WIDTH - 1;
            uint8_t new_row = vga_get_col() > 0 ? vga_get_row() : vga_get_row() - 1;
            if (new_row > (uint8_t)prompt_row ||
                (new_row == (uint8_t)prompt_row && new_col >= (uint8_t)prompt_col))
                vga_cursor_left();
        } else if (c == KEY_RIGHT) {
            vga_cursor_right();
        } else if (c == KEY_UP) {
            /* Moving up is only safe if the row above is still >= prompt_row,
               and if landing on prompt_row the col must be >= prompt_col */
            if (vga_get_row() > (uint8_t)prompt_row) {
                uint8_t new_row = vga_get_row() - 1;
                if (new_row > (uint8_t)prompt_row ||
                    (new_row == (uint8_t)prompt_row && vga_get_col() >= (uint8_t)prompt_col))
                    vga_cursor_up();
            }
        } else if (c == KEY_DOWN) {
            vga_cursor_down();
        } else {
            vga_putchar(c);
        }
    }
}
