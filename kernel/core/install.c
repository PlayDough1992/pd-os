/* ============================================================================
 * PD-OS  —  Installation Wizard
 * ============================================================================
 * Full-screen VGA TUI that copies the running system to a selected ATA drive.
 *
 * Wizard pages:
 *   1. Welcome / overview
 *   2. Drive selection  (UP/DOWN + ENTER to pick, ESC to cancel)
 *   3. Confirmation     (ENTER to proceed, ESC to go back)
 *   4. Progress         (live sector copy + progress bar)
 *   5. Complete         (remove media, ENTER to reboot)
 *
 * Screen layout (80×25 VGA):
 *   Row  0   ╔══ PD-OS INSTALLER ══╗  (title bar)
 *   Row  1   ║  PD-OS Installer   Step N/4  ║  (subtitle)
 *   Row  2   ╠══════════════════════════════╣  (divider)
 *   Rows 3-20  Content area (18 rows, 78 inner cols)
 *   Row 21   ╠══════════════════════════════╣  (divider)
 *   Row 22   ║  [ENTER] Action    [ESC] ...  ║  (navigation)
 *   Row 23   ╚══════════════════════════════╝  (bottom border)
 *   Row 24   (blank)
 * ============================================================================ */

#include "install.h"
#include "kernel.h"
#include "vga.h"
#include "keyboard.h"
#include "ata.h"

/* ---- CP437 box-drawing characters ---------------------------------------- */
#define BX_TL  '\xC9'   /* ╔ */
#define BX_TR  '\xBB'   /* ╗ */
#define BX_BL  '\xC8'   /* ╚ */
#define BX_BR  '\xBC'   /* ╝ */
#define BX_H   '\xCD'   /* ═ */
#define BX_V   '\xBA'   /* ║ */
#define BX_ML  '\xCC'   /* ╠ */
#define BX_MR  '\xB9'   /* ╣ */
#define BX_BAR '\xDB'   /* █  full block  (progress filled) */
#define BX_SHD '\xB0'   /* ░  light shade (progress empty)  */
#define BX_BUL '\xFB'   /* √  used as list bullet */

/* ---- Layout constants ----------------------------------------------------- */
#define W_INNER   78    /* columns between the two BX_V border chars (cols 1-78) */
#define W_TOTAL   80    /* total row width */
#define W_H       25    /* total rows */

#define ROW_TOP    0    /* title bar */
#define ROW_SUB    1    /* step indicator */
#define ROW_DIV1   2    /* top divider */
#define ROW_C0     3    /* first content row */
#define ROW_C_LAST 20   /* last content row */
#define ROW_DIV2   21   /* bottom divider */
#define ROW_NAV    22   /* navigation hints */
#define ROW_BOT    23   /* bottom border */

/* ---- Sector-copy constants ------------------------------------------------ */
#define INST_BOOT_SECS  1024u   /* LBA 0-1023: bootloader + kernel to copy     */
#define INST_CHUNK         8u   /* sectors per copy iteration (4 KB)           */
#define INST_PDFS_ZERO     8u   /* sectors to zero at LBA 1024 for fresh PDFS  */

/* ---- Colors --------------------------------------------------------------- */
#define C_FR_FG   VGA_COLOR_WHITE
#define C_FR_BG   VGA_COLOR_BLUE
#define C_TITLE   VGA_COLOR_YELLOW
#define C_CT_FG   VGA_COLOR_WHITE
#define C_CT_BG   VGA_COLOR_BLACK
#define C_WARN    VGA_COLOR_LIGHT_RED
#define C_OK      VGA_COLOR_LIGHT_GREEN
#define C_DIM     VGA_COLOR_DARK_GREY
#define C_SEL_FG  VGA_COLOR_BLACK
#define C_SEL_BG  VGA_COLOR_LIGHT_CYAN

/* ---- Static buffers ------------------------------------------------------- */
static uint16_t g_saved_screen[W_TOTAL * W_H];   /* pre-wizard VGA snapshot */
static uint8_t  g_copy_buf[INST_CHUNK * 512u];   /* sector copy scratch     */
static uint8_t  g_zero_buf[512u];                 /* zeroed sector for PDFS  */

