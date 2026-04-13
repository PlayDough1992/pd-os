/* ============================================================================
 * PD-Kernel  —  PDFS v3  (Chained Directory Sectors)
 *
 * PDFS v3 removes the hard per-directory entry limit introduced in v2.
 *
 * Directory structure:
 *   Each directory is a singly-linked chain of 512-byte sectors.
 *   Every sector holds 8 × 64-byte dirent slots:
 *     Slots 0-6  usable directory entries  (PDFS_CHAIN_SLOTS = 7)
 *     Slot  7    chain link: if PDFS_FLAG_CHAIN set → start_lba = next sector
 *                            otherwise              → this is the last sector
 *   A new directory starts with one sector.  When all 7 usable slots are full,
 *   chain_alloc_slot() allocates a new sector, links it via slot 7, and
 *   returns slot 0 of the new sector.  Directories grow without bound.
 *
 * Inode encoding (vfs_node_t.inode):
 *   INODE_ENCODE(sec_lba, slot)  where:
 *     sec_lba = absolute LBA of the SPECIFIC sector containing this entry
 *     slot    = index 0-6 within that sector
 *
 * On-disk layout (all LBAs absolute):
 *   base_lba+0   Superblock           (512 bytes)
 *   base_lba+1   Journal slot         (512 bytes, reserved)
 *   base_lba+2   Root dir sector[0]   (512 bytes, chain-extended on demand)
 *   base_lba+3+  File data + additional dir chain sectors
 *
 * v1/v2 disks (version != 3) mount read-only. Run mkpdfs to reformat.
 * ============================================================================ */

#include "pdfs.h"
#include "ata.h"
#include "users.h"
#include "pit.h"

/* ---- Permission context -------------------------------------------------- */

static const user_t *g_caller   = NULL;
static int           g_elevated = 0;

void pdfs_set_context(const user_t *caller, int elevated)
{
    g_caller   = caller;
    g_elevated = elevated;
}

/* ---- Module state --------------------------------------------------------- */

static pdfs_superblock_t g_sb;
static int               g_mounted  = 0;
static int               g_ro       = 0;
static uint32_t          g_base_lba = 0;

/*
 * Static working buffers — kept off the stack to avoid overflow.
 * s_sec: one directory sector (8 slots × 64 B = 512 B).
 *        Used by all dir-chain operations sequentially (single-threaded).
 * s_io:  file data scratch; used only by pdfs_read / pdfs_write.
 */
static pdfs_dirent_t s_sec[8];   /* one dir sector                           */
static uint8_t       s_io[512];  /* file data I/O scratch                    */

/* ---- String / memory helpers --------------------------------------------- */

static int pd_strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static uint32_t pd_strlen(const char *s)
{
    uint32_t n = 0; while (s[n]) n++; return n;
}

static void pd_memcpy(void *dst, const void *src, uint32_t n)
{
    uint8_t *d = (uint8_t *)dst; const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
}

static void pd_memzero(void *dst, uint32_t n)
{
    uint8_t *d = (uint8_t *)dst; while (n--) *d++ = 0;
}

static void pd_strncpy(char *dst, const char *src, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n - 1u && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
}

/* ---- Permission checks ---------------------------------------------------- */

/* write: caller is owner, root, elevated, or other-write bit set */
static int perm_write_ok(const pdfs_dirent_t *de)
{
    if (g_elevated)                       return 1;
    if (!g_caller)                        return 0;
    if (g_caller->flags & USER_FLAG_ROOT) return 1;
    if (de->uid == g_caller->uid)         return 1;
    if (de->mode & PDFS_MODE_WOTH)        return 1;
    return 0;
}

/* execute/traverse: needed to enter a directory (cd / path traversal) */
static int perm_exec_ok(const pdfs_dirent_t *de)
{
    if (g_elevated)                       return 1;
    if (!g_caller)                        return 0;
    if (g_caller->flags & USER_FLAG_ROOT) return 1;
    if (de->uid == g_caller->uid && (de->mode & PDFS_MODE_XUSR)) return 1;
    if (de->mode & PDFS_MODE_XOTH)        return 1;
    return 0;
}

