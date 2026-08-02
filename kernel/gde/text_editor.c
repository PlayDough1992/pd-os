/* ============================================================================
 * PD-OS GDE  —  PD-Text  (simple text file viewer)
 *
 * Layout (window content area):
 *   ┌───────────────────────────────────────────┐
 *   │  [path bar]                               │ TE_HDR_H
 *   ├───────────────────────────────────────────┤
 *   │  [lnum] │  text content (monospace)       │
 *   ├───────────────────────────────────────────┤
 *   │  Ln N / N lines                           │ TE_STAT_H
 *   └───────────────────────────────────────────┘
 *
 * Keys: Up/Down arrow, PgUp/PgDn, Home/End
 * ============================================================================ */

#include "gde.h"
#include "gfx.h"
#include "vfs.h"
#include "kheap.h"

/* ---- Layout -------------------------------------------------------------- */

#define TE_HDR_H     20
#define TE_STAT_H    16
#define TE_PAD_X      8
#define TE_PAD_Y      4

/* ---- Limits -------------------------------------------------------------- */

#define TE_BUF_MAX   (32 * 1024)
#define TE_LINE_MAX  1024

/* ---- Colours ------------------------------------------------------------- */

#define COL_TE_BG        GFX_RGB( 20,  22,  28)
#define COL_TE_HDR       GFX_RGB( 26,  28,  36)
#define COL_TE_HDR_BD    GFX_RGB( 55,  75, 115)
#define COL_TE_STAT      GFX_RGB( 22,  24,  32)
#define COL_TE_STAT_TX   GFX_RGB(130, 130, 145)
#define COL_TE_FG        GFX_RGB(205, 210, 215)
#define COL_TE_LNUM      GFX_RGB( 70,  80, 100)
#define COL_TE_LNUM_BG   GFX_RGB( 23,  25,  33)
#define COL_TE_CUR_LINE  GFX_RGB( 25,  30,  44)
#define COL_TE_PATH      GFX_RGB(100, 155, 220)

/* ---- State --------------------------------------------------------------- */

typedef struct {
    char     path[128];
    char     buf[TE_BUF_MAX];
    uint32_t size;
    int      lstart[TE_LINE_MAX];
    int      nlines;
    int      scroll;
    int      cursor;     /* cursor line */
    int      cur_col;    /* cursor column within line */
    int      dirty;      /* 1 = modified since open/save */
    int      file_menu;  /* 1 = File dropdown visible */
} tedit_state_t;

/* ---- Helpers ------------------------------------------------------------- */

static int te_strlen(const char *s) { int n=0; while(s[n]) n++; return n; }
static void te_strncpy(char *d, const char *s, int n)
{
    int i; for (i=0; i<n-1 && s[i]; i++) d[i]=s[i]; d[i]='\0';
}

/* Write decimal of n (>0) into buf; return chars written. */
static int te_itoa(char *buf, int n)
{
    if (n <= 0) { buf[0]='0'; return 1; }
    char rev[12]; int ri=0;
    while (n) { rev[ri++]='0'+(n%10); n/=10; }
    int i;
    for (i=0; i<ri; i++) buf[i]=rev[ri-1-i];
    return ri;
}

static void te_index_lines(tedit_state_t *st)
{
    st->nlines = 0;
    if (st->size == 0) return;
    st->lstart[st->nlines++] = 0;
    uint32_t i;
    for (i=0; i<st->size && st->nlines<TE_LINE_MAX; i++)
        if (st->buf[i]=='\n' && i+1<st->size)
            st->lstart[st->nlines++]=(int)(i+1);
}

static const char *te_get_line(tedit_state_t *st, int ln, int *lenout)
{
    if (ln<0 || ln>=st->nlines) { *lenout=0; return ""; }
    int start = st->lstart[ln];
    int end   = (ln+1<st->nlines) ? st->lstart[ln+1] : (int)st->size;
    while (end>start && (st->buf[end-1]=='\n' || st->buf[end-1]=='\r')) end--;
    *lenout = end-start;
    return &st->buf[start];
}

/* ---- Editing helpers ----------------------------------------------------- */

static void te_mark_dirty(gde_window_t *win, tedit_state_t *st)
{
    if (st->dirty) return;
    st->dirty = 1;
    if (win->title[0] != '*') {
        int tl = te_strlen(win->title), i;
        for (i = tl; i >= 0; i--) win->title[i+1] = win->title[i];
        win->title[0] = '*';
    }
}

