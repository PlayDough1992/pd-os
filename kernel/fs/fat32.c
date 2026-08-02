/* ============================================================================
 * PD-Kernel  —  FAT32 Driver  (Phase 8c)
 *
 * Implements the vfs_driver_t interface for the FAT32 filesystem.
 *
 * Supported operations:
 *   mount   — reads BPB, validates "FAT32   " signature, derives geometry
 *   open    — scans root directory for 8.3 name match
 *   read    — follows cluster chain, copies bytes into caller buffer
 *   write   — offset-0 full-file write; frees old chain, allocates new
 *   create  — inserts zeroed directory entry in first free slot
 *   unlink  — frees cluster chain, marks directory slot 0xE5
 *   readdir — iterates root directory, returns nth valid file entry
 *
 * Limitations (v1):
 *   - Root directory only (no subdirectory traversal)
 *   - 8.3 filenames (LFN entries ignored / skipped)
 *   - offset parameter to write() must be 0
 * ============================================================================ */

#include "fat32.h"
#include "ata.h"

/* ---- Directory entry attribute flags ------------------------------------- */
#define FAT_ATTR_READONLY  0x01u
#define FAT_ATTR_HIDDEN    0x02u
#define FAT_ATTR_SYSTEM    0x04u
#define FAT_ATTR_VOLUME_ID 0x08u
#define FAT_ATTR_DIRECTORY 0x10u
#define FAT_ATTR_ARCHIVE   0x20u
#define FAT_ATTR_LFN       0x0Fu   /* all four attribute bits set → LFN      */

#define FAT_DIRENT_DELETED 0xE5u   /* first-byte marker for deleted entry    */
#define FAT_DIRENT_END     0x00u   /* first-byte marker for end-of-directory */

#define FAT_EOC            0x0FFFFFFFu
#define FAT_FREE           0x00000000u

/* ---- Module-level state -------------------------------------------------- */
static uint32_t g_base_lba;
static uint32_t g_fat_start_lba;   /* absolute LBA of FAT1 sector 0          */
static uint32_t g_fat_sectors;     /* sectors per FAT copy                    */
static uint32_t g_data_start_lba;  /* absolute LBA of first data sector       */
static uint8_t  g_sec_per_clus;    /* sectors per cluster                     */
static uint32_t g_root_cluster;    /* first cluster of the root directory      */
static uint32_t g_total_clusters;  /* total data clusters                     */
static int      g_mounted = 0;

/* ---- Byte-level I/O helpers ---------------------------------------------- */
static uint16_t f_rd16(const uint8_t *b, uint32_t o)
{
    return (uint16_t)(b[o] | ((uint16_t)b[o + 1] << 8));
}

static uint32_t f_rd32(const uint8_t *b, uint32_t o)
{
    return (uint32_t)b[o]
         | ((uint32_t)b[o + 1] <<  8)
         | ((uint32_t)b[o + 2] << 16)
         | ((uint32_t)b[o + 3] << 24);
}

static void f_wr16(uint8_t *b, uint32_t o, uint16_t v)
{
    b[o]     = (uint8_t)(v & 0xFFu);
    b[o + 1] = (uint8_t)(v >> 8);
}

static void f_wr32(uint8_t *b, uint32_t o, uint32_t v)
{
    b[o]     = (uint8_t)(v & 0xFFu);
    b[o + 1] = (uint8_t)((v >>  8) & 0xFFu);
    b[o + 2] = (uint8_t)((v >> 16) & 0xFFu);
    b[o + 3] = (uint8_t)((v >> 24) & 0xFFu);
}

/* ---- Utility helpers ----------------------------------------------------- */
static int f_memcmp(const void *a, const void *b, uint32_t n)
{
    const uint8_t *p = (const uint8_t *)a;
    const uint8_t *q = (const uint8_t *)b;
    while (n--) { if (*p != *q) return (int)*p - (int)*q; p++; q++; }
    return 0;
}

static void f_memcpy(void *dst, const void *src, uint32_t n)
{
    uint8_t       *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
}

static void f_memset(void *dst, uint8_t v, uint32_t n)
{
    uint8_t *d = (uint8_t *)dst;
    while (n--) *d++ = v;
}

static void f_strncpy(char *d, const char *s, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n - 1u && s[i]; i++) d[i] = s[i];
    d[i] = '\0';
}

/* ---- Cluster ↔ LBA ------------------------------------------------------- */
static uint32_t clus_lba(uint32_t clus)
{
    return g_data_start_lba + ((uint32_t)(clus - 2u) * g_sec_per_clus);
}

