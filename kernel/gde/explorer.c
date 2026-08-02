/* ============================================================================
 * PD-OS GDE  —  PD-Explorer (Windows Explorer-style file browser)
 *
 * Layout (window content area, 820×580 default):
 *   ┌────────────────────────────────────────────────────────┐
 *   │  Address bar:  /current/path                          │ EXP_ADDR_H
 *   ├──────────────────────┬─────────────────────────────────┤
 *   │  Sidebar             │  File pane (Detail view)        │
 *   │  [+]/ (root)         │  [icon] Name   Size    Type     │
 *   │    [-] home          │  ...                            │
 *   │       [+] pd         │                                 │
 *   │    [ ] etc           │                                 │
 *   ├──────────────────────┴─────────────────────────────────┤
 *   │  N items                                               │ EXP_STAT_H
 *   └────────────────────────────────────────────────────────┘
 *
 * Sidebar  : single-click to navigate; [+]/[-] arrows expand/collapse
 * File pane: single-click to select, double-click to enter dir / open file
 * .txt files: opens in PD-Text editor window
 * ============================================================================ */

#include "gde.h"
#include "gfx.h"
#include "pdfs.h"
#include "users.h"
#include "kheap.h"
#include "pit.h"

/* ---- Layout -------------------------------------------------------------- */

#define EXP_SIDE_W      190     /* sidebar pixel width                       */
#define EXP_DIV_W         2     /* divider between sidebar and pane          */
#define EXP_ADDR_H       24     /* address bar height                        */
#define EXP_STAT_H       18     /* status bar height                         */
#define EXP_ROW_H        20     /* file-pane row height                      */
#define EXP_TREE_H       18     /* sidebar row height                        */
#define EXP_ICON_W       14     /* icon draw width (pixels)                  */
#define EXP_ICON_H       12     /* icon draw height (pixels)                 */
#define EXP_INDENT       10     /* sidebar indent per depth level            */
#define EXP_ARROW_W      10     /* expand/collapse arrow column width        */
#define DBLCLICK_TICKS   40     /* 400 ms at 100 Hz PIT                      */

/* ---- Data limits --------------------------------------------------------- */

#define TREE_PATH_MAX   128
#define TREE_MAX         48     /* max sidebar tree nodes                    */
#define PANE_MAX        128     /* max file-pane entries                     */

/* ---- Colours ------------------------------------------------------------- */

#define COL_EXP_BG         GFX_RGB( 18,  18,  23)
#define COL_EXP_SIDE_BG    GFX_RGB( 22,  22,  29)
#define COL_EXP_ADDR_BG    GFX_RGB( 28,  28,  38)
#define COL_EXP_ADDR_BD    GFX_RGB( 60,  80, 120)
#define COL_EXP_DIV        GFX_RGB( 50,  50,  62)
#define COL_EXP_SEL        GFX_RGB( 40,  80, 160)
#define COL_EXP_SIDE_SEL   GFX_RGB( 30,  58, 118)
#define COL_EXP_COL_HDR    GFX_RGB( 26,  26,  34)
#define COL_EXP_HDR_TX     GFX_RGB(120, 120, 130)
#define COL_FOLDER         GFX_RGB(220, 170,  50)
#define COL_FOLDER_BD      GFX_RGB(160, 115,  20)
#define COL_FOLDER_TAB     GFX_RGB(240, 195,  70)
#define COL_FILE_BG        GFX_RGB(205, 210, 225)
#define COL_FILE_BD        GFX_RGB(110, 115, 130)
#define COL_FILE_LINE      GFX_RGB(150, 155, 170)
#define COL_TXT_BG         GFX_RGB(160, 195, 240)
#define COL_TXT_BD         GFX_RGB( 80, 130, 200)
#define COL_EXP_NAME       GFX_RGB(210, 210, 215)
#define COL_EXP_NAME_DIR   GFX_RGB(100, 160, 255)
#define COL_EXP_DENY       GFX_RGB(140,  50,  50)
#define COL_EXP_DIM        GFX_RGB( 70,  70,  80)
#define COL_EXP_STAT_BG    GFX_RGB( 22,  22,  30)
#define COL_EXP_STAT_TX    GFX_RGB(140, 140, 150)
#define COL_EXP_TREE_TX    GFX_RGB(175, 175, 188)
#define COL_EXP_EXPAND_TX  GFX_RGB(100, 135, 185)

