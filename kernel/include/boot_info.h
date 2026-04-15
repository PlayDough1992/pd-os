#pragma once

/* ============================================================================
 * PD-OS  —  Boot-time information block
 * ============================================================================
 * Written by stage2.asm at physical address BOOT_INFO_ADDR (0x4000) before
 * entering protected mode.  Readable by the kernel immediately after startup
 * since 0x4000 is inside the identity-mapped first 4 MB.
 *
 * Layout (64 bytes total):
 *   +0x00  uint32  magic         = 0xB0075E00 ("BOOT SE[ed]")
 *   +0x04  uint32  vbe_fb        physical base address of VBE framebuffer
 *   +0x08  uint16  vbe_width     horizontal resolution in pixels
 *   +0x0A  uint16  vbe_height    vertical resolution in pixels
 *   +0x0C  uint16  vbe_bpp       bits per pixel (32)
 *   +0x0E  uint16  vbe_pitch     bytes per scan line
 *   +0x10  uint8   vbe_ok        1 = VBE mode set successfully, 0 = failed
 *   +0x11  uint8   font_present  1 = 8x16 font copied to physical 0x3000
 * ============================================================================ */

#include "kernel.h"

#define BOOT_INFO_ADDR  0x4000u
#define BOOT_INFO_MAGIC 0xB0075E00u

/* Physical address where stage2 copies the VGA BIOS 8x16 font (4096 bytes) */
#define GDE_FONT_ADDR   0x3000u

typedef struct {
    uint32_t magic;
    uint32_t vbe_fb;
    uint16_t vbe_width;
    uint16_t vbe_height;
    uint16_t vbe_bpp;
    uint16_t vbe_pitch;
    uint8_t  vbe_ok;
    uint8_t  font_present;
} __attribute__((packed)) boot_info_t;

/* Pointer to the boot info block in physical memory */
#define g_boot_info  ((volatile boot_info_t *)BOOT_INFO_ADDR)
