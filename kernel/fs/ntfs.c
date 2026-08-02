/* ============================================================================
 * PD-Kernel  —  NTFS Read-Only Driver  (Phase 8e)
 *
 * Implements the vfs_driver_t interface for NTFS (read-only).
 *
 * Supported operations:
 *   mount   — reads VBR, validates "NTFS    " OEM ID, derives geometry,
 *             locates $MFT by LCN * sectors-per-cluster
 *   open    — scans root dir MFT record index, matches $FILE_NAME
 *   read    — resident $DATA: copy from MFT record buffer
 *             non-resident $DATA: follow data runs (up to 2 extents)
 *   readdir — iterates $INDEX_ROOT entries in root dir MFT record
 *
 * Write operations (write/create/unlink) return -1 — read-only.
 *
 * Limitations (v1):
 *   - Root directory only (no subdirectory traversal — inode stored in node)
 *   - Resident $DATA only for small files; non-resident limited to 2 runs
 *   - No USN fixup correction
 *   - Filenames: Unicode UTF-16LE stripped to ASCII (high byte mapped to '?')
 *   - No $ATTRIBUTE_LIST (all attributes must fit in one 1024-byte MFT record)
 * ============================================================================ */

#include "ntfs.h"
#include "ata.h"
#include "kheap.h"

/* ---- NTFS constants ------------------------------------------------------- */
#define NTFS_MFT_RECORD_SIZE    1024u
#define NTFS_ATTR_END           0xFFFFFFFFu

/* Well-known MFT record numbers */
#define NTFS_MFT_MFT            0u    /* $MFT itself                          */
#define NTFS_MFT_MFTMIRR        1u    /* $MFTMirr                             */
#define NTFS_MFT_LOGFILE        2u    /* $LogFile                             */
#define NTFS_MFT_VOLUME         3u    /* $Volume                              */
#define NTFS_MFT_ATTRDEF        4u    /* $AttrDef                             */
#define NTFS_MFT_ROOT           5u    /* . (root directory)                   */

/* Attribute type codes */
#define ATTR_STANDARD_INFO      0x10u
#define ATTR_FILE_NAME          0x30u
#define ATTR_DATA               0x80u
#define ATTR_INDEX_ROOT         0x90u
#define ATTR_INDEX_ALLOC        0xA0u

/* File flags */
#define MFT_RECORD_IN_USE       0x0001u
#define MFT_RECORD_IS_DIR       0x0002u

/* ---- On-disk structures (little-endian, packed) -------------------------- */

/* VBR (NTFS Boot Sector) — key fields only */
typedef struct __attribute__((packed)) {
    uint8_t  jmp[3];
    char     oem_id[8];           /* "NTFS    "                              */
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint8_t  pad0[26];            /* skip reserved + other BPB fields        */
    uint64_t total_sectors;
    uint64_t mft_lcn;             /* Logical Cluster Number of $MFT          */
    uint64_t mftmirr_lcn;
    int8_t   clusters_per_mft_record; /* if negative: 2^(-n) bytes           */
    uint8_t  pad1[3];
    int8_t   clusters_per_idx_buf;
    uint8_t  pad2[3];
    uint64_t volume_serial;
} ntfs_vbr_t;

/* MFT record header */
typedef struct __attribute__((packed)) {
    uint8_t  magic[4];            /* "FILE"                                  */
    uint16_t update_seq_offset;
    uint16_t update_seq_count;
    uint64_t lsn;
    uint16_t sequence_number;
    uint16_t link_count;
    uint16_t attr_offset;         /* byte offset to first attribute          */
    uint16_t flags;               /* MFT_RECORD_IN_USE | MFT_RECORD_IS_DIR   */
    uint32_t used_size;
    uint32_t alloc_size;
    uint64_t base_record_ref;
    uint16_t next_attr_id;
} ntfs_mft_record_t;

/* Attribute record header (common part) */
typedef struct __attribute__((packed)) {
    uint32_t type;
    uint32_t length;              /* total attribute record length           */
    uint8_t  non_resident;        /* 0 = resident, 1 = non-resident          */
    uint8_t  name_length;
    uint16_t name_offset;
    uint16_t flags;
    uint16_t attr_id;
} ntfs_attr_hdr_t;