/* ---- Sidebar tree node --------------------------------------------------- */

typedef struct {
    char  path[TREE_PATH_MAX];
    char  name[PDFS_NAME_LEN];
    int   depth;
    int   expanded;      /* 1 = children currently shown in tree             */
    int   has_children;  /* -1=unknown, 0=no, 1=yes (dirs only)              */
    int   accessible;    /* 0=permission denied                               */
} tree_node_t;

/* ---- File-pane entry ----------------------------------------------------- */

typedef struct {
    char     name[PDFS_NAME_LEN];
    uint32_t size;
    uint8_t  is_dir;
    uint8_t  denied;
} pane_entry_t;

/* ---- Per-window state ---------------------------------------------------- */

typedef struct {
    char         cwd[TREE_PATH_MAX];   /* path currently shown in file pane  */

    /* Sidebar */
    tree_node_t  tree[TREE_MAX];
    int          ntree;
    int          tree_sel;             /* -1 = none                          */
    int          tree_scroll;
    int          tree_last_row;
    uint32_t     tree_last_tick;

    /* File pane */
    pane_entry_t pane[PANE_MAX];
    int          npane;
    int          pane_sel;             /* -1 = none                          */
    int          pane_scroll;
    int          pane_last_row;
    uint32_t     pane_last_tick;

    int          dir_denied;
} exp_state_t;

/* =========================================================================
 * Helpers
 * ========================================================================= */

static const user_t *exp_current_user(void)
{
    return users_get("root");
}

static int exp_strlen(const char *s)
{
    int n = 0; while (s[n]) n++; return n;
}

