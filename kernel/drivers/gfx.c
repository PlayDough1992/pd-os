/* ============================================================================
 * PD-OS GDE  —  Linear framebuffer graphics driver
 * ============================================================================ */

#include "gfx.h"
#include "boot_info.h"
#include "kernel.h"
#include "paging.h"
#include "cursor_data.h"

/* ---- Driver state -------------------------------------------------------- */

/* Memory layout — each region uses 4 MB PSE identity-mapped pages.
 * Two pages each for back buffer and bg cache gives 8 MB per region,
 * enough for 1920×1080×32bpp (= 7.9 MB).
 *
 *  0x0400000 – 0x07FFFFF  back buffer   page 1 (4 MB)
 *  0x0800000 – 0x0BFFFFF  back buffer   page 2 (4 MB)
 *  0x0C00000 – 0x0FFFFFF  bg cache      page 1 (4 MB)
 *  0x1000000 – 0x13FFFFF  bg cache      page 2 (4 MB)
 *  0x1400000 – 0x17FFFFF  drag cache    page   (4 MB) */
#define GFX_BACKBUF_PHYS     0x400000u
#define GFX_BGCACHE_PHYS     0xC00000u
#define GFX_DRAGCACHE_PHYS   0x1400000u
#define GFX_DRAGCACHE_PIXELS (1024u * 1024u)  /* 4 MB drag cache */

static uint32_t *g_fb       = (void *)0;  /* drawing target  = back buffer  */
static uint32_t *g_fb_real  = (void *)0;  /* real VBE framebuffer           */
static uint32_t *g_bg_cache = (void *)0;  /* pre-rendered background cache  */
static uint32_t *g_drag_cache = (void *)0;/* window content drag snapshot   */
static int       g_drag_cache_w = 0;
static int       g_drag_cache_h = 0;
static int       g_width    = 1024;
static int       g_height   = 768;
static int       g_pitch4   = 1024; /* back buffer pitch in uint32_t = g_width (tight) */
static int       g_fb_pitch4 = 1024; /* real VBE framebuffer pitch in uint32_t (may differ) */

/* Font: 8x16, 256 glyphs.  stage2 copies VGA BIOS font to GDE_FONT_ADDR.    */
static const uint8_t *g_font = (const uint8_t *)GDE_FONT_ADDR;

/* ---- Inline helpers ------------------------------------------------------ */

static inline int gfx_clip(int x, int y) {
    return (x >= 0 && x < g_width && y >= 0 && y < g_height);
}

static inline void gfx_wr(int x, int y, uint32_t c) {
    g_fb[y * g_pitch4 + x] = c;
}

static inline uint32_t gfx_rd(int x, int y) {
    return g_fb[y * g_pitch4 + x];
}

/* ---- Public API ---------------------------------------------------------- */

void gfx_init(uint32_t fb_addr, uint16_t width, uint16_t height, uint16_t pitch)
{
    paging_map_framebuffer(GFX_BACKBUF_PHYS);           /* 0x0400000 */
    paging_map_framebuffer(GFX_BACKBUF_PHYS + 0x400000u); /* 0x0800000 */
    paging_map_framebuffer(GFX_BGCACHE_PHYS);           /* 0x0C00000 */
    paging_map_framebuffer(GFX_BGCACHE_PHYS + 0x400000u); /* 0x1000000 */
    paging_map_framebuffer(GFX_DRAGCACHE_PHYS);          /* 0x1400000 */

    g_fb_real   = (uint32_t *)fb_addr;
    g_fb        = (uint32_t *)GFX_BACKBUF_PHYS;
    g_bg_cache  = (uint32_t *)GFX_BGCACHE_PHYS;
    g_drag_cache = (uint32_t *)GFX_DRAGCACHE_PHYS;
    g_width     = (int)width;
    g_height    = (int)height;
    /* Back buffer uses tight rows (width == pitch) so all draw calls use
     * simple row offsets independent of hardware framebuffer alignment. */
    g_pitch4    = (int)width;
    /* Real framebuffer pitch from boot_info, in uint32_t units.
     * For 1024×768×32bpp the BIOS should report 4096 bytes = 1024 uint32s.
     * Fall back to width if the BIOS reports zero. */
    g_fb_pitch4 = (pitch > 0) ? (int)(pitch / 4) : (int)width;
}

