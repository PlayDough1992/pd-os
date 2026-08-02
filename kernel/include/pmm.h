#pragma once

/* ============================================================================
 * PD-Kernel  —  Physical Memory Manager (PMM)
 *
 * Manages physical page frames (4 KB each) using a flat bitmap.
 *
 * Memory layout (physical):
 *   0x0000–0x4FFF   conventional low memory / BIOS data
 *   0x5000–0x5003   E820 entry count  (written by Stage 2)
 *   0x5004–0x52FF   E820 entries      (written by Stage 2)
 *   0x6000–0x6FFF   PMM bitmap        (4096 bytes = 32768 bits = 128 MB)
 *   0x7C00–0x7DFF   Stage 1 (no longer needed after boot)
 *   0x8000–0x8FFF   Stage 2 (no longer needed after boot)
 *   0x10000–0x1FFFF kernel load area  (copy src, re-usable)
 *   0x100000+        kernel image      (_kernel_start .. _kernel_end)
 * ============================================================================ */

#include "kernel.h"

#define PMM_PAGE_SIZE   4096u          /* bytes per frame                  */
#define PMM_BITMAP_ADDR 0x6000u        /* physical address of the bitmap   */
#define PMM_MAX_FRAMES  (128u * 1024u * 1024u / PMM_PAGE_SIZE)  /* 32768   */

/*
 * Initialise the PMM from the E820 map.
 * Must be called AFTER e820 data is in place (set by Stage 2).
 * Marks every frame outside a TYPE_USABLE region as reserved,
 * then reserves the kernel image, the bitmap itself, and low memory.
 */
void pmm_init(void);

/*
 * Allocate one physical page frame.
 * Returns the physical address of the frame, or 0 on out-of-memory.
 */
uint32_t pmm_alloc(void);

/*
 * Free a physical page frame previously returned by pmm_alloc().
 * Passing an address not returned by pmm_alloc() is undefined behaviour.
 */
void pmm_free(uint32_t phys_addr);

/*
 * Mark a range of physical memory as reserved (used / not allocatable).
 * start and end are rounded to page boundaries.
 */
void pmm_reserve_range(uint32_t start, uint32_t end);

/* Statistics */
uint32_t pmm_free_frames(void);
uint32_t pmm_used_frames(void);
uint32_t pmm_total_frames(void);
