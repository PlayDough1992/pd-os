#pragma once

/* ============================================================================
 * PD-Kernel  —  PDFS v2  (PD Filesystem, extended)  (Phase 8f)
 *
 * Extended native filesystem for PD-OS.
 * New in v2 vs v1:
 *   - Subdirectory support (nested dirs, each with own 2-sector dir table)
 *   - Unix-style permissions (owner/group uid + mode rwxrwxrwx)
 *   - Simple write-ahead journal (1 sector at base_lba+3) for dir flushes
 *   - dirent grown from 32 B → 64 B (16 entries per sector, 32 per dir)
 *   - name length extended from 15 → 27 chars
 *   - version field bumped to 2; v1 disks mount read-only
 *
 * On-disk layout (all LBAs absolute):
 *   base_lba+0     Superblock          (1 sector / 512 bytes)
 *   base_lba+1     Journal sector      (1 sector / 512 bytes)
 *   base_lba+2-5   Root directory      (4 sectors, 32 × 64-byte dirents = 2048 B)
 *   base_lba+6+    File / subdir data  (sector-aligned, contiguous per entry)
 *
 * Directory entry (64 bytes):
 *   name[28]        — null-terminated, max 27 chars
 *   start_lba       — first data sector (file) or first dir sector (subdir)
 *   size            — file size in bytes (0 for dirs)
 *   alloc_sectors   — sectors reserved for this entry's data
 *   flags           — PDFS_FLAG_USED | PDFS_FLAG_DIR
 *   uid, gid        — owner / group (maps to users.h uid)
 *   mode            — permission bits: rwxrwxrwx in low 9 bits (Unix layout)
 *   ctime           — creation time (PIT ticks — placeholder until RTC)
 *   dir_sectors     — for dirs: sectors in their dir table (always 2 for now)
 *   reserved        — pad to 64 bytes
 *
 * Permission check (write/create/unlink/mkdir):
 *   passes if:  caller uid == dirent uid  OR  caller has USER_FLAG_ROOT
 *               OR  g_elevated is set (elev prefix was used)
 *   'other write' bit (mode & 0x0002) also grants access to all.
 *
 * Journal protocol:
 *   Before any directory sector write, copy the new sector to the journal
 *   LBA and set journal header dirty=1 + target_lba.  After the real write,
 *   set dirty=0 and clear journal.  On mount, if dirty=1, replay the journal
 *   sector to the target_lba before loading directory.
 * ============================================================================ */

#include "kernel.h"
#include "vfs.h"
#include "users.h"

/* Magic & version ---------------------------------------------------------- */
#define PDFS_MAGIC        0x50444653u   /* "PDFS" in LE                       */
#define PDFS_VERSION      2u

/* Filesystem constants ----------------------------------------------------- */
#define PDFS_SB_SECTORS   1u            /* superblock: 1 sector               */
#define PDFS_JRNL_SECTORS 1u            /* journal:    1 sector               */
#define PDFS_DIR_SECTORS  4u            /* directory:  4 sectors (32×64B=2048B)*/
#define PDFS_MAX_FILES    32u           /* dirents per directory              */
#define PDFS_NAME_LEN     28u           /* bytes in name field (incl. NUL)    */

/* Dirent flags ------------------------------------------------------------- */
#define PDFS_FLAG_USED    0x01u         /* slot is occupied                   */
#define PDFS_FLAG_DIR     0x02u         /* entry is a subdirectory            */

/* Permission mode bits (Unix layout, low 9 bits) --------------------------- */
#define PDFS_MODE_RUSR    0x100u        /* owner read                         */
#define PDFS_MODE_WUSR    0x080u        /* owner write                        */
#define PDFS_MODE_XUSR    0x040u        /* owner execute                      */
#define PDFS_MODE_RGRP    0x020u        /* group read                         */
#define PDFS_MODE_WGRP    0x010u        /* group write                        */
#define PDFS_MODE_XGRP    0x008u        /* group execute                      */
#define PDFS_MODE_ROTH    0x004u        /* other read                         */
#define PDFS_MODE_WOTH    0x002u        /* other write                        */
#define PDFS_MODE_XOTH    0x001u        /* other execute                      */
#define PDFS_MODE_DEFAULT 0x1B4u        /* rwxr-xr-- (owner rwx, grp rx, oth r) */
#define PDFS_MODE_DIR_DEF 0x1EDu        /* rwxr-xr-x for directories          */