/* ============================================================================
 * String / integer helpers
 * ========================================================================== */

static int wiz_strlen(const char *s)
{
    int n = 0;
    while (s[n]) n++;
    return n;
}

/* Write v as decimal into out[]; returns digit count. */
static int wiz_fmtd(char *out, uint32_t v)
{
    char tmp[12];
    int  n = 0, i;
    if (v == 0) { tmp[n++] = '0'; }
    else { while (v) { tmp[n++] = (char)('0' + v % 10); v /= 10; } }
    for (i = 0; i < n / 2; i++) { char t = tmp[i]; tmp[i] = tmp[n-1-i]; tmp[n-1-i] = t; }
    tmp[n] = '\0';
    for (i = 0; i <= n; i++) out[i] = tmp[i];
    return n;
}

/* Format sector count as "NNN MB" or "NNN KB". */
static void wiz_fmt_size(char *out, uint32_t sectors)
{
    uint32_t mb = sectors / 2048u;
    int len;
    if (mb == 0) {
        uint32_t kb = sectors / 2u;
        len = wiz_fmtd(out, kb);
        out[len] = ' '; out[len+1] = 'K'; out[len+2] = 'B'; out[len+3] = '\0';
    } else {
        len = wiz_fmtd(out, mb);
        out[len] = ' '; out[len+1] = 'M'; out[len+2] = 'B'; out[len+3] = '\0';
    }
}

/* ============================================================================
 * Low-level VGA draw helpers
 * ========================================================================== */

static void wiz_puts(const char *s, vga_color_t fg, vga_color_t bg)
{
    vga_set_color(fg, bg);
    while (*s) vga_putchar(*s++);
}

/* Draw a full-width box horizontal line at `row` with given caps. */
static void wiz_hline(int row, char lcap, char rcap)
{
    int i;
    vga_set_cursor(0, (uint8_t)row);
    vga_set_color(C_FR_FG, C_FR_BG);
    vga_putchar(lcap);
    for (i = 0; i < W_INNER; i++) vga_putchar(BX_H);
    vga_putchar(rcap);
}

/*
 * Draw a content row: BX_V at col 0, fill inner 78 cols with spaces on
 * C_CT_FG/C_CT_BG, BX_V at col 79.
 */
static void wiz_blank_row(int row)
{
    int i;
    vga_set_cursor(0, (uint8_t)row);
    vga_set_color(C_FR_FG, C_FR_BG);
    vga_putchar(BX_V);
    vga_set_color(C_CT_FG, C_CT_BG);
    for (i = 0; i < W_INNER; i++) vga_putchar(' ');
    vga_set_color(C_FR_FG, C_FR_BG);
    vga_putchar(BX_V);
}

/*
 * Draw text left+right aligned inside a bordered row.
 * left_len + right_len MUST be <= W_INNER.
 */
static void wiz_row_lr(int row,
                        const char *left,  vga_color_t lfg,
                        const char *right, vga_color_t rfg,
                        vga_color_t bg)
{
    int ll  = wiz_strlen(left);
    int rl  = wiz_strlen(right);
    int pad = W_INNER - ll - rl;
    int i;
    vga_set_cursor(0, (uint8_t)row);
    vga_set_color(C_FR_FG, C_FR_BG);
    vga_putchar(BX_V);
    wiz_puts(left, lfg, bg);
    vga_set_color(C_CT_FG, bg);
    for (i = 0; i < pad; i++) vga_putchar(' ');
    wiz_puts(right, rfg, bg);
    vga_set_color(C_FR_FG, C_FR_BG);
    vga_putchar(BX_V);
}

/* Center text in a content row. */
static void wiz_row_center(int row, const char *s, vga_color_t fg, vga_color_t bg)
{
    int len  = wiz_strlen(s);
    int lpad = (W_INNER - len) / 2;
    int rpad = W_INNER - len - lpad;
    int i;
    vga_set_cursor(0, (uint8_t)row);
    vga_set_color(C_FR_FG, C_FR_BG);
    vga_putchar(BX_V);
    vga_set_color(C_CT_FG, bg);
    for (i = 0; i < lpad; i++) vga_putchar(' ');
    wiz_puts(s, fg, bg);
    for (i = 0; i < rpad; i++) vga_putchar(' ');
    vga_set_color(C_FR_FG, C_FR_BG);
    vga_putchar(BX_V);
}

