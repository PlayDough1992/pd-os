/* ===============================================================================================
 * PD-Shell  —  Built-in command shell (Tier 1)
 * =============================================================================================== */

#include "shell.h"
#include "kernel.h"
#include "vga.h"
#include "io.h"
#include "keyboard.h"
#include "pit.h"
#include "users.h"
#include "e820.h"
#include "pmm.h"
#include "kheap.h"
#include "ata.h"
#include "vfs.h"
#include "pdfs.h"
#include "process.h"
#include "install.h"
#include "rtl8139.h"
#include "net.h"
#include "arp.h"

/* ---- Session state -------------------------------------------------------- */

static const user_t *g_session_user = NULL;
static int           g_logout       = 0;
static int           g_elevated     = 0;   /* set during an elev sub-command */
static char          g_cwd[128]     = "/"; /* current working directory      */

/* ---- String helpers (no libc) ------------------------------------------- */

static int sh_strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

static void sh_memset(char *p, char v, int n)
{
    while (n--) *p++ = v;
}

/* Forward declarations for helpers used before their definition */
static void normalize_path(const char *input, char *out);
static void ww_draw(const char *buf, int count,
                    uint8_t a_col, uint8_t a_row,
                    int do_render,
                    int cursor_idx, uint8_t *out_col, uint8_t *out_row);

/* Command name list for tab-completion of the first token.
 * Must stay in sync with commands[] and aliases[]. */
static const char * const cmd_name_list[] = {
    "help","clear","print","version","uptime","color","whoami",
    "rammap","raminfo","heapinfo","diskinfo","nettest","netinfo",
    "list","read","write","delete","factreset","makedir",
    "setperm","setowner","goto","copy","move","admin","alias",
    "logout","reboot","shutdown","ps","kill","adduser","deluser",
    "changerpass","changempass","install",
    /* aliases */
    "ls","dir","cat","type","rm","del","erase","mkdir","md","cd",
    "mv","ren","rename","echo","chmod","chown","sudo","runas","cls",
    "exit","ver","useradd","userdel",
    NULL
};

/* ---- Command history ------------------------------------------------------ */

#define HIST_SIZE  32
#define SHELL_BUF_SIZE_H 512   /* matches SHELL_BUF_SIZE used below */

static char g_hist[HIST_SIZE][SHELL_BUF_SIZE_H];
static int  g_hist_count = 0;   /* total entries ever added (unbounded) */

/* Push a non-empty line into the circular history buffer.
 * Duplicate of the immediately previous entry is silently dropped. */
static void hist_push(const char *line)
{
    if (!line || !line[0]) return;

    /* Drop exact duplicate of previous entry */
    if (g_hist_count > 0) {
        int prev = (g_hist_count - 1) % HIST_SIZE;
        int i;
        for (i = 0; i < SHELL_BUF_SIZE_H - 1; i++) {
            if (g_hist[prev][i] != line[i]) goto add;
            if (!line[i]) return;  /* identical */
        }
    }
add:;
    int slot = g_hist_count % HIST_SIZE;
    int i;
    for (i = 0; i < SHELL_BUF_SIZE_H - 1 && line[i]; i++)
        g_hist[slot][i] = line[i];
    g_hist[slot][i] = '\0';
    g_hist_count++;
}

/* Retrieve a history entry by logical index (0 = oldest in window).
 * Returns NULL if idx is out of range. */
static const char *hist_get(int idx)
{
    /* idx is a logical position from oldest (0) to newest (g_hist_count-1) */
    if (idx < 0 || idx >= g_hist_count) return 0;
    /* Only HIST_SIZE entries are kept */
    int oldest = (g_hist_count > HIST_SIZE) ? (g_hist_count - HIST_SIZE) : 0;
    if (idx < oldest) return 0;
    return g_hist[idx % HIST_SIZE];
}

/* ---- Tab completion ------------------------------------------------------- */
/*
 * Given what the user has typed so far in buf[0..count-1], find the last
 * whitespace-delimited token starting at buf[tok_start].  That token is the
 * completion prefix.  Enumerate g_cwd (or the directory portion of the token
 * if a path separator is present) and:
 *   - unique match  → append the rest of the name (+ '/' for dirs)
 *   - multiple      → print a compact list below the prompt, redraw prompt
 *   - no match      → do nothing
 *
 * buf/count/cursor are updated in place.  anchor_col/anchor_row and
 * base_scroll are passed by pointer so the caller can track the new anchor
 * after we print the match list and get a fresh prompt line.
 */
#define TAB_MAX_MATCHES 32

static void tab_complete(char *buf, int *count_p, int *cursor_p,
                         uint8_t *anchor_col_p, uint8_t *anchor_row_p,
                         int *base_scroll_p)
{
    int count = *count_p;

    /* Find start of last token (up to cursor) */
    int tok_start = *cursor_p;
    while (tok_start > 0 && buf[tok_start - 1] != ' ') tok_start--;

    /* The prefix to complete is buf[tok_start .. cursor-1] */
    int pfx_len = *cursor_p - tok_start;
    char prefix[PDFS_NAME_LEN];
    if (pfx_len >= (int)PDFS_NAME_LEN) return; /* can't complete */
    int i;
    for (i = 0; i < pfx_len; i++) prefix[i] = buf[tok_start + i];
    prefix[pfx_len] = '\0';

    /* If the prefix contains a '/', resolve the directory part */
    char scan_dir[128];
    char name_prefix[PDFS_NAME_LEN];
    int  slash = -1;
    for (i = pfx_len - 1; i >= 0; i--) {
        if (prefix[i] == '/') { slash = i; break; }
    }
    if (slash >= 0) {
        /* Directory part: prefix[0..slash] */
        char dir_part[128];
        int dp_len = slash + 1;
        if (dp_len >= 127) return;
        for (i = 0; i < dp_len; i++) dir_part[i] = prefix[i];
        dir_part[dp_len] = '\0';
        normalize_path(dir_part, scan_dir);
        /* Name part after slash */
        int np_len = pfx_len - dp_len;
        if (np_len >= (int)PDFS_NAME_LEN) return;
        for (i = 0; i < np_len; i++) name_prefix[i] = prefix[i + dp_len];
        name_prefix[np_len] = '\0';
    } else {
        /* No slash — complete relative to CWD */
        int ci = 0;
        while (g_cwd[ci] && ci < 127) { scan_dir[ci] = g_cwd[ci]; ci++; }
        scan_dir[ci] = '\0';
        for (i = 0; i < pfx_len; i++) name_prefix[i] = prefix[i];
        name_prefix[pfx_len] = '\0';
    }

    int  np_len = 0;
    while (name_prefix[np_len]) np_len++;

    /* Strip leading '/' for pdfs_stat_dir */
    const char *dp = scan_dir;
    while (*dp == '/') dp++;

    /* Collect matches */
    char matches[TAB_MAX_MATCHES][PDFS_NAME_LEN];
    uint8_t match_is_dir[TAB_MAX_MATCHES];
    int match_count = 0;

    /* First token with no path separator: also complete command/alias names */
    if (tok_start == 0 && slash < 0) {
        int ci;
        for (ci = 0; cmd_name_list[ci] && match_count < TAB_MAX_MATCHES; ci++) {
            const char *cn = cmd_name_list[ci];
            int match = 1;
            for (i = 0; i < np_len; i++) {
                if (cn[i] != name_prefix[i]) { match = 0; break; }
            }
            if (!match) continue;
            int clen = 0;
            while (cn[clen] && clen < (int)PDFS_NAME_LEN - 1) {
                matches[match_count][clen] = cn[clen]; clen++;
            }
            matches[match_count][clen] = '\0';
            match_is_dir[match_count] = 0;
            match_count++;
        }
    }

    /* Filesystem matches (arguments / explicit paths) */
    if (tok_start > 0 || slash >= 0) {
        for (uint32_t idx = 0; match_count < TAB_MAX_MATCHES; idx++) {
            pdfs_dirent_t de;
            if (pdfs_stat_dir(dp, idx, &de) != 0) break;
            int match = 1;
            for (i = 0; i < np_len; i++) {
                if (de.name[i] != name_prefix[i]) { match = 0; break; }
            }
            if (!match) continue;
            for (i = 0; i < (int)PDFS_NAME_LEN; i++) matches[match_count][i] = de.name[i];
            match_is_dir[match_count] = (de.flags & PDFS_FLAG_DIR) ? 1 : 0;
            match_count++;
        }
    }

    if (match_count == 0) return; /* no match — do nothing */

    if (match_count == 1) {
        /* Unique match: complete it */
        const char *m   = matches[0];
        int         mlen = 0;
        while (m[mlen]) mlen++;

        /* Suffix = characters after the prefix */
        int suffix_len = mlen - np_len;

        /* Check there's room in buf */
        int extra = suffix_len + (match_is_dir[0] ? 1 : 0);
        if (count + extra >= SHELL_BUF_SIZE - 1) return;

        /* Insert suffix at cursor */
        /* First make room */
        for (i = count + extra - 1; i >= *cursor_p + extra; i--)
            buf[i] = buf[i - extra];
        /* Write suffix */
        int wp = *cursor_p;
        for (i = np_len; i < mlen; i++) buf[wp++] = m[i];
        if (match_is_dir[0]) buf[wp++] = '/';
        *cursor_p = wp;
        count += extra;
        buf[count] = '\0';
        *count_p = count;

        /* Redraw */
        uint8_t cx, cy;
        vga_clear_chars(*anchor_col_p, *anchor_row_p, count + VGA_WIDTH);
        ww_draw(buf, count, *anchor_col_p, *anchor_row_p, 1, *cursor_p, &cx, &cy);
        vga_set_cursor(cx, cy);

    } else {
        /* Multiple matches: print list below, then redraw prompt */
        vga_putchar('\n');
        {
            int col = 0;
            int m;
            for (m = 0; m < match_count; m++) {
                int mlen = 0;
                while (matches[m][mlen]) mlen++;
                int field = mlen + (match_is_dir[m] ? 1 : 0) + 2; /* +2 space gap */
                if (col > 0 && col + field > VGA_WIDTH) {
                    vga_putchar('\n');
                    col = 0;
                }
                if (match_is_dir[m])
                    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
                else
                    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
                for (i = 0; i < mlen; i++) vga_putchar(matches[m][i]);
                if (match_is_dir[m]) vga_putchar('/');
                vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
                vga_putchar(' ');
                vga_putchar(' ');
                col += field;
            }
            vga_putchar('\n');
        }

        /* Reprint prompt */
        vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
        kprintf("%s", g_session_user ? g_session_user->username : "?");
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        kprintf("@pd-shell:");
        vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
        kprintf("%s", g_cwd);
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        kprintf("> ");

        /* New anchor */
        *anchor_col_p  = vga_get_col();
        *anchor_row_p  = vga_get_row();
        *base_scroll_p = vga_get_scroll_count();

        /* Redraw current buf */
        uint8_t cx, cy;
        ww_draw(buf, count, *anchor_col_p, *anchor_row_p, 1, *cursor_p, &cx, &cy);
        vga_set_cursor(cx, cy);
    }
}

