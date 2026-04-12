/* ============================================================================
 * PD-Kernel  —  PDFS Driver  (Phase 8b)
 *
 * Implements the vfs_driver_t interface for the PDFS filesystem.
 * On-disk layout from pdfs.h:
 *   base_lba+0     Superblock  (1 sector)
 *   base_lba+1..2  Directory   (2 sectors, 32 × 32-byte dirents)
 *   base_lba+3+    File data   (contiguous, sector-aligned per file)
 *
 * In-memory state: g_sb (superblock) and g_dir[32] (directory) are kept
 * write-through — flushed to disk immediately on any modification.
 * ============================================================================ */

#include "pdfs.h"
#include "ata.h"

/* ---- Module state --------------------------------------------------------- */

static pdfs_superblock_t g_sb;
static pdfs_dirent_t     g_dir[PDFS_MAX_FILES];
static int               g_mounted  = 0;
static uint32_t          g_base_lba = 0;

/* ---- Internal helpers ----------------------------------------------------- */

static int pd_strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static uint32_t pd_strlen(const char *s)
{
    uint32_t n = 0;
    while (s[n]) n++;
    return n;
}

static void pd_memcpy(void *dst, const void *src, uint32_t n)
{
    uint8_t       *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
}

static void pd_memzero(void *dst, uint32_t n)
{
    uint8_t *d = (uint8_t *)dst;
    while (n--) *d++ = 0;
}

static void pd_strncpy(char *dst, const char *src, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n - 1u && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
}

static void pdfs_flush_sb(void)
{
    ata_write_sectors(g_base_lba, 1, &g_sb);
}

static void pdfs_flush_dir(void)
{
    ata_write_sectors(g_sb.dir_lba, (uint8_t)g_sb.dir_sectors, g_dir);
}

/* ---- VFS driver callbacks ------------------------------------------------- */

static int pdfs_mount(uint32_t base_lba)
{
    uint8_t buf[512];

    g_base_lba = base_lba;

    if (ata_read_sectors(base_lba, 1, buf) != 0)   return -1;
    pd_memcpy(&g_sb, buf, sizeof(g_sb));

    if (g_sb.magic   != PDFS_MAGIC)   return -2;
    if (g_sb.version != PDFS_VERSION) return -3;

    if (ata_read_sectors(g_sb.dir_lba, (uint8_t)g_sb.dir_sectors, g_dir) != 0)
        return -4;

    g_mounted = 1;
    return 0;
}

static int pdfs_open(const char *name, vfs_node_t *out)
{
    uint32_t i;
    if (!g_mounted) return -1;

    for (i = 0; i < PDFS_MAX_FILES; i++) {
        if ((g_dir[i].flags & PDFS_FLAG_USED) &&
            pd_strcmp(g_dir[i].name, name) == 0) {
            pd_strncpy(out->name, g_dir[i].name, VFS_NAME_MAX);
            out->size      = g_dir[i].size;
            out->inode     = i;
            out->is_dir    = 0;
            out->mount_idx = 0;   /* filled in by VFS layer */
            return 0;
        }
    }
    return -1;
}

static int pdfs_read(vfs_node_t *node, uint32_t offset, uint32_t len, void *buf)
{
    pdfs_dirent_t *de;
    uint8_t  tmp[512];
    uint8_t *out  = (uint8_t *)buf;
    uint32_t to_read, done = 0;

    if (!g_mounted || node->inode >= PDFS_MAX_FILES) return -1;

    de = &g_dir[node->inode];
    if (!(de->flags & PDFS_FLAG_USED)) return -1;
    if (offset >= de->size) return 0;

    to_read = de->size - offset;
    if (len < to_read) to_read = len;

    while (done < to_read) {
        uint32_t abs_off   = offset + done;
        uint32_t sector_i  = abs_off / 512u;
        uint32_t sector_off = abs_off % 512u;
        uint32_t chunk     = 512u - sector_off;
        if (chunk > to_read - done) chunk = to_read - done;

        if (ata_read_sectors(de->start_lba + sector_i, 1, tmp) != 0)
            return -1;

        pd_memcpy(out + done, tmp + sector_off, chunk);
        done += chunk;
    }
    return (int)done;
}