/*
 * Print text at inner col `col` (1-based from the border) on `row`.
 * The caller MUST have already drawn the row borders via wiz_blank_row().
 */
static void wiz_at(int col, int row, const char *s, vga_color_t fg, vga_color_t bg)
{
    vga_set_cursor((uint8_t)col, (uint8_t)row);
    wiz_puts(s, fg, bg);
}

/*
 * Draw one drive-list entry at `row`.
 * Overwrites the inner 78 cols of a row already blanked by wiz_blank_row().
 * The right border BX_V at col 79 is left intact.
 * is_sel: 1 = highlighted, 0 = normal.
 * drv_idx: physical ATA drive index (for the "[N]" label).
 * is_boot: 1 = append "(boot drive)" marker.
 * line: formatted drive description, max 74 chars.
 */
static void wiz_draw_drive_row(int row, int is_sel, int drv_idx,
                                const char *line, int is_boot)
{
    int len = wiz_strlen(line);
    int i;
    /* clamp line to max 74 chars (leaves room for 2 prefix chars + "[N] " suffix) */
    (void)drv_idx;   /* embedded in `line` already */

    /* Boot marker: " [boot]" = 7 chars, appended after line if space allows */
    int boot_len = is_boot ? 7 : 0;
    /* Total inner: 2 (arrow+space) + len + boot_len + padding = 78 */
    int content_len = 2 + len + boot_len;
    if (content_len > W_INNER) content_len = W_INNER;   /* safety cap */
    int padding = W_INNER - content_len;

    vga_set_cursor(1, (uint8_t)row);

    if (is_sel) {
        vga_set_color(C_SEL_FG, C_SEL_BG);
        vga_putchar('\xAF'); vga_putchar(' ');        /* »  */
        wiz_puts(line, C_SEL_FG, C_SEL_BG);
        if (is_boot) wiz_puts(" [boot]", VGA_COLOR_YELLOW, C_SEL_BG);
        for (i = 0; i < padding; i++) vga_putchar(' ');
    } else {
        vga_set_color(C_DIM, C_CT_BG);
        vga_putchar(' '); vga_putchar(' ');
        wiz_puts(line, C_CT_FG, C_CT_BG);
        if (is_boot) wiz_puts(" [boot]", C_DIM, C_CT_BG);
        vga_set_color(C_CT_FG, C_CT_BG);
        for (i = 0; i < padding; i++) vga_putchar(' ');
    }
    /* cursor is at col 79 (border) — border was already drawn, don't overwrite */
}

/* ============================================================================
 * Progress bar
 * ========================================================================== */

/*
 * Full-width progress bar in a content row.
 * Layout: [70 bar chars]  NNN%   (total 78 inner chars)
 *          col1                  col79
 */
static void wiz_progress(int row, uint32_t done, uint32_t total)
{
    int      bar = 70;
    int      filled = (total > 0) ? (int)((done * (uint32_t)bar) / total) : 0;
    int      i;
    uint32_t pct = (total > 0) ? (done * 100u / total) : 0u;
    char     pct_str[8];
    int      plen;

    vga_set_cursor(1, (uint8_t)row);
    vga_set_color(C_CT_FG, C_CT_BG);
    vga_putchar('[');                                    /* col 1  */

    vga_set_color(C_OK, C_CT_BG);
    for (i = 0; i < filled; i++)     vga_putchar(BX_BAR);
    vga_set_color(C_DIM, C_CT_BG);
    for (i = filled; i < bar; i++)   vga_putchar(BX_SHD);

    vga_set_color(C_CT_FG, C_CT_BG);
    vga_putchar(']');                                    /* col 72 */
    vga_putchar(' '); vga_putchar(' ');                  /* col 73-74 */

    /* Right-align pct in 3 chars + '%': cols 75-78 */
    plen = wiz_fmtd(pct_str, pct);
    vga_set_color(C_CT_FG, C_CT_BG);
    while (plen < 3) { vga_putchar(' '); plen++; }     /* left-pad to 3 */
    wiz_puts(pct_str, C_CT_FG, C_CT_BG);
    vga_putchar('%');                                    /* cursor → col 79 */
}

