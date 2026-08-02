#pragma once

/* ============================================================================
 * PD-Kernel  —  FAT32 Filesystem Driver  (Phase 8c)
 *
 * Implements the vfs_driver_t interface for FAT32.
 * Supports: root-directory files, 8.3 names, read + write + create + unlink.
 *
 * Volume layout (base_lba = LBA of the BPB sector):
 *   base_lba + 0               BPB (boot parameter block / VBR)
 *   base_lba + 1               FSInfo
 *   base_lba + [2 .. res-1]    Reserved (zeros)
 *   base_lba + res             FAT1  (fat_sectors sectors)
 *   base_lba + res + fat       FAT2  (fat_sectors sectors)
 *   base_lba + res + 2*fat     Data area — cluster 2 = root directory
 * ============================================================================ */

#include "kernel.h"
#include "vfs.h"

/* Returns the registered VFS driver vtable for FAT32. */
vfs_driver_t *fat32_get_driver(void);
