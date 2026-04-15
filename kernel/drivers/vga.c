/* ============================================================================
 * PD-Kernel  —  VGA 80x25 colour text driver
 * ============================================================================ */

#include "vga.h"
#include "kernel.h"

#define VGA_WIDTH   80
#define VGA_HEIGHT  25
#define VGA_BUFFER  ((volatile uint16_t *)0xB8000)

static uint8_t vga_col  = 0;
static uint8_t vga_row  = 0;
static uint8_t vga_attr = 0x07; /* white on black */

/* Optional output hook — when set, vga_putchar forwards here instead */
static void (*g_putchar_hook)(char) = (void*)0;

void vga_set_hook(void (*fn)(char))
{
    g_putchar_hook = fn;
}

/* ---- Scrollback buffer ---------------------------------------------------- */
#define SB_LINES 200
static uint16_t g_screen[VGA_HEIGHT][VGA_WIDTH];  /* shadow of live display  */
static uint16_t g_sb[SB_LINES][VGA_WIDTH];        /* off-screen ring buffer  */
static int      g_sb_head  = 0;  /* ring write pointer                       */
static int      g_sb_count = 0;  /* valid entries (0..SB_LINES)              */
static int      g_sb_off   = 0;  /* viewport offset: 0=live, N=N lines above */
static int      g_scroll_count = 0; /* total scroll() calls ever             */

static inline uint8_t make_attr(vga_color_t fg, vga_color_t bg)
{
    return (uint8_t)((uint8_t)fg | ((uint8_t)bg << 4));
}

static inline uint16_t make_entry(char c, uint8_t attr)
{
    return (uint16_t)(uint8_t)c | ((uint16_t)attr << 8);
}

static inline void outb(uint16_t port, uint8_t val)
{
    __asm__ volatile ("outb %0, %1" :: "a"(val), "Nd"(port));
}

static void hw_cursor_sync(void)
{
    uint16_t pos = (uint16_t)(vga_row * VGA_WIDTH + vga_col);
    outb(0x3D4, 0x0F); outb(0x3D5, (uint8_t)(pos & 0xFF));
    outb(0x3D4, 0x0E); outb(0x3D5, (uint8_t)(pos >> 8));
}