/* ---- Word-wrap aware redraw helper --------------------------------------- */
/*
 * Simulate or render buf[0..count-1] with soft word-wrap starting from
 * screen position (a_col, a_row).  Words (runs of non-space chars) that
 * would overflow the current line are moved to the next line by padding
 * with spaces first.  Words longer than VGA_WIDTH hard-wrap as normal.
 *
 * do_render : 1 = call vga_putchar for each char, 0 = simulate only
 * cursor_idx: char index whose visual position should be returned;
 *             pass -1 to skip
 * out_col/out_row: receives visual col/row of cursor_idx
 */
static void ww_draw(const char *buf, int count,
                    uint8_t a_col, uint8_t a_row,
                    int do_render,
                    int cursor_idx, uint8_t *out_col, uint8_t *out_row)
{
    int col = (int)a_col;
    int row = (int)a_row;
    int i;
    if (do_render) vga_set_cursor(a_col, a_row);
    for (i = 0; i <= count; i++) {
        /* Soft word-wrap look-ahead */
        if (i < count && buf[i] != ' ') {
            int wlen = 0, j = i;
            while (j < count && buf[j] != ' ') { wlen++; j++; }
            if (col > 0 && col + wlen > VGA_WIDTH) {
                if (do_render) {
                    int k;
                    for (k = col; k < VGA_WIDTH; k++) vga_putchar(' ');
                }
                col = 0;
                row++;
            }
        }
        /* Record cursor visual position BEFORE rendering char i */
        if (i == cursor_idx && out_col) {
            *out_col = (uint8_t)col;
            *out_row = (uint8_t)row;
        }
        if (i == count) break;
        if (do_render) vga_putchar(buf[i]);
        col++;
        if (col >= VGA_WIDTH) { col = 0; row++; }
    }
}

/* ---- Suggestion menu helpers ---------------------------------------------- */

#define SUG_MAX 8

/* Draw suggestion menu starting at menu_row; restores cursor to (ret_cx, ret_cy).
 * Returns number of rows drawn. */
static int sug_draw_menu(char items[][PDFS_NAME_LEN], int count, int sel,
                         int menu_row, uint8_t ret_cx, uint8_t ret_cy)
{
    int rows = 0, r, j;
    for (r = 0; r < count && (menu_row + r) < VGA_HEIGHT; r++) {
        vga_set_cursor(0, (uint8_t)(menu_row + r));
        if (r == sel)
            vga_set_color(VGA_COLOR_BLACK, VGA_COLOR_LIGHT_CYAN);
        else
            vga_set_color(VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK);
        vga_putchar(' '); vga_putchar(' ');
        for (j = 0; items[r][j]; j++) vga_putchar(items[r][j]);
        for (; j < 22; j++) vga_putchar(' ');
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        rows++;
    }
    vga_set_cursor(ret_cx, ret_cy);
    return rows;
}

/* Recompute and redraw suggestion menu after buffer change.
 * Clears old menu, draws new one if >= 2 prefix matches. */
static void sug_update(char *buf, int count, int cursor,
                       uint8_t anchor_col, uint8_t anchor_row,
                       int *sug_active_p, int *sug_sel_p, int *sug_count_p,
                       char sug_items[][PDFS_NAME_LEN],
                       int *sug_menu_row_p, int *sug_rows_p,
                       uint16_t *sug_saved)
{
    int i;
    /* Restore VGA content under old menu (instead of blanking) */
    if (*sug_active_p) {
        if (*sug_rows_p > 0)
            vga_restore_rows(*sug_menu_row_p, *sug_rows_p, sug_saved);
        *sug_active_p = 0;
        *sug_count_p  = 0;
        *sug_rows_p   = 0;
    }
    /* Only suggest for first token (no space before cursor) */
    for (i = 0; i < cursor; i++)
        if (buf[i] == ' ') return;
    if (cursor <= 0) return;

    /* Prefix = buf[0..cursor-1] */
    int pfx_len = (cursor < (int)PDFS_NAME_LEN) ? cursor : (int)PDFS_NAME_LEN - 1;
    char prefix[PDFS_NAME_LEN];
    for (i = 0; i < pfx_len; i++) prefix[i] = buf[i];
    prefix[pfx_len] = '\0';

    /* Collect prefix-matching commands (skip exact matches) */
    int mc = 0;
    for (i = 0; cmd_name_list[i] && mc < SUG_MAX; i++) {
        const char *cn = cmd_name_list[i];
        int m = 1, j;
        for (j = 0; j < pfx_len; j++) {
            if (cn[j] != prefix[j]) { m = 0; break; }
        }
        if (!m || cn[pfx_len] == '\0') continue;
        int k;
        for (k = 0; k < (int)PDFS_NAME_LEN; k++) sug_items[mc][k] = cn[k];
        mc++;
    }

    if (mc < 2) return;
    *sug_count_p = mc;

    /* Menu row: one line below end of rendered input; if no room, go above */
    uint8_t ex, ey;
    ww_draw(buf, count, anchor_col, anchor_row, 0, count, &ex, &ey);
    int menu_row = (int)ey + 1;
    if (menu_row + mc > VGA_HEIGHT) {
        /* Try above the prompt instead */
        menu_row = (int)anchor_row - mc;
        if (menu_row < 0) {
            /* Clip: as many rows as fit above prompt */
            mc = (int)anchor_row;
            if (mc < 2) return;
            menu_row = 0;
            *sug_count_p = mc;
        }
    }

    /* Keep selection in bounds */
    if (*sug_sel_p >= mc) *sug_sel_p = mc - 1;

    *sug_menu_row_p = menu_row;
    *sug_active_p   = 1;

    /* Save rows that the menu will overwrite, then draw menu */
    vga_save_rows(menu_row, mc, sug_saved);
    uint8_t cx, cy;
    ww_draw(buf, count, anchor_col, anchor_row, 0, cursor, &cx, &cy);
    *sug_rows_p = sug_draw_menu(sug_items, mc, *sug_sel_p, menu_row, cx, cy);
}

/* ---- readline ------------------------------------------------------------- */
/*
 * Reads one line of input into buf (max len-1 chars + NUL).
 * Handles: printable chars, backspace (bounded to anchor), left/right arrows,
 *          UP/DOWN arrows for history navigation.
 * Returns number of chars in buf (excluding NUL).
 */
static int readline(char *buf, int len)
{
    int  count = 0;          /* chars in buffer */
    int  cursor = 0;         /* insertion point within buffer */

    /* History navigation: hist_idx == g_hist_count means "current (unsaved) line" */
    int  hist_idx = g_hist_count;
    char hist_saved[SHELL_BUF_SIZE_H];  /* preserves what the user typed before UP */
    hist_saved[0] = '\0';

    /* Suggestion menu state */
    int      sug_active   = 0;
    int      sug_sel      = -1;
    int      sug_count    = 0;
    char     sug_items[SUG_MAX][PDFS_NAME_LEN];
    int      sug_menu_row = 0;
    int      sug_rows     = 0;
    uint16_t sug_saved[SUG_MAX * VGA_WIDTH]; /* VGA cells underneath the menu */

/* Close the suggestion menu and restore the cells it was covering */
#define SUG_CLOSE() do { \
    if (sug_active) { \
        if (sug_rows > 0) \
            vga_restore_rows(sug_menu_row, sug_rows, sug_saved); \
        sug_active = 0; sug_sel = -1; sug_count = 0; sug_rows = 0; \
    } \
} while(0)

    /* Anchor: cursor position on screen where input begins */
    uint8_t anchor_col = vga_get_col();
    uint8_t anchor_row = vga_get_row();
    int     base_scroll = vga_get_scroll_count(); /* detect forced scrolls */

    sh_memset(buf, 0, len);

    for (;;) {
        char c = keyboard_getchar();

        /* --- Scroll viewport (don't snap back, just scroll) --- */
        if (c == KEY_PGUP) { vga_scroll_up(1);   continue; }
        if (c == KEY_PGDN) { vga_scroll_down(1); continue; }

        /* Any other key: snap back to live view before processing */
        vga_scroll_reset();

        /* Adjust anchor if the screen has scrolled since last keypress */
        {
            int cur_scroll = vga_get_scroll_count();
            int delta = cur_scroll - base_scroll;
            if (delta > 0) {
                anchor_row = (anchor_row >= (uint8_t)delta)
                             ? (uint8_t)(anchor_row - delta) : 0;
                base_scroll = cur_scroll;
            }
        }

        /* --- Enter --- */
        if (c == '\n' || c == '\r') {
            SUG_CLOSE();
            buf[count] = '\0';
            vga_putchar('\n');
            return count;
        }

        /* --- Tab: file/dir completion --- */
        if (c == '\t') {
            tab_complete(buf, &count, &cursor,
                         &anchor_col, &anchor_row, &base_scroll);
            continue;
        }

        /* --- Space: confirm highlighted suggestion --- */
        if (c == ' ' && sug_active && sug_sel >= 0) {
            const char *sel_name = sug_items[sug_sel];
            int slen = 0, k;
            while (sel_name[slen]) slen++;
            /* Build: sel_name + ' ' + buf[cursor..count-1] */
            char tmp[SHELL_BUF_SIZE];
            int tp = 0;
            for (k = 0; k < slen && tp < SHELL_BUF_SIZE - 2; k++) tmp[tp++] = sel_name[k];
            tmp[tp++] = ' ';
            for (k = cursor; k < count && tp < SHELL_BUF_SIZE - 1; k++) tmp[tp++] = buf[k];
            tmp[tp] = '\0';
            for (k = 0; k <= tp; k++) buf[k] = tmp[k];
            count  = tp;
            cursor = slen + 1;
            SUG_CLOSE();
            {
                uint8_t cx, cy;
                vga_clear_chars(anchor_col, anchor_row, count + VGA_WIDTH);
                ww_draw(buf, count, anchor_col, anchor_row, 1, cursor, &cx, &cy);
                vga_set_cursor(cx, cy);
            }
            continue;
        }

        /* --- Backspace --- */
        if (c == '\b') {
            if (cursor > 0) {
                /* Shift chars left in buffer */
                int i;
                for (i = cursor - 1; i < count - 1; i++)
                    buf[i] = buf[i + 1];
                buf[--count] = '\0';
                cursor--;

                {
                    uint8_t cx, cy;
                    vga_clear_chars(anchor_col, anchor_row, count + 1 + VGA_WIDTH);
                    ww_draw(buf, count, anchor_col, anchor_row, 1, cursor, &cx, &cy);
                    vga_set_cursor(cx, cy);
                    sug_update(buf, count, cursor, anchor_col, anchor_row,
                               &sug_active, &sug_sel, &sug_count, sug_items,
                               &sug_menu_row, &sug_rows, sug_saved);
                }
            }
            continue;
        }

        /* --- Left arrow --- */
        if (c == KEY_LEFT) {
            if (cursor > 0) {
                cursor--;
                uint8_t cx, cy;
                ww_draw(buf, count, anchor_col, anchor_row, 0, cursor, &cx, &cy);
                vga_set_cursor(cx, cy);
            }
            continue;
        }

        /* --- Right arrow --- */
        if (c == KEY_RIGHT) {
            if (cursor < count) {
                cursor++;
                uint8_t cx, cy;
                ww_draw(buf, count, anchor_col, anchor_row, 0, cursor, &cx, &cy);
                vga_set_cursor(cx, cy);
            }
            continue;
        }

        /* --- Up arrow: go back in history --- */
        if (c == KEY_UP) {
            /* Suggestion menu: navigate up */
            if (sug_active) {
                if (sug_sel > -1) sug_sel--;
                uint8_t cx, cy;
                ww_draw(buf, count, anchor_col, anchor_row, 0, cursor, &cx, &cy);
                sug_draw_menu(sug_items, sug_count, sug_sel, sug_menu_row, cx, cy);
                continue;
            }
            int oldest = (g_hist_count > HIST_SIZE) ? (g_hist_count - HIST_SIZE) : 0;
            if (hist_idx <= oldest) continue;  /* already at oldest */

            /* Save current line the first time we leave it */
            if (hist_idx == g_hist_count) {
                int i;
                for (i = 0; i < count && i < SHELL_BUF_SIZE_H - 1; i++)
                    hist_saved[i] = buf[i];
                hist_saved[i] = '\0';
            }

            hist_idx--;
            const char *entry = hist_get(hist_idx);
            if (!entry) continue;

            /* Load entry into buf */
            int i;
            for (i = 0; i < len - 1 && entry[i]; i++)
                buf[i] = entry[i];
            buf[i] = '\0';

            {
                int old_count = count;
                count = 0;
                for (i = 0; buf[i]; i++) count++;
                cursor = count;
                uint8_t cx, cy;
                vga_clear_chars(anchor_col, anchor_row, old_count + VGA_WIDTH);
                ww_draw(buf, count, anchor_col, anchor_row, 1, cursor, &cx, &cy);
                vga_set_cursor(cx, cy);
            }
            continue;
        }

        /* --- Down arrow: go forward in history --- */
        if (c == KEY_DOWN) {
            /* Suggestion menu: navigate down */
            if (sug_active) {
                if (sug_sel < sug_count - 1) sug_sel++;
                uint8_t cx, cy;
                ww_draw(buf, count, anchor_col, anchor_row, 0, cursor, &cx, &cy);
                sug_draw_menu(sug_items, sug_count, sug_sel, sug_menu_row, cx, cy);
                continue;
            }
            if (hist_idx >= g_hist_count) continue;  /* already at current line */

            hist_idx++;
            const char *entry;
            if (hist_idx == g_hist_count) {
                entry = hist_saved;  /* restore what user had typed */
            } else {
                entry = hist_get(hist_idx);
                if (!entry) continue;
            }

            int i;
            for (i = 0; i < len - 1 && entry[i]; i++)
                buf[i] = entry[i];
            buf[i] = '\0';

            {
                int old_count = count;
                count = 0;
                for (i = 0; buf[i]; i++) count++;
                cursor = count;
                uint8_t cx, cy;
                vga_clear_chars(anchor_col, anchor_row, old_count + VGA_WIDTH);
                ww_draw(buf, count, anchor_col, anchor_row, 1, cursor, &cx, &cy);
                vga_set_cursor(cx, cy);
            }
            continue;
        }

        /* --- Printable character --- */
        if (count >= len - 1) continue;   /* buffer full */

        /* Insert at cursor position */
        int i;
        for (i = count; i > cursor; i--)
            buf[i] = buf[i - 1];
        buf[cursor++] = c;
        count++;

        {
            uint8_t cx, cy;
            vga_clear_chars(anchor_col, anchor_row, count - 1 + VGA_WIDTH);
            ww_draw(buf, count, anchor_col, anchor_row, 1, cursor, &cx, &cy);
            vga_set_cursor(cx, cy);
            sug_update(buf, count, cursor, anchor_col, anchor_row,
                       &sug_active, &sug_sel, &sug_count, sug_items,
                       &sug_menu_row, &sug_rows, sug_saved);
        }
    }
}