/* ---- Background snapshot cache ------------------------------------------ */

void gfx_cache_bg(void)
{
    uint32_t  n   = (uint32_t)g_height * (uint32_t)g_pitch4;
    uint32_t *src = g_fb;
    uint32_t *dst = g_bg_cache;
    __asm__ volatile ("rep movsl" : "+S"(src), "+D"(dst), "+c"(n) :: "memory");
}

void gfx_restore_bg(void)
{
    uint32_t  n   = (uint32_t)g_height * (uint32_t)g_pitch4;
    uint32_t *src = g_bg_cache;
    uint32_t *dst = g_fb;
    __asm__ volatile ("rep movsl" : "+S"(src), "+D"(dst), "+c"(n) :: "memory");
}

/* Restore only a w×h rect from bg cache to back buffer.
 * Use during partial renders — avoids the full 3 MB copy. */
void gfx_restore_bg_rect(int x, int y, int w, int h)
{
    int row;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > g_width)  w = g_width  - x;
    if (y + h > g_height) h = g_height - y;
    if (w <= 0 || h <= 0) return;

    const uint32_t *src = g_bg_cache + y * g_pitch4 + x;
    uint32_t       *dst = g_fb       + y * g_pitch4 + x;
    for (row = 0; row < h; row++) {
        uint32_t        n = (uint32_t)w;
        const uint32_t *s = src;
        uint32_t       *d = dst;
        __asm__ volatile ("rep movsl" : "+S"(s), "+D"(d), "+c"(n) :: "memory");
        src += g_pitch4;
        dst += g_pitch4;
    }
}

/* ---- Window drag content cache ------------------------------------------ */

/* Snapshot the back-buffer content area of a dragged window into the drag
 * cache (1 MB at 0xB00000, within the already-mapped bg-cache PSE page).
 * Call once when a drag begins; call gfx_dragstamp() each drag tick. */
void gfx_dragsave(int x, int y, int w, int h)
{
    int row;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > g_width)  w = g_width  - x;
    if (y + h > g_height) h = g_height - y;
    if (w <= 0 || h <= 0) return;
    /* Clamp to drag cache capacity */
    if ((uint32_t)(w * h) > GFX_DRAGCACHE_PIXELS) {
        if (w > h) w = (int)(GFX_DRAGCACHE_PIXELS / (uint32_t)h);
        else       h = (int)(GFX_DRAGCACHE_PIXELS / (uint32_t)w);
    }
    g_drag_cache_w = w;
    g_drag_cache_h = h;
    const uint32_t *src = g_fb + y * g_pitch4 + x;
    uint32_t       *dst = g_drag_cache;
    for (row = 0; row < h; row++) {
        uint32_t        n = (uint32_t)w;
        const uint32_t *s = src;
        uint32_t       *d = dst;
        __asm__ volatile ("rep movsl" : "+S"(s), "+D"(d), "+c"(n) :: "memory");
        src += g_pitch4;
        dst += w;
    }
}

/* Blit the cached window content snapshot onto the back buffer at (dx, dy).
 * dx/dy are the window's current content-area top-left (WIN_CX, WIN_CY).
 * Replaces draw_content() during drag — a single rep-movsl per row instead
 * of 1628 gfx_draw_char() function calls. */
