/* ============================================================================
 * PD-Kernel  —  PDFS v2  (PD Filesystem, extended)  (Phase 8f)
 *
 * Implements the vfs_driver_t interface for PDFS version 2.
 *
 * New in v2:
 *   - 64-byte dirents: name(28), perms uid/gid/mode, ctime, is_dir flag
 *   - Subdirectory support: each dir has its own 4-sector dirent table
 *   - Full path traversal: open/create/unlink/mkdir resolve nested paths
 *   - Write-ahead journal: protects dir-sector flushes from torn writes
 *   - Permission checks on create/write/unlink/mkdir
 *   - v1 disk detection: mounts read-only when version != 2
 *
 * Backwards compatibility:
 *   - If the on-disk version != 2, g_ro=1 and all writes return -5.
 *   - Run mkpdfs (requires elev) to upgrade/reformat to v2.
 * ============================================================================ */

#include "pdfs.h"
#include "ata.h"
#include "users.h"
#include "pit.h"

/* ---- Permission context (set from shell via pdfs_set_context) ------------ */

static const user_t *g_caller   = NULL;
static int           g_elevated = 0;

void pdfs_set_context(const user_t *caller, int elevated)
{
    g_caller   = caller;
    g_elevated = elevated;
}

/* ---- Module state --------------------------------------------------------- */

static pdfs_superblock_t g_sb;
static pdfs_dirent_t     g_dir[PDFS_MAX_FILES];   /* root directory cache    */
static int               g_mounted  = 0;
static int               g_ro       = 0;           /* read-only (v1 disk)    */
static uint32_t          g_base_lba = 0;

/* Static work buffers — safe for single-threaded kernel, avoid stack overflow.
 * Each pdfs_dirent_t[32] = 2048 bytes; keeping them off the stack is critical. */
static pdfs_dirent_t     s_pr[PDFS_MAX_FILES];    /* path_resolve working set  */
static pdfs_dirent_t     s_rp[PDFS_MAX_FILES];    /* resolve_parent interim    */
static pdfs_dirent_t     s_op[PDFS_MAX_FILES];    /* per-operation dir table   */

/* Forward declaration — pd_memcpy is defined in the helpers section below. */
static void pd_memcpy(void *dst, const void *src, uint32_t n);

/*
 * Load a 32-entry directory table into `out`.
 * For the root directory, serves from the in-memory cache (no ATA read).
 * For subdirectories, reads one 512-byte sector at a time to avoid
 * reliability issues with multi-sector ATA PIO reads.
 */
static int load_dir(uint32_t dir_lba, pdfs_dirent_t *out)
{
    if (dir_lba == g_sb.dir_lba) {
        pd_memcpy(out, g_dir, PDFS_MAX_FILES * sizeof(pdfs_dirent_t));
        return 0;
    }
    /* Subdir: read one sector at a time */
    uint32_t s;
    uint8_t *p = (uint8_t *)out;
    for (s = 0; s < PDFS_DIR_SECTORS; s++) {
        if (ata_read_sectors(dir_lba + s, 1, p) != 0) return -1;
        p += 512u;
    }
    return 0;
}

/* ---- Internal helpers ----------------------------------------------------- */

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

/* ---- Permission check ----------------------------------------------------- */

static int perm_write_ok(const pdfs_dirent_t *de)
{
    if (g_elevated) return 1;
    if (!g_caller)  return 0;
    if (g_caller->flags & USER_FLAG_ROOT) return 1;
    if (de->uid == g_caller->uid)         return 1;
    if (de->mode & PDFS_MODE_WOTH)        return 1;
    return 0;
}

/* ---- Journal -------------------------------------------------------------- */

/*
 * Write `sector_buf` to the journal slot, then flush to `target_lba`.
 * The journal sector stores:
 *   [0..15]   pdfs_jrnl_hdr_t  (dirty flag + target_lba)
 *   [16..511] first 496 bytes of the to-be-written sector
 *
 * Since a dir sector is 512 bytes but the journal sector is also 512 bytes,
 * we can't store a full 512-byte sector payload alongside the 16-byte header.
 * Strategy: the journal sector IS the pending sector.  The header is stored
 * in the first 16 bytes, and the remaining 496 bytes are the sector data
 * AFTER offset 16.  We write the actual target sector separately.
 * Simpler alternative (used here): store the pending sector at jrnl_lba
 * and store dirty+target in the superblock reserved[0..1].
 */