/* ============================================================================
 * Frame
 * ========================================================================== */

static void wiz_draw_frame(int step, int total)
{
    static const char TITLE[] = "PD-OS INSTALLER";
    int tlen = wiz_strlen(TITLE);
    int lpad = (76 - tlen) / 2;   /* space each side of title within 78 inner */
    int rpad = 76 - tlen - lpad;
    int i, r;
    char step_str[12];
    int  slen;

    vga_clear();

    /* Row 0: ╔══ PD-OS INSTALLER ══╗ */
    vga_set_cursor(0, 0);
    vga_set_color(C_TITLE, C_FR_BG);
    vga_putchar(BX_TL);
    vga_set_color(C_FR_FG, C_FR_BG);
    for (i = 0; i < lpad; i++) vga_putchar(BX_H);
    vga_set_color(C_TITLE, C_FR_BG);
    vga_putchar(' ');
    { const char *t = TITLE; while (*t) vga_putchar(*t++); }
    vga_putchar(' ');
    vga_set_color(C_FR_FG, C_FR_BG);
    for (i = 0; i < rpad; i++) vga_putchar(BX_H);
    vga_set_color(C_TITLE, C_FR_BG);
    vga_putchar(BX_TR);

    /* Row 1: ║  PD-OS Installer           Step N/M  ║ */
    {
        step_str[0] = 'S'; step_str[1] = 't'; step_str[2] = 'e';
        step_str[3] = 'p'; step_str[4] = ' ';
        step_str[5] = (char)('0' + step);
        step_str[6] = '/';
        step_str[7] = (char)('0' + total);
        step_str[8] = ' '; step_str[9] = ' '; step_str[10] = '\0';
        slen = wiz_strlen(step_str);
        (void)slen;
        wiz_row_lr(ROW_SUB, "  PD-OS Installer", C_TITLE,
                             step_str, C_DIM, C_FR_BG);
    }

    /* Row 2: ╠═...═╣ */
    wiz_hline(ROW_DIV1, BX_ML, BX_MR);

    /* Rows 3-20: blank content rows */
    for (r = ROW_C0; r <= ROW_C_LAST; r++) wiz_blank_row(r);

    /* Row 21: ╠═...═╣ */
    wiz_hline(ROW_DIV2, BX_ML, BX_MR);

    /* Row 22: navigation row (blank to start) */
    wiz_blank_row(ROW_NAV);

    /* Row 23: ╚═...═╝ */
    wiz_hline(ROW_BOT, BX_BL, BX_BR);
}

static void wiz_set_nav(const char *left, const char *right)
{
    wiz_row_lr(ROW_NAV, left, C_OK, right, C_DIM, C_FR_BG);
}

/* ============================================================================
 * Page 1: Welcome
 * ========================================================================== */

/* Returns 0=next, -1=cancel */
static int page_welcome(void)
{
    wiz_draw_frame(1, 4);
    wiz_set_nav("  [ ENTER ] Continue", "[ ESC ] Cancel  ");

    wiz_row_center(4, "Welcome to the PD-OS Installer", C_TITLE, C_CT_BG);
    wiz_blank_row(5);
    wiz_at(3, 6, "This wizard will install PD-OS onto a selected storage device.", C_CT_FG, C_CT_BG);
    wiz_blank_row(7);
    wiz_at(3, 8, "The following will be written to the target drive:", C_CT_FG, C_CT_BG);
    wiz_at(5, 9,  "\xFB  Bootloader stages 1 + 2  (LBA 0-5)", C_DIM, C_CT_BG);
    wiz_at(5, 10, "\xFB  Kernel image             (LBA 6-1023)", C_DIM, C_CT_BG);
    wiz_at(5, 11, "\xFB  Fresh PDFS filesystem    (auto-initialised on first boot)", C_DIM, C_CT_BG);
    wiz_blank_row(12);
    wiz_at(3, 13, "/!\\ WARNING: ALL existing data on the target drive will be erased.", C_WARN, C_CT_BG);
    wiz_blank_row(14);
    wiz_at(3, 15, "Ensure you have selected the correct target drive before proceeding.", C_CT_FG, C_CT_BG);

    for (;;) {
        char c = keyboard_getchar();
        if (c == '\n' || c == '\r') return 0;
        if (c == '\x1b')            return -1;
    }
}