void gfx_dragstamp(int dx, int dy)
{
    int row;
    if (g_drag_cache_w <= 0 || g_drag_cache_h <= 0) return;
    int x = dx, y = dy, w = g_drag_cache_w, h = g_drag_cache_h;
    /* Clip */
    int sx = 0, sy = 0;
    if (x < 0) { sx = -x; w += x; x = 0; }
    if (y < 0) { sy = -y; h += y; y = 0; }
    if (x + w > g_width)   w = g_width  - x;
    if (y + h > g_height)  h = g_height - y;
    if (w <= 0 || h <= 0) return;
    const uint32_t *src = g_drag_cache + sy * g_drag_cache_w + sx;
    uint32_t       *dst = g_fb + y * g_pitch4 + x;
    for (row = 0; row < h; row++) {
        uint32_t        n = (uint32_t)w;
        const uint32_t *s = src;
        uint32_t       *d = dst;
        __asm__ volatile ("rep movsl" : "+S"(s), "+D"(d), "+c"(n) :: "memory");
        src += g_drag_cache_w;
        dst += g_pitch4;
    }
}

int gfx_dragcache_valid(void) { return g_drag_cache_w > 0; }

/* Corner pixel save/restore for true transparent rounded corners.
 * Iterates the same set of pixels as gfx_round_corners but reads/writes g_fb.
 * buf must hold at least 4*r*r uint32_t elements. */
void gfx_save_corners(int x, int y, int w, int h, int r, uint32_t *buf)
{
    int px, py, r2 = r*r, n = 0;
    for (py=y; py<y+r; py++) for (px=x; px<x+r; px++) {
        int dx=px-(x+r), dy=py-(y+r);
        if (dx*dx+dy*dy > r2 && px>=0 && px<g_width && py>=0 && py<g_height)
            buf[n++] = g_fb[py*g_pitch4+px];
    }
    for (py=y; py<y+r; py++) for (px=x+w-r; px<x+w; px++) {
        int dx=px-(x+w-1-r), dy=py-(y+r);
        if (dx*dx+dy*dy > r2 && px>=0 && px<g_width && py>=0 && py<g_height)
            buf[n++] = g_fb[py*g_pitch4+px];
    }
    for (py=y+h-r; py<y+h; py++) for (px=x; px<x+r; px++) {
        int dx=px-(x+r), dy=py-(y+h-1-r);
        if (dx*dx+dy*dy > r2 && px>=0 && px<g_width && py>=0 && py<g_height)
            buf[n++] = g_fb[py*g_pitch4+px];
    }
    for (py=y+h-r; py<y+h; py++) for (px=x+w-r; px<x+w; px++) {
        int dx=px-(x+w-1-r), dy=py-(y+h-1-r);
        if (dx*dx+dy*dy > r2 && px>=0 && px<g_width && py>=0 && py<g_height)
            buf[n++] = g_fb[py*g_pitch4+px];
    }
}

/* Must iterate in identical order to gfx_save_corners. */
void gfx_restore_corners(int x, int y, int w, int h, int r, const uint32_t *buf)
{
    int px, py, r2 = r*r, n = 0;
    for (py=y; py<y+r; py++) for (px=x; px<x+r; px++) {
        int dx=px-(x+r), dy=py-(y+r);
        if (dx*dx+dy*dy > r2 && px>=0 && px<g_width && py>=0 && py<g_height)
            g_fb[py*g_pitch4+px] = buf[n++];
    }
    for (py=y; py<y+r; py++) for (px=x+w-r; px<x+w; px++) {
        int dx=px-(x+w-1-r), dy=py-(y+r);
        if (dx*dx+dy*dy > r2 && px>=0 && px<g_width && py>=0 && py<g_height)
            g_fb[py*g_pitch4+px] = buf[n++];
    }
    for (py=y+h-r; py<y+h; py++) for (px=x; px<x+r; px++) {
        int dx=px-(x+r), dy=py-(y+h-1-r);
        if (dx*dx+dy*dy > r2 && px>=0 && px<g_width && py>=0 && py<g_height)
            g_fb[py*g_pitch4+px] = buf[n++];
    }
    for (py=y+h-r; py<y+h; py++) for (px=x+w-r; px<x+w; px++) {
        int dx=px-(x+w-1-r), dy=py-(y+h-1-r);
        if (dx*dx+dy*dy > r2 && px>=0 && px<g_width && py>=0 && py<g_height)
            g_fb[py*g_pitch4+px] = buf[n++];
    }
}