/* ---- On-disk superblock (exactly 512 bytes) ------------------------------ */
typedef struct {
    uint32_t magic;             /* PDFS_MAGIC                                */
    uint32_t version;           /* PDFS_VERSION (2)                          */
    uint32_t dir_lba;           /* absolute LBA of the root directory        */
    uint32_t dir_sectors;       /* sectors in root dir table (2)             */
    uint32_t data_lba;          /* absolute LBA where data starts            */
    uint32_t next_free_lba;     /* monotonic allocator pointer               */
    uint32_t jrnl_lba;          /* absolute LBA of journal sector            */
    uint32_t reserved[121];     /* pad to 512 bytes                          */
} __attribute__((packed)) pdfs_superblock_t;

/* ---- On-disk journal header (first 16 bytes of journal sector) ----------- */
typedef struct {
    uint32_t dirty;             /* 1 = pending replay, 0 = clean             */
    uint32_t target_lba;        /* LBA that journal sector should be written to */
    uint32_t reserved[2];
} __attribute__((packed)) pdfs_jrnl_hdr_t;

/* ---- On-disk directory entry (exactly 64 bytes) -------------------------- */
typedef struct {
    char     name[PDFS_NAME_LEN]; /* null-terminated filename (max 27 chars) */
    uint32_t start_lba;           /* first sector of data / subdir table     */
    uint32_t size;                /* file size in bytes (0 for dirs)         */
    uint32_t alloc_sectors;       /* sectors allocated                       */
    uint32_t flags;               /* PDFS_FLAG_USED | PDFS_FLAG_DIR          */
    uint8_t  uid;                 /* owner user id                           */
    uint8_t  gid;                 /* owner group id                          */
    uint16_t mode;                /* permission bits (low 9 bits, Unix style)*/
    uint32_t ctime;               /* creation time (PIT ticks)               */
    uint32_t dir_sectors;         /* for dirs: sectors in their dir table    */
    uint32_t reserved;            /* pad to 64 bytes                         */
} __attribute__((packed)) pdfs_dirent_t;

/* ---- Driver + utilities -------------------------------------------------- */

/* Return the VFS driver struct for registration. */
vfs_driver_t *pdfs_get_driver(void);

/*
 * Set the caller context for permission checks on the next FS operation.
 * Shell must call this before every create/write/unlink/mkdir/chmod/chown.
 */
void pdfs_set_context(const user_t *caller, int elevated);

/*
 * Format PDFS v2 on the disk starting at `base_lba`.
 * Writes superblock + journal + empty root directory.
 * Updates in-memory state so operations work immediately.
 * Returns 0 on success.
 */
int pdfs_format(uint32_t base_lba);

/*
 * Create a subdirectory at `path` with given uid/gid/mode.
 * Returns 0 on success.
 */
int pdfs_mkdir(const char *path, uint8_t uid, uint8_t gid, uint16_t mode);

/*
 * Change mode bits of the file/dir at `path`.
 * Returns 0 on success.
 */
int pdfs_chmod(const char *path, uint16_t mode);

/*
 * Change owner uid/gid of the file/dir at `path`.
 * Returns 0 on success.
 */
int pdfs_chown(const char *path, uint8_t uid, uint8_t gid);

/* Statistics (only valid when mounted). */
uint32_t pdfs_free_sectors(void);
uint32_t pdfs_file_count(void);

/*
 * Fill `out` with the idx-th used root-dir entry (for ls).
 * Returns 0 on success, -1 when idx is out of range.
 */
int pdfs_stat_root(uint32_t idx, pdfs_dirent_t *out);