static void te_insert(gde_window_t *win, tedit_state_t *st, char c, int visible)
{
    if ((int)st->size >= TE_BUF_MAX - 1) return;
    int ln_off = (st->cursor < st->nlines) ? st->lstart[st->cursor] : (int)st->size;
    int ln_len; te_get_line(st, st->cursor, &ln_len);
    int col = (st->cur_col <= ln_len) ? st->cur_col : ln_len;
    int off = ln_off + col, i;
    for (i = (int)st->size; i > off; i--) st->buf[i] = st->buf[i-1];
    st->buf[off] = c;
    st->size++;
    te_index_lines(st);
    if (c == '\n') {
        st->cursor++; st->cur_col = 0;
        if (st->cursor >= st->scroll + visible) st->scroll = st->cursor - visible + 1;
    } else {
        st->cur_col++;
    }
    te_mark_dirty(win, st);
}

static void te_backspace(gde_window_t *win, tedit_state_t *st)
{
    int ln_off = (st->cursor < st->nlines) ? st->lstart[st->cursor] : (int)st->size;
    int ln_len; te_get_line(st, st->cursor, &ln_len);
    int col = (st->cur_col <= ln_len) ? st->cur_col : ln_len;
    int off = ln_off + col;
    if (off == 0) return;
    if (col > 0) {
        st->cur_col--;
    } else {
        st->cursor--;
        int prev_len; te_get_line(st, st->cursor, &prev_len); st->cur_col = prev_len;
        if (st->cursor < st->scroll) st->scroll = st->cursor;
    }
    int i;
    for (i = off-1; i < (int)st->size-1; i++) st->buf[i] = st->buf[i+1];
    if (st->size > 0) st->size--;
    te_index_lines(st);
    te_mark_dirty(win, st);
}

/* Execute File menu action: 0=New 1=Open(nop) 2=Save 3=SaveAs(nop). */
static void te_file_action(gde_window_t *win, tedit_state_t *st, int idx)
{
    if (idx == 0) {
        st->buf[0]='\0'; st->size=0; st->cursor=0; st->cur_col=0; st->scroll=0; st->dirty=0;
        te_index_lines(st);
        te_strncpy(win->title, "PD-Text - New", 64);
        st->path[0] = '\0';
    } else if (idx == 2 && st->path[0]) {
        vfs_node_t node;
        if (vfs_open(st->path, &node) != 0) {
            vfs_create(st->path);
            vfs_open(st->path, &node);
        }
        vfs_write(&node, 0, st->size, st->buf);
        st->dirty = 0;
        if (win->title[0] == '*') {
            int i; for (i = 0; win->title[i]; i++) win->title[i] = win->title[i+1];
        }
    }
}

static void te_mousedown(gde_window_t *win, int mx, int my)
{
    tedit_state_t *st = (tedit_state_t *)win->priv;
    if (!st) return;
    int mb_y = win->y + GDE_TITLEBAR_H;

    if (st->file_menu) {
        int fm_x = win->x + 2, fm_y = mb_y + GDE_MENUBAR_H;
        if (mx >= fm_x && mx < fm_x + 110 && my >= fm_y && my < fm_y + 84) {
            int item = (my - fm_y - 2) / 20;
            if (item >= 0 && item < 4) te_file_action(win, st, item);
        }
        st->file_menu = 0;
        return;
    }
    if (my >= mb_y && my < mb_y + GDE_MENUBAR_H) {
        st->file_menu = (mx >= win->x + 2 && mx < win->x + 42) ? !st->file_menu : 0;
        return;
    }
    /* Reposition text cursor on click in content area */
    int lnum_w = 4 * GFX_CHAR_W + 6;
    int text_y = WIN_CY(win) + TE_HDR_H;
    int text_h = WIN_CH(win) - TE_HDR_H - TE_STAT_H;
    if (my >= text_y && my < text_y + text_h) {
        int row = (my - text_y - TE_PAD_Y) / GFX_CHAR_H;
        int ln  = st->scroll + row;
        if (ln >= 0 && ln < st->nlines) {
            st->cursor = ln;
            int col = (mx - WIN_CX(win) - lnum_w - TE_PAD_X) / GFX_CHAR_W;
            int len; te_get_line(st, ln, &len);
            if (col < 0) col = 0; if (col > len) col = len;
            st->cur_col = col;
        }
    }
}

/* ---- Draw ---------------------------------------------------------------- */