/* ============================================================================
 * Page 2: Drive selection
 * ========================================================================== */

static void draw_drive_list(int ndrvs, int *phys_idx, int sel)
{
    int i, r;

    /* Clear content rows */
    for (r = ROW_C0; r <= ROW_C_LAST; r++) wiz_blank_row(r);

    wiz_at(3, 4, "Select a target drive for installation:", C_CT_FG, C_CT_BG);

    if (ndrvs == 0) {
        wiz_row_center(8,  "No ATA drives detected.", C_WARN, C_CT_BG);
        wiz_row_center(9,  "Cannot proceed with installation.", C_WARN, C_CT_BG);
        return;
    }

    for (i = 0; i < ndrvs; i++) {
        const ata_drive_t *d = ata_get_drive_n(phys_idx[i]);
        int row = 6 + i;
        char size_str[16];
        char line[80];
        int  lpos = 0, j;

        if (row > ROW_C_LAST - 1) break;

        wiz_fmt_size(size_str, d->total_sectors);

        /* Build "[N] Model...  XX MB" — max 71 chars to leave room for boot marker */
        line[lpos++] = '[';
        line[lpos++] = (char)('0' + phys_idx[i]);
        line[lpos++] = ']';
        line[lpos++] = ' ';
        for (j = 0; d->model[j] && j < 36; j++) line[lpos++] = d->model[j];
        /* Pad model field to ensure minimum spacing */
        while (lpos < 42) line[lpos++] = ' ';
        for (j = 0; size_str[j]; j++) line[lpos++] = size_str[j];
        line[lpos] = '\0';

        wiz_blank_row(row);
        wiz_draw_drive_row(row, (i == sel), phys_idx[i], line, (phys_idx[i] == 0));
    }

    {
        int hint_row = 6 + ndrvs + 1;
        if (hint_row <= ROW_C_LAST)
            wiz_at(3, hint_row, "Use \x18\x19 arrows to navigate, ENTER to select.", C_DIM, C_CT_BG);
    }
}

/* Returns physical drive index of selected drive, or -1=cancel. */
static int page_drives(void)
{
    int phys_idx[ATA_MAX_DRIVES];
    int ndrvs = 0, i, sel;

    wiz_draw_frame(2, 4);
    wiz_set_nav("  [ ENTER ] Select", "[ ESC ] Cancel  ");

    for (i = 0; i < ATA_MAX_DRIVES; i++) {
        const ata_drive_t *d = ata_get_drive_n(i);
        if (d && d->present) phys_idx[ndrvs++] = i;
    }

    if (ndrvs == 0) {
        draw_drive_list(0, phys_idx, -1);
        for (;;) { char c = keyboard_getchar(); if (c == '\x1b') return -1; }
    }

    /* Default: prefer drive 1 (typical target), else drive 0 */
    sel = (ndrvs > 1) ? 1 : 0;
    draw_drive_list(ndrvs, phys_idx, sel);

    for (;;) {
        char c = keyboard_getchar();
        if (c == '\x1b')             return -1;
        if (c == KEY_UP   && sel > 0)           { sel--; draw_drive_list(ndrvs, phys_idx, sel); }
        if (c == KEY_DOWN && sel < ndrvs - 1)   { sel++; draw_drive_list(ndrvs, phys_idx, sel); }
        if (c == '\n' || c == '\r') return phys_idx[sel];
    }
}

/* ============================================================================
 * Page 3: Confirmation
 * ========================================================================== */

