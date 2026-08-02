#pragma once

/* ============================================================================
 * PD-Kernel  —  ext2 Filesystem Driver  (Phase 8d)
 *
 * Implements the vfs_driver_t interface for the ext2 (Second Extended FS).
 * Mounts at /mnt/ext2.  Supports read + write + create + unlink + readdir
 * for files in any directory (full path traversal).
 *
 * On-disk layout (base_lba = LBA of the first superblock sector):
 *   base_lba + 0-1      Padding (ext2 SB starts at byte 1024 = sector 2
 *                       relative to partition start)
 *   base_lba + 2        Superblock  (1024 bytes, occupies 2 × 512-byte sectors)
 *   base_lba + 4        Block Group Descriptor Table
 *   ...                 Block bitmap, inode bitmap, inode table, data blocks
 *
 * Limitations (v1):
 *   - Single block group only (covers volumes up to ~128 MB at 1 KB blocks)
 *   - 1 KB block size only
 *   - No journal (ext2, not ext3/4)
 *   - Filenames up to 255 bytes (EXT2_NAME_LEN)
 * ============================================================================ */

#include "kernel.h"
#include "vfs.h"

/* Returns the registered VFS driver vtable for ext2. */
vfs_driver_t *ext2_get_driver(void);