/* read: needed to list directory contents (ls) */
static int perm_read_ok(const pdfs_dirent_t *de)
{
    if (g_elevated)                       return 1;
    if (!g_caller)                        return 0;
    if (g_caller->flags & USER_FLAG_ROOT) return 1;
    if (de->uid == g_caller->uid && (de->mode & PDFS_MODE_RUSR)) return 1;
    if (de->mode & PDFS_MODE_ROTH)        return 1;
    return 0;
}

/* ---- Superblock flush ----------------------------------------------------- */

static void flush_sb(void)
{
    ata_write_sectors(g_base_lba, 1, &g_sb);
}

/* ---- Single-slot write-back ----------------------------------------------- */
/*
 * Read sec_lba into s_sec, replace slot slot_idx with *de, write back.
 */
static int flush_slot(uint32_t sec_lba, uint32_t slot_idx,
                      const pdfs_dirent_t *de)
{
    if (ata_read_sectors(sec_lba, 1, s_sec) != 0) return -1;
    pd_memcpy(&s_sec[slot_idx], de, sizeof(pdfs_dirent_t));
    return ata_write_sectors(sec_lba, 1, s_sec) == 0 ? 0 : -1;
}

/* ---- Directory chain helpers --------------------------------------------- */

/*
 * Search for entry named `name` in the dir chain starting at first_lba.
 * All out-parameters are optional (may be NULL).
 * Returns 0 if found, -1 if not found or I/O error.  Uses s_sec.
 */
static int chain_find(uint32_t first_lba, const char *name,
                      uint32_t *out_sec_lba, uint32_t *out_slot,
                      pdfs_dirent_t *out_de)
{
    uint32_t lba = first_lba;
    if (lba == 0u) return -1;

    while (lba != 0u) {
        if (ata_read_sectors(lba, 1, s_sec) != 0) return -1;

        uint32_t i;
        for (i = 0u; i < PDFS_CHAIN_SLOTS; i++) {
            if ((s_sec[i].flags & PDFS_FLAG_USED) &&
                !(s_sec[i].flags & PDFS_FLAG_CHAIN) &&
                pd_strcmp(s_sec[i].name, name) == 0) {
                if (out_sec_lba) *out_sec_lba = lba;
                if (out_slot)    *out_slot    = i;
                if (out_de)      pd_memcpy(out_de, &s_sec[i], sizeof(pdfs_dirent_t));
                return 0;
            }
        }
        lba = (s_sec[PDFS_CHAIN_LINK].flags & PDFS_FLAG_CHAIN)
              ? s_sec[PDFS_CHAIN_LINK].start_lba : 0u;
    }
    return -1;
}

/*
 * Find a free slot in the dir chain starting at first_lba.
 * If all sectors are full, allocate a new sector and chain it.
 * Returns 0 on success.  Uses s_sec and s_io (only for zeroing new sector).
 */
static int chain_alloc_slot(uint32_t first_lba,
                            uint32_t *out_sec_lba, uint32_t *out_slot)
{
    if (first_lba == 0u) return -1;

    uint32_t lba = first_lba, prev_lba = 0u;

    while (lba != 0u) {
        if (ata_read_sectors(lba, 1, s_sec) != 0) return -1;

        uint32_t i;
        for (i = 0u; i < PDFS_CHAIN_SLOTS; i++) {
            if (!(s_sec[i].flags & PDFS_FLAG_USED)) {
                *out_sec_lba = lba;
                *out_slot    = i;
                return 0;
            }
        }
        prev_lba = lba;
        lba = (s_sec[PDFS_CHAIN_LINK].flags & PDFS_FLAG_CHAIN)
              ? s_sec[PDFS_CHAIN_LINK].start_lba : 0u;
    }

    /* All sectors full — extend the chain with a new sector.
     * s_sec still holds prev_lba's data (last sector in old chain). */
    uint32_t new_lba = g_sb.next_free_lba;
    g_sb.next_free_lba += 1u;
    flush_sb();

    pd_memzero(s_io, 512u);
    if (ata_write_sectors(new_lba, 1, s_io) != 0) return -1;

    /* Link from prev_lba slot 7 (s_sec still has that sector's data). */
    pd_memzero(&s_sec[PDFS_CHAIN_LINK], sizeof(pdfs_dirent_t));
    s_sec[PDFS_CHAIN_LINK].flags     = PDFS_FLAG_CHAIN;
    s_sec[PDFS_CHAIN_LINK].start_lba = new_lba;
    if (ata_write_sectors(prev_lba, 1, s_sec) != 0) return -1;

    *out_sec_lba = new_lba;
    *out_slot    = 0u;
    return 0;
}

