#pragma once

/* ============================================================================
 * PD-Kernel  —  NTFS Read-Only Driver  (Phase 8e)
 *
 * Implements the vfs_driver_t interface for NTFS (read-only).
 * Mounts at /mnt/ntfs.  Supports open, read, readdir.
 * write, create, and unlink always return -1 (read-only).
 *
 * On-disk layout assumptions:
 *   base_lba + 0        VBR / Boot sector (NTFS BPB)
 *   base_lba + ...      $MFT (Master File Table)
 *
 * On-disk structures parsed:
 *   VBR         — bytes-per-sector, sectors-per-cluster, $MFT LCN
 *   MFT record  — FILE signature, attribute list
 *   $FILE_NAME  — file name (Unicode → ASCII, uppercase)
 *   $DATA       — file data (resident or non-resident runs)
 *   $INDEX_ROOT — resident directory index (B-tree node, root only)
 *
 * Limitations (v1 — read-only):
 *   - Single sector-per-cluster assumed (512 B clusters)
 *   - Resident $DATA only for file reads (non-resident $DATA up to 2 runs)
 *   - Root directory index only (no deep subdirectory traversal)
 *   - Filenames truncated to VFS_NAME_MAX-1 chars; non-ASCII mapped to '?'
 *   - No $ATTRIBUTE_LIST support (assumes all attrs fit in one MFT record)
 *   - No USN fixup applied (fixup bytes not corrected in sector reads)
 * ============================================================================ */

#include "kernel.h"
#include "vfs.h"

/* Returns the registered VFS driver vtable for NTFS. */
vfs_driver_t *ntfs_get_driver(void);
