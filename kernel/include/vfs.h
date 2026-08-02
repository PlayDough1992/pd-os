#pragma once

/* ============================================================================
 * PD-Kernel  —  Virtual Filesystem Switch  (Phase 8b)
 *
 * Thin dispatch layer between kernel/shell and filesystem drivers.
 * Each filesystem registers a vfs_driver_t; mount points map paths
 * to driver + base-LBA pairs.
 *
 * Designed for extensibility: FAT32, ext2, NTFS-ro plug in as additional
 * drivers registered with vfs_register() and mounted with vfs_mount().
 *
 * Path convention: absolute from root, e.g. "/readme.txt", "/mnt/fat/doc.txt"
 * ============================================================================ */

#include "kernel.h"

/* Limits ------------------------------------------------------------------- */
#define VFS_MAX_DRIVERS  8
#define VFS_MAX_MOUNTS   8
#define VFS_NAME_MAX     32   /* max filename length including NUL            */

/* Flags -------------------------------------------------------------------- */
#define VFS_FLAG_FILE    0u
#define VFS_FLAG_DIR     1u

/* ---- VFS node (abstract file / directory entry) -------------------------- */
typedef struct {
    char     name[VFS_NAME_MAX];
    uint32_t size;       /* file size in bytes (0 for dirs)                  */
    uint32_t inode;      /* driver-specific handle (e.g. dirent index)       */
    uint8_t  is_dir;     /* 1 if this node is a directory                    */
    uint8_t  mount_idx;  /* index into the internal mount table              */
    uint8_t  pad[2];
} vfs_node_t;

/* ---- Filesystem driver vtable ------------------------------------------- */
/*
 * Each FS driver implements these callbacks.
 * `base_lba` is the absolute LBA of the FS's first sector (superblock).
 * All paths passed to open/create/unlink are relative to the mount root
 * (i.e. the leading slash and mount-point prefix have been stripped).
 */
typedef struct vfs_driver {
    const char *name;   /* e.g. "pdfs", "fat32", "ext2"                      */

    int (*mount)  (uint32_t base_lba);
    int (*open)   (const char *name, vfs_node_t *out);
    int (*read)   (vfs_node_t *node, uint32_t offset, uint32_t len, void *buf);
    int (*write)  (vfs_node_t *node, uint32_t offset, uint32_t len, const void *buf);
    int (*create) (const char *name);
    int (*unlink) (const char *name);
    int (*readdir)(uint32_t idx, vfs_node_t *out);
} vfs_driver_t;

/* ---- Public API ---------------------------------------------------------- */

/* Zero internal state.  Call once at boot. */
void vfs_init(void);

/* Register a filesystem driver so it can be used with vfs_mount(). */
int  vfs_register(vfs_driver_t *drv);

/*
 * Mount a filesystem at `point` (e.g. "/").
 * `fsname` must match a previously registered driver's name.
 * `lba` is the absolute LBA of the filesystem's superblock.
 * Returns 0 on success, negative on error.
 */
int  vfs_mount(const char *point, const char *fsname, uint32_t lba);

/* Open a file by absolute path. Fills *out; returns 0 or negative. */
int  vfs_open   (const char *path, vfs_node_t *out);

/* Read `len` bytes at `offset` from `node` into `buf`. */
int  vfs_read   (vfs_node_t *node, uint32_t offset, uint32_t len, void *buf);

/* Write `len` bytes at `offset` from `buf` into `node`. */
int  vfs_write  (vfs_node_t *node, uint32_t offset, uint32_t len, const void *buf);

/* Create a new file at absolute path `path`. Returns 0 or negative. */
int  vfs_create (const char *path);

/* Delete the file at absolute path `path`. Returns 0 or negative. */
int  vfs_unlink (const char *path);

/*
 * Read the `idx`-th entry in the directory at `path`.
 * Returns 0 and fills *out when entry exists; returns negative when exhausted.
 */
int  vfs_readdir(const char *path, uint32_t idx, vfs_node_t *out);
