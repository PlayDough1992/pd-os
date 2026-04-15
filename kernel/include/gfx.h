#pragma once

/* ============================================================================
 * PD-OS GDE  —  Linear framebuffer graphics driver
 * ============================================================================
 * Targets the VBE 32bpp linear framebuffer set by stage2.
 * All coordinates are in pixels; (0,0) is top-left.
 * Color format: 0x00RRGGBB  (top byte unused / treated as alpha=0).
 *
 * Font: 8×16 bitmap font loaded from the VGA BIOS copy at GDE_FONT_ADDR
 * (physical 0x3000, copied by stage2 before switching to graphics mode).
 * ============================================================================ */

#include "kernel.h"

/* Pack r,g,b into a 32-bit pixel value */
#define GFX_RGB(r,g,b) \
    ((uint32_t)(((uint32_t)(uint8_t)(r) << 16) | \
                ((uint32_t)(uint8_t)(g) <<  8) | \
                 (uint32_t)(uint8_t)(b)))

/* Common palette */
#define GFX_BLACK       GFX_RGB(  0,  0,  0)
#define GFX_WHITE       GFX_RGB(255,255,255)
#define GFX_RED         GFX_RGB(200, 30, 30)
#define GFX_GREEN       GFX_RGB( 30,180, 30)
#define GFX_BLUE        GFX_RGB( 30, 80,200)
#define GFX_CYAN        GFX_RGB(  0,180,200)
#define GFX_YELLOW      GFX_RGB(240,200,  0)
#define GFX_DARK_GREY   GFX_RGB( 60, 60, 65)
#define GFX_MID_GREY    GFX_RGB(130,130,135)
#define GFX_LIGHT_GREY  GFX_RGB(210,210,215)

/* Character dimensions */
#define GFX_CHAR_W   8
#define GFX_CHAR_H  16

/*
 * Initialise the GFX subsystem.
 * fb_addr : physical address of the VBE framebuffer (from boot_info)
 * width   : horizontal resolution in pixels
 * height  : vertical resolution in pixels
 * pitch   : bytes per scan line
 */
void gfx_init(uint32_t fb_addr, uint16_t width, uint16_t height, uint16_t pitch);

/* Dimensions of the current display */
int gfx_width(void);
int gfx_height(void);

/* ---- Primitives ---------------------------------------------------------- */

void     gfx_putpixel(int x, int y, uint32_t color);
uint32_t gfx_getpixel(int x, int y);

void gfx_fill_rect(int x, int y, int w, int h, uint32_t color);
void gfx_draw_rect(int x, int y, int w, int h, uint32_t color);
void gfx_hline(int x, int y, int len, uint32_t color);
void gfx_vline(int x, int y, int len, uint32_t color);

/* Gradient-filled rectangle (vertical gradient, top → bottom) */
void gfx_fill_rect_grad(int x, int y, int w, int h,
                         uint32_t top_color, uint32_t bot_color);

/* ---- Text ---------------------------------------------------------------- */

void gfx_draw_char(int x, int y, char c, uint32_t fg, uint32_t bg);

/* Draw NUL-terminated string; transparent_bg = 1 skips background pixels */
void gfx_draw_string(int x, int y, const char *s,
                      uint32_t fg, uint32_t bg, int transparent_bg);

/* Draw up to `len` chars */
void gfx_draw_string_n(int x, int y, const char *s, int len,
                        uint32_t fg, uint32_t bg, int transparent_bg);

/* Pixel width of a string */
int gfx_string_w(const char *s);
int gfx_string_w_n(const char *s, int len);

/* ---- Region save/restore (for cursor + menus) ---------------------------- */

void gfx_save_region(int x, int y, int w, int h, uint32_t *dst);
void gfx_restore_region(int x, int y, int w, int h, const uint32_t *src);

/* Atomic zero-flicker cursor update. Stamps the sprite at (nx,ny) on the
 * back buffer, blits the union of old (ox,oy) and new (nx,ny) rects to the
 * real FB in ONE pass, then restores the back buffer to cursor-free state.
 * sprite/mask: w*h arrays (mask: 1=opaque, 0=transparent).
 * save: caller-allocated w*h uint32_t scratch buffer. */
void gfx_cursor_blit(int ox, int oy, int nx, int ny, int w, int h,
                     const uint32_t *sprite, const uint8_t *mask, uint32_t *save);

/* ---- Blending ------------------------------------------------------------ */

/* Alpha-blend src over a pixel already at (x,y); alpha 0=transparent,
 * 255=opaque.  Used for rounded corners / shadows (optional). */
void gfx_blend_pixel(int x, int y, uint32_t color, uint8_t alpha);

/* ---- Double-buffering ---------------------------------------------------- */

/* Copy the back buffer to the real VBE framebuffer in one atomic blit.
 * Call once at the end of each rendered frame to eliminate flicker. */
void gfx_flip(void);

/* Copy only a clipped w×h rectangle from back buffer to real FB.
 * Use for cursor updates — writes ~500 pixels instead of 3 MB. */
void gfx_flip_rect(int x, int y, int w, int h);

/* Capture current back buffer as the desktop background snapshot. */
void gfx_cache_bg(void);

/* Restore background snapshot into the back buffer (fast start-of-frame
 * blit replacing the expensive gradient+grid recomputation). */
void gfx_restore_bg(void);

/* Restore only a w×h rect from background snapshot into the back buffer.
 * Use during partial renders to avoid the full 3 MB copy. */
void gfx_restore_bg_rect(int x, int y, int w, int h);

/* ---- Window drag content cache ------------------------------------------- */

/* Save the back-buffer region (window content area) into the drag cache.
 * Call once when a window drag begins; invalidated by gfx_dragsave(0,0,0,0). */
void gfx_dragsave(int x, int y, int w, int h);

/* Blit the saved drag cache to the back buffer at (x, y).
 * Replaces draw_content() during drag — one rep-movsl per row. */
void gfx_dragstamp(int x, int y);

/* Returns 1 if a valid drag cache exists (dragsave was called with w/h > 0). */
int  gfx_dragcache_valid(void);

/* ---- Dirty rect accumulator (partial compositor) ------------------------- */

/* Mark a region of the screen as changed this frame (union-expanded). */
void gfx_dirty_mark(int x, int y, int w, int h);

/* Clear the dirty region (call after presenting). */
void gfx_dirty_reset(void);

/* Read back the accumulated dirty rect. Returns 1 if any dirty, else 0. */
int  gfx_dirty_get(int *x, int *y, int *w, int *h);