static int pdfs_write(vfs_node_t *node, uint32_t offset, uint32_t len,
                      const void *buf)
{
    pdfs_dirent_t  *de;
    const uint8_t  *src = (const uint8_t *)buf;
    uint8_t         tmp[512];
    uint32_t        needed, i, done = 0;

    if (!g_mounted || node->inode >= PDFS_MAX_FILES) return -1;
    if (offset != 0) return -2;   /* PDFS v1: full-file writes only */

    de = &g_dir[node->inode];
    if (!(de->flags & PDFS_FLAG_USED)) return -1;

    if (len == 0) {
        de->size = 0;
        pdfs_flush_dir();
        return 0;
    }

    needed = (len + 511u) / 512u;

    /* Allocate new space when current allocation is insufficient */
    if (de->alloc_sectors < needed) {
        de->start_lba     = g_sb.next_free_lba;
        de->alloc_sectors = needed;
        g_sb.next_free_lba += needed;
        pdfs_flush_sb();
    }

    /* Write data sector by sector */
    for (i = 0; i < needed; i++) {
        uint32_t chunk = len - done;
        if (chunk > 512u) chunk = 512u;
        pd_memzero(tmp, 512u);
        pd_memcpy(tmp, src + done, chunk);
        if (ata_write_sectors(de->start_lba + i, 1, tmp) != 0)
            return -1;
        done += chunk;
    }

    de->size = len;
    pdfs_flush_dir();
    return (int)done;
}

static int pdfs_create(const char *name)
{
    uint32_t i;
    if (!g_mounted) return -1;
    if (pd_strlen(name) >= PDFS_NAME_LEN) return -2;

    for (i = 0; i < PDFS_MAX_FILES; i++)
        if ((g_dir[i].flags & PDFS_FLAG_USED) &&
            pd_strcmp(g_dir[i].name, name) == 0)
            return -3;   /* already exists */

    for (i = 0; i < PDFS_MAX_FILES; i++) {
        if (!(g_dir[i].flags & PDFS_FLAG_USED)) {
            pd_strncpy(g_dir[i].name, name, PDFS_NAME_LEN);
            g_dir[i].start_lba     = 0;
            g_dir[i].size          = 0;
            g_dir[i].alloc_sectors = 0;
            g_dir[i].flags         = PDFS_FLAG_USED;
            pdfs_flush_dir();
            return 0;
        }
    }
    return -4;   /* directory full */
}

static int pdfs_unlink(const char *name)
{
    uint32_t i;
    if (!g_mounted) return -1;

    for (i = 0; i < PDFS_MAX_FILES; i++) {
        if ((g_dir[i].flags & PDFS_FLAG_USED) &&
            pd_strcmp(g_dir[i].name, name) == 0) {
            pd_memzero(&g_dir[i], sizeof(pdfs_dirent_t));
            pdfs_flush_dir();
            return 0;
        }
    }
    return -1;
}

static int pdfs_readdir(uint32_t idx, vfs_node_t *out)
{
    uint32_t count = 0, i;
    if (!g_mounted) return -1;

    for (i = 0; i < PDFS_MAX_FILES; i++) {
        if (g_dir[i].flags & PDFS_FLAG_USED) {
            if (count == idx) {
                pd_strncpy(out->name, g_dir[i].name, VFS_NAME_MAX);
                out->size      = g_dir[i].size;
                out->inode     = i;
                out->is_dir    = 0;
                out->mount_idx = 0;
                return 0;
            }
            count++;
        }
    }
    return -1;   /* no more entries */
}

/* ---- Driver registration -------------------------------------------------- */

static vfs_driver_t pdfs_driver = {
    "pdfs",
    pdfs_mount,
    pdfs_open,
    pdfs_read,
    pdfs_write,
    pdfs_create,
    pdfs_unlink,
    pdfs_readdir
};

vfs_driver_t *pdfs_get_driver(void)
{
    return &pdfs_driver;
}

/* ---- Format --------------------------------------------------------------- */

int pdfs_format(uint32_t base_lba)
{
    /* Build superblock */
    pd_memzero(&g_sb, sizeof(g_sb));
    g_sb.magic          = PDFS_MAGIC;
    g_sb.version        = PDFS_VERSION;
    g_sb.dir_lba        = base_lba + PDFS_SB_SECTORS;
    g_sb.dir_sectors    = PDFS_DIR_SECTORS;
    g_sb.data_lba       = base_lba + PDFS_SB_SECTORS + PDFS_DIR_SECTORS;
    g_sb.next_free_lba  = g_sb.data_lba;

    if (ata_write_sectors(base_lba, 1, &g_sb) != 0) return -1;

    /* Write empty directory */
    pd_memzero(g_dir, sizeof(g_dir));
    if (ata_write_sectors(g_sb.dir_lba, (uint8_t)g_sb.dir_sectors, g_dir) != 0)
        return -2;

    g_base_lba = base_lba;
    g_mounted  = 1;
    return 0;
}

/* ---- Statistics ----------------------------------------------------------- */

uint32_t pdfs_free_sectors(void)
{
    const ata_drive_t *drv;
    if (!g_mounted) return 0;
    drv = ata_get_drive();
    if (!drv->present) return 0;
    if (g_sb.next_free_lba >= drv->total_sectors) return 0;
    return drv->total_sectors - g_sb.next_free_lba;
}

uint32_t pdfs_file_count(void)
{
    uint32_t n = 0, i;
    if (!g_mounted) return 0;
    for (i = 0; i < PDFS_MAX_FILES; i++)
        if (g_dir[i].flags & PDFS_FLAG_USED) n++;
    return n;
}