/* Punch rounded corners from bg-cache (legacy: single static background). */
void gfx_round_corners(int x, int y, int w, int h, int r)
{
    int px, py, r2 = r * r;
    /* Top-left: circle center (x+r, y+r) */
    for (py = y; py < y + r; py++)
        for (px = x; px < x + r; px++) {
            int dx = px-(x+r), dy = py-(y+r);
            if (dx*dx+dy*dy > r2 && px>=0 && px<g_width && py>=0 && py<g_height)
                g_fb[py*g_pitch4+px] = g_bg_cache[py*g_pitch4+px];
        }
    /* Top-right: circle center (x+w-1-r, y+r) */
    for (py = y; py < y + r; py++)
        for (px = x+w-r; px < x+w; px++) {
            int dx = px-(x+w-1-r), dy = py-(y+r);
            if (dx*dx+dy*dy > r2 && px>=0 && px<g_width && py>=0 && py<g_height)
                g_fb[py*g_pitch4+px] = g_bg_cache[py*g_pitch4+px];
        }
    /* Bottom-left: circle center (x+r, y+h-1-r) */
    for (py = y+h-r; py < y+h; py++)
        for (px = x; px < x+r; px++) {
            int dx = px-(x+r), dy = py-(y+h-1-r);
            if (dx*dx+dy*dy > r2 && px>=0 && px<g_width && py>=0 && py<g_height)
                g_fb[py*g_pitch4+px] = g_bg_cache[py*g_pitch4+px];
        }
    /* Bottom-right: circle center (x+w-1-r, y+h-1-r) */
    for (py = y+h-r; py < y+h; py++)
        for (px = x+w-r; px < x+w; px++) {
            int dx = px-(x+w-1-r), dy = py-(y+h-1-r);
            if (dx*dx+dy*dy > r2 && px>=0 && px<g_width && py>=0 && py<g_height)
                g_fb[py*g_pitch4+px] = g_bg_cache[py*g_pitch4+px];
        }
}

/* Tracks the union of all regions that changed this frame.
 * desktop_present() uses this to decide whether to do a partial or full
 * render + flip.  Call gfx_dirty_reset() after presenting. */

static int g_dirty_x1 = 0, g_dirty_y1 = 0;
static int g_dirty_x2 = 0, g_dirty_y2 = 0;
static int g_dirty_used = 0;

void gfx_dirty_reset(void)
{
    g_dirty_x1 = g_dirty_y1 = g_dirty_x2 = g_dirty_y2 = 0;
    g_dirty_used = 0;
}

/* Expand the dirty union rect to include (x,y,w,h). */
void gfx_dirty_mark(int x, int y, int w, int h)
{
    int x2 = x + w, y2 = y + h;
    if (x  < 0)        x  = 0;
    if (y  < 0)        y  = 0;
    if (x2 > g_width)  x2 = g_width;
    if (y2 > g_height) y2 = g_height;
    if (x2 <= x || y2 <= y) return;
    if (!g_dirty_used) {
        g_dirty_x1 = x;  g_dirty_y1 = y;
        g_dirty_x2 = x2; g_dirty_y2 = y2;
        g_dirty_used = 1;
    } else {
        if (x  < g_dirty_x1) g_dirty_x1 = x;
        if (y  < g_dirty_y1) g_dirty_y1 = y;
        if (x2 > g_dirty_x2) g_dirty_x2 = x2;
        if (y2 > g_dirty_y2) g_dirty_y2 = y2;
    }
}

/* Return the accumulated dirty rect.  Returns 1 if any dirty, 0 if none. */
int gfx_dirty_get(int *x, int *y, int *w, int *h)
{
    if (!g_dirty_used) return 0;
    *x = g_dirty_x1;
    *y = g_dirty_y1;
    *w = g_dirty_x2 - g_dirty_x1;
    *h = g_dirty_y2 - g_dirty_y1;
    return 1;
}

