#pragma once

/* ============================================================================
 * PD-Kernel  —  PDFS v3  (PD Filesystem, chained directories)
 *
 * New in v3 vs v2:
 *   - Directory entries are no longer stored in a fixed-size table.
 *   - Each directory sector (512 bytes) holds 8 × 64-byte dirent slots.
 *   - Slot 7 is the CHAIN LINK slot:  if PDFS_FLAG_CHAIN is set its
 *     `start_lba` field points to the next directory sector in the chain.
 *     Slots 0–6 are usable entries (7 per sector).
 *   - When a directory fills up, one new sector is allocated from
 *     `next_free_lba` and linked via the chain slot.  No hard per-directory
 *     entry limit — only disk space limits directory size.
 *   - Inode encoding changes: vfs_node_t.inode = INODE_ENCODE(sec_lba, slot)
 *     where sec_lba is the SPECIFIC sector containing the entry and
 *     slot (0–6) is the position within that sector.
 *   - version field bumped to 3; v2/v1 disks mount read-only.
 *
 * On-disk layout (all LBAs absolute):
 *   base_lba+0     Superblock          (1 sector)
 *   base_lba+1     Journal slot        (1 sector, reserved)
 *   base_lba+2     Root dir sector 0   (1 sector, chain-extended as needed)
 *   base_lba+3+    File / subdir data + additional dir chain sectors
 *
 * Directory entry (64 bytes, unchanged from v2):
 *   name[28]        — null-terminated, max 27 chars
 *   start_lba       — first data sector (file) or first dir sector (subdir)
 *                     for CHAIN entries: LBA of the next dir sector
 *   size            — file size in bytes (0 for dirs and chain entries)
 *   alloc_sectors   — sectors allocated (0 for chain entries)
 *   flags           — PDFS_FLAG_USED | PDFS_FLAG_DIR | PDFS_FLAG_CHAIN
 *   uid, gid        — owner / group
 *   mode            — rwxrwxrwx in low 9 bits (Unix layout)
 *   ctime           — creation time (PIT ticks)
 *   dir_sectors     — for dirs: 1 initially, grows with chain
 *   reserved        — pad to 64 bytes
 *
 * Permission check (write/create/unlink/mkdir):
 *   passes if:  caller uid == dirent uid  OR  caller has USER_FLAG_ROOT
 *               OR  g_elevated is set (elev prefix was used)
 *   'other write' bit (mode & 0x0002) also grants access to all.
 * ============================================================================ */

#include "kernel.h"
#include "vfs.h"
#include "users.h"

/* Magic & version ---------------------------------------------------------- */
#define PDFS_MAGIC        0x50444653u   /* "PDFS" in LE                       */
#define PDFS_VERSION      3u

/* Filesystem constants ----------------------------------------------------- */
#define PDFS_SB_SECTORS   1u            /* superblock: 1 sector               */
#define PDFS_JRNL_SECTORS 1u            /* journal:    1 sector (reserved)    */
#define PDFS_NAME_LEN     28u           /* bytes in name field (incl. NUL)    */
#define PDFS_CHAIN_SLOTS  7u            /* usable dirent slots per sector     */
#define PDFS_CHAIN_LINK   7u            /* slot index of the chain-link       */

/* Dirent flags ------------------------------------------------------------- */
#define PDFS_FLAG_USED    0x01u         /* slot is occupied                   */
#define PDFS_FLAG_DIR     0x02u         /* entry is a subdirectory            */
#define PDFS_FLAG_CHAIN   0x04u         /* slot 7 chain link (v3+)            */

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

/* ---- On-disk directory entry (exactly 64 bytes — unchanged from v2) ------ */
/*
 * For regular files/dirs:  name, start_lba, size, alloc_sectors, flags, ...
 * For chain-link entries (slot 7 when PDFS_FLAG_CHAIN set):
 *   start_lba = LBA of the next directory sector in the chain
 *   All other fields are ignored / zeroed.
 */
typedef struct {
    char     name[PDFS_NAME_LEN]; /* null-terminated filename (max 27 chars) */
    uint32_t start_lba;           /* file/dir: first data sector             */
                                  /* chain:    next dir sector LBA           */
    uint32_t size;                /* file size in bytes (0 for dirs/chain)   */
    uint32_t alloc_sectors;       /* sectors allocated (0 for chain entries) */
    uint32_t flags;               /* PDFS_FLAG_USED | PDFS_FLAG_DIR | _CHAIN */
    uint8_t  uid;                 /* owner user id                           */
    uint8_t  gid;                 /* owner group id                          */
    uint16_t mode;                /* permission bits (low 9 bits, Unix style)*/
    uint32_t ctime;               /* creation time (PIT ticks)               */
    uint32_t dir_sectors;         /* for dirs: 1 initially, grows with chain */
    uint32_t reserved[2];         /* pad to 64 bytes (MUST keep struct = 64B)*/
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
 * Format PDFS v3 on the disk starting at `base_lba`.
 * Writes superblock + journal slot + empty root directory sector.
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

/*
 * Create the standard PD-OS / FHS directory tree.
 * Idempotent: safe to call on every boot and after mkpdfs reformat.
 */
void pdfs_scaffold(void);

/*
 * Create /home/<username> and the standard user subdirectories
 * (Desktop, Documents, Downloads, Pictures, Videos, Music, Templates, Public).
 * The home dir and all private subdirs are owned by uid with mode 0700
 * (rwx------).  Public is 0755 (rwxr-xr-x).
 * Called internally by pdfs_scaffold for built-in users and by cmd_adduser.
 */
void pdfs_create_home(const char *username, uint8_t uid);

/* Statistics (only valid when mounted). */
uint32_t pdfs_free_sectors(void);
uint32_t pdfs_file_count(void);

/* Returns 1 if the mounted filesystem is read-only (old version / bad magic). */
int      pdfs_is_ro(void);

/*
 * Fill `out` with the idx-th used root-dir entry (for ls).
 * Returns 0 on success, -1 when idx is out of range.
 */
int pdfs_stat_root(uint32_t idx, pdfs_dirent_t *out);

/*
 * Fill `out` with the idx-th used entry of the directory at `path`.
 * path "/" or "" lists the root directory (same as pdfs_stat_root).
 * Returns 0 on success, -1 on failure.
 */
int pdfs_stat_dir(const char *path, uint32_t idx, pdfs_dirent_t *out);