/* Resident attribute — value immediately follows this struct */
typedef struct __attribute__((packed)) {
    ntfs_attr_hdr_t hdr;
    uint32_t value_length;
    uint16_t value_offset;        /* relative to start of attribute record   */
    uint16_t resident_flags;
} ntfs_attr_resident_t;

/* Non-resident attribute header */
typedef struct __attribute__((packed)) {
    ntfs_attr_hdr_t hdr;
    uint64_t lowest_vcn;
    uint64_t highest_vcn;
    uint16_t mapping_pairs_offset;/* relative to start of attribute record   */
    uint8_t  compression_unit;
    uint8_t  pad[5];
    uint64_t alloc_size;
    uint64_t data_size;
    uint64_t init_size;
} ntfs_attr_nonresident_t;

/* $FILE_NAME attribute value */
typedef struct __attribute__((packed)) {
    uint64_t parent_ref;          /* MFT reference of parent directory       */
    uint64_t ctime;
    uint64_t atime;
    uint64_t mtime;
    uint64_t rtime;
    uint64_t alloc_size;
    uint64_t data_size;
    uint32_t file_flags;
    uint32_t reparse_tag;
    uint8_t  name_len;            /* length in UTF-16 chars                  */
    uint8_t  namespace;           /* 0=POSIX, 1=Win32, 2=DOS, 3=Win32&DOS   */
    uint16_t name[255];           /* UTF-16LE filename                       */
} ntfs_file_name_t;

/* $INDEX_ROOT value header */
typedef struct __attribute__((packed)) {
    uint32_t indexed_attr_type;   /* 0x30 for $FILE_NAME index               */
    uint32_t collation_rule;
    uint32_t index_alloc_size;
    uint8_t  clusters_per_block;
    uint8_t  pad[3];
    /* Followed by INDEX_BLOCK_HEADER then index entries */
} ntfs_index_root_t;

/* Index block header (starts the node, either in $INDEX_ROOT or $INDEX_ALLOC) */
typedef struct __attribute__((packed)) {
    uint32_t entries_offset;      /* offset to first NTFS_INDEX_ENTRY, from start of this struct */
    uint32_t index_length;        /* total length of used part               */
    uint32_t alloc_length;        /* allocated length                        */
    uint8_t  flags;               /* 1 = has sub-nodes                       */
    uint8_t  pad[3];
} ntfs_index_block_hdr_t;

/* Index entry for $FILE_NAME index */
typedef struct __attribute__((packed)) {
    uint64_t mft_ref;             /* low 48 bits = MFT record number         */
    uint16_t entry_length;        /* total length of this index entry        */
    uint16_t key_length;          /* length of $FILE_NAME key                */
    uint32_t flags;               /* 1 = has sub-node, 2 = last entry        */
} ntfs_index_entry_t;

#define NTFS_IDX_ENTRY_END  0x02u /* flags bit: last entry marker            */

/* ---- Module state -------------------------------------------------------- */
static uint32_t g_base_lba      = 0;
static uint32_t g_secs_per_clus = 0;
static uint32_t g_mft_lba       = 0;   /* absolute LBA of $MFT start          */
static uint32_t g_mft_rec_size  = 0;   /* bytes per MFT record (usually 1024) */
static uint32_t g_mft_secs      = 0;   /* sectors per MFT record              */
static int      g_mounted       = 0;

/* ---- Helpers ------------------------------------------------------------- */

static int nt_strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

static void nt_memcpy(void *dst, const void *src, uint32_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
}

static void nt_memset(void *dst, uint8_t v, uint32_t n)
{
    uint8_t *d = (uint8_t *)dst;
    while (n--) *d++ = v;
}

/* Convert UTF-16LE name of len chars to NUL-terminated ASCII in out[].
 * Non-ASCII chars (>127) become '?'. out must hold at least len+1 bytes. */