void gfx_flip(void)
{
    /* Row-by-row blit: back buffer has tight rows (pitch = width), but the
     * real VBE framebuffer may have a wider hardware pitch.  Copying a flat
     * blob would shift each row right by (fb_pitch - width) pixels, producing
     * the off-center / striped artifact seen on hardware. */
    int row;
    const uint32_t *src = g_fb;
    uint32_t       *dst = g_fb_real;
    for (row = 0; row < g_height; row++) {
        uint32_t        n   = (uint32_t)g_width;
        const uint32_t *s   = src;
        uint32_t       *d   = dst;
        __asm__ volatile ("rep movsl" : "+S"(s), "+D"(d), "+c"(n) :: "memory");
        src += g_width;      /* back buffer: tight rows                     */
        dst += g_fb_pitch4;  /* real FB:     hardware-aligned rows           */
    }
}

int gfx_width(void)  { return g_width;  }
int gfx_height(void) { return g_height; }

/* Switch VBE mode via Bochs BGA I/O ports (accessible from ring 0). */
static inline void vbe_reg_write(uint16_t idx, uint16_t val)
{
    __asm__ volatile("outw %0, %1" :: "a"(idx), "Nd"((uint16_t)0x01CE));
    __asm__ volatile("outw %0, %1" :: "a"(val), "Nd"((uint16_t)0x01CF));
}

void gfx_set_resolution(int w, int h)
{
    vbe_reg_write(4, 0);              /* disable VBE        */
    vbe_reg_write(1, (uint16_t)w);   /* set width          */
    vbe_reg_write(2, (uint16_t)h);   /* set height         */
    vbe_reg_write(3, 32);            /* bpp = 32           */
    vbe_reg_write(4, 0x41);          /* enable + LFB       */
    g_width     = w;
    g_height    = h;
    g_pitch4    = w;
    g_fb_pitch4 = w;
    /* Clear back buffer so old-stride pixels don't bleed into the new frame. */
    { uint32_t n = (uint32_t)w * (uint32_t)h, *p = g_fb;
      __asm__ volatile ("rep stosl" : "+D"(p), "+c"(n) : "a"(0u) : "memory"); }
}

/* Copy only a clipped w×h rectangle from back buffer to the real FB.
 * Used to update just the cursor region — ~500 pixels instead of 3 MB. */
void gfx_flip_rect(int x, int y, int w, int h)
{
    int row;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > g_width)  w = g_width  - x;
    if (y + h > g_height) h = g_height - y;
    if (w <= 0 || h <= 0) return;

    const uint32_t *src = g_fb      + y * g_pitch4    + x;
    uint32_t       *dst = g_fb_real + y * g_fb_pitch4 + x;
    for (row = 0; row < h; row++) {
        uint32_t        n = (uint32_t)w;
        const uint32_t *s = src;
        uint32_t       *d = dst;
        __asm__ volatile ("rep movsl" : "+S"(s), "+D"(d), "+c"(n) :: "memory");
        src += g_pitch4;
        dst += g_fb_pitch4;
    }
}

/* Atomic zero-flicker cursor update.
 *
 * Back buffer must be cursor-free on entry; it is cursor-free on exit.
 *
 * Algorithm:
 *  1. Save back-buffer pixels under the NEW position into |save|.
 *  2. Stamp the cursor sprite (masked) into the back buffer at (nx,ny).
 *  3. Compute the union bounding-box of the old rect (ox,oy) and the new
 *     rect (nx,ny), then blit that union to the real FB in ONE pass.
 *     Because the back buffer at the old position is cursor-free (clean bg)
 *     and at the new position has the cursor stamped, the real FB transitions
 *     from "cursor here" to "cursor there" in a single scan — no intermediate
 *     frame where the cursor is invisible.
 *  4. Restore the back buffer at (nx,ny) from |save| so it stays cursor-free.
 *
 * sprite: w*h pixel array (only opaque entries are used).
 * mask:   w*h byte array; 1 = opaque pixel, 0 = transparent.
 * save:   caller-allocated w*h uint32_t scratch buffer. */