/* ---- FAT entry operations ------------------------------------------------ */

/* Read one FAT32 entry (low 28 bits) from FAT1. */
static uint32_t fat_get(uint32_t clus)
{
    uint8_t  buf[512];
    uint32_t off     = clus * 4u;
    uint32_t fat_sec = g_fat_start_lba + off / 512u;
    uint32_t sec_off = off % 512u;
    if (ata_read_sectors(fat_sec, 1, buf) != 0) return FAT_EOC;
    return f_rd32(buf, sec_off) & 0x0FFFFFFFu;
}

/* Write one FAT32 entry to both FAT copies. */
static void fat_set(uint32_t clus, uint32_t val)
{
    uint8_t  buf[512];
    uint32_t off     = clus * 4u;
    uint32_t sec_off = off % 512u;
    uint32_t copy, sec;
    for (copy = 0; copy < 2u; copy++) {
        sec = g_fat_start_lba + copy * g_fat_sectors + off / 512u;
        if (ata_read_sectors(sec, 1, buf) != 0) continue;
        /* Preserve the reserved high nibble. */
        val = (f_rd32(buf, sec_off) & 0xF0000000u) | (val & 0x0FFFFFFFu);
        f_wr32(buf, sec_off, val);
        ata_write_sectors(sec, 1, buf);
    }
}

/* Find and allocate the first free cluster, marked as EOC.  Returns 0 on failure. */
static uint32_t fat_alloc(void)
{
    uint32_t clus;
    for (clus = 2u; clus < g_total_clusters + 2u; clus++) {
        if (fat_get(clus) == FAT_FREE) {
            fat_set(clus, FAT_EOC);
            return clus;
        }
    }
    return 0u;
}

/* Free the entire cluster chain starting at `start`. */
static void fat_free_chain(uint32_t start)
{
    uint32_t cur = start, next;
    while (cur >= 2u && cur < 0x0FFFFFF8u) {
        next = fat_get(cur);
        fat_set(cur, FAT_FREE);
        cur = next;
    }
}

/* ---- 8.3 name helpers ---------------------------------------------------- */

/*
 * Convert a plain filename (e.g. "hello.txt") to the FAT 8.3 format:
 * 11 bytes, uppercase, space-padded — e.g. "HELLO   TXT".
 */
static void to_83(const char *name, uint8_t out[11])
{
    uint32_t i;
    uint8_t  c;
    int      dot = -1;

    for (i = 0; i < 11u; i++) out[i] = ' ';

    for (i = 0; i < 8u && name[i] && name[i] != '.'; i++) {
        c = (uint8_t)name[i];
        if (c >= 'a' && c <= 'z') c = (uint8_t)(c - 32u);
        out[i] = c;
    }
    if (name[i] == '.') {
        dot = (int)i;
        for (i = 0; i < 3u && name[dot + 1 + (int)i]; i++) {
            c = (uint8_t)name[dot + 1 + (int)i];
            if (c >= 'a' && c <= 'z') c = (uint8_t)(c - 32u);
            out[8u + i] = c;
        }
    }
}

/*
 * Convert a FAT 8.3 directory entry (11 bytes) to a null-terminated filename.
 * "HELLO   TXT" → "HELLO.TXT",  "AB         " → "AB"
 * out must be at least 13 bytes.
 */
static void from_83(const uint8_t entry[11], char out[13])
{
    int i, n = 0;
    for (i = 0; i < 8 && entry[i] != ' '; i++) out[n++] = (char)entry[i];
    if (entry[8] != ' ') {
        out[n++] = '.';
        for (i = 8; i < 11 && entry[i] != ' '; i++) out[n++] = (char)entry[i];
    }
    out[n] = '\0';
}

/* ---- Directory-sector helpers -------------------------------------------- */

/*
 * Each cluster contains (g_sec_per_clus * 512 / 32) directory entries.
 * A 'slot' is a zero-based index across the entire root-dir cluster chain.
 *
 * read_dir_sector():
 *   Reads the 512-byte sector containing directory slot `slot` into `buf`.
 *   Sets *entry_off to the byte offset of the entry within that sector.
 *   Returns 0 on success, -1 on chain exhaustion or I/O error.
 */
static uint32_t slots_per_clus(void)
{
    return (uint32_t)g_sec_per_clus * 512u / 32u;
}

