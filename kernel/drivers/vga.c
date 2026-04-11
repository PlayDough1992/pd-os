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

static inline uint8_t make_attr(vga_color_t fg, vga_color_t bg)
{
    return (uint8_t)((uint8_t)fg | ((uint8_t)bg << 4));
}

static inline uint16_t make_entry(char c, uint8_t attr)
{
    return (uint16_t)(uint8_t)c | ((uint16_t)attr << 8);
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
    uint32_t i;
    for (i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++)
        VGA_BUFFER[i] = blank;
    vga_col = 0;
    vga_row = 0;
}

void vga_set_color(vga_color_t fg, vga_color_t bg)
{
    vga_attr = make_attr(fg, bg);
}

static void scroll(void)
{
    uint16_t blank = make_entry(' ', vga_attr);
    uint32_t row, col;

    /* Shift rows 1..24 up to 0..23 */
    for (row = 1; row < VGA_HEIGHT; row++)
        for (col = 0; col < VGA_WIDTH; col++)
            VGA_BUFFER[(row - 1) * VGA_WIDTH + col] =
                VGA_BUFFER[row * VGA_WIDTH + col];

    /* Clear last row */
    for (col = 0; col < VGA_WIDTH; col++)
        VGA_BUFFER[(VGA_HEIGHT - 1) * VGA_WIDTH + col] = blank;

    vga_row = VGA_HEIGHT - 1;
}

void vga_putchar(char c)
{
    if (c == '\n') {
        vga_col = 0;
        if (++vga_row >= VGA_HEIGHT) scroll();
        return;
    }
    if (c == '\r') {
        vga_col = 0;
        return;
    }
    if (c == '\t') {
        vga_col = (uint8_t)((vga_col + 8) & ~7u);
        if (vga_col >= VGA_WIDTH) {
            vga_col = 0;
            if (++vga_row >= VGA_HEIGHT) scroll();
        }
        return;
    }

    VGA_BUFFER[vga_row * VGA_WIDTH + vga_col] = make_entry(c, vga_attr);
    if (++vga_col >= VGA_WIDTH) {
        vga_col = 0;
        if (++vga_row >= VGA_HEIGHT) scroll();
    }
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
}