/* Use PNG-sourced cursor data (generated by kernel/PNG/gen_cursor.py). */
void gfx_cursor_build_arrow(uint32_t *sprite, uint8_t *mask, int w, int h)
{
    int r, c;
    for (r = 0; r < h; r++)
        for (c = 0; c < w; c++) { sprite[r*w+c] = 0; mask[r*w+c] = 0; }
    for (r = 0; r < h && r < PD_CURSOR_H; r++)
        for (c = 0; c < w && c < PD_CURSOR_W; c++) {
            sprite[r*w+c] = pd_cursor_sprite[r*PD_CURSOR_W+c];
            mask[r*w+c]   = pd_cursor_mask[r*PD_CURSOR_W+c];
        }
}

void gfx_cursor_blit(int ox, int oy, int nx, int ny, int w, int h,
                     const uint32_t *sprite, const uint8_t *mask, uint32_t *save)
{
    int r, c;

    /* Step 1: save back-buffer region under new cursor position */
    for (r = 0; r < h; r++) {
        int py = ny + r;
        for (c = 0; c < w; c++) {
            int px = nx + c;
            save[r * w + c] = (px >= 0 && px < g_width && py >= 0 && py < g_height)
                              ? g_fb[py * g_pitch4 + px] : 0u;
        }
    }

    /* Step 2: stamp cursor onto back buffer at new position */
    for (r = 0; r < h; r++) {
        int py = ny + r;
        if (py < 0 || py >= g_height) continue;
        for (c = 0; c < w; c++) {
            if (!mask[r * w + c]) continue;
            int px = nx + c;
            if (px >= 0 && px < g_width)
                g_fb[py * g_pitch4 + px] = sprite[r * w + c];
        }
    }

    /* Step 3: blit the union rect — single contiguous write to the real FB */
    {
        int ux  = ox < nx ? ox : nx;
        int uy  = oy < ny ? oy : ny;
        int ux2 = (ox + w) > (nx + w) ? (ox + w) : (nx + w);
        int uy2 = (oy + h) > (ny + h) ? (oy + h) : (ny + h);
        gfx_flip_rect(ux, uy, ux2 - ux, uy2 - uy);
    }

    /* Step 4: restore back buffer to cursor-free state */
    for (r = 0; r < h; r++) {
        int py = ny + r;
        if (py < 0 || py >= g_height) continue;
        for (c = 0; c < w; c++) {
            if (!mask[r * w + c]) continue;
            int px = nx + c;
            if (px >= 0 && px < g_width)
                g_fb[py * g_pitch4 + px] = save[r * w + c];
        }
    }
}

/* ---- Primitives ---------------------------------------------------------- */

void gfx_putpixel(int x, int y, uint32_t color)
{
    if (gfx_clip(x, y)) gfx_wr(x, y, color);
}

uint32_t gfx_getpixel(int x, int y)
{
    if (gfx_clip(x, y)) return gfx_rd(x, y);
    return 0;
}

void gfx_fill_rect(int x, int y, int w, int h, uint32_t color)
{
    int r;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > g_width)  w = g_width  - x;
    if (y + h > g_height) h = g_height - y;
    if (w <= 0 || h <= 0) return;

    for (r = y; r < y + h; r++) {
        uint32_t *dst = g_fb + r * g_pitch4 + x;
        uint32_t  cnt = (uint32_t)w;
        __asm__ volatile ("rep stosl" : "+D"(dst), "+c"(cnt) : "a"(color) : "memory");
    }
}

void gfx_draw_rect(int x, int y, int w, int h, uint32_t color)
{
    gfx_hline(x,         y,         w, color);
    gfx_hline(x,         y + h - 1, w, color);
    gfx_vline(x,         y,         h, color);
    gfx_vline(x + w - 1, y,         h, color);
}

void gfx_hline(int x, int y, int len, uint32_t color)
{
    if (y < 0 || y >= g_height) return;
    if (x < 0) { len += x; x = 0; }
    if (x + len > g_width) len = g_width - x;
    if (len <= 0) return;
    uint32_t *dst = g_fb + y * g_pitch4 + x;
    uint32_t  cnt = (uint32_t)len;
    __asm__ volatile ("rep stosl" : "+D"(dst), "+c"(cnt) : "a"(color) : "memory");
}