/* Returns 0=confirmed, -1=back/cancel. */
static int page_confirm(int drv)
{
    const ata_drive_t *d = ata_get_drive_n(drv);
    char size_str[16];
    char drive_line[80];
    int  lpos = 0, j;

    wiz_draw_frame(3, 4);
    wiz_set_nav("  [ ENTER ] Install", "[ ESC ] Go Back  ");

    wiz_row_center(4, "CONFIRM INSTALLATION", C_TITLE, C_CT_BG);
    wiz_blank_row(5);
    wiz_at(3, 6, "Target drive:", C_CT_FG, C_CT_BG);

    if (d && d->present) {
        wiz_fmt_size(size_str, d->total_sectors);
        drive_line[lpos++] = '[';
        drive_line[lpos++] = (char)('0' + drv);
        drive_line[lpos++] = ']';
        drive_line[lpos++] = ' ';
        for (j = 0; d->model[j] && j < 36; j++) drive_line[lpos++] = d->model[j];
        drive_line[lpos++] = ' '; drive_line[lpos++] = '(';
        for (j = 0; size_str[j]; j++) drive_line[lpos++] = size_str[j];
        drive_line[lpos++] = ')'; drive_line[lpos] = '\0';
        wiz_at(5, 7, drive_line, VGA_COLOR_LIGHT_CYAN, C_CT_BG);
    }

    wiz_blank_row(8);
    wiz_row_center(9,  "! WARNING !", C_WARN, C_CT_BG);
    wiz_row_center(10, "ALL DATA ON THE TARGET DRIVE WILL BE PERMANENTLY ERASED.", C_WARN, C_CT_BG);
    wiz_blank_row(11);

    if (drv == 0) {
        wiz_row_center(12, "Drive 0 is the current boot drive.", C_WARN, C_CT_BG);
        wiz_row_center(13, "Installing will overwrite the currently running system!", C_WARN, C_CT_BG);
        wiz_blank_row(14);
        wiz_at(3, 15, "Press ENTER to proceed or ESC to go back.", C_CT_FG, C_CT_BG);
    } else {
        wiz_at(3, 13, "Press ENTER to begin installation, or ESC to go back.", C_CT_FG, C_CT_BG);
    }

    for (;;) {
        char c = keyboard_getchar();
        if (c == '\n' || c == '\r') return 0;
        if (c == '\x1b')            return -1;
    }
}

/* ============================================================================
 * Page 4: Installing
 * ========================================================================== */