static int read_dir_sector(uint32_t slot, uint8_t *buf, uint32_t *entry_off)
{
    uint32_t spc       = slots_per_clus();
    uint32_t clus_idx  = slot / spc;              /* which cluster in chain  */
    uint32_t slot_in_c = slot % spc;              /* slot within that cluster*/
    uint32_t sec_idx   = (slot_in_c * 32u) / 512u;/* sector within cluster   */
    uint32_t clus      = g_root_cluster;
    uint32_t i;

    for (i = 0u; i < clus_idx; i++) {
        clus = fat_get(clus);
        if (clus >= 0x0FFFFFF8u) return -1;
    }

    if (ata_read_sectors(clus_lba(clus) + sec_idx, 1, buf) != 0) return -1;
    *entry_off = (slot_in_c % (512u / 32u)) * 32u;
    return 0;
}

/* Write back a modified directory sector (slot identifies which sector). */
static int write_dir_sector(uint32_t slot, uint8_t *buf)
{
    uint32_t spc       = slots_per_clus();
    uint32_t clus_idx  = slot / spc;
    uint32_t slot_in_c = slot % spc;
    uint32_t sec_idx   = (slot_in_c * 32u) / 512u;
    uint32_t clus      = g_root_cluster;
    uint32_t i;

    for (i = 0u; i < clus_idx; i++) {
        clus = fat_get(clus);
        if (clus >= 0x0FFFFFF8u) return -1;
    }
    return ata_write_sectors(clus_lba(clus) + sec_idx, 1, buf);
}

/* Total directory slot count (free + used) across the root dir chain. */
static uint32_t total_dir_slots(void)
{
    uint32_t count = 0u;
    uint32_t clus  = g_root_cluster;
    while (clus >= 2u && clus < 0x0FFFFFF8u) {
        count += slots_per_clus();
        clus   = fat_get(clus);
    }
    return count;
}

/* ---- VFS driver callbacks ------------------------------------------------ */

static int fat32_mount(uint32_t base_lba)
{
    uint8_t  buf[512];
    uint16_t reserved_secs, fat_count;
    uint32_t fat_sectors_32, total_sectors_32, data_sectors;

    g_base_lba = base_lba;

    if (ata_read_sectors(base_lba, 1, buf) != 0) return -1;
    if (f_rd16(buf, 510) != 0xAA55u)             return -2;
    if (f_rd16(buf, 11)  != 512u)                return -3;   /* BPS != 512 */
    if (f_memcmp(&buf[82], "FAT32   ", 8) != 0)  return -4;   /* not FAT32  */

    g_sec_per_clus   = buf[13];
    reserved_secs    = f_rd16(buf, 14);
    fat_count        = buf[16];
    fat_sectors_32   = f_rd32(buf, 36);
    total_sectors_32 = f_rd32(buf, 32);
    g_root_cluster   = f_rd32(buf, 44);
    g_fat_sectors    = fat_sectors_32;

    g_fat_start_lba  = base_lba + reserved_secs;
    g_data_start_lba = base_lba + reserved_secs + fat_count * fat_sectors_32;
    data_sectors     = total_sectors_32 - reserved_secs - fat_count * fat_sectors_32;
    g_total_clusters = data_sectors / g_sec_per_clus;

    g_mounted = 1;
    return 0;
}

static int fat32_open(const char *name, vfs_node_t *out)
{
    uint8_t  buf[512], name83[11];
    char     fname[13];
    uint32_t total, slot, entry_off;
    uint32_t clus_hi, clus_lo;

    if (!g_mounted) return -1;
    to_83(name, name83);
    total = total_dir_slots();

    for (slot = 0u; slot < total; slot++) {
        if (read_dir_sector(slot, buf, &entry_off) != 0) break;
        if (buf[entry_off] == FAT_DIRENT_END)     break;
        if (buf[entry_off] == FAT_DIRENT_DELETED) continue;

        if (buf[entry_off + 11] == FAT_ATTR_LFN)            continue;
        if (buf[entry_off + 11] & FAT_ATTR_DIRECTORY)       continue;
        if (buf[entry_off + 11] & FAT_ATTR_VOLUME_ID)       continue;
        if (f_memcmp(&buf[entry_off], name83, 11) != 0)     continue;

        from_83(&buf[entry_off], fname);
        f_strncpy(out->name, fname, VFS_NAME_MAX);
        out->size   = f_rd32(buf, entry_off + 28);
        clus_hi     = f_rd16(buf, entry_off + 20);
        clus_lo     = f_rd16(buf, entry_off + 26);
        out->inode  = (clus_hi << 16) | clus_lo;  /* first data cluster */
        out->is_dir = 0;
        return 0;
    }
    return -1;
}

