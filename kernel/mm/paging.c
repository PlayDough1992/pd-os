/* ============================================================================
 * PD-Kernel  —  Paging initialisation  (Phase 7c)
 *
 * Identity-maps the first 4 MB of physical memory (virt == phys) so that:
 *   • The kernel image at 0x100000 is accessible at virtual 0x100000
 *   • The VGA text buffer at 0xB8000 is accessible at virtual 0xB8000
 *   • The PMM bitmap (0x6000), E820 table (0x5000), and stack (0x9FC00)
 *     all remain at the same address after paging is enabled
 *
 * Two structures, each 4 KB, placed in low reserved memory:
 *   0x9000  — Page Directory  (1024 entries)
 *   0xA000  — Page Table 0    (1024 entries, identity maps 0x000000–0x3FFFFF)
 * ============================================================================ */

#include "paging.h"

void paging_init(void)
{
    uint32_t *page_dir    = (uint32_t *)PAGING_DIR_ADDR;
    uint32_t *page_table0 = (uint32_t *)PAGING_TABLE0_ADDR;
    uint32_t i;

    /* ---- Enable PSE (4 MB large pages) in CR4 before installing the PD --- */
    {
        uint32_t cr4;
        __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));
        cr4 |= (1u << 4);   /* CR4.PSE */
        __asm__ volatile ("mov %0, %%cr4" :: "r"(cr4) : "memory");
    }

    /* ---- Zero the page directory ----------------------------------------- */
    for (i = 0; i < 1024; i++)
        page_dir[i] = 0;

    /* ---- Build Page Table 0: identity-map 0x000000 – 0x3FFFFF ------------ */
    for (i = 0; i < 1024; i++)
        page_table0[i] = (i * 0x1000u) | PAGE_PRESENT | PAGE_WRITABLE;

    /* ---- Install PDE[0] → Page Table 0 ------------------------------------ */
    page_dir[0] = PAGING_TABLE0_ADDR | PAGE_PRESENT | PAGE_WRITABLE;

    /* ---- Load CR3 with the physical address of the page directory --------- */
    __asm__ volatile (
        "mov %0, %%cr3"
        :
        : "r"(PAGING_DIR_ADDR)
        : "memory"
    );

    /* ---- Enable paging: set CR0.PG (bit 31) ------------------------------- */
    {
        uint32_t cr0;
        __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
        cr0 |= (1u << 31);
        __asm__ volatile (
            "mov %0, %%cr0"
            :
            : "r"(cr0)
            : "memory"
        );
    }
}

/*
 * Map the VBE framebuffer physical address into virtual address space using
 * a 4 MB PSE large-page PDE.  The virtual address equals the physical address
 * (identity mapping).  fb_addr is rounded down to a 4 MB boundary.
 *
 * The PDE is marked write-through + cache-disable to prevent stale pixel data
 * from sitting in the CPU cache (suitable for MMIO framebuffers).
 */
void paging_map_framebuffer(uint32_t fb_addr)
{
    uint32_t *page_dir = (uint32_t *)PAGING_DIR_ADDR;
    uint32_t  aligned  = fb_addr & 0xFFC00000u;   /* 4 MB boundary */
    uint32_t  pde_idx  = aligned >> 22;           /* which PDE slot */

    /* Large page: PS=1, Present, R/W (no cache-disable for QEMU perf) */
    page_dir[pde_idx] = aligned | PAGE_LARGE | PAGE_PRESENT | PAGE_WRITABLE;

    /* Flush TLB for new mapping */
    __asm__ volatile (
        "mov %%cr3, %%eax\n"
        "mov %%eax, %%cr3\n"
        ::: "eax", "memory"
    );
}