/* Returns 0=success, -1=error. */
static int page_install(int dst_drv)
{
    uint32_t lba;
    int      i;
    char     lba_str[12], total_str[12];

    wiz_draw_frame(4, 4);
    wiz_set_nav("  Please wait...", "Do not power off  ");

    wiz_row_center(4, "Installing PD-OS...", C_TITLE, C_CT_BG);
    wiz_blank_row(5);

    /* ---- Step 1/2: copy LBA 0-1023 (bootloader + kernel) ---- */
    wiz_at(3, 6, "Step 1/2  Copying bootloader + kernel...", C_CT_FG, C_CT_BG);
    wiz_fmtd(total_str, INST_BOOT_SECS);

    for (lba = 0; lba < INST_BOOT_SECS; lba += INST_CHUNK) {
        uint8_t cnt = (uint8_t)((INST_BOOT_SECS - lba >= INST_CHUNK)
                                ? INST_CHUNK
                                : (INST_BOOT_SECS - lba));

        if (ata_read_sectors_drv(0, lba, cnt, g_copy_buf) != 0) {
            wiz_blank_row(10);
            wiz_row_center(10, "ERROR: read failed on source drive.", C_WARN, C_CT_BG);
            wiz_set_nav("  [ ENTER ] Close", "");
            for (;;) { char c = keyboard_getchar();
                        if (c == '\n' || c == '\r' || c == '\x1b') return -1; }
        }

        if (ata_write_sectors_raw(dst_drv, lba, cnt, g_copy_buf) != 0) {
            wiz_blank_row(10);
            wiz_row_center(10, "ERROR: write failed on target drive.", C_WARN, C_CT_BG);
            wiz_set_nav("  [ ENTER ] Close", "");
            for (;;) { char c = keyboard_getchar();
                        if (c == '\n' || c == '\r' || c == '\x1b') return -1; }
        }

        /* Progress bar */
        wiz_progress(8, lba + cnt, INST_BOOT_SECS);

        /* "LBA XXXX / 1024" below the bar */
        {
            int n1 = wiz_fmtd(lba_str, lba + cnt);
            (void)n1;
            vga_set_cursor(3, 9);
            wiz_puts("LBA ", C_DIM, C_CT_BG);
            wiz_puts(lba_str, C_DIM, C_CT_BG);
            wiz_puts(" / ", C_DIM, C_CT_BG);
            wiz_puts(total_str, C_DIM, C_CT_BG);
            /* pad to clear old chars */
            { int k; for (k = 0; k < 12; k++) vga_putchar(' '); }
        }
    }

    wiz_at(3, 6, "Step 1/2  Copying bootloader + kernel...  OK", C_OK, C_CT_BG);

    /* ---- Step 2/2: zero PDFS header on target ---- */
    wiz_blank_row(10);
    wiz_at(3, 10, "Step 2/2  Preparing filesystem area...", C_CT_FG, C_CT_BG);

    for (i = 0; i < 512; i++) g_zero_buf[i] = 0;

    for (i = 0; (uint32_t)i < INST_PDFS_ZERO; i++) {
        if (ata_write_sectors_raw(dst_drv, INST_BOOT_SECS + (uint32_t)i, 1, g_zero_buf) != 0) {
            /* Non-fatal — kernel will re-init PDFS on first boot regardless */
            wiz_at(3, 11, "Warning: could not clear PDFS header (non-fatal).", C_WARN, C_CT_BG);
            break;
        }
    }

    wiz_at(3, 10, "Step 2/2  Preparing filesystem area...    OK", C_OK, C_CT_BG);
    wiz_blank_row(12);
    wiz_row_center(13, "Installation complete!", C_OK, C_CT_BG);
    return 0;
}

/* ============================================================================
 * Page 5: Complete — remove media + reboot
 * ========================================================================== */

static void page_complete(int dst_drv)
{
    char drv_ch[4];
    drv_ch[0] = '['; drv_ch[1] = (char)('0' + dst_drv); drv_ch[2] = ']'; drv_ch[3] = '\0';

    wiz_blank_row(15);
    wiz_at(3, 15, "PD-OS has been installed on drive ", C_CT_FG, C_CT_BG);
    wiz_puts(drv_ch, VGA_COLOR_LIGHT_CYAN, C_CT_BG);
    vga_putchar('.');

    wiz_blank_row(16);
    wiz_blank_row(17);
    wiz_at(3, 17, "Remove the installation media, then press ENTER to reboot.", VGA_COLOR_YELLOW, C_CT_BG);
    wiz_set_nav("  [ ENTER ] Reboot", "");

    for (;;) {
        char c = keyboard_getchar();
        if (c == '\n' || c == '\r') break;
    }

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
    for (;;); /* unreachable */
}

/* ============================================================================
 * Public entry point
 * ========================================================================== */

void install_wizard(void)
{
    int sel_drv, r;

    /* Save current screen so we can restore it if user cancels */
    vga_save_rows(0, W_H, g_saved_screen);

    /* Probe all primary-channel drives (master + slave) */
    ata_probe_all();

    /* ---- Page 1: Welcome ---- */
    r = page_welcome();
    if (r < 0) goto cancel;

    /* ---- Page 2: Drive selection ---- */
    sel_drv = page_drives();
    if (sel_drv < 0) goto cancel;

    /* ---- Page 3: Confirmation ---- */
    r = page_confirm(sel_drv);
    if (r < 0) goto cancel;

    /* ---- Page 4: Install ---- */
    r = page_install(sel_drv);
    if (r < 0) goto cancel;

    /* ---- Page 5: Complete ---- */
    page_complete(sel_drv);
    return;   /* unreachable if reboot succeeds */

cancel:
    vga_restore_rows(0, W_H, g_saved_screen);
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
}