static void utf16_to_ascii(const uint16_t *src, uint8_t len, char *out)
{
    uint8_t i;
    for (i = 0; i < len && i < VFS_NAME_MAX - 1; i++) {
        uint16_t c = src[i];
        out[i] = (c < 128) ? (char)c : '?';
    }
    out[i] = '\0';
}

/* ---- MFT record I/O ------------------------------------------------------ */

/* Read MFT record `rec_no` into buf (must be g_mft_rec_size bytes). */
static int read_mft_record(uint32_t rec_no, void *buf)
{
    uint32_t lba = g_mft_lba + rec_no * g_mft_secs;
    return ata_read_sectors(lba, (uint8_t)g_mft_secs, buf);
}

/* ---- Attribute iteration helpers ----------------------------------------- */

/* Return pointer to first attribute in an MFT record buffer, or NULL.
 * rec must point to the base of the 1024-byte record. */
static const ntfs_attr_hdr_t *first_attr(const uint8_t *rec)
{
    const ntfs_mft_record_t *hdr = (const ntfs_mft_record_t *)rec;
    if (hdr->attr_offset < sizeof(ntfs_mft_record_t)) return NULL;
    if (hdr->attr_offset >= g_mft_rec_size)            return NULL;
    return (const ntfs_attr_hdr_t *)(rec + hdr->attr_offset);
}

/* Advance to the next attribute. Returns NULL at end or on error. */
static const ntfs_attr_hdr_t *next_attr(const uint8_t *rec,
                                        const ntfs_attr_hdr_t *cur)
{
    uint32_t base   = (uint32_t)((const uint8_t *)cur - rec);
    uint32_t stride = cur->length;
    /* Sanity: stride must be dword-aligned and non-zero */
    if (stride == 0 || (stride & 3u) || base + stride + sizeof(ntfs_attr_hdr_t) > g_mft_rec_size)
        return NULL;
    const ntfs_attr_hdr_t *next =
        (const ntfs_attr_hdr_t *)((const uint8_t *)cur + stride);
    if (next->type == NTFS_ATTR_END) return NULL;
    return next;
}

/* Find first attribute of given type in rec; returns NULL if not found. */
static const ntfs_attr_hdr_t *find_attr(const uint8_t *rec, uint32_t type)
{
    const ntfs_attr_hdr_t *a = first_attr(rec);
    while (a) {
        if (a->type == type) return a;
        if (a->type  > type) return NULL;   /* attrs are sorted by type       */
        a = next_attr(rec, a);
    }
    return NULL;
}

/* Get pointer to resident attribute value. Returns NULL if non-resident. */
static const void *resident_value(const ntfs_attr_hdr_t *a, uint32_t *len_out)
{
    if (a->non_resident) return NULL;
    const ntfs_attr_resident_t *r = (const ntfs_attr_resident_t *)a;
    if (len_out) *len_out = r->value_length;
    return (const uint8_t *)a + r->value_offset;
}

/* ---- Data-run decoder (non-resident $DATA) -------------------------------- */
/*
 * NTFS data runs are a compact byte stream encoding (LCN, length) pairs.
 * Each run byte: low nibble = byte count of length field,
 *                high nibble = byte count of LCN delta field.
 * Reads up to max_runs extents into arrays run_lcn[] and run_len[].
 * Returns number of runs decoded.
 */
static int decode_runs(const uint8_t *pairs, uint32_t pairs_len,
                       uint32_t *run_lcn, uint32_t *run_len, int max_runs)
{
    int     count   = 0;
    int64_t cur_lcn = 0;
    uint32_t pos    = 0;

    while (pos < pairs_len && count < max_runs) {
        uint8_t hdr = pairs[pos++];
        if (hdr == 0) break;

        uint8_t len_bytes = hdr & 0x0Fu;
        uint8_t lcn_bytes = (hdr >> 4) & 0x0Fu;

        if (pos + len_bytes + lcn_bytes > pairs_len) break;

        /* Read length field (unsigned) */
        uint64_t run_length = 0;
        uint8_t i;
        for (i = 0; i < len_bytes; i++)
            run_length |= (uint64_t)pairs[pos++] << (i * 8);

        /* Read LCN delta (signed) */
        int64_t lcn_delta = 0;
        for (i = 0; i < lcn_bytes; i++)
            lcn_delta |= (int64_t)pairs[pos++] << (i * 8);
        /* Sign-extend if high bit of last byte is set */
        if (lcn_bytes > 0) {
            int shift = (int)(lcn_bytes * 8u) - 1;
            if ((lcn_delta >> shift) & 1) {
                /* Fill upper bits with 1s without shifting a negative value */
                uint64_t mask = ~((uint64_t)0);
                mask = mask << (lcn_bytes * 8u);
                lcn_delta |= (int64_t)mask;
            }
        }

        cur_lcn += lcn_delta;
        run_lcn[count] = (uint32_t)cur_lcn;
        run_len[count] = (uint32_t)run_length;
        count++;
    }
    return count;
}