void gfx_vline(int x, int y, int len, uint32_t color)
{
    int i;
    if (x < 0 || x >= g_width) return;
    if (y < 0) { len += y; y = 0; }
    if (y + len > g_height) len = g_height - y;
    if (len <= 0) return;
    for (i = 0; i < len; i++) gfx_wr(x, y + i, color);
}

void gfx_fill_rect_grad(int x, int y, int w, int h,
                         uint32_t top_color, uint32_t bot_color)
{
    int r;
    if (h <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (x + w > g_width) w = g_width - x;
    if (w <= 0) return;

    int tr = (int)((top_color >> 16) & 0xFF);
    int tg = (int)((top_color >>  8) & 0xFF);
    int tb = (int)( top_color        & 0xFF);
    int dr = (int)((bot_color >> 16) & 0xFF) - tr;
    int dg = (int)((bot_color >>  8) & 0xFF) - tg;
    int db = (int)( bot_color        & 0xFF) - tb;

    /* Fixed-point 16.16 incremental interpolation — no per-row division */
    int cr_acc = tr << 16, cg_acc = tg << 16, cb_acc = tb << 16;
    int cr_step = (h > 1) ? (dr << 16) / (h - 1) : 0;
    int cg_step = (h > 1) ? (dg << 16) / (h - 1) : 0;
    int cb_step = (h > 1) ? (db << 16) / (h - 1) : 0;

    for (r = 0; r < h; r++, cr_acc += cr_step, cg_acc += cg_step, cb_acc += cb_step) {
        int cy = y + r;
        if (cy < 0 || cy >= g_height) continue;
        uint32_t color = ((uint32_t)((cr_acc >> 16) & 0xFF) << 16) |
                         ((uint32_t)((cg_acc >> 16) & 0xFF) <<  8) |
                          (uint32_t)((cb_acc >> 16) & 0xFF);
        uint32_t *dst = g_fb + cy * g_pitch4 + x;
        uint32_t  cnt = (uint32_t)w;
        __asm__ volatile ("rep stosl" : "+D"(dst), "+c"(cnt) : "a"(color) : "memory");
    }
}

/* ---- Text ---------------------------------------------------------------- */

void gfx_draw_char(int x, int y, char c, uint32_t fg, uint32_t bg)
{
    int row, col;
    uint8_t uc = (uint8_t)c;
    /* Each glyph: 16 bytes, 1 byte per row, MSB = leftmost pixel */
    const uint8_t *glyph = g_font + (int)uc * GFX_CHAR_H;
    for (row = 0; row < GFX_CHAR_H; row++) {
        uint8_t bits = glyph[row];
        for (col = 0; col < GFX_CHAR_W; col++) {
            int px = x + col;
            int py = y + row;
            if (!gfx_clip(px, py)) continue;
            if (bits & (0x80u >> col))
                gfx_wr(px, py, fg);
            else
                gfx_wr(px, py, bg);
        }
    }
}

void gfx_draw_string(int x, int y, const char *s,
                      uint32_t fg, uint32_t bg, int transparent_bg)
{
    int cx = x;
    while (*s) {
        if (transparent_bg && *s == ' ') {
            cx += GFX_CHAR_W;
        } else if (transparent_bg) {
            /* Draw char without overwriting background */
            int row, col;
            uint8_t uc = (uint8_t)*s;
            const uint8_t *glyph = g_font + uc * GFX_CHAR_H;
            for (row = 0; row < GFX_CHAR_H; row++) {
                uint8_t bits = glyph[row];
                for (col = 0; col < GFX_CHAR_W; col++) {
                    if (bits & (0x80u >> col)) {
                        int px = cx + col, py = y + row;
                        if (gfx_clip(px, py)) gfx_wr(px, py, fg);
                    }
                }
            }
            cx += GFX_CHAR_W;
        } else {
            gfx_draw_char(cx, y, *s, fg, bg);
            cx += GFX_CHAR_W;
        }
        s++;
    }
}

void gfx_draw_string_n(int x, int y, const char *s, int len,
                        uint32_t fg, uint32_t bg, int transparent_bg)
{
    int i, cx = x;
    for (i = 0; i < len && s[i]; i++) {
        if (transparent_bg) {
            int row, col;
            uint8_t uc = (uint8_t)s[i];
            const uint8_t *glyph = g_font + uc * GFX_CHAR_H;
            for (row = 0; row < GFX_CHAR_H; row++) {
                uint8_t bits = glyph[row];
                for (col = 0; col < GFX_CHAR_W; col++) {
                    if (bits & (0x80u >> col)) {
                        int px = cx + col, py = y + row;
                        if (gfx_clip(px, py)) gfx_wr(px, py, fg);
                    }
                }
            }
        } else {
            gfx_draw_char(cx, y, s[i], fg, bg);
        }
        cx += GFX_CHAR_W;
    }
}

int gfx_string_w(const char *s)
{
    int n = 0;
    while (*s++) n++;
    return n * GFX_CHAR_W;
}

int gfx_string_w_n(const char *s, int len)
{
    int n = 0;
    while (n < len && s[n]) n++;
    return n * GFX_CHAR_W;
}

/* ---- Region save/restore ------------------------------------------------- */

void gfx_save_region(int x, int y, int w, int h, uint32_t *dst)
{
    int r;
    for (r = 0; r < h; r++) {
        int py = y + r;
        if (py < 0 || py >= g_height) continue;
        int cx = x, cw = w;
        if (cx < 0) { cw += cx; cx = 0; }
        if (cx + cw > g_width) cw = g_width - cx;
        if (cw <= 0) continue;
        const uint32_t *s = g_fb + py * g_pitch4 + cx;
        uint32_t       *d = dst  + r  * w        + (cx - x);
        uint32_t        n = (uint32_t)cw;
        __asm__ volatile ("rep movsl" : "+S"(s), "+D"(d), "+c"(n) :: "memory");
    }
}

void gfx_restore_region(int x, int y, int w, int h, const uint32_t *src)
{
    int r;
    for (r = 0; r < h; r++) {
        int py = y + r;
        if (py < 0 || py >= g_height) continue;
        int cx = x, cw = w;
        if (cx < 0) { cw += cx; cx = 0; }
        if (cx + cw > g_width) cw = g_width - cx;
        if (cw <= 0) continue;
        const uint32_t *s = src  + r  * w        + (cx - x);
        uint32_t       *d = g_fb + py * g_pitch4  + cx;
        uint32_t        n = (uint32_t)cw;
        __asm__ volatile ("rep movsl" : "+S"(s), "+D"(d), "+c"(n) :: "memory");
    }
}

/* ---- Alpha blend --------------------------------------------------------- */

void gfx_blend_pixel(int x, int y, uint32_t color, uint8_t alpha)
{
    if (!gfx_clip(x, y) || alpha == 0) return;
    if (alpha == 255) { gfx_wr(x, y, color); return; }
    uint32_t dst = gfx_rd(x, y);
    uint8_t  dr = (uint8_t)((dst >> 16) & 0xFF);
    uint8_t  dg = (uint8_t)((dst >>  8) & 0xFF);
    uint8_t  db = (uint8_t)( dst        & 0xFF);
    uint8_t  sr = (uint8_t)((color >> 16) & 0xFF);
    uint8_t  sg = (uint8_t)((color >>  8) & 0xFF);
    uint8_t  sb = (uint8_t)( color        & 0xFF);
    uint32_t a  = (uint32_t)alpha;
    uint32_t ia = 255u - a;
    uint32_t or2 = (sr * a + dr * ia) / 255u;
    uint32_t og  = (sg * a + dg * ia) / 255u;
    uint32_t ob  = (sb * a + db * ia) / 255u;
    gfx_wr(x, y, (or2 << 16) | (og << 8) | ob);
}