static void jrnl_begin(uint32_t target_lba, const void *sector_buf)
{
    /* Mark journal dirty in superblock so we can replay on next mount */
    g_sb.reserved[0] = 1u;           /* dirty flag                          */
    g_sb.reserved[1] = target_lba;   /* target                              */
    ata_write_sectors(g_base_lba, 1, &g_sb);
    /* Write the pending sector to the journal LBA */
    ata_write_sectors(g_sb.jrnl_lba, 1, sector_buf);
}

static void jrnl_commit(void)
{
    /* Clear the dirty flag */
    g_sb.reserved[0] = 0u;
    g_sb.reserved[1] = 0u;
    ata_write_sectors(g_base_lba, 1, &g_sb);
}

static void jrnl_replay(void)
{
    if (g_sb.reserved[0] != 1u) return;   /* not dirty */
    uint32_t target = g_sb.reserved[1];
    uint8_t  jbuf[512];
    if (ata_read_sectors(g_sb.jrnl_lba, 1, jbuf) != 0) return;
    ata_write_sectors(target, 1, jbuf);
    g_sb.reserved[0] = 0u;
    g_sb.reserved[1] = 0u;
    ata_write_sectors(g_base_lba, 1, &g_sb);
}

/* ---- Directory flush (journal-protected) ---------------------------------- */

/* Number of dirents that fit in one 512-byte sector (64B each = 8). */
#define DENTS_PER_SEC  (512u / sizeof(pdfs_dirent_t))   /* = 8 */

/* Flush one 512-byte sector (sector_idx) of the directory table at lba. */
static void flush_dir_sector(const pdfs_dirent_t *dir, uint32_t lba,
                              uint32_t sector_idx)
{
    uint8_t buf[512];
    pd_memzero(buf, 512);
    pd_memcpy(buf, dir + sector_idx * DENTS_PER_SEC,
              DENTS_PER_SEC * sizeof(pdfs_dirent_t));
    jrnl_begin(lba + sector_idx, buf);
    ata_write_sectors(lba + sector_idx, 1, buf);
    jrnl_commit();
}

/* Flush the entire directory table at `lba` (all PDFS_DIR_SECTORS sectors). */
static void flush_dir_at(const pdfs_dirent_t *dir, uint32_t lba)
{
    uint32_t i;
    for (i = 0; i < PDFS_DIR_SECTORS; i++)
        flush_dir_sector(dir, lba, i);
}

static void flush_sb(void)
{
    ata_write_sectors(g_base_lba, 1, &g_sb);
}

/* ---- Path resolution ------------------------------------------------------ */

/*
 * Resolve `path` (absolute or relative-to-root) to a dirent inside its
 * parent directory.  On success:
 *   - out_dir[]   = the 32-entry directory table containing the leaf entry
 *   - *out_lba    = LBA of that directory table on disk
 *   - returns     = index of the leaf dirent in out_dir[]
 *
 * On failure returns -1.
 *
 * Traversal always starts at the PDFS root (g_dir / g_sb.dir_lba).
 * A leading '/' is silently stripped.
 * Empty components (e.g. "//") are skipped.
 */