/* ---- Password readline (masked) ------------------------------------------ */
/*
 * Like readline but echoes '*' for each character.
 * No cursor movement -- password input is strictly append/backspace.
 */
static int readline_masked(char *buf, int len)
{
    int     count      = 0;
    uint8_t anchor_col = vga_get_col();
    uint8_t anchor_row = vga_get_row();

    sh_memset(buf, 0, len);

    for (;;) {
        char c = keyboard_getchar();

        if (c == '\n' || c == '\r') {
            buf[count] = '\0';
            vga_putchar('\n');
            return count;
        }

        if (c == '\b') {
            if (count > 0) {
                buf[--count] = '\0';
                vga_set_cursor(anchor_col, anchor_row);
                int i;
                for (i = 0; i < count; i++) vga_putchar('*');
                vga_putchar(' ');
                vga_set_cursor(
                    (uint8_t)((anchor_col + count) % VGA_WIDTH),
                    (uint8_t)(anchor_row + (anchor_col + count) / VGA_WIDTH));
            }
            continue;
        }

        if (c == KEY_LEFT || c == KEY_RIGHT || c == KEY_UP || c == KEY_DOWN)
            continue;
        if (c < 0x20) continue;
        if (count >= len - 1) continue;

        buf[count++] = c;
        vga_putchar('*');
    }
}

/* ---- Tokenizer ------------------------------------------------------------ */

static int tokenize(char *line, char *argv[], int max_args)
{
    int argc = 0;
    char *p = line;

    while (*p) {
        /* Skip spaces */
        while (*p == ' ') p++;
        if (!*p) break;
        if (argc >= max_args) break;

        argv[argc++] = p;

        /* Find end of token */
        while (*p && *p != ' ') p++;
        if (*p) *p++ = '\0';
    }
    return argc;
}

/* ---- Built-in commands ---------------------------------------------------- */

/* ---- Help row helper: prints one aligned, word-wrapped help entry --------- */
#define HELP_CMD_COL  22   /* chars reserved for command+args (after 4-space indent) */
#define HELP_DESC_COL 28   /* column where descriptions start (4 + 22 + 2 gap)      */
#define HELP_DESC_W   52   /* available width for description text (80 - 28)         */

static void help_row(const char *cmd, const char *desc)
{
    int i;
    /* Yellow: 4-space indent + command padded to HELP_CMD_COL */
    vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
    for (i = 0; i < 4; i++) vga_putchar(' ');
    int clen = 0;
    while (cmd[clen]) { vga_putchar(cmd[clen]); clen++; }
    for (i = clen; i < HELP_CMD_COL; i++) vga_putchar(' ');
    vga_putchar(' '); vga_putchar(' ');   /* 2-space gap */

    /* White: description with word wrapping */
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    const char *p = desc;
    int col = 0;
    while (*p) {
        /* Skip inter-word spaces; note whether there was a separator */
        int had_space = 0;
        while (*p == ' ') { p++; had_space = 1; }
        if (!*p) break;

        /* Measure next word */
        int wlen = 0;
        while (p[wlen] && p[wlen] != ' ') wlen++;

        /* Wrap if word won't fit on current line */
        int need = (col == 0) ? wlen : col + 1 + wlen;
        if (need > HELP_DESC_W && col > 0) {
            vga_putchar('\n');
            for (i = 0; i < HELP_DESC_COL; i++) vga_putchar(' ');
            col = 0;
        } else if (had_space && col > 0) {
            vga_putchar(' '); col++;
        }

        for (i = 0; i < wlen; i++) { vga_putchar(p[i]); col++; }
        p += wlen;
    }
    vga_putchar('\n');
}

static void cmd_help(int argc, char *argv[])
{
    (void)argc; (void)argv;
    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    kprintf("\n  PD-Shell built-in commands:\n\n");
    help_row("help",                 "Show this help message");
    help_row("clear",                "Clear the screen");
    help_row("print [text]",         "Print text to the screen");
    help_row("version",              "Show kernel and shell version");
    help_row("uptime",               "Show system uptime in seconds");
    help_row("color [fg] [bg]",      "Set text color (0-15 each)");
    help_row("whoami",               "Show current user");
    help_row("rammap",               "Show physical memory map (E820)");
    help_row("raminfo",              "Show memory usage (free/used/total)");
    help_row("heapinfo",             "Show kernel heap stats");
    help_row("diskinfo",             "Show ATA drive info and layout");
    help_row("nettest",              "Send ARP broadcast and dump any reply (RTL8139 smoke test)");
    help_row("netinfo",              "Show network configuration (IP, MAC, gateway, DNS)");
    help_row("list",                 "List files on PDFS");
    help_row("read <file>",          "Print file contents");
    help_row("write <f> <text>",     "Create or overwrite a file");
    help_row("delete <file>",        "Delete a file");
    help_row("factreset",             "Factory reset PD-OS (requires admin, wipes all data)");
    help_row("makedir <dir>",        "Create a subdirectory");
    help_row("setperm <f> <oct>",    "Set file permissions (octal mode)");
    help_row("setowner <f> <u>:<g>", "Set file owner (e.g. setowner f.txt pd:pd, requires admin)");
    help_row("goto [path]",          "Change directory (~, .., /abs, relative)");
    help_row("copy <src> <dst>",     "Copy a file");
    help_row("move <src> <dst>",     "Move or rename a file");
    help_row("admin <cmd>",          "Run a command with admin privileges");
    help_row("alias [name]",         "List all aliases, or look up one alias");
    help_row("logout",               "Log out and return to login screen");
    help_row("reboot",               "Reboot the system");
    help_row("shutdown",             "Shut the system down completely");
    help_row("ps",                                    "List running processes");
    help_row("kill <pid>",                            "Terminate a process by PID");
    help_row("adduser <user> <admin|regular> <pass>", "Create a new user account");
    help_row("deluser <user>",                        "Delete a user account (requires admin)");
    help_row("changerpass",                           "Change root password (requires admin, root password only)");
    help_row("changempass",                           "Change your own password (forces re-login)");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    kprintf("\n");
}

static void cmd_clear(int argc, char *argv[])
{
    (void)argc; (void)argv;
    vga_clear();
}

static void cmd_print(int argc, char *argv[])
{
    int i;
    kprintf("  ");
    for (i = 1; i < argc; i++) {
        kprintf("%s", argv[i]);
        if (i < argc - 1) vga_putchar(' ');
    }
    vga_putchar('\n');
}