/* ---- Path resolution ------------------------------------------------------ */

/*
 * Resolve path to a specific dirent.  All out-params optional.
 * Returns 0 on success, -1 on failure.  Uses s_sec via chain_find.
 */
static int path_resolve(const char *path,
                        uint32_t      *out_sec_lba,
                        uint32_t      *out_slot,
                        pdfs_dirent_t *out_de)
{
    uint32_t cur_dir = g_sb.dir_lba;

    while (*path == '/') path++;
    if (*path == '\0') return -1;

    for (;;) {
        char comp[PDFS_NAME_LEN];
        uint32_t ci = 0u;
        while (*path && *path != '/' && ci < PDFS_NAME_LEN - 1u)
            comp[ci++] = *path++;
        comp[ci] = '\0';
        while (*path == '/') path++;

        if (comp[0] == '\0') return -1;

        uint32_t found_sec, found_slot;
        pdfs_dirent_t de;
        if (chain_find(cur_dir, comp, &found_sec, &found_slot, &de) != 0)
            return -1;

        if (*path == '\0') {
            /* Final component: if it's a directory, require execute to enter it.
             * This enforces cd-permission and blocks traversal into private dirs. */
            if ((de.flags & PDFS_FLAG_DIR) && !perm_exec_ok(&de)) return -4;
            if (out_sec_lba) *out_sec_lba = found_sec;
            if (out_slot)    *out_slot    = found_slot;
            if (out_de)      pd_memcpy(out_de, &de, sizeof(de));
            return 0;
        }

        if (!(de.flags & PDFS_FLAG_DIR)) return -1;
        /* Intermediate directory: require execute to traverse it. */
        if (!perm_exec_ok(&de)) return -4;
        cur_dir = de.start_lba;
    }
}

/*
 * Resolve the parent directory of path.
 *   *parent_first_lba — first sector of parent dir chain
 *   *name_out         — points to the leaf component within path
 * Returns 0 on success, -1 on failure.
 */
static int resolve_parent(const char *path,
                          uint32_t    *parent_first_lba,
                          const char **name_out)
{
    const char *last_slash = NULL;
    const char *p = path;
    while (*p) { if (*p == '/') last_slash = p; p++; }

    if (!last_slash || last_slash == path) {
        *parent_first_lba = g_sb.dir_lba;
        *name_out = last_slash ? last_slash + 1 : path;
        return 0;
    }

    char parent[128];
    uint32_t plen = (uint32_t)(last_slash - path);
    if (plen >= 128u) return -1;
    uint32_t k;
    for (k = 0u; k < plen; k++) parent[k] = path[k];
    parent[plen] = '\0';

    pdfs_dirent_t de;
    if (path_resolve(parent, NULL, NULL, &de) != 0) return -1;
    if (!(de.flags & PDFS_FLAG_DIR))              return -1;

    *parent_first_lba = de.start_lba;
    *name_out         = last_slash + 1;
    return 0;
}

/* ---- Inode encoding ------------------------------------------------------- */
/* v3: lba = specific sector containing this entry; slot = index 0-6 */
#define INODE_ENCODE(lba, slot)  (((lba) << 5u) | ((slot) & 0x1Fu))
#define INODE_IDX(inode)         ((inode) & 0x1Fu)
#define INODE_LBA(inode)         ((inode) >> 5u)

/* ---- VFS driver callbacks ------------------------------------------------- */

static int pdfs_mount(uint32_t base_lba)
{
    uint8_t buf[512];
    g_base_lba = base_lba;
    g_ro       = 0;

    if (ata_read_sectors(base_lba, 1, buf) != 0) return -1;
    pd_memcpy(&g_sb, buf, sizeof(g_sb));

    if (g_sb.magic != PDFS_MAGIC) return -2;
    if (g_sb.version != PDFS_VERSION) g_ro = 1;  /* old version: read-only */

    g_mounted = 1;
    return 0;
}

