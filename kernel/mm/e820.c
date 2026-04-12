/* ============================================================================
 * PD-Kernel  —  E820 BIOS physical memory map reader
 * ============================================================================ */

#include "e820.h"
#include "vga.h"
#include "io.h"

/* ---- Accessors ------------------------------------------------------------ */

uint32_t e820_count(void)
{
    return *(volatile uint32_t *)E820_PHYS_COUNT;
}

const e820_entry_t *e820_entries(void)
{
    return (const e820_entry_t *)E820_PHYS_ENTRIES;
}

/* ---- Usable RAM total ----------------------------------------------------- */

uint64_t e820_usable_bytes(void)
{
    const e820_entry_t *e = e820_entries();
    uint32_t n = e820_count();
    uint64_t total = 0;
    uint32_t i;

    for (i = 0; i < n; i++) {
        if (e[i].type == E820_TYPE_USABLE)
            total += e[i].length;
    }
    return total;
}

/* ---- Helpers -------------------------------------------------------------- */

static const char *type_name(uint32_t t)
{
    switch (t) {
    case E820_TYPE_USABLE:   return "Usable RAM";
    case E820_TYPE_RESERVED: return "Reserved";
    case E820_TYPE_ACPI:     return "ACPI Reclaimable";
    case E820_TYPE_NVS:      return "ACPI NVS";
    case E820_TYPE_BAD:      return "Bad memory";
    default:                 return "Unknown";
    }
}

/*
 * Print a uint64_t as a zero-padded 16-digit hex string prefixed with "0x".
 * kprintf only supports %x (uint32_t), so we build the string manually.
 */
static void print_hex64(uint64_t v)
{
    const char *digits = "0123456789abcdef";
    char buf[19];   /* "0x" + 16 hex chars + NUL */
    int i;

    buf[0]  = '0';
    buf[1]  = 'x';
    buf[18] = '\0';

    for (i = 17; i >= 2; i--) {
        buf[i] = digits[v & 0xF];
        v >>= 4;
    }

    kprintf("%s", buf);
}

/*
 * Print a byte count in a human-readable form (B / KB / MB / GB).
 * Stays in 32-bit arithmetic — enough for up to ~4 GB.
 */
static void print_size(uint64_t bytes)
{
    if (bytes >= (1024ULL * 1024 * 1024)) {
        kprintf("%u GB", (uint32_t)(bytes / (1024 * 1024 * 1024)));
    } else if (bytes >= (1024ULL * 1024)) {
        kprintf("%u MB", (uint32_t)(bytes / (1024 * 1024)));
    } else if (bytes >= 1024ULL) {
        kprintf("%u KB", (uint32_t)(bytes / 1024));
    } else {
        kprintf("%u B", (uint32_t)bytes);
    }
}

/* ---- e820_print ----------------------------------------------------------- */

void e820_print(void)
{
    uint32_t            n = e820_count();
    const e820_entry_t *e = e820_entries();
    uint64_t            usable;
    uint32_t            i;

    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    kprintf("\n  Physical memory map  (%u entr%s):\n",
            n, n == 1 ? "y" : "ies");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);

    if (n == 0) {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        kprintf("  [!] E820 not supported — memory map unavailable.\n");
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        return;
    }

    for (i = 0; i < n; i++) {
        /* Colour by type so usable regions stand out */
        if (e[i].type == E820_TYPE_USABLE)
            vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
        else if (e[i].type == E820_TYPE_BAD)
            vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        else
            vga_set_color(VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK);

        kprintf("  [%u] ", i);
        print_hex64(e[i].base);
        kprintf("  len=");
        print_hex64(e[i].length);
        kprintf("  (");
        print_size(e[i].length);
        kprintf(")  %s\n", type_name(e[i].type));
    }

    usable = e820_usable_bytes();
    vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
    kprintf("\n  Total usable RAM: ");
    print_size(usable);
    kprintf("\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
}
