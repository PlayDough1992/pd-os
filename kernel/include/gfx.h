#ifndef GFX_H
#define GFX_H

#include "kernel.h"

typedef uint32_t gfx_color_t;

/* Colour helpers */
#define GFX_RGB(r,g,b) (((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))
#define GFX_R(c) (((c) >> 16) & 0xFF)
#define GFX_G(c) (((c) >>  8) & 0xFF)
#define GFX_B(c) ( (c)        & 0xFF)

/* PD-OS colour palette */
#define GFX_COLOR_DESKTOP      GFX_RGB(0,   112, 112)  /* teal desktop          */
#define GFX_COLOR_TASKBAR      GFX_RGB(212, 208, 200)  /* silver taskbar        */
#define GFX_COLOR_TITLEBAR_ACT GFX_RGB( 10,  36, 106)  /* active title (dark)   */
#define GFX_COLOR_TITLEBAR_HI  GFX_RGB(166, 202, 240)  /* active title (light)  */
#define GFX_COLOR_TITLEBAR_INA GFX_RGB(128, 128, 128)  /* inactive title        */
#define GFX_COLOR_WIN_BORDER   GFX_RGB(212, 208, 200)  /* window border         */
#define GFX_COLOR_WIN_CLIENT   GFX_RGB(255, 255, 255)  /* client area           */
#define GFX_COLOR_START_BTN    GFX_RGB( 92, 175,  57)  /* start button green    */
#define GFX_COLOR_WHITE        GFX_RGB(255, 255, 255)
#define GFX_COLOR_BLACK        GFX_RGB(  0,   0,   0)
#define GFX_COLOR_SHADOW       GFX_RGB( 64,  64,  64)
#define GFX_COLOR_TEXT_LIGHT   GFX_RGB(255, 255, 255)
#define GFX_COLOR_TEXT_DARK    GFX_RGB(  0,   0,   0)
#define GFX_TRANSPARENT        0xFFFFFFFFu  /* sentinel: no-draw */

/* Lifecycle */
void gfx_init(void);
int  gfx_is_active(void);
int  gfx_width(void);
int  gfx_height(void);

/* Primitives */
void gfx_clear(gfx_color_t c);
void gfx_pixel(int x, int y, gfx_color_t c);
void gfx_hline(int x, int y, int len, gfx_color_t c);
void gfx_vline(int x, int y, int len, gfx_color_t c);
void gfx_fill_rect(int x, int y, int w, int h, gfx_color_t c);
void gfx_draw_rect(int x, int y, int w, int h, gfx_color_t c);
void gfx_rounded_rect(int x, int y, int w, int h, int r, gfx_color_t fill,
                      gfx_color_t border);

/* Text (8×8 bitmap font) */
void gfx_char(int x, int y, char c, gfx_color_t fg, gfx_color_t bg);
void gfx_text(int x, int y, const char *s, gfx_color_t fg, gfx_color_t bg);
int  gfx_text_width(const char *s);   /* pixels */

/* Read pixel colour back from the framebuffer */
gfx_color_t gfx_read_pixel(int x, int y);

/* kprint_redirect-compatible putchar */
void gfx_putchar(char c);
/* Move text cursor (for kprintf routing) */
void gfx_set_cursor(int x, int y);
void gfx_get_cursor(int *x, int *y);

/* Gradient fill (vertical, two colours) */
void gfx_gradient_rect(int x, int y, int w, int h,
                        gfx_color_t top, gfx_color_t bottom);

#endif /* GFX_H */