static int pdfs_open(const char *name, vfs_node_t *out)
{
    int r;
    if (!g_mounted) return -1;
    while (*name == '/') name++;
    if (*name == '\0') return -1;

    uint32_t sec_lba, slot;
    pdfs_dirent_t de;
    r = path_resolve(name, &sec_lba, &slot, &de);
    if (r != 0) return r;   /* propagate -4 (perm denied) vs -1 (not found) */

    pd_strncpy(out->name, de.name, VFS_NAME_MAX);
    out->size      = de.size;
    out->inode     = INODE_ENCODE(sec_lba, slot);
    out->is_dir    = (de.flags & PDFS_FLAG_DIR) ? 1u : 0u;
    out->mount_idx = 0u;
    return 0;
}

static int pdfs_read(vfs_node_t *node, uint32_t offset, uint32_t len, void *buf)
{
    if (!g_mounted) return -1;

    uint32_t sec_lba = INODE_LBA(node->inode);
    uint32_t slot    = INODE_IDX(node->inode);

    if (ata_read_sectors(sec_lba, 1, s_sec) != 0) return -1;
    pdfs_dirent_t de;
    pd_memcpy(&de, &s_sec[slot], sizeof(de));

    if (!(de.flags & PDFS_FLAG_USED) || (de.flags & PDFS_FLAG_DIR)) return -1;
    if (offset >= de.size) return 0;

    uint32_t to_read = de.size - offset;
    if (len < to_read) to_read = len;

    uint8_t *outp = (uint8_t *)buf;
    uint32_t done = 0u;
    while (done < to_read) {
        uint32_t abs_off    = offset + done;
        uint32_t sector_i   = abs_off / 512u;
        uint32_t sector_off = abs_off % 512u;
        uint32_t chunk      = 512u - sector_off;
        if (chunk > to_read - done) chunk = to_read - done;
        if (ata_read_sectors(de.start_lba + sector_i, 1, s_io) != 0) return -1;
        pd_memcpy(outp + done, s_io + sector_off, chunk);
        done += chunk;
    }
    return (int)done;
}

static int pdfs_write(vfs_node_t *node, uint32_t offset, uint32_t len,
                      const void *buf)
{
    if (!g_mounted) return -1;
    if (g_ro)       return -5;
    if (offset != 0) return -2;

    uint32_t sec_lba = INODE_LBA(node->inode);
    uint32_t slot    = INODE_IDX(node->inode);

    if (ata_read_sectors(sec_lba, 1, s_sec) != 0) return -1;
    pdfs_dirent_t de;
    pd_memcpy(&de, &s_sec[slot], sizeof(de));

    if (!(de.flags & PDFS_FLAG_USED) || (de.flags & PDFS_FLAG_DIR)) return -1;
    if (!perm_write_ok(&de)) return -3;

    if (len == 0u) {
        de.size = 0u;
        pd_memcpy(&s_sec[slot], &de, sizeof(de));
        return ata_write_sectors(sec_lba, 1, s_sec) == 0 ? 0 : -1;
    }

    uint32_t needed = (len + 511u) / 512u;
    if (de.alloc_sectors < needed) {
        de.start_lba     = g_sb.next_free_lba;
        de.alloc_sectors = needed;
        g_sb.next_free_lba += needed;
        flush_sb();   /* writes g_sb to g_base_lba; does not touch s_sec */
    }

    const uint8_t *src = (const uint8_t *)buf;
    uint32_t done = 0u, i;
    for (i = 0u; i < needed; i++) {
        pd_memzero(s_io, 512u);
        uint32_t chunk = len - done;
        if (chunk > 512u) chunk = 512u;
        pd_memcpy(s_io, src + done, chunk);
        if (ata_write_sectors(de.start_lba + i, 1, s_io) != 0) return -1;
        done += chunk;
    }

    /* s_sec still holds the dir sector from the read above — write back. */
    de.size = len;
    pd_memcpy(&s_sec[slot], &de, sizeof(de));
    return ata_write_sectors(sec_lba, 1, s_sec) == 0 ? (int)done : -1;
}

