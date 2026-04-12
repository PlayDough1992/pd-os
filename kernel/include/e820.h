#pragma once

/* ============================================================================
 * PD-Kernel  —  E820 BIOS physical memory map
 *
 * The bootloader (Stage 2) calls BIOS INT 15h/E820 in real mode and stores
 * the results at two fixed physical addresses BEFORE entering protected mode:
 *
 *   0x5000  uint32_t  e820_count   — number of valid entries
 *   0x5004  e820_entry_t[]         — array of 24-byte entries
 *
 * These addresses lie in conventional free memory and are NOT overwritten
 * by the kernel copy or the protected-mode stack.  The kernel reads them
 * directly as physical pointers (no paging yet at this stage).
 * When paging is added, the first 4 MB will be identity-mapped so these
 * pointers remain valid.
 * ============================================================================ */

#include "kernel.h"

/* Physical addresses written by the bootloader (must match stage2.asm) */
#define E820_PHYS_COUNT    0x5000u
#define E820_PHYS_ENTRIES  0x5004u

#define E820_MAX_ENTRIES   32

/* Entry type values (ACPI 3.0) */
#define E820_TYPE_USABLE   1   /* free RAM — safe to use             */
#define E820_TYPE_RESERVED 2   /* hardware / firmware reserved       */
#define E820_TYPE_ACPI     3   /* ACPI reclaimable after ACPI tables parsed */
#define E820_TYPE_NVS      4   /* ACPI NVS — must preserve over S3   */
#define E820_TYPE_BAD      5   /* bad memory — do not use            */

typedef struct {
    uint64_t base;    /* physical base address                        */
    uint64_t length;  /* region length in bytes (0 = skip)            */
    uint32_t type;    /* E820_TYPE_* above                            */
    uint32_t acpi;    /* ACPI 3.0 extended attributes (bit 0 = valid) */
} __attribute__((packed)) e820_entry_t;

/* Return the number of entries the bootloader recorded (0 = E820 unsupported) */
uint32_t            e820_count(void);

/* Return pointer to the first entry in the bootloader-built table */
const e820_entry_t *e820_entries(void);

/* Return the total usable RAM in bytes (sum of TYPE_USABLE regions) */
uint64_t            e820_usable_bytes(void);

/* Print the full map to the kernel console */
void                e820_print(void);
