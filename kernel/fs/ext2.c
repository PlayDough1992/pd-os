/* ============================================================================
 * PD-Kernel  —  ext2 Driver  (Phase 8d)
 *
 * Implements the vfs_driver_t interface for ext2.
 *
 * Supported operations:
 *   mount   — reads superblock, validates magic 0xEF53, derives geometry
 *   open    — walks directory path, resolves inode
 *   read    — reads data blocks via direct + singly-indirect block pointers
 *   write   — offset-0 full-file write; frees old blocks, allocates new
 *   create  — allocates inode + adds dirent to parent directory
 *   unlink  — frees inode + data blocks, removes dirent from directory
 *   readdir — iterates directory entries by index
 *
 * Limitations (v1 — single block group, 1 KB blocks, no journal):
 *   - block_size = 1024 bytes only
 *   - single block group (up to ~128 MB volume)
 *   - write offset must be 0 (full-file replace)
 *   - no hard-link ref-count management beyond basic decrement
 * ============================================================================ */

#include "ext2.h"
#include "ata.h"
#include "kheap.h"

/* ---- ext2 on-disk constants ---------------------------------------------- */
#define EXT2_MAGIC          0xEF53u
#define EXT2_ROOT_INO       2u
#define EXT2_NAME_LEN       255u

/* inode type/permission masks */
#define EXT2_S_IFREG        0x8000u
#define EXT2_S_IFDIR        0x4000u

/* directory entry file_type field */
#define EXT2_FT_UNKNOWN     0u
#define EXT2_FT_REG_FILE    1u
#define EXT2_FT_DIR         2u

/* ---- On-disk structures (little-endian) ---------------------------------- */

/* Superblock — at byte offset 1024 from the start of the filesystem */
typedef struct {
    uint32_t s_inodes_count;
    uint32_t s_blocks_count;
    uint32_t s_r_blocks_count;
    uint32_t s_free_blocks_count;
    uint32_t s_free_inodes_count;
    uint32_t s_first_data_block;   /* 1 for 1 KB blocks, 0 for >=2 KB         */
    uint32_t s_log_block_size;     /* block_size = 1024 << s_log_block_size    */
    uint32_t s_log_frag_size;
    uint32_t s_blocks_per_group;
    uint32_t s_frags_per_group;
    uint32_t s_inodes_per_group;
    uint32_t s_mtime;
    uint32_t s_wtime;
    uint16_t s_mnt_count;
    uint16_t s_max_mnt_count;
    uint16_t s_magic;              /* 0xEF53                                   */
    uint16_t s_state;
    uint16_t s_errors;
    uint16_t s_minor_rev_level;
    uint32_t s_lastcheck;
    uint32_t s_checkinterval;
    uint32_t s_creator_os;
    uint32_t s_rev_level;
    uint16_t s_def_resuid;
    uint16_t s_def_resgid;
    /* Extended superblock fields (rev >= 1) */
    uint32_t s_first_ino;
    uint16_t s_inode_size;
    uint16_t s_block_group_nr;
    uint32_t s_feature_compat;
    uint32_t s_feature_incompat;
    uint32_t s_feature_ro_compat;
    uint8_t  s_uuid[16];
    uint8_t  s_volume_name[16];
    uint8_t  s_last_mounted[64];
    uint32_t s_algo_bitmap;
    /* padding to 1024 bytes */
    uint8_t  pad[820];
} __attribute__((packed)) ext2_superblock_t;

/* Block Group Descriptor */
typedef struct {
    uint32_t bg_block_bitmap;
    uint32_t bg_inode_bitmap;
    uint32_t bg_inode_table;
    uint16_t bg_free_blocks_count;
    uint16_t bg_free_inodes_count;
    uint16_t bg_used_dirs_count;
    uint16_t bg_pad;
    uint8_t  bg_reserved[12];
} __attribute__((packed)) ext2_bgd_t;