static int pdfs_create(const char *path)
{
    if (!g_mounted) return -1;
    if (g_ro)       return -5;
    while (*path == '/') path++;

    uint32_t parent_first;
    const char *fname;
    if (resolve_parent(path, &parent_first, &fname) != 0) return -1;
    if (pd_strlen(fname) == 0u || pd_strlen(fname) >= PDFS_NAME_LEN) return -2;

    if (chain_find(parent_first, fname, NULL, NULL, NULL) == 0) return -3;

    uint32_t new_sec, new_slot;
    if (chain_alloc_slot(parent_first, &new_sec, &new_slot) != 0) return -4;

    pdfs_dirent_t de;
    pd_memzero(&de, sizeof(de));
    pd_strncpy(de.name, fname, PDFS_NAME_LEN);
    de.flags = PDFS_FLAG_USED;
    de.mode  = PDFS_MODE_DEFAULT;
    de.uid   = g_caller ? g_caller->uid : 0u;
    de.ctime = pit_get_ticks();
    return flush_slot(new_sec, new_slot, &de);
}

static int pdfs_unlink(const char *path)
{
    if (!g_mounted) return -1;
    if (g_ro)       return -5;
    while (*path == '/') path++;

    uint32_t sec_lba, slot;
    pdfs_dirent_t de;
    if (path_resolve(path, &sec_lba, &slot, &de) != 0) return -1;
    if (de.flags & PDFS_FLAG_DIR) return -2;
    if (!perm_write_ok(&de))      return -3;

    pd_memzero(&de, sizeof(de));
    return flush_slot(sec_lba, slot, &de);
}

static int pdfs_readdir(uint32_t idx, vfs_node_t *out)
{
    if (!g_mounted) return -1;

    uint32_t count = 0u, lba = g_sb.dir_lba;
    while (lba != 0u) {
        if (ata_read_sectors(lba, 1, s_sec) != 0) return -1;
        uint32_t i;
        for (i = 0u; i < PDFS_CHAIN_SLOTS; i++) {
            if (s_sec[i].flags & PDFS_FLAG_USED) {
                if (count == idx) {
                    pd_strncpy(out->name, s_sec[i].name, VFS_NAME_MAX);
                    out->size      = s_sec[i].size;
                    out->inode     = INODE_ENCODE(lba, i);
                    out->is_dir    = (s_sec[i].flags & PDFS_FLAG_DIR) ? 1u : 0u;
                    out->mount_idx = 0u;
                    return 0;
                }
                count++;
            }
        }
        lba = (s_sec[PDFS_CHAIN_LINK].flags & PDFS_FLAG_CHAIN)
              ? s_sec[PDFS_CHAIN_LINK].start_lba : 0u;
    }
    return -1;
}

/* ---- Public API: format --------------------------------------------------- */

int pdfs_format(uint32_t base_lba)
{
    pd_memzero(&g_sb, sizeof(g_sb));
    g_sb.magic         = PDFS_MAGIC;
    g_sb.version       = PDFS_VERSION;
    g_sb.jrnl_lba      = base_lba + 1u;
    g_sb.dir_lba       = base_lba + 2u;
    g_sb.dir_sectors   = 1u;
    g_sb.data_lba      = base_lba + 3u;
    g_sb.next_free_lba = base_lba + 3u;

    if (ata_write_sectors(base_lba, 1, &g_sb) != 0) return -1;

    pd_memzero(s_io, 512u);
    if (ata_write_sectors(g_sb.jrnl_lba, 1, s_io) != 0) return -2;
    if (ata_write_sectors(g_sb.dir_lba,  1, s_io) != 0) return -3;

    g_base_lba = base_lba;
    g_mounted  = 1;
    g_ro       = 0;
    return 0;
}

/* ---- Public API: mkdir, chmod, chown ------------------------------------- */