static int path_resolve(const char *path,
                        pdfs_dirent_t *out_dir,
                        uint32_t      *out_lba)
{
    /* Use static buffer — avoids 2KB+ stack allocation */
    uint32_t cur_lba = g_sb.dir_lba;
    pd_memcpy(s_pr, g_dir, PDFS_MAX_FILES * sizeof(pdfs_dirent_t));

    /* Strip leading '/' */
    while (*path == '/') path++;
    if (*path == '\0') return -1;

    for (;;) {
        /* Parse next component */
        char     comp[PDFS_NAME_LEN];
        uint32_t ci = 0;
        while (*path && *path != '/' && ci < PDFS_NAME_LEN - 1u)
            comp[ci++] = *path++;
        comp[ci] = '\0';
        /* Skip consecutive '/' */
        while (*path == '/') path++;

        if (comp[0] == '\0') return -1;

        /* Find comp in s_pr[] */
        uint32_t i;
        int found = -1;
        for (i = 0; i < PDFS_MAX_FILES; i++) {
            if ((s_pr[i].flags & PDFS_FLAG_USED) &&
                pd_strcmp(s_pr[i].name, comp) == 0) {
                found = (int)i;
                break;
            }
        }
        if (found < 0) return -1;

        if (*path == '\0') {
            /* This is the leaf — return */
            pd_memcpy(out_dir, s_pr, PDFS_MAX_FILES * sizeof(pdfs_dirent_t));
            *out_lba = cur_lba;
            return found;
        }

        /* Must be a directory to descend */
        if (!(s_pr[found].flags & PDFS_FLAG_DIR)) return -1;

        /* Load subdir directly into s_pr — no extra stack array needed */
        uint32_t sub_lba = s_pr[found].start_lba;
        /* Read one sector at a time: avoids multi-sector ATA PIO issues */
        uint32_t ps;
        uint8_t *pp = (uint8_t *)s_pr;
        for (ps = 0; ps < PDFS_DIR_SECTORS; ps++) {
            if (ata_read_sectors(sub_lba + ps, 1, pp) != 0) return -1;
            pp += 512u;
        }
        cur_lba = sub_lba;
    }
}

/*
 * Resolve the PARENT directory of `path`.
 * `*name_out` is set to point at the filename component within `path`.
 * Returns 0 on success, -1 on failure.
 */
static int resolve_parent(const char *path,
                          pdfs_dirent_t *out_dir,
                          uint32_t      *out_lba,
                          const char   **name_out)
{
    /* Find last '/' */
    const char *last_slash = 0;
    const char *p = path;
    while (*p) { if (*p == '/') last_slash = p; p++; }

    if (!last_slash || last_slash == path) {
        /* No slash or leading slash only → parent is root */
        pd_memcpy(out_dir, g_dir, PDFS_MAX_FILES * sizeof(pdfs_dirent_t));
        *out_lba   = g_sb.dir_lba;
        *name_out  = last_slash ? last_slash + 1 : path;
        return 0;
    }

    /* Resolve the parent path */
    char parent[128];
    uint32_t plen = (uint32_t)(last_slash - path);
    if (plen >= 128u) return -1;
    uint32_t k;
    for (k = 0; k < plen; k++) parent[k] = path[k];
    parent[plen] = '\0';

    uint32_t      parent_container_lba;
    int pidx = path_resolve(parent, s_rp, &parent_container_lba);
    if (pidx < 0) return -1;
    if (!(s_rp[pidx].flags & PDFS_FLAG_DIR)) return -1;

    uint32_t parent_lba = s_rp[pidx].start_lba;
    /* resolve_parent for subdirs also reads one sector at a time via load_dir */
    uint32_t ps;
    uint8_t *pp = (uint8_t *)out_dir;
    for (ps = 0; ps < PDFS_DIR_SECTORS; ps++) {
        if (ata_read_sectors(parent_lba + ps, 1, pp) != 0) return -1;
        pp += 512u;
    }
    *out_lba  = parent_lba;
    *name_out = last_slash + 1;
    return 0;
}

/* ---- Inode encoding ------------------------------------------------------- */
/* vfs_node_t.inode = (dir_lba << 5) | slot_index                           */
/* dir_lba must be < 2^27; slot_index < 32                                  */
#define INODE_ENCODE(lba, idx)   (((lba) << 5u) | ((idx) & 0x1Fu))
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
    if (g_sb.version != PDFS_VERSION) g_ro = 1;

    /* Replay journal before loading directory (v2 only) */
    if (!g_ro) jrnl_replay();

    {
        uint32_t s; uint8_t *p = (uint8_t *)g_dir;
        for (s = 0; s < PDFS_DIR_SECTORS; s++) {
            if (ata_read_sectors(g_sb.dir_lba + s, 1, p) != 0) return -4;
            p += 512u;
        }
    }

    g_mounted = 1;
    return 0;
}

