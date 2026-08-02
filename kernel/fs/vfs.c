/* ============================================================================
 * PD-Kernel  —  Virtual Filesystem Switch  (Phase 8b)
 *
 * Maintains a driver registry (up to VFS_MAX_DRIVERS entries) and a mount
 * table (up to VFS_MAX_MOUNTS entries).  All dispatch functions find the best
 * matching mount point for a given path before forwarding to the driver.
 *
 * Adding a new filesystem driver (e.g. FAT32) requires only:
 *   1. Implement the vfs_driver_t callbacks
 *   2. vfs_register(&fat32_driver)
 *   3. vfs_mount("/mnt/fat", "fat32", first_sector)
 * ============================================================================ */

#include "vfs.h"

/* ---- Internal state ------------------------------------------------------- */

static vfs_driver_t *drivers[VFS_MAX_DRIVERS];
static int           driver_count = 0;

typedef struct {
    char          point[16];    /* mount path, e.g. "/"  or "/mnt/fat"       */
    vfs_driver_t *driver;
    uint32_t      base_lba;
} mount_t;

static mount_t mounts[VFS_MAX_MOUNTS];
static int     mount_count = 0;

/* ---- Helpers -------------------------------------------------------------- */

static int vfs_strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static vfs_driver_t *find_driver(const char *name)
{
    int i;
    for (i = 0; i < driver_count; i++)
        if (vfs_strcmp(drivers[i]->name, name) == 0)
            return drivers[i];
    return NULL;
}

/*
 * Find the mount with the longest prefix that matches `path`.
 * Returns NULL if no mount point matches.
 */
static mount_t *find_mount(const char *path)
{
    int best = -1, best_len = -1, i;
    for (i = 0; i < mount_count; i++) {
        const char *mp = mounts[i].point;
        int j = 0;
        while (mp[j] && mp[j] == path[j]) j++;
        if (mp[j] == '\0' && j > best_len) {
            best = i;
            best_len = j;
        }
    }
    if (best < 0) return NULL;
    return &mounts[best];
}

/*
 * Strip the mount-point prefix from `path`.
 * E.g. path="/readme.txt", point="/" → "readme.txt"
 *      path="/mnt/fat/x",  point="/mnt/fat" → "x"
 */
static const char *strip_prefix(const char *path, const char *point)
{
    while (*point && *path == *point) { path++; point++; }
    if (*path == '/') path++;
    return path;
}

/* ---- Public API ----------------------------------------------------------- */

void vfs_init(void)
{
    int i;
    driver_count = 0;
    mount_count  = 0;
    for (i = 0; i < VFS_MAX_DRIVERS; i++) drivers[i] = NULL;
    for (i = 0; i < VFS_MAX_MOUNTS;  i++) {
        mounts[i].driver   = NULL;
        mounts[i].point[0] = '\0';
    }
}

int vfs_register(vfs_driver_t *drv)
{
    if (!drv || driver_count >= VFS_MAX_DRIVERS) return -1;
    drivers[driver_count++] = drv;
    return 0;
}

int vfs_mount(const char *point, const char *fsname, uint32_t lba)
{
    vfs_driver_t *drv;
    int i;

    if (mount_count >= VFS_MAX_MOUNTS) return -1;
    drv = find_driver(fsname);
    if (!drv) return -2;
    if (drv->mount(lba) != 0) return -3;

    for (i = 0; i < 15 && point[i]; i++) mounts[mount_count].point[i] = point[i];
    mounts[mount_count].point[i] = '\0';
    mounts[mount_count].driver   = drv;
    mounts[mount_count].base_lba = lba;
    mount_count++;
    return 0;
}

int vfs_open(const char *path, vfs_node_t *out)
{
    mount_t *m = find_mount(path);
    int r;
    if (!m) return -1;
    r = m->driver->open(strip_prefix(path, m->point), out);
    if (r == 0) out->mount_idx = (uint8_t)(m - mounts);
    return r;
}

int vfs_read(vfs_node_t *node, uint32_t offset, uint32_t len, void *buf)
{
    if ((int)node->mount_idx >= mount_count || !mounts[node->mount_idx].driver)
        return -1;
    return mounts[node->mount_idx].driver->read(node, offset, len, buf);
}

int vfs_write(vfs_node_t *node, uint32_t offset, uint32_t len, const void *buf)
{
    if ((int)node->mount_idx >= mount_count || !mounts[node->mount_idx].driver)
        return -1;
    return mounts[node->mount_idx].driver->write(node, offset, len, buf);
}

int vfs_create(const char *path)
{
    mount_t *m = find_mount(path);
    if (!m) return -1;
    return m->driver->create(strip_prefix(path, m->point));
}

int vfs_unlink(const char *path)
{
    mount_t *m = find_mount(path);
    if (!m) return -1;
    return m->driver->unlink(strip_prefix(path, m->point));
}

int vfs_readdir(const char *path, uint32_t idx, vfs_node_t *out)
{
    mount_t *m = find_mount(path);
    int r;
    if (!m) return -1;
    r = m->driver->readdir(idx, out);
    if (r == 0) out->mount_idx = (uint8_t)(m - mounts);
    return r;
}