int pdfs_mkdir(const char *path, uint8_t uid, uint8_t gid, uint16_t mode)
{
    if (!g_mounted) return -1;
    if (g_ro)       return -5;
    while (*path == '/') path++;

    uint32_t parent_first;
    const char *dname;
    if (resolve_parent(path, &parent_first, &dname) != 0) return -1;
    if (pd_strlen(dname) == 0u || pd_strlen(dname) >= PDFS_NAME_LEN) return -2;

    /* -3 = already exists: callers (scaffold) treat this as idempotent */
    if (chain_find(parent_first, dname, NULL, NULL, NULL) == 0) return -3;

    /* Allocate the new directory's initial single sector. */
    uint32_t sub_lba = g_sb.next_free_lba;
    g_sb.next_free_lba += 1u;
    flush_sb();

    pd_memzero(s_io, 512u);
    if (ata_write_sectors(sub_lba, 1, s_io) != 0) return -6;

    uint32_t new_sec, new_slot;
    if (chain_alloc_slot(parent_first, &new_sec, &new_slot) != 0) return -4;

    pdfs_dirent_t de;
    pd_memzero(&de, sizeof(de));
    pd_strncpy(de.name, dname, PDFS_NAME_LEN);
    de.start_lba     = sub_lba;
    de.alloc_sectors = 1u;
    de.flags         = PDFS_FLAG_USED | PDFS_FLAG_DIR;
    de.uid           = uid;
    de.gid           = gid;
    de.mode          = mode ? mode : PDFS_MODE_DIR_DEF;
    de.dir_sectors   = 1u;
    de.ctime         = pit_get_ticks();
    return flush_slot(new_sec, new_slot, &de);
}

int pdfs_chmod(const char *path, uint16_t mode)
{
    if (!g_mounted) return -1;
    if (g_ro)       return -5;
    while (*path == '/') path++;

    uint32_t sec_lba, slot;
    pdfs_dirent_t de;
    if (path_resolve(path, &sec_lba, &slot, &de) != 0) return -1;
    if (!perm_write_ok(&de)) return -3;

    de.mode = mode;
    return flush_slot(sec_lba, slot, &de);
}

int pdfs_chown(const char *path, uint8_t uid, uint8_t gid)
{
    if (!g_mounted) return -1;
    if (g_ro)       return -5;
    while (*path == '/') path++;

    if (!g_elevated && !(g_caller && (g_caller->flags & USER_FLAG_ROOT)))
        return -3;

    uint32_t sec_lba, slot;
    pdfs_dirent_t de;
    if (path_resolve(path, &sec_lba, &slot, &de) != 0) return -1;

    de.uid = uid;
    de.gid = gid;
    return flush_slot(sec_lba, slot, &de);
}

/* ---- Statistics ----------------------------------------------------------- */

uint32_t pdfs_free_sectors(void)
{
    const ata_drive_t *drv;
    if (!g_mounted) return 0u;
    drv = ata_get_drive();
    if (!drv->present) return 0u;
    if (g_sb.next_free_lba >= drv->total_sectors) return 0u;
    return drv->total_sectors - g_sb.next_free_lba;
}

uint32_t pdfs_file_count(void)
{
    if (!g_mounted) return 0u;
    uint32_t n = 0u, lba = g_sb.dir_lba;
    while (lba != 0u) {
        if (ata_read_sectors(lba, 1, s_sec) != 0) break;
        uint32_t i;
        for (i = 0u; i < PDFS_CHAIN_SLOTS; i++)
            if (s_sec[i].flags & PDFS_FLAG_USED) n++;
        lba = (s_sec[PDFS_CHAIN_LINK].flags & PDFS_FLAG_CHAIN)
              ? s_sec[PDFS_CHAIN_LINK].start_lba : 0u;
    }
    return n;
}

int pdfs_is_ro(void) { return g_ro; }

/* ---- Directory enumeration ----------------------------------------------- */

int pdfs_stat_root(uint32_t idx, pdfs_dirent_t *out)
{
    return pdfs_stat_dir("", idx, out);
}