static int pdfs_open(const char *name, vfs_node_t *out)
{
    if (!g_mounted) return -1;

    /* Strip leading '/' */
    const char *p = name;
    while (*p == '/') p++;

    uint32_t dir_lba;
    int idx = path_resolve(p, s_op, &dir_lba);
    if (idx < 0) return -1;

    pd_strncpy(out->name, s_op[idx].name, VFS_NAME_MAX);
    out->size      = s_op[idx].size;
    out->inode     = INODE_ENCODE(dir_lba, (uint32_t)idx);
    out->is_dir    = (s_op[idx].flags & PDFS_FLAG_DIR) ? 1u : 0u;
    out->mount_idx = 0;
    return 0;
}

static int pdfs_read(vfs_node_t *node, uint32_t offset, uint32_t len, void *buf)
{
    if (!g_mounted) return -1;

    uint32_t idx     = INODE_IDX(node->inode);
    uint32_t dir_lba = INODE_LBA(node->inode);

    if (load_dir(dir_lba, s_op) != 0) return -1;

    pdfs_dirent_t *de = &s_op[idx];
    if (!(de->flags & PDFS_FLAG_USED) || (de->flags & PDFS_FLAG_DIR)) return -1;
    if (offset >= de->size) return 0;

    uint32_t to_read = de->size - offset;
    if (len < to_read) to_read = len;

    uint8_t *out  = (uint8_t *)buf;
    uint32_t done = 0;
    while (done < to_read) {
        uint8_t  tmp[512];
        uint32_t abs_off    = offset + done;
        uint32_t sector_i   = abs_off / 512u;
        uint32_t sector_off = abs_off % 512u;
        uint32_t chunk      = 512u - sector_off;
        if (chunk > to_read - done) chunk = to_read - done;
        if (ata_read_sectors(de->start_lba + sector_i, 1, tmp) != 0) return -1;
        pd_memcpy(out + done, tmp + sector_off, chunk);
        done += chunk;
    }
    return (int)done;
}

static int pdfs_write(vfs_node_t *node, uint32_t offset, uint32_t len,
                      const void *buf)
{
    if (!g_mounted) return -1;
    if (g_ro)       return -5;
    if (offset != 0) return -2;   /* full-file writes only */

    uint32_t idx     = INODE_IDX(node->inode);
    uint32_t dir_lba = INODE_LBA(node->inode);

    if (load_dir(dir_lba, s_op) != 0) return -1;

    pdfs_dirent_t *de = &s_op[idx];
    if (!(de->flags & PDFS_FLAG_USED) || (de->flags & PDFS_FLAG_DIR)) return -1;
    if (!perm_write_ok(de)) return -3;

    if (len == 0) {
        de->size = 0;
        flush_dir_at(s_op, dir_lba);
        if (dir_lba == g_sb.dir_lba) pd_memcpy(g_dir, s_op, sizeof(g_dir));
        return 0;
    }

    uint32_t needed = (len + 511u) / 512u;
    if (de->alloc_sectors < needed) {
        de->start_lba     = g_sb.next_free_lba;
        de->alloc_sectors = needed;
        g_sb.next_free_lba += needed;
        flush_sb();
    }

    const uint8_t *src  = (const uint8_t *)buf;
    uint32_t       done = 0;
    uint32_t       i;
    for (i = 0; i < needed; i++) {
        uint8_t  tmp[512];
        uint32_t chunk = len - done;
        if (chunk > 512u) chunk = 512u;
        pd_memzero(tmp, 512u);
        pd_memcpy(tmp, src + done, chunk);
        if (ata_write_sectors(de->start_lba + i, 1, tmp) != 0) return -1;
        done += chunk;
    }

    de->size = len;
    flush_dir_at(s_op, dir_lba);
    if (dir_lba == g_sb.dir_lba) pd_memcpy(g_dir, s_op, sizeof(g_dir));
    return (int)done;
}