static void te_draw(gde_window_t *win)
{
    tedit_state_t *st = (tedit_state_t *)win->priv;
    if (!st) return;

    int cx=WIN_CX(win), cy=WIN_CY(win), cw=WIN_CW(win), ch=WIN_CH(win);

    /* File menu button in the menu bar strip (drawn over win_draw_titlebar's fill) */
    {
        int mb_y  = win->y + GDE_TITLEBAR_H;
        int mb_ty = mb_y + (GDE_MENUBAR_H - GFX_CHAR_H) / 2;
        uint32_t fbg = st->file_menu ? COL_MENU_HOVER : COL_WIN_MENUBAR;
        gfx_fill_rect(win->x + 2, mb_y + 1, 40, GDE_MENUBAR_H - 2, fbg);
        gfx_draw_string(win->x + 8, mb_ty, "File", COL_MENU_TEXT, fbg, 0);
        if (st->file_menu) {
            static const char *fm[] = {"New", "Open...", "Save", "Save As..."};
            int fm_x = win->x + 2, fm_y = mb_y + GDE_MENUBAR_H, i;
            gfx_fill_rect(fm_x+2, fm_y+2, 112, 86, GFX_RGB(0,0,0));
            gfx_fill_rect(fm_x, fm_y, 110, 84, COL_MENU_BG);
            gfx_draw_rect(fm_x, fm_y, 110, 84, COL_MENU_BORDER);
            for (i = 0; i < 4; i++)
                gfx_draw_string(fm_x+8, fm_y+2+i*20+2, fm[i], COL_MENU_TEXT, COL_MENU_BG, 0);
        }
    }

    /* Header: current file path */
    gfx_fill_rect(cx, cy, cw, TE_HDR_H, COL_TE_HDR);
    gfx_hline(cx, cy+TE_HDR_H-1, cw, COL_TE_HDR_BD);
    gfx_draw_string(cx+6, cy+(TE_HDR_H-GFX_CHAR_H)/2,
                    st->path[0] ? st->path : "(new)", COL_TE_PATH, COL_TE_HDR, 0);

    int text_y = cy+TE_HDR_H;
    int text_h = ch-TE_HDR_H-TE_STAT_H;
    gfx_fill_rect(cx, text_y, cw, text_h, COL_TE_BG);

    int lnum_w = 4*GFX_CHAR_W+6;
    gfx_fill_rect(cx, text_y, lnum_w, text_h, COL_TE_LNUM_BG);
    gfx_vline(cx+lnum_w-1, text_y, text_h, COL_TE_HDR_BD);

    int visible = (text_h-TE_PAD_Y)/GFX_CHAR_H;
    int i;
    for (i=0; i<visible; i++) {
        int ln = st->scroll+i;
        if (ln>=st->nlines) break;
        int ry = text_y+TE_PAD_Y+i*GFX_CHAR_H;

        if (ln==st->cursor)
            gfx_fill_rect(cx+lnum_w, ry, cw-lnum_w, GFX_CHAR_H, COL_TE_CUR_LINE);

        /* Line number (right-aligned in gutter) */
        char lbuf[8]; int llen = te_itoa(lbuf, ln+1); lbuf[llen]='\0';
        gfx_draw_string(cx+lnum_w-(llen*GFX_CHAR_W)-4, ry,
                        lbuf, COL_TE_LNUM, COL_TE_LNUM_BG, 0);

        /* Text */
        int tlen; const char *txt = te_get_line(st, ln, &tlen);
        int cols = (cw-lnum_w-TE_PAD_X)/GFX_CHAR_W;
        if (cols<1) cols=1;
        uint32_t cbg = (ln==st->cursor) ? COL_TE_CUR_LINE : COL_TE_BG;
        gfx_draw_string_n(cx+lnum_w+TE_PAD_X, ry, txt,
                          (tlen<cols?tlen:cols), COL_TE_FG, cbg, 0);
    }

    /* Fill gap below last line */
    int bot_y = text_y+TE_PAD_Y+i*GFX_CHAR_H;
    if (bot_y < text_y+text_h)
        gfx_fill_rect(cx, bot_y, cw, text_y+text_h-bot_y, COL_TE_BG);

    /* Text insertion cursor (always visible when window focused) */
    if (win->focused) {
        int cur_vis = st->cursor - st->scroll;
        if (cur_vis >= 0 && cur_vis < visible) {
            int ry2   = text_y + TE_PAD_Y + cur_vis * GFX_CHAR_H;
            int cur_x = cx + lnum_w + TE_PAD_X + st->cur_col * GFX_CHAR_W;
            if (cur_x < cx + cw - 2)
                gfx_fill_rect(cur_x, ry2 + 2, 2, GFX_CHAR_H - 4, GFX_RGB(150, 205, 255));
        }
    }

    /* Status bar */
    int sy = cy+ch-TE_STAT_H;
    gfx_fill_rect(cx, sy, cw, TE_STAT_H, COL_TE_STAT);
    gfx_hline(cx, sy, cw, COL_TE_HDR_BD);

    char stat[64]; int si=0, j;
    const char *s1="Ln ";
    for (j=0; s1[j]; j++) stat[si++]=s1[j];
    si += te_itoa(&stat[si], st->cursor+1);
    const char *s2=" / ";
    for (j=0; s2[j] && si<58; j++) stat[si++]=s2[j];
    si += te_itoa(&stat[si], st->nlines);
    const char *s3=" lines";
    for (j=0; s3[j] && si<58; j++) stat[si++]=s3[j];
    stat[si]='\0';
    gfx_draw_string(cx+8, sy+(TE_STAT_H-GFX_CHAR_H)/2,
                    stat, COL_TE_STAT_TX, COL_TE_STAT, 0);
}

