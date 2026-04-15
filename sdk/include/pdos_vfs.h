#pragma once

/* ============================================================================
 * PD-OS DE SDK  —  pdos_vfs.h
 * Filesystem node type visible to DE binaries.
 * ============================================================================ */

#include "pdos_types.h"

#define VFS_NAME_MAX  32

typedef struct {
    char     name[VFS_NAME_MAX];
    uint32_t size;
    uint32_t inode;
    uint8_t  is_dir;
    uint8_t  mount_idx;
    uint8_t  pad[2];
} vfs_node_t;