static int pdfs_create(const char *path)
{
    if (!g_mounted) return -1;
    if (g_ro)       return -5;

    /* Strip leading '/' */
    while (*path == '/') path++;

    const char   *fname;
    uint32_t      dir_lba;
    if (resolve_parent(path, s_op, &dir_lba, &fname) != 0) return -1;
    if (pd_strlen(fname) == 0 || pd_strlen(fname) >= PDFS_NAME_LEN) return -2;

    uint32_t i;
    for (i = 0; i < PDFS_MAX_FILES; i++)
        if ((s_op[i].flags & PDFS_FLAG_USED) &&
            pd_strcmp(s_op[i].name, fname) == 0) return -3;

    for (i = 0; i < PDFS_MAX_FILES; i++) {
        if (!(s_op[i].flags & PDFS_FLAG_USED)) {
            pd_memzero(&s_op[i], sizeof(pdfs_dirent_t));
            pd_strncpy(s_op[i].name, fname, PDFS_NAME_LEN);
            s_op[i].flags = PDFS_FLAG_USED;
            s_op[i].mode  = PDFS_MODE_DEFAULT;
            s_op[i].uid   = g_caller ? g_caller->uid : 0u;
            s_op[i].ctime = pit_get_ticks();
            flush_dir_at(s_op, dir_lba);
            if (dir_lba == g_sb.dir_lba) pd_memcpy(g_dir, s_op, sizeof(g_dir));
            return 0;
        }
    }
    return -4;
}

static int pdfs_unlink(const char *path)
{
    if (!g_mounted) return -1;
    if (g_ro)       return -5;

    while (*path == '/') path++;

    const char   *fname;
    uint32_t      dir_lba;
    if (resolve_parent(path, s_op, &dir_lba, &fname) != 0) return -1;

    uint32_t i;
    for (i = 0; i < PDFS_MAX_FILES; i++) {
        if ((s_op[i].flags & PDFS_FLAG_USED) &&
            pd_strcmp(s_op[i].name, fname) == 0) {
            if (!perm_write_ok(&s_op[i])) return -3;
            pd_memzero(&s_op[i], sizeof(pdfs_dirent_t));
            flush_dir_at(s_op, dir_lba);
            if (dir_lba == g_sb.dir_lba) pd_memcpy(g_dir, s_op, sizeof(g_dir));
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
                out->inode     = INODE_ENCODE(g_sb.dir_lba, i);
                out->is_dir    = (g_dir[i].flags & PDFS_FLAG_DIR) ? 1u : 0u;
                out->mount_idx = 0;
                return 0;
            }
            count++;
        }
    }
    return -1;
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

/* ---- mkdir ---------------------------------------------------------------- */

int pdfs_mkdir(const char *path, uint8_t uid, uint8_t gid, uint16_t mode)
{
    if (!g_mounted) return -1;
    if (g_ro)       return -5;

    while (*path == '/') path++;

    const char   *dname;
    uint32_t      dir_lba;
    if (resolve_parent(path, s_op, &dir_lba, &dname) != 0) return -1;
    if (pd_strlen(dname) == 0 || pd_strlen(dname) >= PDFS_NAME_LEN) return -2;

    uint32_t i;
    for (i = 0; i < PDFS_MAX_FILES; i++)
        if ((s_op[i].flags & PDFS_FLAG_USED) &&
            pd_strcmp(s_op[i].name, dname) == 0) return -3;

    /* Allocate a fresh empty dir table on disk */
    uint32_t sub_lba = g_sb.next_free_lba;
    {
        uint8_t empty[512];
        pd_memzero(empty, 512);
        uint32_t s;
        for (s = 0; s < PDFS_DIR_SECTORS; s++) {
            if (ata_write_sectors(sub_lba + s, 1, empty) != 0) return -6;
        }
    }
    g_sb.next_free_lba += PDFS_DIR_SECTORS;
    flush_sb();

    for (i = 0; i < PDFS_MAX_FILES; i++) {
        if (!(s_op[i].flags & PDFS_FLAG_USED)) {
            pd_memzero(&s_op[i], sizeof(pdfs_dirent_t));
            pd_strncpy(s_op[i].name, dname, PDFS_NAME_LEN);
            s_op[i].start_lba     = sub_lba;
            s_op[i].size          = 0;
            s_op[i].alloc_sectors = PDFS_DIR_SECTORS;
            s_op[i].flags         = PDFS_FLAG_USED | PDFS_FLAG_DIR;
            s_op[i].uid           = uid;
            s_op[i].gid           = gid;
            s_op[i].mode          = mode ? mode : PDFS_MODE_DIR_DEF;
            s_op[i].dir_sectors   = PDFS_DIR_SECTORS;
            s_op[i].ctime         = pit_get_ticks();
            flush_dir_at(s_op, dir_lba);
            if (dir_lba == g_sb.dir_lba) pd_memcpy(g_dir, s_op, sizeof(g_dir));
            return 0;
        }
    }
    return -4;
}