/* ---- Root directory index iteration -------------------------------------- */
/*
 * Calls cb(mft_ref_low32, file_name, cb_data) for each non-special entry in
 * the root directory's $INDEX_ROOT.  Stops at the end marker.
 * Returns number of entries visited, or -1 on error.
 */
typedef int (*idx_cb_t)(uint32_t mft_ref, const char *name, void *data);

static int iter_root_index(const uint8_t *root_rec, idx_cb_t cb, void *cb_data)
{
    const ntfs_attr_hdr_t *a = find_attr(root_rec, ATTR_INDEX_ROOT);
    if (!a) return -1;

    uint32_t vlen = 0;
    const uint8_t *val = (const uint8_t *)resident_value(a, &vlen);
    if (!val || vlen < sizeof(ntfs_index_root_t) + sizeof(ntfs_index_block_hdr_t))
        return -1;

    const ntfs_index_root_t    *irt = (const ntfs_index_root_t *)val;
    const ntfs_index_block_hdr_t *ibh =
        (const ntfs_index_block_hdr_t *)(val + sizeof(ntfs_index_root_t));

    uint32_t off    = ibh->entries_offset;   /* relative to ibh */
    uint32_t limit  = ibh->index_length;
    int      count  = 0;

    while (off + sizeof(ntfs_index_entry_t) <= limit) {
        const ntfs_index_entry_t *ie =
            (const ntfs_index_entry_t *)((const uint8_t *)ibh + off);

        if (ie->entry_length < sizeof(ntfs_index_entry_t)) break;

        if (ie->flags & NTFS_IDX_ENTRY_END) break;

        /* Only process entries that have a key ($FILE_NAME) */
        if (ie->key_length >= 67u) {   /* min $FILE_NAME key: fixed 66 bytes + 1 char name */
            /* Copy the key bytes to a plain byte buffer to avoid packed-pointer warnings */
            uint8_t fn_buf[sizeof(ntfs_file_name_t)];
            uint32_t key_copy_len = ie->key_length;
            if (key_copy_len > sizeof(fn_buf)) key_copy_len = sizeof(fn_buf);
            nt_memset(fn_buf, 0, sizeof(fn_buf));
            nt_memcpy(fn_buf, (const uint8_t *)ie + sizeof(ntfs_index_entry_t), key_copy_len);
            /* name_len is at offset 64, namespace at 65, name UTF-16 at offset 66 */
            uint8_t  fn_name_len = fn_buf[64];
            const uint16_t *fn_name_ptr = (const uint16_t *)(fn_buf + 66);
            char name[VFS_NAME_MAX];
            utf16_to_ascii(fn_name_ptr, fn_name_len, name);
            uint32_t ref = (uint32_t)(ie->mft_ref & 0x0000FFFFFFFFFFFFull);
            int r = cb(ref, name, cb_data);
            if (r != 0) return r;
            count++;
        }
        off += ie->entry_length;
    }
    (void)irt; /* may be used later for collation checks */
    return count;
}

/* ---- Mount --------------------------------------------------------------- */

