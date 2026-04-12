#pragma once

/* ============================================================================
 * PD-Kernel  —  Paging (Phase 7c)
 *
 * Sets up a flat identity-mapped page directory covering the first 4 MB of
 * physical memory.  Virtual address == physical address in this region, so
 * all existing kernel pointers (VGA, PMM bitmap, E820 table, stack, kernel
 * image) continue to work without modification.
 *
 * Memory layout reserved for paging structures:
 *   0x9000–0x9FFF   Page Directory  (1024 × 4-byte PDEs)
 *   0xA000–0xAFFF   Page Table 0    (1024 × 4-byte PTEs, maps 0–4 MB)
 *
 * Both addresses are within the low-1MB region that pmm_init() already
 * marks reserved, so pmm_alloc() will never hand them out.
 * ============================================================================ */

#include "kernel.h"

/* Physical addresses of paging structures ---------------------------------- */
#define PAGING_DIR_ADDR     0x9000u     /* page directory  (4 KB)            */
#define PAGING_TABLE0_ADDR  0xA000u     /* page table 0    (4 KB, 0–4 MB)    */

/* Page-entry flag bits ----------------------------------------------------- */
#define PAGE_PRESENT   (1u << 0)        /* P  — entry is valid               */
#define PAGE_WRITABLE  (1u << 1)        /* R/W — read+write                  */
#define PAGE_USER      (1u << 2)        /* U/S — user-mode accessible        */

/*
 * Initialise paging.
 *
 * Zeroes the page directory, identity-maps the first 4 MB using Page Table 0,
 * loads CR3, and sets CR0.PG.  After this call the CPU is running with paging
 * enabled; because the mapping is identity (virt == phys), execution
 * continues seamlessly.
 *
 * Must be called AFTER pmm_init() (so the PMM knows which frames are used).
 */
void paging_init(void);
