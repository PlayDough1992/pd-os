/* ============================================================================
 * PD-Kernel  —  kernel_main()  (Phase 6: PD-Shell + User System)
 * ============================================================================ */

#include "kernel.h"
#include "vga.h"
#include "io.h"
#include "idt.h"
#include "pic.h"
#include "pit.h"
#include "keyboard.h"
#include "users.h"
#include "login.h"
#include "shell.h"
#include "e820.h"
#include "pmm.h"
#include "paging.h"
#include "kheap.h"

void kernel_main(void)
{
    vga_init();

    /* ---- Banner ---------------------------------------------------------- */
    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    kprintf("============================================================\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    kprintf("      PD-Kernel  v0.1  -  Phase 6: PD-Shell + Users\n");
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

    kprintf("  (0) Initialising user table...");
    users_init();
    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    kprintf("  (X) COMPLETE\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);

    kprintf("  (0) Reading memory map...");
    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    kprintf("  (X) %u entr%s\n",
            e820_count(), e820_count() == 1 ? "y" : "ies");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);

    kprintf("  (0) Initialising PMM...");
    pmm_init();
    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    kprintf("  (X) %u MB free\n",
            (pmm_free_frames() * PMM_PAGE_SIZE) / (1024u * 1024u));
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);

    kprintf("  (0) Enabling paging...");
    paging_init();
    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    kprintf("  (X) Identity-mapped 4 MB\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);

    kprintf("  (0) Initialising kernel heap...");
    kheap_init();
    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    kprintf("  (X) %u KB heap ready\n",
            kheap_free_bytes() / 1024u);
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);

    /* ---- Enable interrupts ----------------------------------------------- */
    __asm__ volatile ("sti");
    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    kprintf("\n  Interrupts online.\n");

    /* ---- Login + shell loop ---------------------------------------------- */
    /*
     * Show the login screen on boot and after every logout.
     * login_prompt() never returns NULL — it halts on too many failures.
     */
    while (1) {
        const user_t *user = login_prompt();
        shell_run(user);
    }
}