static int ntfs_mount(uint32_t base_lba)
{
    uint8_t *buf = (uint8_t *)kmalloc(512);
    if (!buf) return -1;

    if (ata_read_sectors(base_lba, 1, buf) != 0) { kfree(buf); return -1; }

    /* Validate OEM ID */
    if (buf[3]  != 'N' || buf[4]  != 'T' || buf[5]  != 'F' || buf[6]  != 'S' ||
        buf[7]  != ' ' || buf[8]  != ' ' || buf[9]  != ' ' || buf[10] != ' ') {
        kfree(buf);
        return -1;
    }

    const ntfs_vbr_t *vbr = (const ntfs_vbr_t *)buf;

    uint16_t bps = vbr->bytes_per_sector;
    if (bps == 0) { kfree(buf); return -1; }

    uint8_t spc = vbr->sectors_per_cluster;
    if (spc == 0) spc = 1;

    /* MFT record size: if clusters_per_mft_record >= 0 → records per cluster;
     * if < 0 → 2^(-clusters_per_mft_record) bytes */
    uint32_t rec_bytes = NTFS_MFT_RECORD_SIZE;  /* default */
    if (vbr->clusters_per_mft_record > 0) {
        rec_bytes = (uint32_t)vbr->clusters_per_mft_record * spc * bps;
    } else if (vbr->clusters_per_mft_record < 0) {
        rec_bytes = 1u << (uint8_t)(-(vbr->clusters_per_mft_record));
    }
    if (rec_bytes < 512 || rec_bytes > 4096) rec_bytes = NTFS_MFT_RECORD_SIZE;

    uint32_t secs_per_rec = rec_bytes / 512u;
    if (secs_per_rec == 0) secs_per_rec = 1;

    /* $MFT absolute LBA */
    uint32_t mft_lba_abs = base_lba + (uint32_t)(vbr->mft_lcn) * spc;

    g_base_lba      = base_lba;
    g_secs_per_clus = spc;
    g_mft_lba       = mft_lba_abs;
    g_mft_rec_size  = rec_bytes;
    g_mft_secs      = secs_per_rec;
    g_mounted       = 1;

    kfree(buf);
    return 0;
}

/* ---- Lookup helpers ------------------------------------------------------- */

/* Callback data for name search in root index */
typedef struct {
    const char *target;
    uint32_t    found_ref;
} find_ctx_t;

static int find_cb(uint32_t mft_ref, const char *name, void *data)
{
    find_ctx_t *ctx = (find_ctx_t *)data;
    if (nt_strcmp(name, ctx->target) == 0) {
        ctx->found_ref = mft_ref;
        return 1;   /* stop iteration */
    }
    return 0;
}

/* ---- VFS callbacks -------------------------------------------------------- */

static int ntfs_open(const char *name, vfs_node_t *out)
{
    if (!g_mounted) return -1;

    /* Load root dir record (MFT#5) */
    uint8_t *root_rec = (uint8_t *)kmalloc(g_mft_rec_size);
    if (!root_rec) return -1;

    if (read_mft_record(NTFS_MFT_ROOT, root_rec) != 0) {
        kfree(root_rec); return -1;
    }

    find_ctx_t ctx;
    ctx.target    = name;
    ctx.found_ref = 0;

    int r = iter_root_index(root_rec, find_cb, &ctx);
    kfree(root_rec);

    if (r <= 0 && ctx.found_ref == 0) return -1;

    /* Load the found file's MFT record */
    uint8_t *file_rec = (uint8_t *)kmalloc(g_mft_rec_size);
    if (!file_rec) return -1;

    if (read_mft_record(ctx.found_ref, file_rec) != 0) {
        kfree(file_rec); return -1;
    }

    const ntfs_mft_record_t *mhdr = (const ntfs_mft_record_t *)file_rec;
    if (!(mhdr->flags & MFT_RECORD_IN_USE)) { kfree(file_rec); return -1; }

    /* Determine size from $DATA */
    uint32_t fsize = 0;
    const ntfs_attr_hdr_t *data_attr = find_attr(file_rec, ATTR_DATA);
    if (data_attr) {
        if (!data_attr->non_resident) {
            uint32_t vlen = 0;
            resident_value(data_attr, &vlen);
            fsize = vlen;
        } else {
            /* Copy non-resident header to avoid unaligned packed access */
            ntfs_attr_nonresident_t nr_copy;
            nt_memcpy(&nr_copy, data_attr, sizeof(nr_copy));
            fsize = (uint32_t)nr_copy.data_size;
        }
    }

    nt_memset(out->name, 0, VFS_NAME_MAX);
    uint8_t ni = 0;
    while (name[ni] && ni < VFS_NAME_MAX - 1) { out->name[ni] = name[ni]; ni++; }
    out->size    = fsize;
    out->inode   = ctx.found_ref;
    out->is_dir  = (mhdr->flags & MFT_RECORD_IS_DIR) ? 1u : 0u;

    kfree(file_rec);
    return 0;
}

