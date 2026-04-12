/* ============================================================================
 * PD-Kernel  —  Physical Memory Manager  (bitmap allocator)
 *
 * One bit per 4 KB page frame.  Bit = 1 → used/reserved, bit = 0 → free.
 * The bitmap lives at PMM_BITMAP_ADDR (0x6000) in low physical memory.
 * ============================================================================ */

#include "pmm.h"
#include "e820.h"
#include "kernel.h"

/* Linker-script symbols — defined as arrays so taking their address gives
 * the symbol value without an extra dereference. */
extern uint8_t _kernel_start[];
extern uint8_t _kernel_end[];

/* ---- Bitmap helpers ------------------------------------------------------- */

static uint32_t *bitmap = (uint32_t *)PMM_BITMAP_ADDR;

/* Number of frames actually tracked (set during init from E820 data) */
static uint32_t total_frames = 0;
static uint32_t free_count   = 0;

static void bitmap_set(uint32_t frame)
{
    bitmap[frame / 32] |= (1u << (frame % 32));
}

static void bitmap_clear(uint32_t frame)
{
    bitmap[frame / 32] &= ~(1u << (frame % 32));
}

static int bitmap_test(uint32_t frame)
{
    return (bitmap[frame / 32] >> (frame % 32)) & 1u;
}

/* ---- Range helpers -------------------------------------------------------- */

static uint32_t addr_to_frame(uint32_t addr)
{
    return addr / PMM_PAGE_SIZE;
}

/* Round addr DOWN to page boundary */
static uint32_t page_floor(uint32_t addr)
{
    return addr & ~(PMM_PAGE_SIZE - 1u);
}

/* Round addr UP to next page boundary */
static uint32_t page_ceil(uint32_t addr)
{
    return (addr + PMM_PAGE_SIZE - 1u) & ~(PMM_PAGE_SIZE - 1u);
}

/* ---- Public API ----------------------------------------------------------- */

void pmm_reserve_range(uint32_t start, uint32_t end)
{
    uint32_t frame = addr_to_frame(page_floor(start));
    uint32_t last  = addr_to_frame(page_ceil(end));
    for (; frame < last && frame < total_frames; frame++) {
        if (!bitmap_test(frame)) {
            bitmap_set(frame);
            if (free_count > 0) free_count--;
        }
    }
}

static void pmm_free_range(uint32_t start, uint32_t end)
{
    uint32_t frame = addr_to_frame(page_ceil(start));
    uint32_t last  = addr_to_frame(page_floor(end));
    for (; frame < last && frame < total_frames; frame++) {
        if (bitmap_test(frame)) {
            bitmap_clear(frame);
            free_count++;
        }
    }
}

void pmm_init(void)
{
    uint32_t            n = e820_count();
    const e820_entry_t *e = e820_entries();
    uint32_t            i;
    uint32_t            bitmap_bytes;

    /* Find highest usable address to set total_frames */
    uint32_t highest = 0;
    for (i = 0; i < n; i++) {
        if (e[i].type == E820_TYPE_USABLE) {
            /* Only track up to 4 GB (32-bit PMM) */
            uint64_t top = e[i].base + e[i].length;
            if (top > 0xFFFFFFFFULL) top = 0xFFFFFFFFULL;
            if ((uint32_t)top > highest)
                highest = (uint32_t)top;
        }
    }

    /* Cap at our bitmap capacity */
    total_frames = highest / PMM_PAGE_SIZE;
    if (total_frames > PMM_MAX_FRAMES)
        total_frames = PMM_MAX_FRAMES;

    /* Mark everything as reserved (all bits = 1) */
    bitmap_bytes = (total_frames + 7) / 8;
    {
        uint8_t *b = (uint8_t *)bitmap;
        uint32_t j;
        for (j = 0; j < bitmap_bytes; j++) b[j] = 0xFF;
    }
    free_count = 0;

    /* Free the TYPE_USABLE regions */
    for (i = 0; i < n; i++) {
        if (e[i].type == E820_TYPE_USABLE) {
            uint32_t base = (uint32_t)e[i].base;
            uint32_t len  = (e[i].length > 0xFFFFFFFFULL)
                            ? 0xFFFFFFFFu : (uint32_t)e[i].length;
            pmm_free_range(base, base + len);
        }
    }

    /* ---- Reserve regions that must never be given out ---- */

    /* Low 1 MB: BIOS, IVT, BDA, VGA, ROM */
    pmm_reserve_range(0x00000000u, 0x00100000u);

    /* E820 data written by Stage 2 (0x5000–0x52FF) */
    pmm_reserve_range(0x5000u, 0x5300u);

    /* This bitmap itself (0x6000–0x6FFF) */
    pmm_reserve_range(PMM_BITMAP_ADDR, PMM_BITMAP_ADDR + PMM_PAGE_SIZE);

    /* Kernel image (_kernel_start .. _kernel_end, rounded to pages) */
    pmm_reserve_range((uint32_t)_kernel_start, (uint32_t)_kernel_end);
}

uint32_t pmm_alloc(void)
{
    uint32_t i;

    /* Linear scan through bitmap words for any free bit.
     * Start after frame 256 (1 MB) so low memory is never returned. */
    for (i = 256 / 32; i < (total_frames + 31) / 32; i++) {
        if (bitmap[i] == 0xFFFFFFFFu) continue;   /* all used — skip word */

        /* Find first clear bit in this word */
        uint32_t word = ~bitmap[i];   /* invert: 1 = free */
        /* isolate lowest set bit */
        uint32_t bit = 0;
        uint32_t tmp = word;
        while (!(tmp & 1u)) { tmp >>= 1; bit++; }

        uint32_t frame = i * 32 + bit;
        if (frame >= total_frames) return 0;

        bitmap_set(frame);
        if (free_count > 0) free_count--;
        return frame * PMM_PAGE_SIZE;
    }
    return 0;   /* out of memory */
}

void pmm_free(uint32_t phys_addr)
{
    uint32_t frame = phys_addr / PMM_PAGE_SIZE;
    if (frame == 0 || frame >= total_frames) return;
    if (bitmap_test(frame)) {
        bitmap_clear(frame);
        free_count++;
    }
}

uint32_t pmm_free_frames(void)  { return free_count; }
uint32_t pmm_used_frames(void)  { return total_frames - free_count; }
uint32_t pmm_total_frames(void) { return total_frames; }