/* ---- Keys ---------------------------------------------------------------- */

static void te_key(gde_window_t *win, char k)
{
    tedit_state_t *st = (tedit_state_t *)win->priv;
    if (!st) return;

    if (st->file_menu) { st->file_menu = 0; }

    int text_h  = WIN_CH(win) - TE_HDR_H - TE_STAT_H;
    int visible = (text_h - TE_PAD_Y) / GFX_CHAR_H;
    if (visible < 1) visible = 1;

    if (k == 19) { te_file_action(win, st, 2); return; }  /* Ctrl+S = Save  */
    if (k == 14) { te_file_action(win, st, 0); return; }  /* Ctrl+N = New   */

    if (k == 0x26) {                          /* Up   */
        if (st->cursor > 0) st->cursor--;
        if (st->cursor < st->scroll) st->scroll = st->cursor;
    } else if (k == 0x28) {                   /* Down */
        if (st->cursor < st->nlines - 1) st->cursor++;
        if (st->cursor >= st->scroll + visible) st->scroll = st->cursor - visible + 1;
    } else if (k == 0x25) {                   /* Left */
        if (st->cur_col > 0) {
            st->cur_col--;
        } else if (st->cursor > 0) {
            st->cursor--;
            int ln; te_get_line(st, st->cursor, &ln); st->cur_col = ln;
            if (st->cursor < st->scroll) st->scroll = st->cursor;
        }
    } else if (k == 0x27) {                   /* Right */
        int ln; te_get_line(st, st->cursor, &ln);
        if (st->cur_col < ln) {
            st->cur_col++;
        } else if (st->cursor < st->nlines - 1) {
            st->cursor++; st->cur_col = 0;
            if (st->cursor >= st->scroll + visible) st->scroll = st->cursor - visible + 1;
        }
    } else if (k == 0x21) {                   /* PgUp */
        st->cursor -= visible; if (st->cursor < 0) st->cursor = 0;
        st->scroll = st->cursor;
    } else if (k == 0x22) {                   /* PgDn */
        st->cursor += visible;
        if (st->cursor >= st->nlines) st->cursor = st->nlines > 0 ? st->nlines-1 : 0;
        if (st->cursor >= st->scroll + visible) st->scroll = st->cursor - visible + 1;
    } else if (k == 0x24) {                   /* Home */
        st->cur_col = 0;
    } else if (k == 0x23) {                   /* End */
        int ln; te_get_line(st, st->cursor, &ln); st->cur_col = ln;
    } else if (k == 8) {                      /* Backspace */
        te_backspace(win, st);
    } else if (k == '\r' || k == '\n') {      /* Enter */
        te_insert(win, st, '\n', visible);
    } else if (k >= 32 && (uint8_t)k < 127) { /* Printable ASCII */
        te_insert(win, st, k, visible);
    }
}

/* ---- Public API ---------------------------------------------------------- */

void text_editor_open(const char *path)
{
    char title[80];
    const char *name = path;
    const char *p = path;
    while (*p) { if (*p=='/') name=p+1; p++; }
    te_strncpy(title, "PD-Text \x97 ", 80);   /* \x97 = em-dash cp437 */
    int tl = te_strlen(title);
    te_strncpy(title+tl, name, 80-tl);

    tedit_state_t *st = (tedit_state_t *)kmalloc(sizeof(tedit_state_t));
    if (!st) return;
    te_strncpy(st->path, path, 128);
    st->size=0; st->nlines=0; st->scroll=0; st->cursor=0; st->cur_col=0; st->dirty=0; st->file_menu=0;

    vfs_node_t node;
    if (vfs_open(path, &node)==0) {
        uint32_t toread = node.size<TE_BUF_MAX ? node.size : TE_BUF_MAX;
        int r = vfs_read(&node, 0, toread, st->buf);
        st->size = (r>0) ? (uint32_t)r : toread;
    }
    if (st->size==0) {
        const char *msg="(empty or unreadable)\n";
        int ml=te_strlen(msg); uint32_t i;
        for (i=0;(int)i<ml;i++) st->buf[i]=msg[i];
        st->size=(uint32_t)ml;
    }
    te_index_lines(st);

    gde_window_t *w = wm_create(title, 160, 100, 720, 520, te_draw, te_key);
    if (!w) { kfree(st); return; }
    w->priv         = st;
    w->on_mousedown = te_mousedown;
}
