/* ============================================================================
 * PD-Kernel  —  Kernel Heap  (Phase 7d)
 *
 * First-fit free-list allocator over a fixed 1 MB region (0x200000–0x2FFFFF).
 *
 * Each block in the list looks like:
 *   ┌────────────────────┐
 *   │  kheap_block_t     │  ← 16-byte header
 *   │  (magic/size/free) │
 *   ├────────────────────┤
 *   │  payload …         │  ← what kmalloc() returns
 *   └────────────────────┘
 *
 * On init the whole pool is one free block.
 * kmalloc(): first-fit search; splits the block if enough headroom remains.
 * kfree():   marks free, then coalesces all adjacent free pairs.
 * ============================================================================ */

#include "kheap.h"
#include "pmm.h"

#define HEADER_SZ ((uint32_t)sizeof(kheap_block_t))   /* 16 bytes */
#define MIN_SPLIT (HEADER_SZ + 4u)                    /* minimum leftover to split */

static kheap_block_t *heap_head = (kheap_block_t *)KHEAP_START;

/* ---- kheap_init ----------------------------------------------------------- */

void kheap_init(void)
{
    /* Claim the backing physical frames so pmm_alloc() never reuses them */
    pmm_reserve_range(KHEAP_START, KHEAP_END);

    /* Single free block spanning the entire pool */
    heap_head->magic = KHEAP_MAGIC;
    heap_head->size  = KHEAP_SIZE - HEADER_SZ;
    heap_head->free  = 1;
    heap_head->next  = NULL;
}

/* ---- kmalloc -------------------------------------------------------------- */

void *kmalloc(uint32_t size)
{
    kheap_block_t *blk;

    if (size == 0)
        return NULL;

    /* Align payload size to 4 bytes */
    size = (size + 3u) & ~3u;

    blk = heap_head;
    while (blk) {
        if (blk->free && blk->size >= size) {

            /* Split only if the remainder fits a full header + minimum payload */
            if (blk->size >= size + MIN_SPLIT) {
                kheap_block_t *split =
                    (kheap_block_t *)((uint8_t *)blk + HEADER_SZ + size);
                split->magic = KHEAP_MAGIC;
                split->size  = blk->size - size - HEADER_SZ;
                split->free  = 1;
                split->next  = blk->next;
                blk->next    = split;
                blk->size    = size;
            }

            blk->free = 0;
            return (void *)((uint8_t *)blk + HEADER_SZ);
        }
        blk = blk->next;
    }

    return NULL;   /* out of heap memory */
}

/* ---- kfree ---------------------------------------------------------------- */

void kfree(void *ptr)
{
    kheap_block_t *blk, *cur;

    if (!ptr)
        return;

    blk = (kheap_block_t *)((uint8_t *)ptr - HEADER_SZ);

    /* Validate — don't corrupt the heap on a bad pointer */
    if (blk->magic != KHEAP_MAGIC)
        return;
    if (blk->free)
        return;   /* double-free: silently ignore */

    blk->free = 1;

    /* Coalesce all adjacent free pairs in one pass */
    cur = heap_head;
    while (cur && cur->next) {
        if (cur->free && cur->next->free) {
            cur->size += HEADER_SZ + cur->next->size;
            cur->next  = cur->next->next;
            /* Don't advance — coalesce again if the new next is also free */
        } else {
            cur = cur->next;
        }
    }
}

/* ---- Statistics ----------------------------------------------------------- */

uint32_t kheap_free_bytes(void)
{
    uint32_t total = 0;
    kheap_block_t *cur = heap_head;
    while (cur) {
        if (cur->free) total += cur->size;
        cur = cur->next;
    }
    return total;
}

uint32_t kheap_used_bytes(void)
{
    uint32_t total = 0;
    kheap_block_t *cur = heap_head;
    while (cur) {
        if (!cur->free) total += cur->size;
        cur = cur->next;
    }
    return total;
}

uint32_t kheap_block_count(void)
{
    uint32_t n = 0;
    kheap_block_t *cur = heap_head;
    while (cur) { n++; cur = cur->next; }
    return n;
}
