#pragma once

/* ============================================================================
 * PD-Kernel  —  VGA text-mode driver (80x25)
 * ============================================================================ */

#include "kernel.h"

#define VGA_WIDTH  80
#define VGA_HEIGHT 25

/* VGA color codes (4-bit) */
typedef enum {
    VGA_COLOR_BLACK          = 0,
    VGA_COLOR_BLUE           = 1,
    VGA_COLOR_GREEN          = 2,
    VGA_COLOR_CYAN           = 3,
    VGA_COLOR_RED            = 4,
    VGA_COLOR_MAGENTA        = 5,
    VGA_COLOR_BROWN          = 6,
    VGA_COLOR_LIGHT_GREY     = 7,
    VGA_COLOR_DARK_GREY      = 8,
    VGA_COLOR_LIGHT_BLUE     = 9,
    VGA_COLOR_LIGHT_GREEN    = 10,
    VGA_COLOR_LIGHT_CYAN     = 11,
    VGA_COLOR_LIGHT_RED      = 12,
    VGA_COLOR_LIGHT_MAGENTA  = 13,
    VGA_COLOR_YELLOW         = 14,
    VGA_COLOR_WHITE          = 15,
} vga_color_t;

void vga_init(void);
void vga_clear(void);
void vga_set_color(vga_color_t fg, vga_color_t bg);
void vga_putchar(char c);
void vga_backspace(void);
uint8_t vga_get_col(void);
uint8_t vga_get_row(void);
void vga_cursor_left(void);
void vga_cursor_right(void);
void vga_cursor_up(void);
void vga_cursor_down(void);
void vga_puts(const char *s);
void vga_set_cursor(uint8_t x, uint8_t y);
void vga_scroll_up(int lines);
void vga_scroll_down(int lines);
void vga_scroll_reset(void);
int  vga_get_scroll_count(void);
void vga_clear_chars(uint8_t start_col, uint8_t start_row, int n);
