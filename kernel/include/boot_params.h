#pragma once

/* ============================================================================
 * PD-Kernel  —  Boot parameters passed from Stage 2 to the kernel
 *               (Phase 12a)
 *
 * Stage 2 writes this struct to physical address 0x5300 before entering
 * protected mode.  The kernel reads it after paging is enabled (0x5300
 * is within the first 4 MB identity-mapped region so it is always readable).
 *
 * Layout at 0x5300 (12 bytes):
 *   +0   uint32  fb_addr    — physical address of VESA linear framebuffer
 *   +4   uint16  fb_width   — horizontal resolution in pixels
 *   +6   uint16  fb_height  — vertical resolution in pixels
 *   +8   uint16  fb_pitch   — bytes per scan line (>= fb_width * (fb_bpp/8))
 *   +10  uint8   fb_bpp     — bits per pixel (16, 24, or 32)
 *   +11  uint8   fb_ok      — 1 = VBE mode set, 0 = text mode fallback
 * ============================================================================ */

#include "kernel.h"

#define BOOT_PARAMS_ADDR  0x5300u

typedef struct {
    uint32_t fb_addr;    /* VESA linear framebuffer physical address          */
    uint16_t fb_width;   /* horizontal resolution (pixels)                    */
    uint16_t fb_height;  /* vertical resolution (pixels)                      */
    uint16_t fb_pitch;   /* bytes per scan line                               */
    uint8_t  fb_bpp;     /* bits per pixel (16/24/32)                         */
    uint8_t  fb_ok;      /* 1 = VBE active, 0 = VGA text fallback             */
} __attribute__((packed)) boot_params_t;

/* Convenience pointer — valid after paging_init() maps the first 4 MB */
#define boot_params  ((boot_params_t *)BOOT_PARAMS_ADDR)