static int fat32_read(vfs_node_t *node, uint32_t offset, uint32_t len, void *buf_out)
{
    uint8_t  sec_buf[512];
    uint8_t *out         = (uint8_t *)buf_out;
    uint32_t clus        = node->inode;
    uint32_t bytes_read  = 0u;
    uint32_t clus_size   = (uint32_t)g_sec_per_clus * 512u;
    uint32_t i, clus_off, sec_idx, sec_off, to_copy;

    if (!g_mounted || len == 0u)        return 0;
    if (offset >= node->size)           return 0;
    if (offset + len > node->size)      len = node->size - offset;

    /* Advance to the cluster containing `offset`. */
    for (i = 0u; i < offset / clus_size; i++) {
        clus = fat_get(clus);
        if (clus >= 0x0FFFFFF8u) return (int)bytes_read;
    }

    clus_off = offset % clus_size;

    while (bytes_read < len && clus >= 2u && clus < 0x0FFFFFF8u) {
        sec_idx = clus_off / 512u;
        sec_off = clus_off % 512u;

        while (sec_idx < g_sec_per_clus && bytes_read < len) {
            if (ata_read_sectors(clus_lba(clus) + sec_idx, 1, sec_buf) != 0)
                return -1;
            to_copy = 512u - sec_off;
            if (to_copy > len - bytes_read) to_copy = len - bytes_read;
            f_memcpy(out + bytes_read, sec_buf + sec_off, to_copy);
            bytes_read += to_copy;
            sec_off     = 0u;
            sec_idx++;
        }
        clus_off = 0u;
        clus = fat_get(clus);
    }
    return (int)bytes_read;
}

static int fat32_write(vfs_node_t *node, uint32_t offset, uint32_t len,
                       const void *buf_in)
{
    uint8_t         sec_buf[512];
    const uint8_t  *src           = (const uint8_t *)buf_in;
    uint32_t        bytes_written = 0u;
    uint32_t        remaining     = len;
    uint32_t        first_clus    = 0u;
    uint32_t        prev_clus     = 0u;
    uint32_t        cur_clus;
    uint32_t        sec_idx, to_write, lba;
    /* dir-update locals */
    uint8_t         dbuf[512], name83[11];
    uint32_t        total, slot, eoff;

    if (!g_mounted)  return -1;
    if (offset != 0) return -2;     /* only full-file (offset=0) writes in v1 */

    /* Free existing cluster chain if file already has data. */
    if (node->inode >= 2u) fat_free_chain(node->inode);

    /* Allocate clusters and write data sector by sector. */
    while (remaining > 0u) {
        cur_clus = fat_alloc();
        if (cur_clus == 0u) return -3;   /* disk full */

        if (first_clus == 0u) first_clus = cur_clus;
        if (prev_clus  != 0u) fat_set(prev_clus, cur_clus);   /* chain link */

        for (sec_idx = 0u; sec_idx < g_sec_per_clus && remaining > 0u; sec_idx++) {
            lba      = clus_lba(cur_clus) + sec_idx;
            to_write = (remaining > 512u) ? 512u : remaining;
            f_memset(sec_buf, 0, 512);
            f_memcpy(sec_buf, src + bytes_written, to_write);
            if (ata_write_sectors(lba, 1, sec_buf) != 0) return -4;
            bytes_written += to_write;
            remaining     -= to_write;
        }
        prev_clus = cur_clus;
    }

    /* Update the directory entry with the new start cluster and size. */
    to_83(node->name, name83);
    total = total_dir_slots();
    for (slot = 0u; slot < total; slot++) {
        if (read_dir_sector(slot, dbuf, &eoff) != 0) break;
        if (dbuf[eoff] == FAT_DIRENT_END)              break;
        if (dbuf[eoff] == FAT_DIRENT_DELETED)          continue;
        if (dbuf[eoff + 11] == FAT_ATTR_LFN)           continue;
        if (dbuf[eoff + 11] & FAT_ATTR_DIRECTORY)      continue;
        if (dbuf[eoff + 11] & FAT_ATTR_VOLUME_ID)      continue;
        if (f_memcmp(&dbuf[eoff], name83, 11) != 0)    continue;

        f_wr16(dbuf, eoff + 20, (uint16_t)(first_clus >> 16));
        f_wr16(dbuf, eoff + 26, (uint16_t)(first_clus & 0xFFFFu));
        f_wr32(dbuf, eoff + 28, len);
        write_dir_sector(slot, dbuf);
        break;
    }

    node->inode = first_clus;
    node->size  = len;
    return (int)bytes_written;
}