static void cmd_version(int argc, char *argv[])
{
    (void)argc; (void)argv;
    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    kprintf("\n  PD-Kernel  v0.1\n");
    kprintf("  PD-Shell   v0.1  (Tier 1 - built-ins only)\n\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
}

static void cmd_uptime(int argc, char *argv[])
{
    (void)argc; (void)argv;
    uint32_t secs = pit_get_ticks() / 100;   /* PIT runs at 100 Hz */
    kprintf("  Uptime: %u second%s\n", secs, secs == 1 ? "" : "s");
}

static void cmd_color(int argc, char *argv[])
{
    if (argc < 3) {
        kprintf("  Usage: color <fg 0-15> <bg 0-15>\n");
        return;
    }
    /* Simple atoi — no libc */
    int fg = 0, bg = 0;
    char *p;
    for (p = argv[1]; *p >= '0' && *p <= '9'; p++) fg = fg * 10 + (*p - '0');
    for (p = argv[2]; *p >= '0' && *p <= '9'; p++) bg = bg * 10 + (*p - '0');
    if (fg > 15) fg = 15;
    if (bg > 15) bg = 15;
    vga_set_color((vga_color_t)fg, (vga_color_t)bg);
    kprintf("  Color set.\n");
}

static void cmd_reboot(int argc, char *argv[])
{
    (void)argc; (void)argv;
    kprintf("  Rebooting...\n");
    /* Pulse CPU reset line via keyboard controller */
    __asm__ volatile (
        "cli\n"
        "1: inb $0x64, %al\n"
        "testb $0x02, %al\n"
        "jnz 1b\n"
        "movb $0xFE, %al\n"
        "outb %al, $0x64\n"
        "hlt\n"
    );
}

static void cmd_whoami(int argc, char *argv[])
{
    (void)argc; (void)argv;
    if (!g_session_user) return;
    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    kprintf("  %s", g_session_user->username);
    vga_set_color(VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK);
    kprintf("  (uid=%u%s)\n",
            (uint32_t)g_session_user->uid,
            (g_session_user->flags & USER_FLAG_ROOT) ? ", root" : "");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
}

static void cmd_rammap(int argc, char *argv[])
{
    (void)argc; (void)argv;
    e820_print();
}

static void cmd_raminfo(int argc, char *argv[])
{
    (void)argc; (void)argv;
    uint32_t free_mb  = (pmm_free_frames()  * PMM_PAGE_SIZE) / (1024u * 1024u);
    uint32_t used_mb  = (pmm_used_frames()  * PMM_PAGE_SIZE) / (1024u * 1024u);
    uint32_t total_mb = (pmm_total_frames() * PMM_PAGE_SIZE) / (1024u * 1024u);
    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    kprintf("\n  Memory usage:\n");
    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    kprintf("    Free:   %u MB  (%u frames)\n", free_mb,  pmm_free_frames());
    vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
    kprintf("    Used:   %u MB  (%u frames)\n", used_mb,  pmm_used_frames());
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    kprintf("    Total:  %u MB  (%u frames)\n\n", total_mb, pmm_total_frames());
}

static void cmd_heapinfo(int argc, char *argv[])
{
    (void)argc; (void)argv;
    uint32_t free_b  = kheap_free_bytes();
    uint32_t used_b  = kheap_used_bytes();
    uint32_t blocks  = kheap_block_count();
    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    kprintf("\n  Kernel heap (0x200000 - 0x2FFFFF, 1 MB pool):\n");
    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    kprintf("    Free:   %u bytes  (%u KB)\n", free_b, free_b / 1024u);
    vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
    kprintf("    Used:   %u bytes  (%u KB)\n", used_b, used_b / 1024u);
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    kprintf("    Blocks: %u\n\n", blocks);
}

/* ---- netinfo ------------------------------------------------------------- */

static void cmd_netinfo(int argc, char *argv[])
{
    (void)argc; (void)argv;

    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    kprintf("\n  Network Configuration\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);

    if (!rtl8139_present()) {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        kprintf("  No RTL8139 detected\n");
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        kprintf("\n");
        return;
    }

    uint8_t mac[6];
    rtl8139_get_mac(mac);

    /* MAC */
    kprintf("  MAC     : %x:%x:%x:%x:%x:%x\n",
            (uint32_t)mac[0], (uint32_t)mac[1], (uint32_t)mac[2],
            (uint32_t)mac[3], (uint32_t)mac[4], (uint32_t)mac[5]);

    /* IPs (static SLIRP config) */
    uint32_t self = NET_IP_SELF;
    uint32_t gw   = NET_IP_GW;
    uint32_t dns  = NET_IP_DNS;
    uint32_t mask = NET_MASK;
    kprintf("  IP      : %u.%u.%u.%u\n",
            (self >> 24) & 0xFFu, (self >> 16) & 0xFFu,
            (self >>  8) & 0xFFu,  self        & 0xFFu);
    kprintf("  Mask    : %u.%u.%u.%u\n",
            (mask >> 24) & 0xFFu, (mask >> 16) & 0xFFu,
            (mask >>  8) & 0xFFu,  mask        & 0xFFu);
    kprintf("  Gateway : %u.%u.%u.%u\n",
            (gw >> 24) & 0xFFu, (gw >> 16) & 0xFFu,
            (gw >>  8) & 0xFFu,  gw        & 0xFFu);
    kprintf("  DNS     : %u.%u.%u.%u\n",
            (dns >> 24) & 0xFFu, (dns >> 16) & 0xFFu,
            (dns >>  8) & 0xFFu,  dns       & 0xFFu);

    /* Resolve gateway MAC from ARP cache */
    uint8_t gw_mac[6];
    if (arp_resolve(NET_IP_GW, gw_mac)) {
        vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
        kprintf("  GW MAC  : %x:%x:%x:%x:%x:%x  (ARP OK)\n",
                (uint32_t)gw_mac[0], (uint32_t)gw_mac[1],
                (uint32_t)gw_mac[2], (uint32_t)gw_mac[3],
                (uint32_t)gw_mac[4], (uint32_t)gw_mac[5]);
    } else {
        vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
        kprintf("  GW MAC  : unresolved\n");
    }
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    kprintf("\n");
}

/* ---- nettest ------------------------------------------------------------- */

#define ETH_ETYPE_ARP  0x0806u

static void cmd_nettest(int argc, char *argv[])
{
    (void)argc; (void)argv;

    if (!rtl8139_present()) {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        kprintf("  nettest: no RTL8139 detected\n");
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        return;
    }

    uint8_t mac[6];
    rtl8139_get_mac(mac);

    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    kprintf("\n  RTL8139 present\n");
    kprintf("  MAC: %x:%x:%x:%x:%x:%x\n",
            (uint32_t)mac[0], (uint32_t)mac[1], (uint32_t)mac[2],
            (uint32_t)mac[3], (uint32_t)mac[4], (uint32_t)mac[5]);
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);

    /* Build a gratuitous ARP request (opcode 1) as a broadcast.
     * This makes QEMU's SLIRP respond with an ARP reply, which proves
     * TX, ISR, and RX all work without needing an IP stack.             */
    uint8_t frame[42];
    int     i;
    for (i = 0; i < 42; i++) frame[i] = 0;

    /* -- Ethernet header -- */
    /* Destination: broadcast ff:ff:ff:ff:ff:ff */
    for (i = 0; i < 6; i++) frame[i] = 0xFFu;
    /* Source: our MAC */
    for (i = 0; i < 6; i++) frame[6 + i] = mac[i];
    /* EtherType: ARP (0x0806) big-endian */
    frame[12] = 0x08u;
    frame[13] = 0x06u;

    /* -- ARP payload (28 bytes at offset 14) -- */
    /* HTYPE = Ethernet (1) */
    frame[14] = 0x00u; frame[15] = 0x01u;
    /* PTYPE = IPv4 (0x0800) */
    frame[16] = 0x08u; frame[17] = 0x00u;
    /* HLEN = 6, PLEN = 4 */
    frame[18] = 6u; frame[19] = 4u;
    /* OPER = request (1) */
    frame[20] = 0x00u; frame[21] = 0x01u;
    /* SHA = sender hardware address (our MAC) */
    for (i = 0; i < 6; i++) frame[22 + i] = mac[i];
    /* SPA = sender protocol address: 10.0.2.15 (QEMU SLIRP default) */
    frame[28] = 10u; frame[29] = 0u; frame[30] = 2u; frame[31] = 15u;
    /* THA = target hardware address: zeros (unknown) */
    for (i = 0; i < 6; i++) frame[32 + i] = 0x00u;
    /* TPA = target protocol address: 10.0.2.2 (QEMU SLIRP gateway) */
    frame[38] = 10u; frame[39] = 0u; frame[40] = 2u; frame[41] = 2u;

    kprintf("  Sending ARP request to 10.0.2.2 (QEMU gateway)...\n");
    if (rtl8139_send(frame, 42) != 0) {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        kprintf("  TX failed\n");
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        return;
    }
    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    kprintf("  TX OK\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);

    /* Wait up to ~500 ms (50 ticks @ 100 Hz) for a reply */
    uint32_t deadline = pit_get_ticks() + 50u;
    uint8_t  rxbuf[NET_MTU];
    uint16_t rxlen = 0;
    int      got_reply = 0;

    while (pit_get_ticks() < deadline) {
        if (rtl8139_recv(rxbuf, &rxlen)) {
            got_reply = 1;
            break;
        }
    }

    if (!got_reply) {
        vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
        kprintf("  No reply received within 500 ms\n");
        kprintf("  (Add -netdev user,id=net0 -device rtl8139,netdev=net0 to QEMU for SLIRP)\n");
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        return;
    }

    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    kprintf("  RX OK  (%u bytes)\n", (uint32_t)rxlen);
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);

    /* Dump the first 28 bytes of the received frame in hex rows of 14 */
    kprintf("  Raw frame dump (first %u bytes):\n",
            rxlen < 28u ? (uint32_t)rxlen : 28u);
    uint16_t dump_len = rxlen < 28u ? rxlen : 28u;
    uint16_t j;
    for (j = 0; j < dump_len; j++) {
        if (j % 14u == 0) kprintf("    ");
        kprintf("%x ", (uint32_t)rxbuf[j]);
        if ((j + 1u) % 14u == 0) kprintf("\n");
    }
    if (dump_len % 14u != 0) kprintf("\n");

    /* Interpret EtherType */
    if (rxlen >= 14u) {
        uint16_t etype = ((uint16_t)rxbuf[12] << 8) | rxbuf[13];
        if (etype == ETH_ETYPE_ARP) {
            uint16_t op = ((uint16_t)rxbuf[20] << 8) | rxbuf[21];
            vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
            kprintf("  EtherType: ARP  opcode=%u (%s)\n",
                    (uint32_t)op,
                    op == 2u ? "reply" : op == 1u ? "request" : "?");
            if (op == 2u && rxlen >= 42u) {
                kprintf("  Replier MAC: %x:%x:%x:%x:%x:%x\n",
                        (uint32_t)rxbuf[22], (uint32_t)rxbuf[23],
                        (uint32_t)rxbuf[24], (uint32_t)rxbuf[25],
                        (uint32_t)rxbuf[26], (uint32_t)rxbuf[27]);
            }
            vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        } else {
            kprintf("  EtherType: 0x%x\n", (uint32_t)etype);
        }
    }
    kprintf("\n");
}

static void cmd_diskinfo(int argc, char *argv[])
{
    (void)argc; (void)argv;
    const ata_drive_t *drv = ata_get_drive();
    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    kprintf("\n  ATA Primary Master:\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    if (!drv->present) {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        kprintf("    No drive detected\n\n");
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        return;
    }
    kprintf("    Model:    %s\n", drv->model);
    kprintf("    Sectors:  %u\n", drv->total_sectors);
    kprintf("    Capacity: %u KB  (%u MB)\n",
            drv->total_sectors / 2u,
            drv->total_sectors / 2048u);
    kprintf("    LBA28:    %s\n", drv->lba_supported ? "yes" : "no");
    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    kprintf("\n  Disk layout:\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    kprintf("    LBA   0        Stage 1 bootloader\n");
    kprintf("    LBA   1-5      Stage 2 bootloader\n");
    kprintf("    LBA   6-133    Kernel image (64 KB window)\n");
    kprintf("    LBA  200-205   PDFS v2 metadata\n");
    kprintf("    LBA  206-2047  PDFS v2 data\n");
    kprintf("    LBA  2048-4095 FAT32 volume (/mnt/fat)\n");
    kprintf("    LBA  4096-69631 ext2 volume  (/mnt/ext2)\n");
    kprintf("    LBA  69632+    NTFS volume  (/mnt/ntfs, read-only)\n\n");
}

/* ---- Filesystem helpers --------------------------------------------------- */

/*
 * Build a canonical absolute path from `input` against the current `g_cwd`.
 * Supports:
 *   /absolute/path     — used directly
 *   ~                  — expands to /home/<username>
 *   ~/subdir           — expands to /home/<username>/subdir
 *   ..                 — walk up one level
 *   .                  — current directory (no-op)
 *   relative/name      — appended to g_cwd
 * Output written into `out` (must be at least 128 bytes).
 */
static void normalize_path(const char *input, char *out)
{
    char buf[128];
    int  len = 0;

    if (input[0] == '/') {
        /* Absolute path */
        buf[len++] = '/';
        input++;
    } else if (input[0] == '~') {
        /* Expand ~ to /home/<username> */
        const char *u = (g_session_user && g_session_user->username[0])
                        ? g_session_user->username : "user";
        buf[len++] = '/';
        const char *h = "home";
        while (*h && len < 120) buf[len++] = *h++;
        buf[len++] = '/';
        while (*u && len < 120) buf[len++] = *u++;
        buf[len] = '\0';
        input++;
        if (*input == '/') input++;
    } else {
        /* Relative — start from g_cwd */
        int ci = 0;
        while (g_cwd[ci] && len < 120) buf[len++] = g_cwd[ci++];
        buf[len] = '\0';
    }

    /* Process remaining components */
    while (*input) {
        while (*input == '/') input++;
        if (!*input) break;

        /* Extract next component */
        char comp[32]; int ci = 0;
        while (*input && *input != '/' && ci < 30) comp[ci++] = *input++;
        comp[ci] = '\0';

        if (ci == 2 && comp[0] == '.' && comp[1] == '.') {
            /* Go up — find last '/' */
            int last = 0, j;
            for (j = 0; j < len; j++) if (buf[j] == '/') last = j;
            if (last == 0) { buf[1] = '\0'; len = 1; }
            else           { buf[last] = '\0'; len = last; }
        } else if (ci == 1 && comp[0] == '.') {
            /* Current dir — skip */
        } else {
            /* Append /component */
            if (len == 0 || buf[len - 1] != '/') buf[len++] = '/';
            int j = 0;
            while (comp[j] && len < 126) buf[len++] = comp[j++];
            buf[len] = '\0';
        }
    }

    if (len == 0) { buf[0] = '/'; buf[1] = '\0'; len = 1; }
    int k; for (k = 0; k <= len && k < 127; k++) out[k] = buf[k];
    out[k] = '\0';
}

/* Thin wrapper kept for callers that don't need the full normalize_path name. */
static void make_path(char *out, const char *name) { normalize_path(name, out); }

/* Print s padded to `width` spaces (no libc needed). */
static void sh_pad(const char *s, int width)
{
    int len = 0;
    while (s[len]) { vga_putchar(s[len]); len++; }
    while (len < width) { vga_putchar(' '); len++; }
}

/* ---- FS commands ---------------------------------------------------------- */

/* Format Unix mode bits as rwxrwxrwx string into buf[10] (incl NUL). */
static void fmt_mode(uint16_t mode, char *buf)
{
    buf[0] = (mode & 0x100u) ? 'r' : '-';
    buf[1] = (mode & 0x080u) ? 'w' : '-';
    buf[2] = (mode & 0x040u) ? 'x' : '-';
    buf[3] = (mode & 0x020u) ? 'r' : '-';
    buf[4] = (mode & 0x010u) ? 'w' : '-';
    buf[5] = (mode & 0x008u) ? 'x' : '-';
    buf[6] = (mode & 0x004u) ? 'r' : '-';
    buf[7] = (mode & 0x002u) ? 'w' : '-';
    buf[8] = (mode & 0x001u) ? 'x' : '-';
    buf[9] = '\0';
}

static void cmd_list(int argc, char *argv[])
{
    uint32_t    count = 0;
    char        dir_path[128];

    /* Arg or fall back to CWD */
    if (argc >= 2)
        normalize_path(argv[1], dir_path);
    else {
        int ci = 0;
        while (g_cwd[ci] && ci < 127) { dir_path[ci] = g_cwd[ci]; ci++; }
        dir_path[ci] = '\0';
    }

    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    if (dir_path[0] == '/' && dir_path[1] == '\0')
        kprintf("\n  PDFS  /  (%u KB free)\n", pdfs_free_sectors() / 2u);
    else
        kprintf("\n  PDFS  %s  (%u KB free)\n", dir_path, pdfs_free_sectors() / 2u);
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    kprintf("  ");
    sh_pad("Name", 20);
    kprintf("  ");
    sh_pad("Mode", 10);
    kprintf("  Uid  Size\n");
    kprintf("  --------------------  ----------  ---  --------\n");

    /* Strip leading '/' for pdfs_stat_dir */
    const char *dp = dir_path;
    while (*dp == '/') dp++;

    pdfs_set_context(g_session_user, g_elevated);

    for (;;) {
        pdfs_dirent_t de;
        int r = pdfs_stat_dir(dp, count, &de);
        if (r == -4 && count == 0) {
            vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
            kprintf("  permission denied\n\n");
            vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
            return;
        }
        if (r != 0) break;
        char mbuf[10];
        fmt_mode(de.mode, mbuf);
        kprintf("  ");
        if (de.flags & PDFS_FLAG_DIR) vga_set_color(VGA_COLOR_LIGHT_CYAN,  VGA_COLOR_BLACK);
        else                          vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
        sh_pad(de.name, 20);
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        kprintf("  ");
        sh_pad(mbuf, 10);
        kprintf("  %u", (uint32_t)de.uid);
        if (de.flags & PDFS_FLAG_DIR) kprintf("    <DIR>\n");
        else                          kprintf("    %u bytes\n", de.size);
        count++;
    }

    if (count == 0) {
        vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
        kprintf("  (empty)\n");
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    } else {
        kprintf("  %u entr%s\n", count, count == 1 ? "y" : "ies");
    }
    kprintf("\n");
}

static void cmd_read(int argc, char *argv[])
{
    vfs_node_t node;
    char path[128];
    char *buf;
    int   r, i;

    if (argc < 2) { kprintf("  Usage: read <file>\n"); return; }

    make_path(path, argv[1]);
    if (vfs_open(path, &node) != 0) {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        kprintf("  read: '%s': file not found\n", argv[1]);
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        return;
    }
    if (node.size == 0) { kprintf("  (empty file)\n"); return; }

    /* Read up to 4 KB */
    {
        uint32_t to_read = node.size > 4096u ? 4096u : node.size;
        buf = (char *)kmalloc(to_read + 1u);
        if (!buf) { kprintf("  read: out of memory\n"); return; }

        r = vfs_read(&node, 0, to_read, buf);
        if (r < 0) {
            kprintf("  read: read error\n");
            kfree(buf);
            return;
        }
        buf[r] = '\0';
        kprintf("\n");
        for (i = 0; i < r; i++) vga_putchar(buf[i]);
        kprintf("\n");
        if (node.size > 4096u)
            kprintf("  ... (truncated at 4096 bytes)\n");
        kfree(buf);
    }
}

static void cmd_write(int argc, char *argv[])
{
    vfs_node_t node;
    char path[128];
    char content[256];
    uint32_t clen = 0;
    int i;

    if (argc < 2) { kprintf("  Usage: write <file> [text]\n"); return; }

    make_path(path, argv[1]);
    pdfs_set_context(g_session_user, g_elevated);

    /* Assemble content from remaining args */
    for (i = 2; i < argc; i++) {
        char *w = argv[i];
        while (*w && clen < 253u) content[clen++] = *w++;
        if (i < argc - 1 && clen < 253u) content[clen++] = ' ';
    }
    content[clen++] = '\n';
    content[clen]   = '\0';

    /* Open or create */
    if (vfs_open(path, &node) != 0) {
        if (vfs_create(path) != 0) {
            vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
            kprintf("  write: cannot create '%s'\n", argv[1]);
            vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
            return;
        }
        vfs_open(path, &node);
    }

    if (vfs_write(&node, 0, clen, content) < 0) {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        kprintf("  write: failed\n");
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    } else {
        kprintf("  Wrote %u bytes to '%s'\n", clen, argv[1]);
    }
}

static void cmd_delete(int argc, char *argv[])
{
    char path[128];
    if (argc < 2) { kprintf("  Usage: delete <file>\n"); return; }
    make_path(path, argv[1]);
    pdfs_set_context(g_session_user, g_elevated);
    int r = vfs_unlink(path);
    if (r == 0)
        kprintf("  Removed '%s'\n", argv[1]);
    else {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        if (r == -3)
            kprintf("  delete: '%s': permission denied\n", argv[1]);
        else
            kprintf("  delete: '%s': file not found\n", argv[1]);
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    }
}

static void cmd_factreset(int argc, char *argv[])
{
    char yn[4];
    (void)argc; (void)argv;

    /* Require elevated privileges */
    if (!g_elevated && !(g_session_user && (g_session_user->flags & USER_FLAG_ROOT))) {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        kprintf("  factreset: requires elevated privileges (use admin factreset)\n");
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        return;
    }

    /* Confirmation prompt */
    vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
    kprintf("\n  WARNING!!! This command will wipe all user data and factory\n");
    kprintf("  reset this install. Only root and pd users will be registered,\n");
    kprintf("  just like a new installation.\n\n");
    vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
    kprintf("  Are you sure you wish to proceed? (y/n): ");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);

    readline_masked(yn, sizeof(yn));
    if (yn[0] != 'y' && yn[0] != 'Y') {
        kprintf("  factreset: aborted.\n");
        return;
    }

    kprintf("  Formatting PDFS v3 at LBA 200... ");
    if (pdfs_format(200) != 0) {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        kprintf("FAILED\n");
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        return;
    }
    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    kprintf("done\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    kprintf("  Building filesystem structure... ");
    pdfs_scaffold();
    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    kprintf("done\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    kprintf("  Factory reset complete. Logging out...\n");
    g_logout = 1;
}

static void cmd_makedir(int argc, char *argv[])
{
    char path[128];
    if (argc < 2) { kprintf("  Usage: makedir <dir>\n"); return; }
    make_path(path, argv[1]);
    pdfs_set_context(g_session_user, g_elevated);
    uint8_t uid = g_session_user ? g_session_user->uid : 0u;
    if (pdfs_mkdir(path, uid, 0, 0) == 0)
        kprintf("  makedir: created '%s'\n", argv[1]);
    else {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        kprintf("  makedir: failed (exists, full, or read-only)\n");
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    }
}

static void cmd_setperm(int argc, char *argv[])
{
    char path[128];
    int  mode = 0;
    char *p;
    if (argc < 3) { kprintf("  Usage: setperm <file> <octal-mode>\n"); return; }
    /* Parse octal */
    for (p = argv[2]; *p >= '0' && *p <= '7'; p++) mode = mode * 8 + (*p - '0');
    make_path(path, argv[1]);
    pdfs_set_context(g_session_user, g_elevated);
    if (pdfs_chmod(path, (uint16_t)mode) == 0)
        kprintf("  setperm: mode set to 0%u\n", (uint32_t)mode);
    else {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        kprintf("  setperm: failed (not found or permission denied)\n");
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    }
}

/* Parse a uid/gid token: either a username looked up in the user table,
 * or a plain decimal number.  Returns the resolved id, or 0 if not found. */
static uint8_t resolve_id(const char *token)
{
    /* Try user table first */
    const user_t *u = users_get(token);
    if (u) return u->uid;
    /* Fall back to decimal */
    uint8_t v = 0;
    const char *p;
    for (p = token; *p >= '0' && *p <= '9'; p++) v = (uint8_t)(v * 10 + (*p - '0'));
    return v;
}

static void cmd_setowner(int argc, char *argv[])
{
    char path[128];
    char utoken[32], gtoken[32];
    uint8_t uid, gid;

    /* Accept:  seto <file> <user>:<group>
     *      or  seto <file> <user> <group>   */
    if (argc < 3) {
        kprintf("  Usage: setowner <file> <user>:<group>\n");
        kprintf("  e.g.:  setowner file.txt pd:pd\n");
        return;
    }

    if (argc == 3) {
        /* Single arg — split on ':' */
        char *colon = argv[2];
        uint32_t i = 0;
        while (colon[i] && colon[i] != ':') i++;
        if (colon[i] != ':') {
            kprintf("  Usage: setowner <file> <user>:<group>\n");
            return;
        }
        uint32_t k;
        for (k = 0; k < i && k < 31u; k++) utoken[k] = colon[k];
        utoken[k] = '\0';
        /* gtoken from after ':' */
        k = 0;
        const char *gp = colon + i + 1;
        while (*gp && k < 31u) gtoken[k++] = *gp++;
        gtoken[k] = '\0';
    } else {
        /* Two separate args */
        uint32_t k = 0;
        while (argv[2][k] && k < 31u) { utoken[k] = argv[2][k]; k++; }
        utoken[k] = '\0';
        k = 0;
        while (argv[3][k] && k < 31u) { gtoken[k] = argv[3][k]; k++; }
        gtoken[k] = '\0';
    }

    uid = resolve_id(utoken);
    gid = resolve_id(gtoken);

    make_path(path, argv[1]);
    pdfs_set_context(g_session_user, g_elevated);
    if (pdfs_chown(path, uid, gid) == 0)
        kprintf("  setowner: owner set to %s:%s\n", utoken, gtoken);
    else {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        kprintf("  setowner: failed (not found or permission denied)\n");
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    }
}

static void cmd_goto(int argc, char *argv[])
{
    char target[128];

    /* No arg → go to home directory */
    if (argc < 2) {
        normalize_path("~", target);
    } else {
        normalize_path(argv[1], target);
    }

    /* Access policy:
     *  - true root (uid 0)      : unrestricted
     *  - elevated admin         : unrestricted
     *  - everyone else          : only /home and subdirectories */
    int is_root_user      = (g_session_user && g_session_user->uid == 0);
    int is_elevated_admin = (g_elevated && g_session_user &&
                             (g_session_user->flags & USER_FLAG_ROOT));
    if (!is_root_user && !is_elevated_admin) {
        int inside_home = (target[0]=='/' && target[1]=='h' &&
                           target[2]=='o' && target[3]=='m' &&
                           target[4]=='e' &&
                           (target[5]=='\0' || target[5]=='/'));
        if (!inside_home) {
            vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
            kprintf("  goto: permission denied\n");
            vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
            return;
        }
    }

    /* Root dir shortcut - only reachable by root or elevated admins */
    if (target[0] == '/' && target[1] == '\0') {
        g_cwd[0] = '/'; g_cwd[1] = '\0';
        return;
    }

    /* If the target is a strict prefix of the current cwd (navigating up with
     * '..'), the directory is guaranteed to exist — skip the FS lookup and
     * commit directly.  Policy checks above already enforce /home boundaries. */
    {
        int tlen = 0, clen = 0;
        while (target[tlen]) tlen++;
        while (g_cwd[clen])  clen++;
        /* target must be shorter, match byte-for-byte, and cwd must have '/' next */
        if (tlen < clen && tlen > 0) {
            int match = 1, ki;
            for (ki = 0; ki < tlen; ki++) {
                if (target[ki] != g_cwd[ki]) { match = 0; break; }
            }
            if (match && (g_cwd[tlen] == '/' || g_cwd[tlen] == '\0')) {
                int i = 0;
                while (target[i] && i < 127) { g_cwd[i] = target[i]; i++; }
                g_cwd[i] = '\0';
                return;
            }
        }
    }

    /* Verify the target exists and is a directory */
    pdfs_set_context(g_session_user, g_elevated);
    vfs_node_t node;
    int r = vfs_open(target, &node);
    if (r == -4) {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        kprintf("  goto: '%s': permission denied\n",
                argc >= 2 ? argv[1] : "~");
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        return;
    }
    if (r != 0 || !node.is_dir) {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        kprintf("  goto: '%s': no such directory\n",
                argc >= 2 ? argv[1] : "~");
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        return;
    }

    /* Commit */
    int i = 0;
    while (target[i] && i < 127) { g_cwd[i] = target[i]; i++; }
    g_cwd[i] = '\0';
}

static void cmd_copy(int argc, char *argv[])
{
    char src[128], dst[128];
    vfs_node_t src_node, dst_node;
    char *buf;
    int r;

    if (argc < 3) { kprintf("  Usage: copy <src> <dst>\n"); return; }

    normalize_path(argv[1], src);
    normalize_path(argv[2], dst);

    if (vfs_open(src, &src_node) != 0) {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        kprintf("  copy: '%s': not found\n", argv[1]);
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        return;
    }
    if (src_node.is_dir) {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        kprintf("  copy: '%s' is a directory\n", argv[1]);
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        return;
    }

    pdfs_set_context(g_session_user, g_elevated);

    /* Allocate read buffer */
    uint32_t sz = src_node.size;
    if (sz == 0) sz = 1;
    buf = (char *)kmalloc(sz);
    if (!buf) { kprintf("  copy: out of memory\n"); return; }

    r = (sz > 1 || src_node.size > 0) ? vfs_read(&src_node, 0, src_node.size, buf) : 0;
    if (r < 0) {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        kprintf("  copy: read error\n");
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        kfree(buf);
        return;
    }

    /* Create dst if it doesn't exist */
    if (vfs_open(dst, &dst_node) != 0) {
        if (vfs_create(dst) != 0) {
            vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
            kprintf("  copy: cannot create '%s'\n", argv[2]);
            vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
            kfree(buf);
            return;
        }
        vfs_open(dst, &dst_node);
    }

    if (src_node.size > 0) {
        if (vfs_write(&dst_node, 0, (uint32_t)r, buf) < 0) {
            vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
            kprintf("  copy: write failed\n");
            vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
            kfree(buf);
            return;
        }
    }
    kprintf("  Copied %u bytes: '%s' -> '%s'\n", (uint32_t)r, argv[1], argv[2]);
    kfree(buf);
}

static void cmd_move(int argc, char *argv[])
{
    char src[128], dst[128];
    vfs_node_t src_node, dst_node;
    char *buf;
    int r;

    if (argc < 3) { kprintf("  Usage: move <src> <dst>\n"); return; }

    normalize_path(argv[1], src);
    normalize_path(argv[2], dst);

    if (vfs_open(src, &src_node) != 0) {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        kprintf("  move: '%s': not found\n", argv[1]);
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        return;
    }
    if (src_node.is_dir) {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        kprintf("  move: '%s' is a directory\n", argv[1]);
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        return;
    }

    pdfs_set_context(g_session_user, g_elevated);

    uint32_t sz = src_node.size;
    if (sz == 0) sz = 1;
    buf = (char *)kmalloc(sz);
    if (!buf) { kprintf("  move: out of memory\n"); return; }

    r = (src_node.size > 0) ? vfs_read(&src_node, 0, src_node.size, buf) : 0;
    if (r < 0) {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        kprintf("  move: read error\n");
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        kfree(buf);
        return;
    }

    /* Create dst */
    if (vfs_open(dst, &dst_node) != 0) {
        if (vfs_create(dst) != 0) {
            vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
            kprintf("  move: cannot create '%s'\n", argv[2]);
            vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
            kfree(buf);
            return;
        }
        vfs_open(dst, &dst_node);
    }

    if (src_node.size > 0) {
        if (vfs_write(&dst_node, 0, (uint32_t)r, buf) < 0) {
            vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
            kprintf("  move: write failed\n");
            vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
            kfree(buf);
            return;
        }
    }
    kfree(buf);

    /* Remove source */
    if (vfs_unlink(src) != 0) {
        vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
        kprintf("  move: copied but could not remove source '%s'\n", argv[1]);
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        return;
    }
    kprintf("  Moved '%s' -> '%s'\n", argv[1], argv[2]);
}

static void cmd_logout(int argc, char *argv[])
{
    (void)argc; (void)argv;
    kprintf("  Logging out...\n");
    g_logout = 1;
}

static void cmd_shutdown(int argc, char *argv[])
{
    (void)argc; (void)argv;
    kprintf("  Shutting down...\n");
    /* ACPI shutdown (QEMU default ACPI PM1a control port) */
    __asm__ volatile (
        "cli\n"
        "outw %w0, %w1\n"   /* QEMU ACPI: port 0x604, value 0x2000 */
        : : "a"((uint16_t)0x2000), "Nd"((uint16_t)0x604)
    );
    /* Fallback: older Bochs/QEMU ISA ACPI port */
    __asm__ volatile (
        "outw %w0, %w1\n"
        : : "a"((uint16_t)0x2000), "Nd"((uint16_t)0xB004)
    );
    /* Last resort: halt */
    __asm__ volatile ("1: hlt\njmp 1b\n");
}

static void cmd_admin(int argc, char *argv[]);   /* defined after commands[] */

/* ---- Alias table ---------------------------------------------------------- */

typedef struct {
    const char *alias;
    const char *native;
} alias_t;

/* Flat table — used by the shell dispatcher */
static const alias_t aliases[] = {
    { "ls",     "list"     },
    { "dir",    "list"     },
    { "cat",    "read"     },
    { "type",   "read"     },
    { "rm",     "delete"   },
    { "del",    "delete"   },
    { "erase",  "delete"   },
    { "mkdir",  "makedir"  },
    { "md",     "makedir"  },
    { "cd",     "goto"     },
    { "mv",     "move"     },
    { "ren",    "move"     },
    { "rename", "move"     },
    { "echo",   "print"    },
    { "chmod",  "setperm"  },
    { "chown",  "setowner" },
    { "sudo",   "admin"    },
    { "runas",  "admin"    },
    { "cls",     "clear"    },
    { "exit",    "logout"   },
    { "ver",     "version"  },
    { "useradd", "adduser"  },
    { "userdel", "deluser"  },
    { NULL,      NULL       }
};

/* Grouped table — used only for display */
typedef struct {
    const char *aliases_str;  /* comma-separated aliases */
    const char *native;
    const char *desc;
} alias_group_t;

static const alias_group_t alias_groups[] = {
    { "ls, dir",         "list",     "List files in the current directory"       },
    { "cat, type",       "read",     "Print the contents of a file"              },
    { "rm, del, erase",  "delete",   "Delete a file"                             },
    { "mkdir, md",       "makedir",  "Create a subdirectory"                     },
    { "cd",              "goto",     "Change the current directory"              },
    { "mv, ren, rename", "move",     "Move or rename a file"                     },
    { "echo",            "print",    "Print text to the screen"                  },
    { "chmod",           "setperm",  "Set file permissions (octal mode)"         },
    { "chown",           "setowner", "Set file owner"                            },
    { "sudo, runas",     "admin",    "Run a command with admin privileges"       },
    { "cls",             "clear",    "Clear the screen"                          },
    { "exit",            "logout",   "Log out and return to the login screen"    },
    { "ver",             "version",  "Show kernel and shell version"             },
    { "useradd",         "adduser",  "Create a new user account"                 },
    { "userdel",         "deluser",  "Delete a user account"                     },
    { NULL,              NULL,       NULL                                         }
};

/* ---- Alias row: same column geometry as help_row -------------------- */
#define ALIAS_LEFT_W  28   /* space for "aliases  ->  native" (after 4-space indent) */
#define ALIAS_DESC_COL 34  /* 4 + 28 + 2-gap */
#define ALIAS_DESC_W  46   /* 80 - 34 */

static void alias_row(const alias_group_t *g)
{
    int i;
    /* 4-space indent, then yellow aliases */
    for (i = 0; i < 4; i++) vga_putchar(' ');
    vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
    int left = 0;
    const char *a = g->aliases_str;
    while (*a) { vga_putchar(*a); a++; left++; }
    /* "  ->  " separator */
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    kprintf("  ->  "); left += 6;
    /* native in light green */
    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    const char *n = g->native;
    while (*n) { vga_putchar(*n); n++; left++; }
    /* Pad to ALIAS_LEFT_W then 2-space gap */
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    for (i = left; i < ALIAS_LEFT_W; i++) vga_putchar(' ');
    vga_putchar(' '); vga_putchar(' ');

    /* Description with word-wrap (same logic as help_row) */
    const char *p = g->desc;
    int col = 0;
    while (*p) {
        int had_space = 0;
        while (*p == ' ') { p++; had_space = 1; }
        if (!*p) break;
        int wlen = 0;
        while (p[wlen] && p[wlen] != ' ') wlen++;
        int need = (col == 0) ? wlen : col + 1 + wlen;
        if (need > ALIAS_DESC_W && col > 0) {
            vga_putchar('\n');
            for (i = 0; i < ALIAS_DESC_COL; i++) vga_putchar(' ');
            col = 0;
        } else if (had_space && col > 0) {
            vga_putchar(' '); col++;
        }
        for (i = 0; i < wlen; i++) { vga_putchar(p[i]); col++; }
        p += wlen;
    }
    vga_putchar('\n');
}

static void cmd_alias(int argc, char *argv[])
{
    int i;
    if (argc >= 2) {
        /* Look up a single alias */
        for (i = 0; aliases[i].alias != NULL; i++) {
            if (sh_strcmp(argv[1], aliases[i].alias) == 0) {
                vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
                kprintf("  %s", aliases[i].alias);
                vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
                kprintf("  ->  ");
                vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
                kprintf("%s\n", aliases[i].native);
                vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
                return;
            }
        }
        vga_set_color(VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK);
        kprintf("  '%s' is not an alias\n", argv[1]);
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        return;
    }
    /* Print formatted table */
    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    kprintf("\n  Aliases (Linux/Windows synonyms for PD-OS commands):\n\n");
    for (i = 0; alias_groups[i].aliases_str != NULL; i++)
        alias_row(&alias_groups[i]);
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    kprintf("\n");
}

/* ---- ps / kill ----------------------------------------------------------- */

static void ps_pad(const char *s, int width)
{
    int n = 0;
    while (s[n] && n < width) { vga_putchar(s[n]); n++; }
    while (n < width) { vga_putchar(' '); n++; }
}

static void ps_pad_uint(uint32_t v, int width)
{
    char tmp[12];
    int n = 0, i;
    if (v == 0) { tmp[n++] = '0'; }
    else { uint32_t x = v; while (x) { tmp[n++] = '0' + (x % 10); x /= 10; } }
    /* reverse */
    for (i = 0; i < n / 2; i++) { char c = tmp[i]; tmp[i] = tmp[n-1-i]; tmp[n-1-i] = c; }
    tmp[n] = '\0';
    ps_pad(tmp, width);
}

static void cmd_ps(int argc, char *argv[])
{
    int i;
    static const char * const state_names[] = {
        "unused", "runnable", "running", "dead"
    };
    (void)argc; (void)argv;
    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    kprintf("  "); ps_pad("PID",   4); ps_pad("STATE",    11);
                   ps_pad("TICKS", 9); kprintf("NAME\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    for (i = 0; i < PROC_MAX; i++) {
        pcb_t *p = proc_get_slot(i);
        if (!p || p->state == PROC_UNUSED) continue;
        if (p->state == PROC_RUNNING)
            vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
        else if (p->state == PROC_DEAD)
            vga_set_color(VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK);
        else
            vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        kprintf("  ");
        ps_pad_uint(p->pid,         4);
        ps_pad(state_names[p->state], 11);
        ps_pad_uint(p->ticks_total, 9);
        kprintf("%s\n", p->name);
    }
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
}

static void cmd_kill(int argc, char *argv[])
{
    uint32_t pid;
    int i;
    if (argc < 2) {
        kprintf("  Usage: kill <pid>\n");
        return;
    }
    pid = 0;
    for (i = 0; argv[1][i] >= '0' && argv[1][i] <= '9'; i++)
        pid = pid * 10 + (uint32_t)(argv[1][i] - '0');
    if (proc_kill(pid) == 0) {
        vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
        kprintf("  Process %u terminated.\n", pid);
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    } else {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        kprintf("  kill: no such process (pid %u)\n", pid);
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    }
}

/* ---- adduser / deluser ---------------------------------------------------- */

static void cmd_adduser(int argc, char *argv[])
{
    const user_t *nu;
    uint8_t is_admin;
    int r;

    if (argc < 4) {
        kprintf("  Usage: adduser <username> <admin|regular> <password>\n");
        return;
    }

    is_admin = (sh_strcmp(argv[2], "admin") == 0) ? 1u : 0u;

    /* Creating an admin account always requires elevation */
    if (is_admin &&
        !g_elevated &&
        !(g_session_user && (g_session_user->flags & USER_FLAG_ROOT))) {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        kprintf("  adduser: adding an admin requires elevated privileges\n");
        kprintf("           use: admin adduser %s admin <password>\n", argv[1]);
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        return;
    }

    r = users_add(argv[1], is_admin, argv[3]);
    if (r == -1) {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        kprintf("  adduser: user table is full\n");
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        return;
    }
    if (r == -2) {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        kprintf("  adduser: invalid username\n");
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        return;
    }
    if (r == -3) {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        kprintf("  adduser: user '%s' already exists\n", argv[1]);
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        return;
    }
    if (r != 0) {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        kprintf("  adduser: failed\n");
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        return;
    }

    /* Create home directory owned by the new user */
    nu = users_get(argv[1]);
    if (nu)
        pdfs_create_home(argv[1], nu->uid);

    /* Persist credentials to /usr/<uid>.pduc */
    if (users_save_to_disk(argv[1]) != 0) {
        vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
        kprintf("  adduser: warning: could not save credentials to disk\n");
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    }

    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    kprintf("  adduser: user '%s' created (%s)\n",
            argv[1], is_admin ? "admin" : "regular");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
}

static void cmd_deluser(int argc, char *argv[])
{
    int r;

    if (argc < 2) {
        kprintf("  Usage: deluser <username>\n");
        return;
    }

    /* Requires elevation (run via admin deluser ...) */
    if (!g_elevated &&
        !(g_session_user && (g_session_user->flags & USER_FLAG_ROOT))) {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        kprintf("  deluser: requires elevated privileges (use admin deluser <username>)\n");
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        return;
    }

    /* Cannot delete the root account */
    if (sh_strcmp(argv[1], "root") == 0) {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        kprintf("  deluser: cannot delete the root account\n");
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        return;
    }

    /* Cannot delete the currently logged-in user */
    if (g_session_user && sh_strcmp(g_session_user->username, argv[1]) == 0) {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        kprintf("  deluser: cannot delete the currently logged-in user\n");
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        return;
    }

    if (!users_get(argv[1])) {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        kprintf("  deluser: user '%s' not found\n", argv[1]);
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        return;
    }

    r = users_remove(argv[1]);
    if (r == -2) {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        kprintf("  deluser: cannot delete the root account\n");
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        return;
    }
    if (r != 0) {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        kprintf("  deluser: failed to remove '%s'\n", argv[1]);
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        return;
    }

    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    kprintf("  deluser: user '%s' removed\n", argv[1]);
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
}

/* ---- changerpass / changempass -------------------------------------------- */
/*
 * changerpass: change the ROOT account password.
 *   - Must be run via admin/sudo (g_elevated required).
 *   - Asks for the CURRENT root password again internally, regardless of who
 *     is calling — no other password is accepted.
 *   - After success, if the session user is root, forces an immediate logout.
 *
 * changempass: change the calling user's OWN password.
 *   - Any logged-in user may run this (no elevation needed).
 *   - Asks for the caller's current password for verification.
 *   - Forces logout after success to require a fresh login.
 */

static void cmd_changerpass(int argc, char *argv[])
{
    char cur[64], np1[64], np2[64];
    (void)argc; (void)argv;

    /* Must be run under admin/sudo (g_elevated) */
    if (!g_elevated) {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        kprintf("  changerpass: must be run with admin/sudo\n");
        kprintf("               e.g. admin changerpass\n");
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        return;
    }

    /* Re-authenticate with the ROOT password — no other password accepted */
    vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
    kprintf("  [changerpass] current root password: ");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    readline_masked(cur, sizeof(cur));

    if (!users_verify("root", cur)) {
        sh_memset(cur, 0, (int)sizeof(cur));
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        kprintf("  changerpass: authentication failure\n");
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        return;
    }
    sh_memset(cur, 0, (int)sizeof(cur));

    /* New password (with confirmation) */
    vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
    kprintf("  [changerpass] new root password: ");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    readline_masked(np1, sizeof(np1));

    vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
    kprintf("  [changerpass] confirm new password: ");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    readline_masked(np2, sizeof(np2));

    /* Compare np1 == np2 */
    {
        int i = 0;
        while (np1[i] && np1[i] == np2[i]) i++;
        if (np1[i] != np2[i]) {
            sh_memset(np1, 0, (int)sizeof(np1));
            sh_memset(np2, 0, (int)sizeof(np2));
            vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
            kprintf("  changerpass: passwords do not match\n");
            vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
            return;
        }
    }

    users_change_password("root", np1);
    sh_memset(np1, 0, (int)sizeof(np1));
    sh_memset(np2, 0, (int)sizeof(np2));

    /* Persist the new root password hash to /usr/0.pduc */
    if (users_save_to_disk("root") != 0) {
        vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
        kprintf("  changerpass: warning: password changed in memory but disk save failed\n");
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    }

    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    kprintf("  changerpass: root password changed successfully\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);

    /* Force logout if the current session is root */
    if (g_session_user && (g_session_user->flags & USER_FLAG_ROOT)) {
        kprintf("  Security: logging out to enforce re-authentication...\n");
        g_logout = 1;
    }
}

static void cmd_changempass(int argc, char *argv[])
{
    char cur[64], np1[64], np2[64];
    (void)argc; (void)argv;

    if (!g_session_user) {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        kprintf("  changempass: no active session\n");
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        return;
    }

    /* Verify current password of the calling user */
    vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
    kprintf("  [changempass] current password for %s: ",
            g_session_user->username);
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    readline_masked(cur, sizeof(cur));

    if (!users_verify(g_session_user->username, cur)) {
        sh_memset(cur, 0, (int)sizeof(cur));
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        kprintf("  changempass: authentication failure\n");
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        return;
    }
    sh_memset(cur, 0, (int)sizeof(cur));

    /* New password (with confirmation) */
    vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
    kprintf("  [changempass] new password: ");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    readline_masked(np1, sizeof(np1));

    vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
    kprintf("  [changempass] confirm new password: ");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    readline_masked(np2, sizeof(np2));

    /* Compare np1 == np2 */
    {
        int i = 0;
        while (np1[i] && np1[i] == np2[i]) i++;
        if (np1[i] != np2[i]) {
            sh_memset(np1, 0, (int)sizeof(np1));
            sh_memset(np2, 0, (int)sizeof(np2));
            vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
            kprintf("  changempass: passwords do not match\n");
            vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
            return;
        }
    }

    users_change_password(g_session_user->username, np1);
    sh_memset(np1, 0, (int)sizeof(np1));
    sh_memset(np2, 0, (int)sizeof(np2));

    /* Persist the new password hash to disk */
    if (users_save_to_disk(g_session_user->username) != 0) {
        vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
        kprintf("  changempass: warning: password changed in memory but disk save failed\n");
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    }

    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    kprintf("  changempass: password changed. Logging out for security...\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    g_logout = 1;
}

/* ---- install -------------------------------------------------------------- */

static void cmd_install(int argc, char *argv[])
{
    (void)argc; (void)argv;

    /* Requires root or elevated privileges */
    if (!g_elevated &&
        !(g_session_user && (g_session_user->flags & USER_FLAG_ROOT))) {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        kprintf("  install: requires elevated privileges\n");
        kprintf("           use: admin install\n");
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        return;
    }

    install_wizard();
}

/* ---- Command table -------------------------------------------------------- */

typedef struct {
    const char *name;
    void (*fn)(int argc, char *argv[]);
} command_t;

static const command_t commands[] = {
    { "help",     cmd_help     },
    { "clear",    cmd_clear    },
    { "print",    cmd_print    },
    { "version",  cmd_version  },
    { "uptime",   cmd_uptime   },
    { "color",    cmd_color    },
    { "whoami",   cmd_whoami   },
    { "rammap",   cmd_rammap   },
    { "raminfo",  cmd_raminfo  },
    { "heapinfo", cmd_heapinfo },
    { "diskinfo", cmd_diskinfo },
    { "nettest",  cmd_nettest  },
    { "netinfo",  cmd_netinfo  },
    { "list",     cmd_list     },
    { "read",     cmd_read     },
    { "write",    cmd_write    },
    { "delete",   cmd_delete   },
    { "factreset", cmd_factreset },
    { "makedir",  cmd_makedir  },
    { "setperm",  cmd_setperm  },
    { "setowner", cmd_setowner },
    { "goto",     cmd_goto     },
    { "copy",     cmd_copy     },
    { "move",     cmd_move     },
    { "admin",    cmd_admin    },
    { "alias",    cmd_alias    },
    { "logout",   cmd_logout   },
    { "reboot",   cmd_reboot   },
    { "shutdown", cmd_shutdown },
    { "ps",       cmd_ps       },
    { "kill",     cmd_kill     },
    { "adduser",     cmd_adduser     },
    { "deluser",     cmd_deluser     },
    { "changerpass", cmd_changerpass },
    { "changempass", cmd_changempass },
    { "install",     cmd_install     },
    { NULL,          NULL            }
};

/* ---- elev: privileged command dispatch ------------------------------------ */

static void cmd_admin(int argc, char *argv[])
{
    char pwd[64];
    int  i, found;

    if (argc < 2) {
        kprintf("  Usage: admin <command> [args...]\n");
        return;
    }

    /* Prevent recursion */
    if (sh_strcmp(argv[1], "admin") == 0) {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        kprintf("  admin: cannot elevate admin\n");
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        return;
    }

    /* Root users skip re-authentication */
    if (!(g_session_user && (g_session_user->flags & USER_FLAG_ROOT))) {
        vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
        kprintf("  [admin] root password: ");
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        readline_masked(pwd, sizeof(pwd));

        if (!users_verify("root", pwd)) {
            sh_memset(pwd, 0, (int)sizeof(pwd));
            vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
            kprintf("  admin: authentication failure\n");
            vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
            return;
        }
        sh_memset(pwd, 0, (int)sizeof(pwd));
    }

    g_elevated = 1;
    found = 0;
    for (i = 0; commands[i].name != NULL; i++) {
        if (sh_strcmp(argv[1], commands[i].name) == 0) {
            commands[i].fn(argc - 1, argv + 1);
            found = 1;
            break;
        }
    }
    g_elevated = 0;

    if (!found) {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        kprintf("  admin: unknown command: %s\n", argv[1]);
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    }
}

/* ---- Shell banner --------------------------------------------------------- */

static void shell_banner(void)
{
    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    kprintf("===============================================================================\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    kprintf("               PD-Shell  v0.1  -  Type 'help' for commands\n");
    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    kprintf("===============================================================================\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    kprintf("\n");
}

/* ---- Main shell loop ------------------------------------------------------ */

void shell_run(const user_t *user)
{
    char line[SHELL_BUF_SIZE];
    char *argv[SHELL_MAX_ARGS];

    g_session_user = user;
    g_logout       = 0;

    /* Initial working directory:
     *   root  (uid 0)  → / (literal filesystem root)
     *   others         → /home/<username> */
    if (user->uid == 0) {
        g_cwd[0] = '/'; g_cwd[1] = '\0';
    } else {
        const char *pfx = "/home/";
        int i = 0;
        while (*pfx && i < 120) g_cwd[i++] = *pfx++;
        int j = 0;
        while (user->username[j] && i < 126) g_cwd[i++] = user->username[j++];
        g_cwd[i] = '\0';
    }

    vga_clear();
    shell_banner();

    for (;;) {
        /* Prompt: username@pd-shell:/cwd> */
        vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
        kprintf("%s", user->username);
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        kprintf("@pd-shell:");
        vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
        kprintf("%s", g_cwd);
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        kprintf("> ");

        int n = readline(line, SHELL_BUF_SIZE);

        if (n == 0) continue;   /* blank line */
        hist_push(line);

        int argc = tokenize(line, argv, SHELL_MAX_ARGS);
        if (argc == 0) continue;

        /* Look up command */
        int i;
        int found = 0;
        for (i = 0; commands[i].name != NULL; i++) {
            if (sh_strcmp(argv[0], commands[i].name) == 0) {
                commands[i].fn(argc, argv);
                found = 1;
                break;
            }
        }

        if (g_logout) return;

        if (!found) {
            /* Check alias table */
            int ai;
            for (ai = 0; aliases[ai].alias != NULL; ai++) {
                if (sh_strcmp(argv[0], aliases[ai].alias) == 0) {
                    /* Print hint then run the native command */
                    vga_set_color(VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK);
                    kprintf("  (alias for '%s')\n", aliases[ai].native);
                    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
                    int ci;
                    for (ci = 0; commands[ci].name != NULL; ci++) {
                        if (sh_strcmp(aliases[ai].native, commands[ci].name) == 0) {
                            commands[ci].fn(argc, argv);
                            found = 1;
                            break;
                        }
                    }
                    break;
                }
            }
        }

        if (g_logout) return;

        if (!found) {
            vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
            kprintf("  Unknown command: ");
            vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
            kprintf("%s", argv[0]);
            kprintf("  (type 'help' for commands)\n");
        }
    }
}

/* ---- shell_exec_line ------------------------------------------------------ */

void shell_exec_line(const user_t *user, const char *line_in)
{
    char  line[SHELL_BUF_SIZE];
    char *argv[SHELL_MAX_ARGS];
    int   i, n;

    /* Copy into mutable buffer */
    for (n = 0; line_in[n] && n < SHELL_BUF_SIZE - 1; n++)
        line[n] = line_in[n];
    line[n] = '\0';

    g_session_user = user;

    int argc = tokenize(line, argv, SHELL_MAX_ARGS);
    if (argc == 0) return;

    for (i = 0; commands[i].name != NULL; i++) {
        if (sh_strcmp(argv[0], commands[i].name) == 0) {
            commands[i].fn(argc, argv);
            return;
        }
    }
    kprintf("Unknown command: %s\n", argv[0]);;
}