static void exp_strncpy(char *dst, const char *src, int n)
{
    int i;
    for (i = 0; i < n - 1 && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
}

static int exp_strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static void path_join(char *dst, int dsz, const char *base, const char *leaf)
{
    int di = 0, j;
    int bl = exp_strlen(base);
    int ll = exp_strlen(leaf);
    for (j = 0; j < bl && di < dsz - 1; j++) dst[di++] = base[j];
    if (di > 0 && dst[di-1] != '/' && di < dsz - 1) dst[di++] = '/';
    for (j = 0; j < ll && di < dsz - 1; j++) dst[di++] = leaf[j];
    dst[di] = '\0';
}

static void path_up(char *path)
{
    int len = exp_strlen(path);
    if (len <= 1) return;
    int i = len - 1;
    if (path[i] == '/') i--;
    while (i > 0 && path[i] != '/') i--;
    if (i == 0) path[1] = '\0';
    else        path[i] = '\0';
}

static int itoa_dec(char *buf, uint32_t n)
{
    if (n == 0) { buf[0] = '0'; return 1; }
    char tmp[12]; int len = 0, i;
    while (n) { tmp[len++] = '0' + (n % 10); n /= 10; }
    for (i = 0; i < len; i++) buf[i] = tmp[len - 1 - i];
    return len;
}

static int is_txt(const char *name)
{
    int n = exp_strlen(name);
    return n >= 4 && name[n-4] == '.' && name[n-3] == 't'
                  && name[n-2] == 'x' && name[n-1] == 't';
}

/* ---- Permission check ---------------------------------------------------- */

static int can_access(const pdfs_dirent_t *de, const user_t *u, int is_dir)
{
    if (!u) return 0;
    if (u->flags & USER_FLAG_ROOT) return 1;
    uint16_t owner_bit = is_dir ? 0x040u : 0x100u;
    uint16_t other_bit = is_dir ? 0x001u : 0x004u;
    if (de->uid == u->uid && (de->mode & owner_bit)) return 1;
    if (de->mode & other_bit) return 1;
    return 0;
}

/* =========================================================================
 * Small icon drawing  (pixel-art, using only fill_rect / hline)
 * ========================================================================= */

static void draw_folder_icon(int x, int y)
{
    gfx_fill_rect(x,   y,       6, 3,              COL_FOLDER_TAB);
    gfx_fill_rect(x,   y + 3,   EXP_ICON_W, EXP_ICON_H - 3, COL_FOLDER);
    gfx_draw_rect(x,   y + 3,   EXP_ICON_W, EXP_ICON_H - 3, COL_FOLDER_BD);
}

static void draw_file_icon(int x, int y, int txt)
{
    uint32_t body = txt ? COL_TXT_BG  : COL_FILE_BG;
    uint32_t bd   = txt ? COL_TXT_BD  : COL_FILE_BD;
    uint32_t line = txt ? COL_TXT_BD  : COL_FILE_LINE;
    gfx_fill_rect(x,     y,     12,  EXP_ICON_H + 2, body);
    gfx_fill_rect(x + 9, y,      3,  3,               COL_EXP_BG);
    gfx_draw_rect(x,     y,     12,  EXP_ICON_H + 2,  bd);
    gfx_hline(x + 2, y + 4,  8, line);
    gfx_hline(x + 2, y + 6,  8, line);
    gfx_hline(x + 2, y + 8,  6, line);
    gfx_hline(x + 2, y + 10, 7, line);
}

/* =========================================================================
 * File-pane management
 * ========================================================================= */

static void pane_load(exp_state_t *st)
{
    const user_t *u = exp_current_user();
    pdfs_set_context(u, 0);

    st->npane       = 0;
    st->pane_sel    = -1;
    st->pane_scroll = 0;
    st->dir_denied  = 0;

    if (!(st->cwd[0] == '/' && st->cwd[1] == '\0')) {
        pane_entry_t *up = &st->pane[st->npane++];
        exp_strncpy(up->name, "..", PDFS_NAME_LEN);
        up->size   = 0;
        up->is_dir = 1;
        up->denied = 0;
    }

    uint32_t idx = 0;
    while (st->npane < PANE_MAX) {
        pdfs_dirent_t de;
        int r = pdfs_stat_dir(st->cwd, idx, &de);
        if (r == -4) { st->dir_denied = 1; break; }
        if (r != 0)  break;
        idx++;
        pane_entry_t *e = &st->pane[st->npane++];
        exp_strncpy(e->name, de.name, PDFS_NAME_LEN);
        e->size   = de.size;
        e->is_dir = (de.flags & PDFS_FLAG_DIR) ? 1 : 0;
        e->denied = can_access(&de, u, e->is_dir) ? 0 : 1;
    }
}

static void pane_navigate(exp_state_t *st, const char *newpath)
{
    exp_strncpy(st->cwd, newpath, TREE_PATH_MAX);
    pane_load(st);
}

/* =========================================================================
 * Sidebar tree management
 * ========================================================================= */

static void tree_probe(exp_state_t *st, int ni)
{
    tree_node_t *n = &st->tree[ni];
    if (n->has_children >= 0) return;
    const user_t *u = exp_current_user();
    pdfs_set_context(u, 0);
    uint32_t i;
    for (i = 0; i < 128u; i++) {
        pdfs_dirent_t de;
        int r = pdfs_stat_dir(n->path, i, &de);
        if (r == -4) { n->accessible = 0; n->has_children = 0; return; }
        if (r != 0)  break;
        if (de.flags & PDFS_FLAG_DIR) { n->has_children = 1; return; }
    }
    n->has_children = 0;
}

static void tree_remove_children(exp_state_t *st, int ni)
{
    int depth = st->tree[ni].depth;
    int start = ni + 1;
    int end   = start;
    while (end < st->ntree && st->tree[end].depth > depth) end++;
    int removed = end - start;
    int i;
    for (i = start; i + removed < st->ntree; i++)
        st->tree[i] = st->tree[i + removed];
    st->ntree -= removed;
    st->tree[ni].expanded = 0;
    if (st->tree_sel >= start && st->tree_sel < end)
        st->tree_sel = ni;
    else if (st->tree_sel >= end)
        st->tree_sel -= removed;
}

static void tree_expand(exp_state_t *st, int ni)
{
    tree_node_t *n = &st->tree[ni];
    if (n->expanded) return;

    const user_t *u = exp_current_user();
    pdfs_set_context(u, 0);

    char   child_names[16][PDFS_NAME_LEN];
    int    child_acc[16];
    int    nchildren = 0;

    uint32_t idx;
    for (idx = 0; idx < 256u && nchildren < 16; idx++) {
        pdfs_dirent_t de;
        int r = pdfs_stat_dir(n->path, idx, &de);
        if (r == -4) break;
        if (r != 0)  break;
        if (!(de.flags & PDFS_FLAG_DIR)) continue;
        exp_strncpy(child_names[nchildren], de.name, PDFS_NAME_LEN);
        child_acc[nchildren] = can_access(&de, u, 1);
        nchildren++;
    }

    if (st->ntree + nchildren > TREE_MAX) {
        n->expanded = 1;
        return;
    }

    int insert_pos = ni + 1;
    int i;
    for (i = st->ntree - 1; i >= insert_pos; i--)
        st->tree[i + nchildren] = st->tree[i];
    st->ntree += nchildren;

    for (i = 0; i < nchildren; i++) {
        tree_node_t *c = &st->tree[insert_pos + i];
        path_join(c->path, TREE_PATH_MAX, n->path, child_names[i]);
        exp_strncpy(c->name, child_names[i], PDFS_NAME_LEN);
        c->depth        = n->depth + 1;
        c->expanded     = 0;
        c->has_children = -1;
        c->accessible   = child_acc[i];
    }

    n->expanded     = 1;
    n->has_children = (nchildren > 0) ? 1 : 0;

    for (i = 0; i < nchildren; i++)
        tree_probe(st, insert_pos + i);
}

static void tree_init(exp_state_t *st)
{
    st->ntree      = 0;
    st->tree_sel   = 0;
    st->tree_scroll = 0;

    tree_node_t *root = &st->tree[0];
    exp_strncpy(root->path, "/", TREE_PATH_MAX);
    exp_strncpy(root->name, "/  (root)", PDFS_NAME_LEN);
    root->depth        = 0;
    root->expanded     = 0;
    root->has_children = 1;
    root->accessible   = 1;
    st->ntree = 1;

    tree_expand(st, 0);
}

static int tree_find(exp_state_t *st, const char *path)
{
    int i;
    for (i = 0; i < st->ntree; i++)
        if (exp_strcmp(st->tree[i].path, path) == 0) return i;
    return -1;
}

/* =========================================================================
 * Drawing
 * ========================================================================= */

static void format_size(char *buf, uint32_t sz)
{
    if (sz == 0) { buf[0] = '-'; buf[1] = '\0'; return; }
    if (sz >= 1024u * 1024u) {
        int i = itoa_dec(buf, sz / (1024u * 1024u));
        buf[i++] = ' '; buf[i++] = 'M'; buf[i++] = 'B'; buf[i] = '\0';
    } else if (sz >= 1024u) {
        int i = itoa_dec(buf, sz / 1024u);
        buf[i++] = ' '; buf[i++] = 'K'; buf[i++] = 'B'; buf[i] = '\0';
    } else {
        int i = itoa_dec(buf, sz);
        buf[i++] = ' '; buf[i++] = 'B'; buf[i] = '\0';
    }
}

static void draw_sidebar(exp_state_t *st, int x, int y, int w, int h)
{
    gfx_fill_rect(x, y, w, h, COL_EXP_SIDE_BG);
    int visible = h / EXP_TREE_H;
    int i;
    for (i = 0; i < visible; i++) {
        int ni = st->tree_scroll + i;
        if (ni >= st->ntree) break;
        tree_node_t *n = &st->tree[ni];

        int ry    = y + i * EXP_TREE_H;
        int row_y = ry + (EXP_TREE_H - GFX_CHAR_H) / 2;
        uint32_t bg = (ni == st->tree_sel) ? COL_EXP_SIDE_SEL : COL_EXP_SIDE_BG;
        gfx_fill_rect(x, ry, w, EXP_TREE_H, bg);

        int rx = x + n->depth * EXP_INDENT;

        /* Expand/collapse arrow */
        if (n->has_children != 0) {
            char arrow = n->expanded ? '-' : '+';
            gfx_draw_char(rx, row_y, arrow, COL_EXP_EXPAND_TX, bg);
        }
        rx += EXP_ARROW_W;

        /* Folder icon */
        int icon_y = ry + (EXP_TREE_H - EXP_ICON_H) / 2;
        if (n->accessible) {
            draw_folder_icon(rx, icon_y);
        } else {
            gfx_fill_rect(rx, icon_y, EXP_ICON_W, EXP_ICON_H, COL_EXP_DENY);
        }
        rx += EXP_ICON_W + 4;

        /* Name */
        int name_chars = (x + w - rx - 2) / GFX_CHAR_W;
        if (name_chars < 1) name_chars = 1;
        uint32_t tc = n->accessible ? COL_EXP_TREE_TX : COL_EXP_DENY;
        gfx_draw_string_n(rx, row_y, n->name, name_chars, tc, bg, 0);
    }
    int used_y = y + (i < st->ntree ? i : st->ntree) * EXP_TREE_H;
    if (used_y < y + h)
        gfx_fill_rect(x, used_y, w, y + h - used_y, COL_EXP_SIDE_BG);
}

static void draw_file_pane(exp_state_t *st, int x, int y, int w, int h)
{
    gfx_fill_rect(x, y, w, h, COL_EXP_BG);

    /* Column header */
    int col_size = w - 130;
    int col_type = w - 56;
    gfx_fill_rect(x, y, w, EXP_ROW_H, COL_EXP_COL_HDR);
    gfx_hline(x, y + EXP_ROW_H - 1, w, COL_EXP_DIV);
    int hdr_y = y + (EXP_ROW_H - GFX_CHAR_H) / 2;
    gfx_draw_string(x + EXP_ICON_W + 6, hdr_y, "Name",
                    COL_EXP_HDR_TX, COL_EXP_COL_HDR, 0);
    gfx_draw_string(x + col_size,       hdr_y, "Size",
                    COL_EXP_HDR_TX, COL_EXP_COL_HDR, 0);
    gfx_draw_string(x + col_type,       hdr_y, "Type",
                    COL_EXP_HDR_TX, COL_EXP_COL_HDR, 0);

    if (st->dir_denied) {
        gfx_draw_string(x + 8, y + EXP_ROW_H + 8,
                        "Permission denied — cannot read this directory.",
                        COL_EXP_DENY, COL_EXP_BG, 0);
        return;
    }

    int list_y  = y + EXP_ROW_H;
    int list_h  = h - EXP_ROW_H;
    int visible = list_h / EXP_ROW_H;
    int i;
    for (i = 0; i < visible; i++) {
        int ei = st->pane_scroll + i;
        if (ei >= st->npane) break;
        pane_entry_t *e = &st->pane[ei];

        int ry = list_y + i * EXP_ROW_H;
        int ty = ry + (EXP_ROW_H - GFX_CHAR_H) / 2;
        uint32_t bg = (ei == st->pane_sel) ? COL_EXP_SEL : COL_EXP_BG;
        gfx_fill_rect(x, ry, w, EXP_ROW_H, bg);
        gfx_hline(x, ry + EXP_ROW_H - 1, w, COL_EXP_DIV);

        /* Icon */
        int icon_y = ry + (EXP_ROW_H - EXP_ICON_H) / 2;
        if (e->denied) {
            gfx_fill_rect(x + 2, icon_y, EXP_ICON_W, EXP_ICON_H, COL_EXP_DENY);
        } else if (e->is_dir) {
            if (!(e->name[0] == '.' && e->name[1] == '.' && e->name[2] == '\0'))
                draw_folder_icon(x + 2, icon_y);
            else
                gfx_draw_string(x + 2, ty, "..", COL_EXP_DIM, bg, 0);
        } else {
            draw_file_icon(x + 2, icon_y, is_txt(e->name));
        }

        /* Name */
        int name_chars = (col_size - (EXP_ICON_W + 6) - 4) / GFX_CHAR_W;
        if (name_chars < 1) name_chars = 1;
        uint32_t tc;
        if      (e->denied)        tc = COL_EXP_DENY;
        else if (e->is_dir)        tc = COL_EXP_NAME_DIR;
        else if (is_txt(e->name))  tc = COL_TXT_BG;
        else                       tc = COL_EXP_NAME;
        gfx_draw_string_n(x + EXP_ICON_W + 6, ty, e->name, name_chars, tc, bg, 0);

        if (!e->denied) {
            if (!e->is_dir) {
                char sz[16]; format_size(sz, e->size);
                gfx_draw_string(x + col_size, ty, sz, COL_EXP_STAT_TX, bg, 0);
                gfx_draw_string(x + col_type, ty,
                                is_txt(e->name) ? "Text" : "File",
                                COL_EXP_STAT_TX, bg, 0);
            } else if (!(e->name[0] == '.' && e->name[1] == '.')) {
                gfx_draw_string(x + col_type, ty, "Folder",
                                COL_EXP_STAT_TX, bg, 0);
            }
        }
    }

    int rem_y = list_y + (i < st->npane ? i : st->npane) * EXP_ROW_H;
    if (rem_y < y + h)
        gfx_fill_rect(x, rem_y, w, y + h - rem_y, COL_EXP_BG);
}

/* ---- Main draw callback -------------------------------------------------- */

static void exp_draw(gde_window_t *win)
{
    exp_state_t *st = (exp_state_t *)win->priv;
    if (!st) return;

    int cx = WIN_CX(win);
    int cy = WIN_CY(win);
    int cw = WIN_CW(win);
    int ch = WIN_CH(win);

    /* Address bar */
    gfx_fill_rect(cx, cy, cw, EXP_ADDR_H, COL_EXP_ADDR_BG);
    gfx_hline(cx, cy + EXP_ADDR_H - 1, cw, COL_EXP_ADDR_BD);
    gfx_draw_string(cx + 6, cy + (EXP_ADDR_H - GFX_CHAR_H) / 2,
                    st->cwd, GFX_WHITE, COL_EXP_ADDR_BG, 0);

    int content_y = cy + EXP_ADDR_H;
    int content_h = ch - EXP_ADDR_H - EXP_STAT_H;

    draw_sidebar(st, cx, content_y, EXP_SIDE_W, content_h);
    gfx_fill_rect(cx + EXP_SIDE_W, content_y, EXP_DIV_W, content_h, COL_EXP_DIV);

    int px = cx + EXP_SIDE_W + EXP_DIV_W;
    int pw = cw - EXP_SIDE_W - EXP_DIV_W;
    draw_file_pane(st, px, content_y, pw, content_h);

    /* Status bar */
    int sy = cy + ch - EXP_STAT_H;
    gfx_fill_rect(cx, sy, cw, EXP_STAT_H, COL_EXP_STAT_BG);
    gfx_hline(cx, sy, cw, COL_EXP_ADDR_BD);

    char stat[80]; int si = 0;
    si += itoa_dec(&stat[si], (uint32_t)st->npane);
    const char *s1 = " items"; int j;
    for (j = 0; s1[j] && si < 74; j++) stat[si++] = s1[j];
    if (st->dir_denied) {
        const char *s2 = "  — permission denied";
        for (j = 0; s2[j] && si < 74; j++) stat[si++] = s2[j];
    }
    stat[si] = '\0';
    gfx_draw_string(cx + 8, sy + (EXP_STAT_H - GFX_CHAR_H) / 2,
                    stat, COL_EXP_STAT_TX, COL_EXP_STAT_BG, 0);
}

/* =========================================================================
 * Keyboard handler
 * ========================================================================= */

static void exp_key(gde_window_t *win, char k)
{
    exp_state_t *st = (exp_state_t *)win->priv;
    if (!st) return;

    int content_h = WIN_CH(win) - EXP_ADDR_H - EXP_STAT_H;
    int visible   = (content_h - EXP_ROW_H) / EXP_ROW_H;

    if (k == 0x26) {   /* Up arrow */
        if (st->pane_sel > 0) {
            st->pane_sel--;
            if (st->pane_sel < st->pane_scroll)
                st->pane_scroll = st->pane_sel;
        }
    } else if (k == 0x28) {   /* Down arrow */
        if (st->pane_sel < st->npane - 1) {
            st->pane_sel++;
            if (st->pane_sel >= st->pane_scroll + visible)
                st->pane_scroll = st->pane_sel - visible + 1;
        }
    } else if (k == '\r' || k == '\n') {
        if (st->pane_sel >= 0 && st->pane_sel < st->npane) {
            pane_entry_t *e = &st->pane[st->pane_sel];
            if (e->denied) return;
            if (e->is_dir) {
                char np[TREE_PATH_MAX];
                if (e->name[0] == '.' && e->name[1] == '.' && e->name[2] == '\0') {
                    exp_strncpy(np, st->cwd, TREE_PATH_MAX);
                    path_up(np);
                } else {
                    path_join(np, TREE_PATH_MAX, st->cwd, e->name);
                }
                pane_navigate(st, np);
                int ni = tree_find(st, np);
                if (ni >= 0) st->tree_sel = ni;
            } else if (is_txt(e->name)) {
                char fp[TREE_PATH_MAX];
                path_join(fp, TREE_PATH_MAX, st->cwd, e->name);
                text_editor_open(fp);
            }
        }
    } else if (k == 8) {   /* Backspace — up */
        char np[TREE_PATH_MAX];
        exp_strncpy(np, st->cwd, TREE_PATH_MAX);
        path_up(np);
        pane_navigate(st, np);
        int ni = tree_find(st, np);
        if (ni >= 0) st->tree_sel = ni;
    }
}

/* =========================================================================
 * Mouse handler
 * ========================================================================= */

static void exp_mousedown(gde_window_t *win, int mx, int my)
{
    exp_state_t *st = (exp_state_t *)win->priv;
    if (!st) return;

    int cx = WIN_CX(win);
    int cy = WIN_CY(win);
    int ch = WIN_CH(win);

    int content_y = cy + EXP_ADDR_H;
    int content_h = ch - EXP_ADDR_H - EXP_STAT_H;

    if (my < content_y || my >= content_y + content_h) return;

    int rel_x = mx - cx;
    int rel_y = my - content_y;

    /* ---- Sidebar ---- */
    if (rel_x >= 0 && rel_x < EXP_SIDE_W) {
        int row = rel_y / EXP_TREE_H;
        int ni  = st->tree_scroll + row;
        if (ni < 0 || ni >= st->ntree) return;

        tree_node_t *n = &st->tree[ni];
        int arrow_lx = cx + n->depth * EXP_INDENT;
        int arrow_rx = arrow_lx + EXP_ARROW_W + 2;

        if (mx >= arrow_lx && mx < arrow_rx) {
            /* Toggle expand/collapse */
            if (n->expanded)
                tree_remove_children(st, ni);
            else if (n->has_children != 0)
                tree_expand(st, ni);
        } else {
            /* Navigate */
            st->tree_sel = ni;
            if (n->accessible)
                pane_navigate(st, n->path);
        }
        return;
    }

    /* ---- File pane ---- */
    if (rel_x >= EXP_SIDE_W + EXP_DIV_W) {
        if (rel_y < EXP_ROW_H) return;   /* column header — ignore */
        int row = (rel_y - EXP_ROW_H) / EXP_ROW_H;
        int ei  = st->pane_scroll + row;
        if (ei < 0 || ei >= st->npane) return;

        uint32_t now = pit_get_ticks();

        if (st->pane_last_row == ei &&
            (now - st->pane_last_tick) <= DBLCLICK_TICKS) {
            /* Double-click — navigate or open */
            pane_entry_t *e = &st->pane[ei];
            if (e->denied) { st->pane_last_row = -1; return; }
            if (e->is_dir) {
                char np[TREE_PATH_MAX];
                if (e->name[0] == '.' && e->name[1] == '.' && e->name[2] == '\0') {
                    exp_strncpy(np, st->cwd, TREE_PATH_MAX);
                    path_up(np);
                } else {
                    path_join(np, TREE_PATH_MAX, st->cwd, e->name);
                }
                pane_navigate(st, np);
                int ni = tree_find(st, np);
                if (ni >= 0) st->tree_sel = ni;
            } else if (is_txt(e->name)) {
                char fp[TREE_PATH_MAX];
                path_join(fp, TREE_PATH_MAX, st->cwd, e->name);
                text_editor_open(fp);
            }
            st->pane_last_row = -1;
        } else {
            /* Single click — select */
            st->pane_sel      = ei;
            st->pane_last_row  = ei;
            st->pane_last_tick = now;
        }
    }
}

/* =========================================================================
 * Public entry point
 * ========================================================================= */

void explorer_open(void)
{
    exp_state_t *st = (exp_state_t *)kmalloc(sizeof(exp_state_t));
    if (!st) return;

    exp_strncpy(st->cwd, "/", TREE_PATH_MAX);
    st->npane         = 0;
    st->pane_sel      = -1;
    st->pane_scroll   = 0;
    st->pane_last_row  = -1;
    st->pane_last_tick = 0;
    st->dir_denied    = 0;
    st->tree_sel      = 0;
    st->tree_scroll   = 0;
    st->tree_last_row  = -1;
    st->tree_last_tick = 0;

    tree_init(st);
    pane_load(st);

    gde_window_t *w = wm_create("PD-Explorer", 100, 50, 820, 580,
                                 exp_draw, exp_key);
    if (!w) { kfree(st); return; }

    w->priv         = st;
    w->on_mousedown = exp_mousedown;
}
