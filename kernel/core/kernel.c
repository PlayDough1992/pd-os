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
#include "pci.h"
#include "usb.h"

static inline uint8_t k_inb(uint16_t port)
{ uint8_t v; __asm__ volatile("inb %1,%0":"=a"(v):"Nd"(port)); return v; }
#include "rtl8139.h"
#include "net.h"
#include "vfs.h"
#include "pdfs.h"
#include "fat32.h"
#include "ext2.h"
#include "ntfs.h"
#include "process.h"
#ifdef GDE_BUILD
#include "gde.h"
#include "de_loader.h"
#include "boot_info.h"
#endif

/* Idle task: yields CPU immediately so GDE runs without interruption */
static void idle_task(void)
{
    for (;;) { proc_sleep(); __asm__ volatile ("hlt"); }
}

void kernel_main(void)
{
    vga_init();

    /* ---- Banner ---------------------------------------------------------- */
    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    kprintf("===============================================================================\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    kprintf("               PD-Kernel  v0.1  -  Phase 10: Process Management\n");
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

#ifdef GDE_BUILD
    /* ---- GDE path: VBE mode was set by stage2 ---------------------------- */
    if (g_boot_info->magic == BOOT_INFO_MAGIC && g_boot_info->vbe_ok) {
        /* Finish remaining init (heap + ATA + filesystems) before GDE */
#endif

    kprintf("  (0) Initialising kernel heap...");
    kheap_init();
    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    kprintf("  (X) %u KB heap ready\n",
            kheap_free_bytes() / 1024u);
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);

    kprintf("  (0) Probing ATA drives...");
    ata_init();  /* also populates g_drives[0] */
    {
        int found = 0, n;
        for (n = 0; n < ATA_MAX_DRIVES; n++) {
            const ata_drive_t *d = ata_get_drive_n(n);
            if (d && d->present) {
                vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
                kprintf("  (X) Drive %u: %s (%u MB)\n",
                        n, d->model, d->total_sectors / 2048u);
                found++;
            }
        }
        if (!found) {
            vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
            kprintf("  (!) No legacy ATA drive (status=0x%02x)\n",
                    k_inb(0x1F7));
            /* Check if an AHCI SATA controller exists on PCI */
            {
                uint8_t ab = 0, ad = 0, af = 0;
                if (pci_find_class(0x01, 0x06, 0x01, &ab, &ad, &af)) {
                    vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
                    kprintf("  (!) AHCI controller found at PCI %02x:%02x.%x\n",
                            ab, ad, af);
                    kprintf("  (!) BIOS SATA mode is AHCI — change to\n");
                    kprintf("  (!) 'Compatible' or 'IDE' mode to use installer.\n");
                }
            }
        }
    }
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);

    /* ---- Enable interrupts before wizard so keyboard IRQ fires ----------- */
    proc_init();
    proc_create("idle", idle_task);
    __asm__ volatile ("sti");

#ifndef GDE_BUILD
    /* ---- Installer wizard ------------------------------------------------ */
    {
        int n, n_drives = 0;
        int drive_idx[ATA_MAX_DRIVES];

        for (n = 0; n < ATA_MAX_DRIVES; n++) {
            const ata_drive_t *d = ata_get_drive_n(n);
            if (d && d->present)
                drive_idx[n_drives++] = n;
        }

        if (n_drives > 0) {
            char ch;
            int  sel = -1;

            kprintf("\n  +---------------------------------+\n");
            kprintf("  |   PD-OS USB Installer Wizard    |\n");
            kprintf("  +---------------------------------+\n");
            kprintf("  Select a drive to install PD-OS onto:\n\n");
            for (n = 0; n < n_drives; n++) {
                const ata_drive_t *d = ata_get_drive_n(drive_idx[n]);
                kprintf("    [%u]  Drive %u: %s (%u MB)\n",
                        n + 1, drive_idx[n], d->model,
                        d->total_sectors / 2048u);
            }
            kprintf("\n    [S]  Skip installer, boot normally\n\n");
            kprintf("  WARNING: ALL DATA ON THE CHOSEN DRIVE WILL BE LOST!\n\n");

            /* Keep prompting until valid key */
            do {
                kprintf("  Your choice: ");
                ch = keyboard_getchar();
                kprintf("%c\n", ch);
                if (ch == 's' || ch == 'S') { sel = -1; break; }
                if (ch >= '1' && ch < '1' + n_drives) {
                    sel = drive_idx[ch - '1'];
                    break;
                }
                vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
                kprintf("  Invalid choice — try again.\n");
                vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
            } while (1);

            if (sel >= 0) {
                const ata_drive_t *target = ata_get_drive_n(sel);
                kprintf("\n  Installing to drive %u: %s\n", sel, target->model);
                kprintf("  (0) Initialising USB storage...\n");
                usb_init();
                if (usb_available()) {
                    uint8_t  ibuf[512];
                    uint32_t ilba;
                    uint32_t ierr = 0;
                    kprintf("  (0) Copying 512 sectors...\n");
                    for (ilba = 0; ilba < 512; ilba++) {
                        if (usb_read_sector(ilba, ibuf) != 0)
                            { ierr++; continue; }
                        if (ata_write_sectors_raw(sel, ilba, 1, ibuf) != 0)
                            { ierr++; }
                        if ((ilba & 63u) == 0u)
                            kprintf("  %u / 512\n", ilba);
                    }
                    if (ierr == 0) {
                        vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
                        kprintf("  (X) Install complete.\n");
                    } else {
                        vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
                        kprintf("  (!) Install complete with %u errors.\n", ierr);
                    }
                    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
                    kprintf("  Remove USB drive and reboot to boot from SATA.\n");
                } else {
                    vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
                    kprintf("  (!) USB storage not found — install aborted.\n");
                    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
                }
                for (;;) __asm__ volatile ("hlt");
            }
            /* User pressed S — boot normally, no USB init,
               BIOS retains full keyboard/mouse emulation.                */
        } else {
            /* No SATA drives — USB-only mode, init USB for disk I/O      */
            kprintf("  (0) Probing USB (EHCI)...");
            usb_init();
            vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        }
    }
#endif /* !GDE_BUILD */

    kprintf("  (0) Probing network card...");
    rtl8139_init();
    net_init();
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);

    kprintf("  (0) Mounting filesystems...");
    vfs_init();
    vfs_register(pdfs_get_driver());
    vfs_register(fat32_get_driver());
    vfs_register(ext2_get_driver());
    vfs_register(ntfs_get_driver());
    {
        /* Mount secondary filesystems FIRST (before any PDFS write operations)
         * so their ATA reads complete in a clean drive state.               */
        int fat_ok  = vfs_mount("/mnt/fat",  "fat32", 2048);
        int ext_ok  = vfs_mount("/mnt/ext2", "ext2",  4096);
        int ntfs_ok = vfs_mount("/mnt/ntfs", "ntfs",  69632);

        /* PDFS init: format→scaffold→mount if blank/stale, else mount→scaffold */
        int pdfs_ok = vfs_mount("/", "pdfs", 1024);

        if (pdfs_ok == 0 && pdfs_is_ro()) {
            vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
            kprintf("  (!) PDFS outdated — auto-upgrading to v3...\n");
            vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
            pdfs_ok = -1;
        }

        if (pdfs_ok != 0) {
            vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
            kprintf("  (!) PDFS not found — auto-formatting...\n");
            vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
            if (pdfs_format(1024) != 0) {
                vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
                kprintf("  (!) PDFS format failed: ATA error\n");
                vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
            } else {
                kprintf("  (0) Building filesystem structure...");
                pdfs_scaffold();
                vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
                kprintf("  (X) COMPLETE\n");
                vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
                pdfs_ok = vfs_mount("/", "pdfs", 1024);
            }
        }

        if (pdfs_ok == 0) {
            vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
            kprintf("  (X) PDFS at /  (%u KB free)\n",
                    pdfs_free_sectors() / 2u);
            vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
            kprintf("  (0) Enforcing filesystem structure...");
            pdfs_scaffold();
            vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
            kprintf("  (X) COMPLETE\n");
            vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
            kprintf("  (0) Loading user credentials from disk...");
            users_load_from_disk();
            vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
            kprintf("  (X) COMPLETE\n");
            vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        } else {
            vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
            kprintf("  (!) PDFS init failed: no ATA drive detected\n");
            vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        }

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

    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    kprintf("\n  Interrupts online.  (%d tasks)\n", proc_count_active());
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);

#ifdef GDE_BUILD
    }  /* end of "if VBE ok" block opened after paging_init() */
    if (g_boot_info->magic == BOOT_INFO_MAGIC && g_boot_info->vbe_ok) {
        /* Publish the kernel API table so external DEs can call kernel fns */
        de_populate_api();
        /* Try to load an external DE from /sys/de/<active>.bin on PDFS.
         * If found it is jumped to and never returns.
         * If absent or unreadable, fall through to the built-in GDE. */
        de_load_and_run();
        /* No external DE — spawn the built-in GDE as a kernel process */
        proc_create("gde-session", gde_process_main);
        /* Kernel main thread yields immediately so GDE gets the CPU */
        for (;;) { proc_sleep(); __asm__ volatile ("hlt"); }
    }
    /* VBE failed — fall through to text-mode login */
#endif

    /* ---- Login + shell loop ---------------------------------------------- */
    while (1) {
        const user_t *user = login_prompt();
        shell_run(user);
    }
}
