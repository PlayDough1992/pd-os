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

    /* ---- Enable PSE (4 MB pages) in CR4 before building tables ----------- */
    {
        uint32_t cr4;
        __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));
        cr4 |= (1u << 4);   /* CR4.PSE */
        __asm__ volatile ("mov %0, %%cr4" : : "r"(cr4) : "memory");
    }

    /* ---- Zero the page directory ----------------------------------------- */
    for (i = 0; i < 1024; i++)
        page_dir[i] = 0;

    /* ---- Build Page Table 0: identity-map 0x000000 – 0x3FFFFF (4 KB pg) -- */
    for (i = 0; i < 1024; i++)
        page_table0[i] = (i * 0x1000u) | PAGE_PRESENT | PAGE_WRITABLE;

    /* ---- Install PDE[0] → Page Table 0 (4 KB pages, no PSE bit) ----------- */
    page_dir[0] = PAGING_TABLE0_ADDR | PAGE_PRESENT | PAGE_WRITABLE;

    /* ---- Load CR3 --------------------------------------------------------- */
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

void paging_map_frame(uint32_t phys_addr)
{
    uint32_t *page_dir = (uint32_t *)PAGING_DIR_ADDR;
    /* Which 4 MB chunk? */
    uint32_t pde_idx = phys_addr >> 22;
    /* 4 MB-aligned base of this chunk */
    uint32_t base    = pde_idx << 22;
    /* PSE PDE: physical base | Present | Writable | PSE */
    page_dir[pde_idx] = base | PAGE_PRESENT | PAGE_WRITABLE | PAGE_PSE;
    /* Flush TLB */
    __asm__ volatile (
        "mov %%cr3, %%eax ; mov %%eax, %%cr3"
        : : : "eax", "memory"
    );
}