/* Inode */
typedef struct {
    uint16_t i_mode;
    uint16_t i_uid;
    uint32_t i_size;
    uint32_t i_atime;
    uint32_t i_ctime;
    uint32_t i_mtime;
    uint32_t i_dtime;
    uint16_t i_gid;
    uint16_t i_links_count;
    uint32_t i_blocks;     /* 512-byte units                                  */
    uint32_t i_flags;
    uint32_t i_osd1;
    uint32_t i_block[15];  /* 12 direct + 1 indirect + 1 dbl + 1 triple       */
    uint32_t i_generation;
    uint32_t i_file_acl;
    uint32_t i_dir_acl;
    uint32_t i_faddr;
    uint8_t  i_osd2[12];
} __attribute__((packed)) ext2_inode_t;

/* Directory entry (variable length) */
typedef struct {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t  name_len;
    uint8_t  file_type;
    char     name[EXT2_NAME_LEN];
} __attribute__((packed)) ext2_dirent_t;

/* ---- Module state -------------------------------------------------------- */
static uint32_t      g_base_lba;
static uint32_t      g_block_size;    /* bytes per block (1024 in v1)         */
static uint32_t      g_secs_per_blk;  /* 512-byte sectors per block           */
static uint32_t      g_inodes_per_group;
static uint32_t      g_inode_size;
static ext2_bgd_t    g_bgd;           /* single block group descriptor        */
static int           g_mounted = 0;

/* ---- Low-level I/O ------------------------------------------------------- */

/* Read one ext2 block (1024 bytes = 2 sectors) into a caller-supplied buffer.
 * `blk` is an absolute block number within the filesystem. */
static int read_block(uint32_t blk, void *buf)
{
    uint32_t lba = g_base_lba + blk * g_secs_per_blk;
    return ata_read_sectors(lba, (uint8_t)g_secs_per_blk, buf);
}

static int write_block(uint32_t blk, const void *buf)
{
    uint32_t lba = g_base_lba + blk * g_secs_per_blk;
    return ata_write_sectors(lba, (uint8_t)g_secs_per_blk, buf);
}

/* ---- Bitmap helpers ------------------------------------------------------ */

static int bitmap_test(uint8_t *bm, uint32_t bit)
{
    return (bm[bit / 8u] >> (bit % 8u)) & 1u;
}

static void bitmap_set(uint8_t *bm, uint32_t bit)
{
    bm[bit / 8u] |= (uint8_t)(1u << (bit % 8u));
}

static void bitmap_clear(uint8_t *bm, uint32_t bit)
{
    bm[bit / 8u] &= (uint8_t)~(1u << (bit % 8u));
}