int pdfs_stat_dir(const char *path, uint32_t idx, pdfs_dirent_t *out)
{
    if (!g_mounted) return -1;
    while (*path == '/') path++;

    uint32_t first_lba;
    if (*path == '\0') {
        first_lba = g_sb.dir_lba;
    } else {
        pdfs_dirent_t de;
        int r = path_resolve(path, NULL, NULL, &de);
        if (r != 0) return r;                        /* -4 perm denied, -1 not found */
        if (!(de.flags & PDFS_FLAG_DIR)) return -1;
        if (!perm_read_ok(&de)) return -4;           /* have exec but not read (ls blocked) */
        first_lba = de.start_lba;
    }

    uint32_t count = 0u, lba = first_lba;
    while (lba != 0u) {
        if (ata_read_sectors(lba, 1, s_sec) != 0) return -1;
        uint32_t i;
        for (i = 0u; i < PDFS_CHAIN_SLOTS; i++) {
            if (s_sec[i].flags & PDFS_FLAG_USED) {
                if (count == idx) {
                    pd_memcpy(out, &s_sec[i], sizeof(*out));
                    return 0;
                }
                count++;
            }
        }
        lba = (s_sec[PDFS_CHAIN_LINK].flags & PDFS_FLAG_CHAIN)
              ? s_sec[PDFS_CHAIN_LINK].start_lba : 0u;
    }
    return -1;
}

/* ---- Public API: create user home directory ------------------------------ */
/*
 * Create /home/<username> with mode 0700 (rwx------) owned by uid,
 * plus the standard XDG subdirectories.  Public/ gets 0755 so other
 * users can read it.  Runs fully elevated — caller need not set context.
 */
void pdfs_create_home(const char *username, uint8_t uid)
{
    char path[64];
    uint32_t ulen = pd_strlen(username);
    uint32_t i, j, s;

    static const char * const subdirs[] = {
        "Desktop", "Documents", "Downloads", "Pictures",
        "Videos",  "Music",     "Templates", NULL
    };

    if (ulen == 0u || ulen > 27u) return;

    pdfs_set_context(NULL, 1);   /* elevated — root creates home dirs */

    /* Build: /home/<username>  (mode 0700 = rwx------) */
    path[0]='/' ; path[1]='h'; path[2]='o'; path[3]='m';
    path[4]='e' ; path[5]='/';
    for (j = 0u; j < ulen; j++) path[6u + j] = username[j];
    path[6u + ulen] = '\0';
    pdfs_mkdir(path, uid, uid, 0x1C0u);

    /* Private subdirs: same 0700 mode */
    for (s = 0u; subdirs[s]; s++) {
        uint32_t slen = pd_strlen(subdirs[s]);
        path[6u + ulen] = '/';
        for (i = 0u; i < slen; i++) path[6u + ulen + 1u + i] = subdirs[s][i];
        path[6u + ulen + 1u + slen] = '\0';
        pdfs_mkdir(path, uid, uid, 0x1C0u);
    }

    /* Public/ — world-readable (0755 = rwxr-xr-x) */
    path[6u + ulen] = '/';
    path[6u + ulen + 1u] = 'P'; path[6u + ulen + 2u] = 'u';
    path[6u + ulen + 3u] = 'b'; path[6u + ulen + 4u] = 'l';
    path[6u + ulen + 5u] = 'i'; path[6u + ulen + 6u] = 'c';
    path[6u + ulen + 7u] = '\0';
    pdfs_mkdir(path, uid, uid, 0x1EDu);

    pdfs_set_context(NULL, 0);
}

/* ---- Filesystem scaffold ------------------------------------------------- */
/*
 * Create the standard PD-OS / FHS directory tree.
 * All pdfs_mkdir calls return -3 (already exists) when the dir is present,
 * which is silently ignored — making this function safely idempotent.
 * Called at boot (after mount) and after mkpdfs reformat.
 */
