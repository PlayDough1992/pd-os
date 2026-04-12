#pragma once

/* ============================================================================
 * PD-Kernel  —  ATA/IDE PIO Driver  (Phase 8a)
 *
 * Implements 28-bit LBA PIO access to the primary ATA channel,
 * master drive only.  No DMA, no IRQs — pure polling mode.
 *
 * I/O port map (primary channel):
 *   0x1F0   Data register         (16-bit reads/writes)
 *   0x1F1   Error/Features
 *   0x1F2   Sector count
 *   0x1F3   LBA bits  0-7
 *   0x1F4   LBA bits  8-15
 *   0x1F5   LBA bits 16-23
 *   0x1F6   Drive/Head  (bit 6 = LBA mode, bit 4 = drive select 0/1)
 *   0x1F7   Command / Status
 *   0x3F6   Alternate status / Device-control
 * ============================================================================ */

#include "kernel.h"

/* ATA sector payload size --------------------------------------------------  */
#define ATA_SECTOR_SIZE  512u

/* Drive info populated by ata_init() --------------------------------------- */
typedef struct {
    uint8_t  present;           /* 1 = drive found via IDENTIFY            */
    uint8_t  lba_supported;     /* 1 = drive supports 28-bit LBA           */
    char     model[41];         /* null-terminated model string (40 chars) */
    uint32_t total_sectors;     /* total addressable 28-bit LBA sectors     */
} ata_drive_t;

/*
 * Probe the primary master drive.
 * Sends IDENTIFY; fills the internal ata_drive_t.
 * Must be called after interrupts are stable (though the driver polls).
 */
void ata_init(void);

/* Return pointer to the populated (or empty) drive info. */
const ata_drive_t *ata_get_drive(void);

/*
 * Read `count` 512-byte sectors starting at 28-bit LBA address `lba`
 * into the caller's buffer `buf` (must be >= count * 512 bytes).
 *
 * Returns  0 on success.
 * Returns -1 if the drive is not present, the LBA is out of range,
 *            or a hardware error is reported.
 */
int ata_read_sectors(uint32_t lba, uint8_t count, void *buf);

/*
 * Write `count` 512-byte sectors from `buf` to LBA `lba`.
 * Follows each sector write with a cache-flush command.
 *
 * Returns  0 on success, -1 on error.
 */
int ata_write_sectors(uint32_t lba, uint8_t count, const void *buf);