void vga_init(void)
{
    vga_col  = 0;
    vga_row  = 0;
    vga_attr = make_attr(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    vga_clear();
}

void vga_clear(void)
{
    uint16_t blank = make_entry(' ', vga_attr);
    uint32_t row, col;
    for (row = 0; row < VGA_HEIGHT; row++)
        for (col = 0; col < VGA_WIDTH; col++) {
            VGA_BUFFER[row * VGA_WIDTH + col] = blank;
            g_screen[row][col] = blank;
        }
    g_sb_head  = 0;
    g_sb_count = 0;
    g_sb_off   = 0;
    vga_col = 0;
    vga_row = 0;
    hw_cursor_sync();
}

void vga_set_color(vga_color_t fg, vga_color_t bg)
{
    vga_attr = make_attr(fg, bg);
}

static void vga_render_viewport(void)
{
    int      off = g_sb_off;
    uint32_t row, col;

    for (row = 0; row < VGA_HEIGHT; row++) {
        uint16_t *dst = (uint16_t *)VGA_BUFFER + row * VGA_WIDTH;
        if ((int)row < off) {
            /* From scrollback: row 0 = oldest visible, row (off-1) = most recent */
            int sb_back = off - 1 - (int)row;  /* 0=most-recent SB line */
            if (sb_back < g_sb_count) {
                int idx = ((g_sb_head - 1 - sb_back) % SB_LINES
                           + SB_LINES) % SB_LINES;
                for (col = 0; col < VGA_WIDTH; col++)
                    dst[col] = g_sb[idx][col];
            } else {
                uint16_t blank = make_entry(' ', 0x07);
                for (col = 0; col < VGA_WIDTH; col++)
                    dst[col] = blank;
            }
        } else {
            /* From live screen */
            int sr = (int)row - off;
            for (col = 0; col < VGA_WIDTH; col++)
                dst[col] = g_screen[sr][col];
        }
    }

    if (off == 0) {
        hw_cursor_sync();
    } else {
        /* Move hardware cursor off visible area to hide it */
        uint16_t pos = (uint16_t)(VGA_HEIGHT * VGA_WIDTH);
        outb(0x3D4, 0x0F); outb(0x3D5, (uint8_t)(pos & 0xFF));
        outb(0x3D4, 0x0E); outb(0x3D5, (uint8_t)(pos >> 8));
    }
}

static void scroll(void)
{
    uint16_t blank = make_entry(' ', vga_attr);
    uint32_t row, col;

    /* Save the row about to scroll off into the scrollback ring */
    for (col = 0; col < VGA_WIDTH; col++)
        g_sb[g_sb_head][col] = g_screen[0][col];
    g_sb_head = (g_sb_head + 1) % SB_LINES;
    if (g_sb_count < SB_LINES) g_sb_count++;

    /* Shift g_screen up by one row */
    for (row = 1; row < VGA_HEIGHT; row++)
        for (col = 0; col < VGA_WIDTH; col++)
            g_screen[row - 1][col] = g_screen[row][col];

    /* Clear the new last row */
    for (col = 0; col < VGA_WIDTH; col++)
        g_screen[VGA_HEIGHT - 1][col] = blank;

    vga_row = VGA_HEIGHT - 1;
    g_scroll_count++;

    /* Keep viewport anchored to same content when user is scrolled up */
    if (g_sb_off > 0) {
        g_sb_off++;
        if (g_sb_off > g_sb_count) g_sb_off = g_sb_count;
    }

    vga_render_viewport();
}

void vga_putchar(char c)
{
    uint16_t entry;
    if (g_putchar_hook) { g_putchar_hook(c); return; }
    if (c == '\n') {
        vga_col = 0;
        if (++vga_row >= VGA_HEIGHT) scroll(); /* scroll() redraws + syncs */
        else if (g_sb_off == 0) hw_cursor_sync();
        return;
    }
    if (c == '\r') {
        vga_col = 0;
        if (g_sb_off == 0) hw_cursor_sync();
        return;
    }
    if (c == '\t') {
        vga_col = (uint8_t)((vga_col + 8) & ~7u);
        if (vga_col >= VGA_WIDTH) {
            vga_col = 0;
            if (++vga_row >= VGA_HEIGHT) scroll();
        }
        if (g_sb_off == 0) hw_cursor_sync();
        return;
    }
    entry = make_entry(c, vga_attr);
    g_screen[vga_row][vga_col] = entry;
    if (g_sb_off == 0)
        VGA_BUFFER[vga_row * VGA_WIDTH + vga_col] = entry;
    if (++vga_col >= VGA_WIDTH) {
        vga_col = 0;
        if (++vga_row >= VGA_HEIGHT) scroll();
    }
    if (g_sb_off == 0) hw_cursor_sync();
}

void vga_backspace(void)
{
    if (vga_col > 0) {
        vga_col--;
    } else if (vga_row > 0) {
        vga_row--;
        vga_col = VGA_WIDTH - 1;
    } else {
        return;  /* already at top-left */
    }
    uint16_t blank = make_entry(' ', vga_attr);
    g_screen[vga_row][vga_col] = blank;
    if (g_sb_off == 0)
        VGA_BUFFER[vga_row * VGA_WIDTH + vga_col] = blank;
    hw_cursor_sync();
}

uint8_t vga_get_col(void) { return vga_col; }
uint8_t vga_get_row(void) { return vga_row; }
int     vga_get_scroll_count(void) { return g_scroll_count; }

void vga_cursor_left(void)
{
    if (vga_col > 0) {
        vga_col--;
    } else if (vga_row > 0) {
        vga_row--;
        vga_col = VGA_WIDTH - 1;
    }
    hw_cursor_sync();
}

void vga_cursor_right(void)
{
    if (++vga_col >= VGA_WIDTH) {
        vga_col = 0;
        if (vga_row < VGA_HEIGHT - 1) vga_row++;
        else vga_col = VGA_WIDTH - 1;
    }
    hw_cursor_sync();
}

void vga_cursor_up(void)
{
    if (vga_row > 0) vga_row--;
    hw_cursor_sync();
}

void vga_cursor_down(void)
{
    if (vga_row < VGA_HEIGHT - 1) vga_row++;
    hw_cursor_sync();
}

void vga_puts(const char *s)
{
    while (*s)
        vga_putchar(*s++);
}

void vga_set_cursor(uint8_t x, uint8_t y)
{
    vga_col = x;
    vga_row = y;
    hw_cursor_sync();
}

void vga_scroll_up(int lines)
{
    g_sb_off += lines;
    if (g_sb_off > g_sb_count) g_sb_off = g_sb_count;
    vga_render_viewport();
}

void vga_scroll_down(int lines)
{
    g_sb_off -= lines;
    if (g_sb_off < 0) g_sb_off = 0;
    vga_render_viewport();
}

void vga_scroll_reset(void)
{
    if (g_sb_off != 0) {
        g_sb_off = 0;
        vga_render_viewport();
    }
}

/* Clear n characters starting at (start_col, start_row) by writing spaces
 * directly to the shadow + VGA buffer.  Does NOT call vga_putchar so it
 * never causes a scroll and never alters vga_col/vga_row. */
void vga_clear_chars(uint8_t start_col, uint8_t start_row, int n)
{
    int pos     = (int)start_row * VGA_WIDTH + (int)start_col;
    int max_pos = (int)(VGA_HEIGHT * VGA_WIDTH);
    uint16_t blank = make_entry(' ', vga_attr);
    int i;
    for (i = 0; i < n && pos + i < max_pos; i++) {
        int p = pos + i;
        g_screen[p / VGA_WIDTH][p % VGA_WIDTH] = blank;
        if (g_sb_off == 0)
            VGA_BUFFER[p] = blank;
    }
}

/* Copy nrows full rows from the shadow buffer into dst (row-major, 80 cells/row). */
void vga_save_rows(int start_row, int nrows, uint16_t *dst)
{
    int r, c;
    for (r = 0; r < nrows && (start_row + r) < VGA_HEIGHT; r++)
        for (c = 0; c < VGA_WIDTH; c++)
            dst[r * VGA_WIDTH + c] = g_screen[start_row + r][c];
}

/* Write nrows full rows from src back into the shadow buffer and VGA hardware. */
void vga_restore_rows(int start_row, int nrows, const uint16_t *src)
{
    int r, c;
    for (r = 0; r < nrows && (start_row + r) < VGA_HEIGHT; r++)
        for (c = 0; c < VGA_WIDTH; c++) {
            uint16_t v = src[r * VGA_WIDTH + c];
            g_screen[start_row + r][c] = v;
            if (g_sb_off == 0)
                VGA_BUFFER[(start_row + r) * VGA_WIDTH + c] = v;
        }
}