void pdfs_scaffold(void)
{
    static const char ver[] = "PD-OS v0.1\n";
    vfs_node_t nd;

    pdfs_set_context(NULL, 1);   /* elevated — bypasses permission checks */

    /* ---- Root-level FHS directories --------------------------------------- */
    pdfs_mkdir("/bin",   0, 0, PDFS_MODE_DIR_DEF);
    pdfs_mkdir("/sbin",  0, 0, PDFS_MODE_DIR_DEF);
    pdfs_mkdir("/lib",   0, 0, PDFS_MODE_DIR_DEF);
    pdfs_mkdir("/root",  0, 0, 0x1C0u);              /* rwx------ */
    pdfs_mkdir("/home",  0, 0, PDFS_MODE_DIR_DEF);
    pdfs_mkdir("/etc",   0, 0, PDFS_MODE_DIR_DEF);
    pdfs_mkdir("/var",   0, 0, PDFS_MODE_DIR_DEF);
    pdfs_mkdir("/usr",   0, 0, 0x1C0u);   /* rwx------: root only              */
    pdfs_chmod("/usr",   0x1C0u);          /* enforce on every boot/reformat     */
    pdfs_chmod("/home",  PDFS_MODE_DIR_DEF); /* enforce world-traversable          */
    pdfs_mkdir("/opt",   0, 0, PDFS_MODE_DIR_DEF);
    pdfs_mkdir("/tmp",   0, 0, 0x1FFu);              /* rwxrwxrwx */
    pdfs_mkdir("/dev",   0, 0, PDFS_MODE_DIR_DEF);
    pdfs_mkdir("/proc",  0, 0, PDFS_MODE_DIR_DEF);
    pdfs_mkdir("/mnt",   0, 0, PDFS_MODE_DIR_DEF);

    /* ---- /home/pd user dirs (rwx------ for all private dirs) -------------- */
    pdfs_mkdir("/home/pd",           1, 1, 0x1C0u);   /* rwx------ */
    pdfs_mkdir("/home/pd/Desktop",   1, 1, 0x1C0u);
    pdfs_mkdir("/home/pd/Documents", 1, 1, 0x1C0u);
    pdfs_mkdir("/home/pd/Downloads", 1, 1, 0x1C0u);
    pdfs_mkdir("/home/pd/Pictures",  1, 1, 0x1C0u);
    pdfs_mkdir("/home/pd/Videos",    1, 1, 0x1C0u);
    pdfs_mkdir("/home/pd/Music",     1, 1, 0x1C0u);
    pdfs_mkdir("/home/pd/Templates", 1, 1, 0x1C0u);
    pdfs_mkdir("/home/pd/Public",    1, 1, 0x1EDu);   /* rwxr-xr-x */

    /* ---- /root user dirs -------------------------------------------------- */
    pdfs_mkdir("/root/Desktop",   0, 0, 0x1C0u);
    pdfs_mkdir("/root/Documents", 0, 0, 0x1C0u);
    pdfs_mkdir("/root/Downloads", 0, 0, 0x1C0u);
    pdfs_mkdir("/root/Pictures",  0, 0, 0x1C0u);
    pdfs_mkdir("/root/Videos",    0, 0, 0x1C0u);
    pdfs_mkdir("/root/Music",     0, 0, 0x1C0u);
    pdfs_mkdir("/root/Templates", 0, 0, 0x1C0u);
    pdfs_mkdir("/root/Public",    0, 0, 0x1EDu);

    /* ---- /etc subdirs ----------------------------------------------------- */
    pdfs_mkdir("/etc/pd-os", 0, 0, PDFS_MODE_DIR_DEF);

    /* ---- /var subdirs ----------------------------------------------------- */
    pdfs_mkdir("/var/log",   0, 0, PDFS_MODE_DIR_DEF);
    pdfs_mkdir("/var/tmp",   0, 0, 0x1FFu);
    pdfs_mkdir("/var/cache", 0, 0, PDFS_MODE_DIR_DEF);

    /* ---- /usr subdirs ----------------------------------------------------- */
    pdfs_mkdir("/usr/bin",       0, 0, PDFS_MODE_DIR_DEF);
    pdfs_mkdir("/usr/sbin",      0, 0, PDFS_MODE_DIR_DEF);
    pdfs_mkdir("/usr/lib",       0, 0, PDFS_MODE_DIR_DEF);
    pdfs_mkdir("/usr/share",     0, 0, PDFS_MODE_DIR_DEF);
    pdfs_mkdir("/usr/local",     0, 0, PDFS_MODE_DIR_DEF);
    pdfs_mkdir("/usr/local/bin", 0, 0, PDFS_MODE_DIR_DEF);

    /* ---- /etc/pd-os/version (create only if absent) ----------------------- */
    if (vfs_open("/etc/pd-os/version", &nd) != 0) {
        if (vfs_create("/etc/pd-os/version") == 0 &&
            vfs_open("/etc/pd-os/version", &nd) == 0)
            vfs_write(&nd, 0, (uint32_t)(sizeof(ver) - 1u), ver);
    }

    pdfs_set_context(NULL, 0);
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