static int ntfs_read(vfs_node_t *node, uint32_t offset, uint32_t len, void *buf)
{
    if (!g_mounted || !buf) return -1;

    uint8_t *file_rec = (uint8_t *)kmalloc(g_mft_rec_size);
    if (!file_rec) return -1;

    if (read_mft_record(node->inode, file_rec) != 0) {
        kfree(file_rec); return -1;
    }

    const ntfs_attr_hdr_t *data_attr = find_attr(file_rec, ATTR_DATA);
    if (!data_attr) { kfree(file_rec); return -1; }

    int bytes_read = -1;

    if (!data_attr->non_resident) {
        /* --- Resident $DATA --- */
        uint32_t vlen = 0;
        const uint8_t *val = (const uint8_t *)resident_value(data_attr, &vlen);
        if (val && offset < vlen) {
            uint32_t avail = vlen - offset;
            uint32_t copy  = (len < avail) ? len : avail;
            nt_memcpy(buf, val + offset, copy);
            bytes_read = (int)copy;
        } else {
            bytes_read = 0;
        }
    } else {
        /* --- Non-resident $DATA: decode runs --- */
        ntfs_attr_nonresident_t nr_copy;
        nt_memcpy(&nr_copy, data_attr, sizeof(nr_copy));
        uint32_t file_size = (uint32_t)nr_copy.data_size;
        if (offset >= file_size) { kfree(file_rec); return 0; }

        const uint8_t *pairs = (const uint8_t *)data_attr + nr_copy.mapping_pairs_offset;
        uint32_t pairs_len   = data_attr->length - nr_copy.mapping_pairs_offset;

        /* Decode up to 4 runs */
        uint32_t run_lcn[4], run_len_cls[4];
        int nruns = decode_runs(pairs, pairs_len, run_lcn, run_len_cls, 4);

        uint32_t clus_bytes    = g_secs_per_clus * 512u;
        uint32_t bytes_read_u  = 0;
        uint8_t *out_ptr       = (uint8_t *)buf;
        uint32_t remaining     = len;
        uint32_t vcn           = 0;   /* current virtual file offset in clusters */

        int ri;
        for (ri = 0; ri < nruns && remaining > 0; ri++) {
            uint32_t run_byte_start = vcn * clus_bytes;
            uint32_t run_byte_end   = run_byte_start + run_len_cls[ri] * clus_bytes;

            if (offset >= run_byte_end) {
                vcn += run_len_cls[ri];
                continue;
            }

            /* Byte offset within this run where we start reading */
            uint32_t in_run_off = (offset > run_byte_start) ? offset - run_byte_start : 0;
            uint32_t avail      = run_byte_end - run_byte_start - in_run_off;
            uint32_t copy       = (remaining < avail) ? remaining : avail;

            /* Trim to actual file size */
            uint32_t already_read = bytes_read_u;
            uint32_t file_rem     = file_size - (offset + already_read);
            if (copy > file_rem) copy = file_rem;
            if (copy == 0) break;

            /* Read sector by sector */
            uint32_t lba_start = g_base_lba + (uint32_t)run_lcn[ri] * g_secs_per_clus
                                + in_run_off / 512u;
            uint32_t sec_off   = in_run_off % 512u;
            uint32_t to_copy   = copy;

            uint8_t *sec_buf = (uint8_t *)kmalloc(512);
            if (!sec_buf) break;

            while (to_copy > 0) {
                if (ata_read_sectors(lba_start, 1, sec_buf) != 0) {
                    kfree(sec_buf); goto done_nr;
                }
                uint32_t chunk = 512u - sec_off;
                if (chunk > to_copy) chunk = to_copy;
                nt_memcpy(out_ptr, sec_buf + sec_off, chunk);
                out_ptr    += chunk;
                to_copy    -= chunk;
                bytes_read_u += chunk;
                remaining  -= chunk;
                lba_start++;
                sec_off = 0;
            }
            kfree(sec_buf);
            vcn += run_len_cls[ri];
        }
done_nr:
        bytes_read = (int)bytes_read_u;
    }

    kfree(file_rec);
    return bytes_read;
}

