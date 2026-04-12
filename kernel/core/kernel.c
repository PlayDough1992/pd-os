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
#include "ata.h"
#include "vfs.h"
#include "pdfs.h"
#include "fat32.h"
#include "ext2.h"
#include "ntfs.h"

void kernel_main(void)
{
    vga_init();

    /* ---- Banner ---------------------------------------------------------- */
    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    kprintf("===============================================================================\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    kprintf("               PD-Kernel  v0.1  -  Phase 6: PD-Shell + Users\n");
    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    kprintf("===============================================================================\n");

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

    kprintf("  (0) Probing ATA drives...");
    ata_init();
    {
        const ata_drive_t *drv = ata_get_drive();
        if (drv->present) {
            vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
            kprintf("  (X) %s (%u KB)\n",
                    drv->model,
                    (drv->total_sectors / 2u));
        } else {
            vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
            kprintf("  (!) No ATA drive detected\n");
        }
    }
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);

    kprintf("  (0) Mounting filesystems...");
    vfs_init();
    vfs_register(pdfs_get_driver());
    vfs_register(fat32_get_driver());
    vfs_register(ext2_get_driver());
    vfs_register(ntfs_get_driver());
    {
        int pdfs_ok = vfs_mount("/",         "pdfs",  200);
        int fat_ok  = vfs_mount("/mnt/fat",  "fat32", 2048);
        int ext_ok  = vfs_mount("/mnt/ext2", "ext2",  4096);
        int ntfs_ok = vfs_mount("/mnt/ntfs", "ntfs",  69632);
        if (pdfs_ok == 0) {
            vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
            kprintf("  (X) PDFS at /  (%u KB free, %u files)\n",
                    pdfs_free_sectors() / 2u,
                    pdfs_file_count());
        } else {
            vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
            kprintf("  (!) PDFS not found  (run 'mkpdfs')\n");
        }
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        if (fat_ok == 0) {
            vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
            kprintf("  (X) FAT32 at /mnt/fat\n");
        } else {
            vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
            kprintf("  (!) FAT32 not found at LBA 2048\n");
        }
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        if (ext_ok == 0) {
            vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
            kprintf("  (X) ext2 at /mnt/ext2\n");
        } else {
            vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
            kprintf("  (!) ext2 not found at LBA 4096\n");
        }
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        if (ntfs_ok == 0) {
            vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
            kprintf("  (X) NTFS at /mnt/ntfs  (read-only)\n");
        } else {
            vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
            kprintf("  (!) NTFS not found at LBA 69632\n");
        }
    }
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
