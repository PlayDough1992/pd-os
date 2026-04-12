#pragma once

/* ============================================================================
 * PD-Kernel  —  PDFS  (PD Filesystem)  (Phase 8b)
 *
 * Simple flat filesystem purpose-built for the first PD-OS data partition.
 * No subdirectories, no permissions, no journaling — designed to be
 * correct, readable, and a clean reference for future FS implementations.
 *
 * On-disk layout (all LBAs absolute):
 *   LBA 69          Superblock          (1 sector / 512 bytes)
 *   LBA 70-71       Root directory      (2 sectors / 1024 bytes, 32 entries)
 *   LBA 72+         File data           (sector-aligned, contiguous per file)
 *
 * Block structures:
 *
 *   pdfs_superblock_t  — exactly 512 bytes, magic 0x50444653 ("PDFS")
 *   pdfs_dirent_t      — exactly 32 bytes, 16 per sector, 32 total in dir
 *
 * Allocation strategy (v1):
 *   next_free_lba advances monotonically.  Deleted files leave holes that
 *   are reclaimed only by a full reformat (mkpdfs).  Sufficient for a
 *   1.44 MB development disk; a compacting GC can be added later.
 * ============================================================================ */

#include "kernel.h"
#include "vfs.h"

/* Magic & version ---------------------------------------------------------- */
#define PDFS_MAGIC        0x50444653u   /* "PDFS" in little-endian memory     */
#define PDFS_VERSION      1u

/* Filesystem constants ----------------------------------------------------- */
#define PDFS_SB_SECTORS   1u            /* superblock occupies 1 sector       */
#define PDFS_DIR_SECTORS  2u            /* directory occupies 2 sectors       */
#define PDFS_MAX_FILES    32u           /* max files (32 entries × 32 bytes)  */
#define PDFS_NAME_LEN     16u           /* bytes in dirent name (incl. NUL)   */

/* Dirent flags ------------------------------------------------------------- */
#define PDFS_FLAG_USED    1u

/* ---- On-disk superblock (exactly 512 bytes) ------------------------------ */
typedef struct {
    uint32_t magic;             /* PDFS_MAGIC                                */
    uint32_t version;           /* PDFS_VERSION                              */
    uint32_t dir_lba;           /* absolute LBA of the root directory        */
    uint32_t dir_sectors;       /* number of sectors the directory uses      */
    uint32_t data_lba;          /* absolute LBA where file data starts       */
    uint32_t next_free_lba;     /* next sector to allocate for new file data */
    uint32_t reserved[122];     /* pad to 512 bytes (24 + 488 = 512)         */
} __attribute__((packed)) pdfs_superblock_t;

/* ---- On-disk directory entry (exactly 32 bytes) -------------------------- */
typedef struct {
    char     name[PDFS_NAME_LEN];   /* null-terminated filename (max 15 ch) */
    uint32_t start_lba;             /* first sector of file content          */
    uint32_t size;                  /* file size in bytes                    */
    uint32_t alloc_sectors;         /* sectors allocated for this file       */
    uint32_t flags;                 /* PDFS_FLAG_USED etc.                   */
} __attribute__((packed)) pdfs_dirent_t;

/* ---- Driver + utilities -------------------------------------------------- */

/* Return the VFS driver struct for registration. */
vfs_driver_t *pdfs_get_driver(void);

/*
 * Format PDFS on the disk starting at `base_lba`.
 * Writes a fresh superblock + empty directory.
 * Also updates the in-memory driver state so operations work immediately.
 * Returns 0 on success.
 */
int pdfs_format(uint32_t base_lba);

/* Statistics (only valid when mounted). */
uint32_t pdfs_free_sectors(void);
uint32_t pdfs_file_count(void);