/* Read-only: write/create/unlink are not supported */
static int ntfs_write(vfs_node_t *node, uint32_t offset, uint32_t len, const void *buf)
{
    (void)node; (void)offset; (void)len; (void)buf;
    return -1;  /* read-only */
}

static int ntfs_create(const char *name)
{
    (void)name;
    return -1;  /* read-only */
}

static int ntfs_unlink(const char *name)
{
    (void)name;
    return -1;  /* read-only */
}

/* ---- readdir ------------------------------------------------------------- */

typedef struct {
    uint32_t  target_idx;
    uint32_t  cur_idx;
    vfs_node_t *out;
    int        found;
} readdir_ctx_t;

static int readdir_cb(uint32_t mft_ref, const char *name, void *data)
{
    readdir_ctx_t *ctx = (readdir_ctx_t *)data;

    if (ctx->cur_idx == ctx->target_idx) {
        /* Load this record to get size and is_dir */
        uint8_t *rec = (uint8_t *)kmalloc(NTFS_MFT_RECORD_SIZE);
        if (!rec) return -1;

        uint32_t fsize  = 0;
        uint8_t  is_dir = 0;

        if (read_mft_record(mft_ref, rec) == 0) {
            const ntfs_mft_record_t *mhdr = (const ntfs_mft_record_t *)rec;
            is_dir = (mhdr->flags & MFT_RECORD_IS_DIR) ? 1u : 0u;
            const ntfs_attr_hdr_t *da = find_attr(rec, ATTR_DATA);
            if (da && !da->non_resident) {
                uint32_t vlen2 = 0;
                resident_value(da, &vlen2);
                fsize = vlen2;
            } else if (da && da->non_resident) {
                ntfs_attr_nonresident_t nr2;
                nt_memcpy(&nr2, da, sizeof(nr2));
                fsize = (uint32_t)nr2.data_size;
            }
        }
        kfree(rec);

        nt_memset(ctx->out->name, 0, VFS_NAME_MAX);
        uint8_t ni = 0;
        while (name[ni] && ni < VFS_NAME_MAX - 1) { ctx->out->name[ni] = name[ni]; ni++; }
        ctx->out->size   = fsize;
        ctx->out->inode  = mft_ref;
        ctx->out->is_dir = is_dir;
        ctx->found       = 1;
        return 1;  /* stop */
    }
    ctx->cur_idx++;
    return 0;
}

static int ntfs_readdir(uint32_t idx, vfs_node_t *out)
{
    if (!g_mounted) return -1;

    uint8_t *root_rec = (uint8_t *)kmalloc(g_mft_rec_size);
    if (!root_rec) return -1;

    if (read_mft_record(NTFS_MFT_ROOT, root_rec) != 0) {
        kfree(root_rec); return -1;
    }

    readdir_ctx_t ctx;
    ctx.target_idx = idx;
    ctx.cur_idx    = 0;
    ctx.out        = out;
    ctx.found      = 0;

    iter_root_index(root_rec, readdir_cb, &ctx);
    kfree(root_rec);

    return ctx.found ? 0 : -1;
}

/* ---- Driver registration ------------------------------------------------- */

static vfs_driver_t ntfs_driver = {
    "ntfs",
    ntfs_mount,
    ntfs_open,
    ntfs_read,
    ntfs_write,
    ntfs_create,
    ntfs_unlink,
    ntfs_readdir
};

vfs_driver_t *ntfs_get_driver(void)
{
    return &ntfs_driver;
}
