#pragma once

/* ============================================================================
 * PD-Kernel  —  Kernel Heap  (Phase 7d)
 *
 * A simple first-fit free-list heap backed by a 1 MB region of physical
 * memory that lives entirely within the identity-mapped first 4 MB.
 *
 * Memory layout:
 *   0x200000–0x2FFFFF   Heap pool (1 MB, within 4 MB identity map)
 *
 * Block structure (16 bytes per header):
 *   [ magic | size | free | *next ] → payload bytes
 *
 * pmm_reserve_range() is called during kheap_init() so the PMM never
 * hands out these frames to pmm_alloc() callers.
 * ============================================================================ */

#include "kernel.h"

/* Heap region ---------------------------------------------------------------- */
#define KHEAP_START   0x200000u                         /* first heap byte   */
#define KHEAP_SIZE    (1u * 1024u * 1024u)              /* 1 MB pool         */
#define KHEAP_END     (KHEAP_START + KHEAP_SIZE)        /* exclusive end     */

/* Sentinel magic for detecting heap corruption ------------------------------ */
#define KHEAP_MAGIC   0xDEADBEEFu

/*
 * Block header prepended to every allocation.
 * Payload immediately follows in memory.
 */
typedef struct kheap_block {
    uint32_t            magic;   /* KHEAP_MAGIC — detects corruption / bad ptr */
    uint32_t            size;    /* payload bytes  (NOT including header)       */
    uint32_t            free;    /* 1 = free, 0 = allocated                    */
    struct kheap_block *next;    /* next block in the linked list               */
} kheap_block_t;

/*
 * Initialise the kernel heap.
 * Must be called AFTER paging_init() so heap addresses are mapped.
 * Calls pmm_reserve_range() to claim the backing physical frames.
 */
void kheap_init(void);

/*
 * Allocate at least `size` bytes.
 * Returns a pointer to the payload (never to the header).
 * Returns NULL on out-of-memory.
 * Aligned to 4 bytes.
 */
void *kmalloc(uint32_t size);

/*
 * Free a pointer previously returned by kmalloc().
 * Coalesces adjacent free blocks.
 * Safe to call with NULL.
 */
void kfree(void *ptr);

/* Statistics ---------------------------------------------------------------- */
uint32_t kheap_free_bytes(void);
uint32_t kheap_used_bytes(void);
uint32_t kheap_block_count(void);