/* ---- Utility ------------------------------------------------------------- */
static int e2_strncmp(const char *a, const char *b, uint32_t n)
{
    while (n-- && *a && *a == *b) { a++; b++; }
    return n == (uint32_t)-1 ? 0 : (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static uint32_t e2_strlen(const char *s)
{
    uint32_t n = 0; while (s[n]) n++; return n;
}

static void e2_memcpy(void *d, const void *s, uint32_t n)
{
    uint8_t *dd = (uint8_t *)d; const uint8_t *ss = (const uint8_t *)s;
    while (n--) *dd++ = *ss++;
}

static void e2_memset(void *d, uint8_t v, uint32_t n)
{
    uint8_t *dd = (uint8_t *)d; while (n--) *dd++ = v;
}

static void e2_strncpy(char *d, const char *s, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n - 1u && s[i]; i++) d[i] = s[i];
    d[i] = '\0';
}

/* ---- Inode I/O ----------------------------------------------------------- */

static int read_inode(uint32_t ino, ext2_inode_t *out)
{
    uint8_t  *buf;
    uint32_t  idx      = (ino - 1u) % g_inodes_per_group;
    uint32_t  blk_off  = (idx * g_inode_size) / g_block_size;
    uint32_t  byte_off = (idx * g_inode_size) % g_block_size;
    uint32_t  blk      = g_bgd.bg_inode_table + blk_off;
    int ret;

    buf = (uint8_t *)kmalloc(g_block_size);
    if (!buf) return -1;
    ret = read_block(blk, buf);
    if (ret == 0) e2_memcpy(out, buf + byte_off, sizeof(ext2_inode_t));
    kfree(buf);
    return ret;
}

static int write_inode(uint32_t ino, const ext2_inode_t *in)
{
    uint8_t  *buf;
    uint32_t  idx      = (ino - 1u) % g_inodes_per_group;
    uint32_t  blk_off  = (idx * g_inode_size) / g_block_size;
    uint32_t  byte_off = (idx * g_inode_size) % g_block_size;
    uint32_t  blk      = g_bgd.bg_inode_table + blk_off;
    int ret;

    buf = (uint8_t *)kmalloc(g_block_size);
    if (!buf) return -1;
    ret = read_block(blk, buf);
    if (ret == 0) {
        e2_memcpy(buf + byte_off, in, sizeof(ext2_inode_t));
        ret = write_block(blk, buf);
    }
    kfree(buf);
    return ret;
}

/* ---- BGD flush ----------------------------------------------------------- */
static void flush_bgd(void)
{
    uint8_t *buf = (uint8_t *)kmalloc(g_block_size);
    if (!buf) return;
    /* BGD table is at block 2 (for 1 KB blocks where first_data_block=1) */
    read_block(2, buf);
    e2_memcpy(buf, &g_bgd, sizeof(g_bgd));
    write_block(2, buf);
    kfree(buf);
}

/* ---- Block allocation ---------------------------------------------------- */

/* Allocate one free data block; returns block number or 0 on failure. */
static uint32_t alloc_block(void)
{
    uint8_t  *bm = (uint8_t *)kmalloc(g_block_size);
    uint32_t  i, blk = 0;
    if (!bm) return 0;
    if (read_block(g_bgd.bg_block_bitmap, bm) != 0) { kfree(bm); return 0; }
    for (i = 0; i < g_block_size * 8u; i++) {
        if (!bitmap_test(bm, i)) {
            bitmap_set(bm, i);
            write_block(g_bgd.bg_block_bitmap, bm);
            g_bgd.bg_free_blocks_count--;
            flush_bgd();
            blk = i + 1u; /* block numbers are 1-based relative to group     */
            /* Absolute block = group_start + offset.  For single group
             * with first_data_block=1, absolute = 1 + i when i>=0.         */
            blk = 1u + i;
            break;
        }
    }
    kfree(bm);
    return blk;
}

static void free_block(uint32_t blk)
{
    uint8_t *bm = (uint8_t *)kmalloc(g_block_size);
    if (!bm) return;
    if (read_block(g_bgd.bg_block_bitmap, bm) != 0) { kfree(bm); return; }
    if (blk >= 1u) bitmap_clear(bm, blk - 1u);
    write_block(g_bgd.bg_block_bitmap, bm);
    g_bgd.bg_free_blocks_count++;
    flush_bgd();
    kfree(bm);
}

/* ---- Inode allocation ---------------------------------------------------- */

static uint32_t alloc_inode(void)
{
    uint8_t  *bm = (uint8_t *)kmalloc(g_block_size);
    uint32_t  i, ino = 0;
    if (!bm) return 0;
    if (read_block(g_bgd.bg_inode_bitmap, bm) != 0) { kfree(bm); return 0; }
    for (i = 0; i < g_inodes_per_group; i++) {
        if (!bitmap_test(bm, i)) {
            bitmap_set(bm, i);
            write_block(g_bgd.bg_inode_bitmap, bm);
            g_bgd.bg_free_inodes_count--;
            flush_bgd();
            ino = i + 1u;
            break;
        }
    }
    kfree(bm);
    return ino;
}

static void free_inode(uint32_t ino)
{
    uint8_t *bm = (uint8_t *)kmalloc(g_block_size);
    if (!bm) return;
    if (read_block(g_bgd.bg_inode_bitmap, bm) != 0) { kfree(bm); return; }
    if (ino >= 1u) bitmap_clear(bm, ino - 1u);
    write_block(g_bgd.bg_inode_bitmap, bm);
    g_bgd.bg_free_inodes_count++;
    flush_bgd();
    kfree(bm);
}

/* ---- Data block access via inode ---------------------------------------- */

/*
 * Resolve logical block index `idx` within an inode to an absolute block
 * number.  Supports direct (0-11) and singly-indirect (12-267) blocks.
 * Returns 0 if the block is sparse/unallocated.
 */
static uint32_t inode_get_block(const ext2_inode_t *ino, uint32_t idx)
{
    uint32_t *ind_buf;
    uint32_t  result;

    if (idx < 12u) return ino->i_block[idx];

    idx -= 12u;
    if (idx < g_block_size / 4u) {
        if (ino->i_block[12] == 0u) return 0u;
        ind_buf = (uint32_t *)kmalloc(g_block_size);
        if (!ind_buf) return 0u;
        read_block(ino->i_block[12], ind_buf);
        result = ind_buf[idx];
        kfree(ind_buf);
        return result;
    }
    /* Double/triple indirect not implemented in v1 */
    return 0u;
}

/*
 * Set logical block index `idx` in an inode to absolute block `blk`.
 * Allocates the indirect block itself if needed.
 */
static int inode_set_block(ext2_inode_t *ino, uint32_t idx, uint32_t blk)
{
    uint32_t *ind_buf;

    if (idx < 12u) { ino->i_block[idx] = blk; return 0; }

    idx -= 12u;
    if (idx < g_block_size / 4u) {
        if (ino->i_block[12] == 0u) {
            uint8_t *z;
            ino->i_block[12] = alloc_block();
            if (ino->i_block[12] == 0u) return -1;
            z = (uint8_t *)kmalloc(g_block_size);
            if (z) { e2_memset(z, 0, g_block_size); write_block(ino->i_block[12], z); kfree(z); }
        }
        ind_buf = (uint32_t *)kmalloc(g_block_size);
        if (!ind_buf) return -1;
        read_block(ino->i_block[12], ind_buf);
        ind_buf[idx] = blk;
        write_block(ino->i_block[12], ind_buf);
        kfree(ind_buf);
        return 0;
    }
    return -1;
}

/* Free all data blocks referenced by an inode (direct + singly indirect). */
static void free_inode_blocks(ext2_inode_t *ino)
{
    uint32_t  i, ptrs_per_blk = g_block_size / 4u;
    uint32_t *ind_buf;

    for (i = 0u; i < 12u; i++) {
        if (ino->i_block[i]) { free_block(ino->i_block[i]); ino->i_block[i] = 0; }
    }
    if (ino->i_block[12]) {
        ind_buf = (uint32_t *)kmalloc(g_block_size);
        if (ind_buf) {
            read_block(ino->i_block[12], ind_buf);
            for (i = 0u; i < ptrs_per_blk; i++)
                if (ind_buf[i]) free_block(ind_buf[i]);
            kfree(ind_buf);
        }
        free_block(ino->i_block[12]);
        ino->i_block[12] = 0;
    }
}

/* ---- Directory helpers --------------------------------------------------- */

/*
 * Look up `name` in the directory inode `dir_ino`.
 * Returns the inode number of the entry, or 0 if not found.
 */
static uint32_t dir_lookup(uint32_t dir_ino_num, const char *name)
{
    ext2_inode_t  dir_ino;
    uint8_t      *blk_buf;
    uint32_t      blk_idx = 0u, pos, nlen = e2_strlen(name);
    ext2_dirent_t *de;

    if (read_inode(dir_ino_num, &dir_ino) != 0) return 0u;
    blk_buf = (uint8_t *)kmalloc(g_block_size);
    if (!blk_buf) return 0u;

    while (1) {
        uint32_t blk = inode_get_block(&dir_ino, blk_idx);
        if (blk == 0u) break;
        if (read_block(blk, blk_buf) != 0) break;
        pos = 0u;
        while (pos < g_block_size) {
            de = (ext2_dirent_t *)(blk_buf + pos);
            if (de->rec_len == 0u) break;
            if (de->inode != 0u && de->name_len == (uint8_t)nlen &&
                e2_strncmp(de->name, name, nlen) == 0) {
                uint32_t found = de->inode;
                kfree(blk_buf);
                return found;
            }
            pos += de->rec_len;
        }
        blk_idx++;
    }
    kfree(blk_buf);
    return 0u;
}

/*
 * Walk an absolute path (e.g. "home/caleb/notes.txt") relative to root,
 * returning the inode number of the final component, and optionally the
 * inode number + name of the parent directory in *parent_ino / *fname.
 * Pass NULL for parent_ino/fname if not needed.
 */
static uint32_t path_resolve(const char *path, uint32_t *parent_ino, const char **fname)
{
    char     part[256];
    uint32_t cur = EXT2_ROOT_INO;
    uint32_t par = EXT2_ROOT_INO;
    const char *p = path;
    const char *last_part = path;

    /* Strip leading slash */
    while (*p == '/') p++;

    while (*p) {
        uint32_t i = 0u;
        const char *seg_start = p;
        while (*p && *p != '/') { if (i < 255u) part[i++] = *p; p++; }
        part[i] = '\0';
        while (*p == '/') p++;

        if (i == 0u) continue;
        last_part = seg_start;
        par = cur;
        cur = dir_lookup(cur, part);
        if (cur == 0u) {
            /* Not found — if this is the last component, report parent */
            if (*p == '\0') {
                if (parent_ino) *parent_ino = par;
                if (fname)      *fname      = last_part;
            }
            return 0u;
        }
    }
    if (parent_ino) *parent_ino = par;
    if (fname)      *fname      = last_part;
    return cur;
}

/*
 * Add a directory entry for (name → ino) into directory `dir_ino_num`.
 * Tries to fit the new entry into existing block slack, then allocates a
 * new block if necessary.
 */
static int dir_add_entry(uint32_t dir_ino_num, const char *name,
                         uint32_t ino, uint8_t ftype)
{
    ext2_inode_t  dir_ino;
    uint8_t      *buf;
    uint32_t      nlen    = e2_strlen(name);
    uint32_t      needed  = (uint32_t)(8u + nlen + 3u) & ~3u; /* 4-byte aligned */
    uint32_t      blk_idx = 0u, pos;
    ext2_dirent_t *de;
    int           ret = -1;

    if (read_inode(dir_ino_num, &dir_ino) != 0) return -1;
    buf = (uint8_t *)kmalloc(g_block_size);
    if (!buf) return -1;

    /* Scan existing blocks for slack space */
    while (1) {
        uint32_t blk = inode_get_block(&dir_ino, blk_idx);
        if (blk == 0u) break;
        if (read_block(blk, buf) != 0) break;
        pos = 0u;
        while (pos < g_block_size) {
            uint32_t actual_len;
            de = (ext2_dirent_t *)(buf + pos);
            if (de->rec_len == 0u) break;

            actual_len = (8u + de->name_len + 3u) & ~3u;
            if (de->inode == 0u && de->rec_len >= needed) {
                /* Empty slot big enough */
                de->inode     = ino;
                de->name_len  = (uint8_t)nlen;
                de->file_type = ftype;
                e2_memcpy(de->name, name, nlen);
                write_block(blk, buf);
                ret = 0;
                goto done;
            }
            if (de->rec_len >= actual_len + needed) {
                /* Split existing entry */
                ext2_dirent_t *ne = (ext2_dirent_t *)(buf + pos + actual_len);
                ne->inode     = ino;
                ne->rec_len   = (uint16_t)(de->rec_len - actual_len);
                ne->name_len  = (uint8_t)nlen;
                ne->file_type = ftype;
                e2_memcpy(ne->name, name, nlen);
                de->rec_len   = (uint16_t)actual_len;
                write_block(blk, buf);
                ret = 0;
                goto done;
            }
            pos += de->rec_len;
        }
        blk_idx++;
    }

    /* Allocate a new directory block */
    {
        uint32_t new_blk = alloc_block();
        if (new_blk == 0u) goto done;
        e2_memset(buf, 0, g_block_size);
        de            = (ext2_dirent_t *)buf;
        de->inode     = ino;
        de->rec_len   = (uint16_t)g_block_size;
        de->name_len  = (uint8_t)nlen;
        de->file_type = ftype;
        e2_memcpy(de->name, name, nlen);
        write_block(new_blk, buf);
        inode_set_block(&dir_ino, blk_idx, new_blk);
        dir_ino.i_size   += g_block_size;
        dir_ino.i_blocks += g_secs_per_blk * 2u; /* 512-byte block units × 2 */
        write_inode(dir_ino_num, &dir_ino);
        ret = 0;
    }

done:
    kfree(buf);
    return ret;
}

/*
 * Remove the directory entry with `name` from directory `dir_ino_num`.
 * Merges the entry's rec_len into the previous entry (or zeroes inode).
 */
static int dir_remove_entry(uint32_t dir_ino_num, const char *name)
{
    ext2_inode_t  dir_ino;
    uint8_t      *buf;
    uint32_t      nlen = e2_strlen(name);
    uint32_t      blk_idx = 0u, pos;
    ext2_dirent_t *de, *prev;
    int           ret = -1;

    if (read_inode(dir_ino_num, &dir_ino) != 0) return -1;
    buf = (uint8_t *)kmalloc(g_block_size);
    if (!buf) return -1;

    while (1) {
        uint32_t blk = inode_get_block(&dir_ino, blk_idx);
        if (blk == 0u) break;
        if (read_block(blk, buf) != 0) break;
        pos = 0u; prev = NULL;
        while (pos < g_block_size) {
            de = (ext2_dirent_t *)(buf + pos);
            if (de->rec_len == 0u) break;
            if (de->inode != 0u && de->name_len == (uint8_t)nlen &&
                e2_strncmp(de->name, name, nlen) == 0) {
                if (prev) prev->rec_len += de->rec_len;
                else      de->inode = 0u;
                write_block(blk, buf);
                ret = 0;
                goto done;
            }
            prev = de;
            pos += de->rec_len;
        }
        blk_idx++;
    }
done:
    kfree(buf);
    return ret;
}

/* ---- VFS driver callbacks ------------------------------------------------ */

static int ext2_mount(uint32_t base_lba)
{
    ext2_superblock_t *sb;
    uint8_t           *buf;
    int                ret = -1;

    /* Superblock sits at byte 1024 from partition start = sector 2 (0-based)
     * In terms of absolute LBA: base_lba + 2 */
    buf = (uint8_t *)kmalloc(1024);
    if (!buf) return -1;

    if (ata_read_sectors(base_lba + 2u, 2u, buf) != 0) goto fail;

    sb = (ext2_superblock_t *)buf;
    if (sb->s_magic != EXT2_MAGIC) goto fail;
    if (sb->s_log_block_size != 0u) goto fail;  /* only 1 KB blocks in v1    */

    g_base_lba        = base_lba;
    g_block_size      = 1024u;
    g_secs_per_blk    = 2u;
    g_inodes_per_group = sb->s_inodes_per_group;
    g_inode_size      = (sb->s_rev_level >= 1u) ? sb->s_inode_size : 128u;

    /* Read the Block Group Descriptor (block 2 for 1 KB blocks) */
    kfree(buf);
    buf = (uint8_t *)kmalloc(g_block_size);
    if (!buf) return -1;
    if (read_block(2u, buf) != 0) goto fail;
    e2_memcpy(&g_bgd, buf, sizeof(g_bgd));

    g_mounted = 1;
    ret = 0;

fail:
    kfree(buf);
    return ret;
}

static int ext2_open(const char *path, vfs_node_t *out)
{
    ext2_inode_t ino;
    uint32_t     ino_num;
    const char  *fname = path;

    if (!g_mounted) return -1;

    ino_num = path_resolve(path, NULL, &fname);
    if (ino_num == 0u) return -1;
    if (read_inode(ino_num, &ino) != 0) return -1;

    /* Locate the last path component for the name */
    {
        const char *p = path, *last = path;
        while (*p) { if (*p == '/') last = p + 1; p++; }
        e2_strncpy(out->name, last[0] ? last : path, VFS_NAME_MAX);
    }
    out->size      = ino.i_size;
    out->inode     = ino_num;
    out->is_dir    = (ino.i_mode & EXT2_S_IFDIR) ? 1u : 0u;
    return 0;
}

static int ext2_read(vfs_node_t *node, uint32_t offset, uint32_t len, void *buf_out)
{
    ext2_inode_t ino;
    uint8_t     *blk_buf;
    uint8_t     *out      = (uint8_t *)buf_out;
    uint32_t     bytes_read = 0u;
    uint32_t     file_off;

    if (!g_mounted || len == 0u)        return 0;
    if (read_inode(node->inode, &ino) != 0) return -1;
    if (offset >= ino.i_size)           return 0;
    if (offset + len > ino.i_size)      len = ino.i_size - offset;

    blk_buf = (uint8_t *)kmalloc(g_block_size);
    if (!blk_buf) return -1;

    file_off = offset;
    while (bytes_read < len) {
        uint32_t blk_idx  = file_off / g_block_size;
        uint32_t blk_off  = file_off % g_block_size;
        uint32_t blk      = inode_get_block(&ino, blk_idx);
        uint32_t to_copy  = g_block_size - blk_off;
        if (to_copy > len - bytes_read) to_copy = len - bytes_read;

        if (blk == 0u) {
            e2_memset(out + bytes_read, 0, to_copy);
        } else {
            if (read_block(blk, blk_buf) != 0) { kfree(blk_buf); return -1; }
            e2_memcpy(out + bytes_read, blk_buf + blk_off, to_copy);
        }
        bytes_read += to_copy;
        file_off   += to_copy;
    }
    kfree(blk_buf);
    return (int)bytes_read;
}

static int ext2_write(vfs_node_t *node, uint32_t offset, uint32_t len,
                      const void *buf_in)
{
    ext2_inode_t    ino;
    uint8_t        *blk_buf;
    const uint8_t  *src         = (const uint8_t *)buf_in;
    uint32_t        written     = 0u;
    uint32_t        blk_count, i;

    if (!g_mounted)  return -1;
    if (offset != 0) return -2;
    if (read_inode(node->inode, &ino) != 0) return -1;

    /* Free existing blocks */
    free_inode_blocks(&ino);
    ino.i_size   = 0u;
    ino.i_blocks = 0u;
    e2_memset(ino.i_block, 0, sizeof(ino.i_block));

    blk_buf   = (uint8_t *)kmalloc(g_block_size);
    if (!blk_buf) return -1;
    blk_count = (len + g_block_size - 1u) / g_block_size;

    for (i = 0u; i < blk_count; i++) {
        uint32_t new_blk  = alloc_block();
        uint32_t to_write = (len - written > g_block_size) ? g_block_size : len - written;
        if (new_blk == 0u) { kfree(blk_buf); return -3; }
        e2_memset(blk_buf, 0, g_block_size);
        e2_memcpy(blk_buf, src + written, to_write);
        write_block(new_blk, blk_buf);
        inode_set_block(&ino, i, new_blk);
        written         += to_write;
        ino.i_blocks    += g_secs_per_blk * 2u;
    }
    ino.i_size = len;
    write_inode(node->inode, &ino);
    node->size = len;
    kfree(blk_buf);
    return (int)written;
}

static int ext2_create(const char *path)
{
    uint32_t      par_ino_num, new_ino_num;
    const char   *fname = NULL;
    ext2_inode_t  new_ino;

    if (!g_mounted) return -1;
    path_resolve(path, &par_ino_num, &fname);
    if (fname == NULL || fname[0] == '\0') return -2;
    if (par_ino_num == 0u) return -3;

    /* Check it doesn't already exist */
    if (dir_lookup(par_ino_num, fname) != 0u) return -4;

    new_ino_num = alloc_inode();
    if (new_ino_num == 0u) return -5;

    e2_memset(&new_ino, 0, sizeof(new_ino));
    new_ino.i_mode        = EXT2_S_IFREG | 0644u;
    new_ino.i_links_count = 1u;
    write_inode(new_ino_num, &new_ino);

    return dir_add_entry(par_ino_num, fname, new_ino_num, EXT2_FT_REG_FILE);
}

static int ext2_unlink(const char *path)
{
    ext2_inode_t ino;
    uint32_t     par_ino_num, ino_num;
    const char  *fname = NULL;

    if (!g_mounted) return -1;
    ino_num = path_resolve(path, &par_ino_num, &fname);
    if (ino_num == 0u || fname == NULL) return -1;
    if (read_inode(ino_num, &ino) != 0) return -1;

    free_inode_blocks(&ino);
    ino.i_links_count = 0u;
    ino.i_dtime       = 1u;   /* flag as deleted  */
    write_inode(ino_num, &ino);
    free_inode(ino_num);

    return dir_remove_entry(par_ino_num, fname);
}

static int ext2_readdir(uint32_t idx, vfs_node_t *out)
{
    /* readdir on "/" — iterate root directory entries */
    ext2_inode_t  dir_ino;
    uint8_t      *buf;
    uint32_t      blk_idx = 0u, pos, count = 0u;
    ext2_dirent_t *de;
    int           ret = -1;

    if (!g_mounted) return -1;
    if (read_inode(EXT2_ROOT_INO, &dir_ino) != 0) return -1;
    buf = (uint8_t *)kmalloc(g_block_size);
    if (!buf) return -1;

    while (1) {
        uint32_t blk = inode_get_block(&dir_ino, blk_idx);
        if (blk == 0u) break;
        if (read_block(blk, buf) != 0) break;
        pos = 0u;
        while (pos < g_block_size) {
            de = (ext2_dirent_t *)(buf + pos);
            if (de->rec_len == 0u) break;
            if (de->inode != 0u) {
                /* skip . and .. */
                if (!(de->name_len == 1u && de->name[0] == '.') &&
                    !(de->name_len == 2u && de->name[0] == '.' && de->name[1] == '.')) {
                    if (count == idx) {
                        ext2_inode_t fino;
                        e2_strncpy(out->name, de->name, de->name_len + 1u < VFS_NAME_MAX
                                                         ? de->name_len + 1u : VFS_NAME_MAX);
                        out->name[de->name_len] = '\0';
                        out->inode  = de->inode;
                        out->is_dir = (de->file_type == EXT2_FT_DIR) ? 1u : 0u;
                        out->size   = 0u;
                        if (read_inode(de->inode, &fino) == 0)
                            out->size = fino.i_size;
                        ret = 0;
                        goto done;
                    }
                    count++;
                }
            }
            pos += de->rec_len;
        }
        blk_idx++;
    }
done:
    kfree(buf);
    return ret;
}

/* ---- Driver registration ------------------------------------------------- */
static vfs_driver_t g_ext2_driver = {
    "ext2",
    ext2_mount,
    ext2_open,
    ext2_read,
    ext2_write,
    ext2_create,
    ext2_unlink,
    ext2_readdir,
};

vfs_driver_t *ext2_get_driver(void) { return &g_ext2_driver; }