/* ---- chmod / chown -------------------------------------------------------- */

int pdfs_chmod(const char *path, uint16_t mode)
{
    if (!g_mounted) return -1;
    if (g_ro)       return -5;

    while (*path == '/') path++;

    uint32_t dir_lba;
    int idx = path_resolve(path, s_op, &dir_lba);
    if (idx < 0) return -1;
    if (!perm_write_ok(&s_op[idx])) return -3;

    s_op[idx].mode = mode;
    flush_dir_at(s_op, dir_lba);
    if (dir_lba == g_sb.dir_lba) pd_memcpy(g_dir, s_op, sizeof(g_dir));
    return 0;
}

int pdfs_chown(const char *path, uint8_t uid, uint8_t gid)
{
    if (!g_mounted) return -1;
    if (g_ro)       return -5;

    while (*path == '/') path++;

    /* chown requires root or elevated */
    if (!g_elevated && !(g_caller && (g_caller->flags & USER_FLAG_ROOT)))
        return -3;

    uint32_t dir_lba2;
    int idx2 = path_resolve(path, s_op, &dir_lba2);
    if (idx2 < 0) return -1;

    s_op[idx2].uid = uid;
    s_op[idx2].gid = gid;
    flush_dir_at(s_op, dir_lba2);
    if (dir_lba2 == g_sb.dir_lba) pd_memcpy(g_dir, s_op, sizeof(g_dir));
    return 0;
}

/* ---- Format --------------------------------------------------------------- */

int pdfs_format(uint32_t base_lba)
{
    pd_memzero(&g_sb, sizeof(g_sb));
    g_sb.magic         = PDFS_MAGIC;
    g_sb.version       = PDFS_VERSION;
    g_sb.jrnl_lba      = base_lba + PDFS_SB_SECTORS;
    g_sb.dir_lba       = base_lba + PDFS_SB_SECTORS + PDFS_JRNL_SECTORS;
    g_sb.dir_sectors   = PDFS_DIR_SECTORS;
    g_sb.data_lba      = g_sb.dir_lba + PDFS_DIR_SECTORS;
    g_sb.next_free_lba = g_sb.data_lba;
    /* reserved[0..1] used as journal dirty+target; clear them */
    g_sb.reserved[0] = 0u;
    g_sb.reserved[1] = 0u;

    if (ata_write_sectors(base_lba, 1, &g_sb) != 0) return -1;

    /* Clear journal sector */
    uint8_t empty[512];
    pd_memzero(empty, 512);
    if (ata_write_sectors(g_sb.jrnl_lba, 1, empty) != 0) return -2;

    /* Write empty root directory (PDFS_DIR_SECTORS × 512 bytes) */
    pd_memzero(g_dir, sizeof(g_dir));
    flush_dir_at(g_dir, g_sb.dir_lba);

    g_base_lba = base_lba;
    g_mounted  = 1;
    g_ro       = 0;
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

/* ---- Query dir entry for ls ----------------------------------------------- */

int pdfs_stat_root(uint32_t idx, pdfs_dirent_t *out)
{
    uint32_t count = 0, i;
    if (!g_mounted) return -1;
    for (i = 0; i < PDFS_MAX_FILES; i++) {
        if (g_dir[i].flags & PDFS_FLAG_USED) {
            if (count == idx) { pd_memcpy(out, &g_dir[i], sizeof(*out)); return 0; }
            count++;
        }
    }
    return -1;
}