static int fat32_create(const char *name)
{
    uint8_t  buf[512], name83[11];
    uint32_t total, slot, eoff;

    if (!g_mounted) return -1;
    to_83(name, name83);
    total = total_dir_slots();

    for (slot = 0u; slot < total; slot++) {
        if (read_dir_sector(slot, buf, &eoff) != 0) return -3;
        if (buf[eoff] == FAT_DIRENT_END || buf[eoff] == FAT_DIRENT_DELETED) break;
        if (buf[eoff + 11] == FAT_ATTR_LFN)          continue;
        if (buf[eoff + 11] & FAT_ATTR_VOLUME_ID)     continue;
        if (f_memcmp(&buf[eoff], name83, 11) == 0)   return -2;   /* exists */
    }
    if (slot >= total) return -4;   /* directory full */

    f_memset(&buf[eoff], 0, 32);
    f_memcpy(&buf[eoff], name83, 11);
    buf[eoff + 11] = FAT_ATTR_ARCHIVE;
    /* cluster_hi=0, cluster_lo=0, size=0 — filled on first write */
    return write_dir_sector(slot, buf);
}

static int fat32_unlink(const char *name)
{
    uint8_t  buf[512], name83[11];
    uint32_t total, slot, eoff, clus_hi, clus_lo, first_clus;

    if (!g_mounted) return -1;
    to_83(name, name83);
    total = total_dir_slots();

    for (slot = 0u; slot < total; slot++) {
        if (read_dir_sector(slot, buf, &eoff) != 0) break;
        if (buf[eoff] == FAT_DIRENT_END)              break;
        if (buf[eoff] == FAT_DIRENT_DELETED)          continue;
        if (buf[eoff + 11] == FAT_ATTR_LFN)           continue;
        if (buf[eoff + 11] & FAT_ATTR_DIRECTORY)      continue;
        if (buf[eoff + 11] & FAT_ATTR_VOLUME_ID)      continue;
        if (f_memcmp(&buf[eoff], name83, 11) != 0)    continue;

        clus_hi   = f_rd16(buf, eoff + 20);
        clus_lo   = f_rd16(buf, eoff + 26);
        first_clus = (clus_hi << 16) | clus_lo;
        if (first_clus >= 2u) fat_free_chain(first_clus);

        buf[eoff] = FAT_DIRENT_DELETED;
        return write_dir_sector(slot, buf);
    }
    return -1;
}

static int fat32_readdir(uint32_t idx, vfs_node_t *out)
{
    uint8_t  buf[512];
    char     fname[13];
    uint32_t total, slot, eoff, count = 0u, clus_hi, clus_lo;

    if (!g_mounted) return -1;
    total = total_dir_slots();

    for (slot = 0u; slot < total; slot++) {
        if (read_dir_sector(slot, buf, &eoff) != 0) break;
        if (buf[eoff] == FAT_DIRENT_END)              break;
        if (buf[eoff] == FAT_DIRENT_DELETED)          continue;
        if (buf[eoff + 11] == FAT_ATTR_LFN)           continue;
        if (buf[eoff + 11] & FAT_ATTR_DIRECTORY)      continue;
        if (buf[eoff + 11] & FAT_ATTR_VOLUME_ID)      continue;

        if (count == idx) {
            from_83(&buf[eoff], fname);
            f_strncpy(out->name, fname, VFS_NAME_MAX);
            out->size   = f_rd32(buf, eoff + 28);
            clus_hi     = f_rd16(buf, eoff + 20);
            clus_lo     = f_rd16(buf, eoff + 26);
            out->inode  = (clus_hi << 16) | clus_lo;
            out->is_dir = 0;
            return 0;
        }
        count++;
    }
    return -1;
}

/* ---- Driver registration ------------------------------------------------- */
static vfs_driver_t g_fat32_driver = {
    "fat32",
    fat32_mount,
    fat32_open,
    fat32_read,
    fat32_write,
    fat32_create,
    fat32_unlink,
    fat32_readdir,
};

vfs_driver_t *fat32_get_driver(void) { return &g_fat32_driver; }
