/*
 * gfx.c — VESA Linear Framebuffer driver
 *
 * Graphics mode is set using the Bochs VBE interface (I/O ports 0x01CE/0x01CF).
 * This works in protected mode with no BIOS calls, making it safe to call
 * after IDT init.  PCI is scanned to find the VGA BAR0 (LFB base address).
 *
 * Rounded-rectangle corner algorithm: for each corner of radius r, pixels
 * within the bounding r×r square are tested with an integer circle check
 * (dx*dx + dy*dy > r*r → outside → skip).
 */

#include "gfx.h"
#include "boot_params.h"
#include "paging.h"
#include "kernel.h"

/* ------------------------------------------------------------------ */
/* 8×8 VGA-style bitmap font, ASCII 0x20-0x7E                         */
/* Each character is 8 bytes (one byte per row, MSB = leftmost pixel) */
/* ------------------------------------------------------------------ */
static const uint8_t g_font8[95][8] = {
/* 0x20 space */ {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
/* 0x21 ! */     {0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00},
/* 0x22 " */     {0x66,0x66,0x24,0x00,0x00,0x00,0x00,0x00},
/* 0x23 # */     {0x6C,0x6C,0xFE,0x6C,0xFE,0x6C,0x6C,0x00},
/* 0x24 $ */     {0x18,0x3E,0x60,0x3C,0x06,0x7C,0x18,0x00},
/* 0x25 % */     {0x00,0xC6,0xCC,0x18,0x30,0x66,0xC6,0x00},
/* 0x26 & */     {0x38,0x6C,0x38,0x76,0xDC,0xCC,0x76,0x00},
/* 0x27 ' */     {0x18,0x18,0x30,0x00,0x00,0x00,0x00,0x00},
/* 0x28 ( */     {0x0C,0x18,0x30,0x30,0x30,0x18,0x0C,0x00},
/* 0x29 ) */     {0x30,0x18,0x0C,0x0C,0x0C,0x18,0x30,0x00},
/* 0x2A * */     {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00},
/* 0x2B + */     {0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00},
/* 0x2C , */     {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x30},
/* 0x2D - */     {0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00},
/* 0x2E . */     {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00},
/* 0x2F / */     {0x06,0x0C,0x18,0x30,0x60,0xC0,0x80,0x00},
/* 0x30 0 */     {0x7C,0xC6,0xCE,0xDE,0xF6,0xE6,0x7C,0x00},
/* 0x31 1 */     {0x18,0x38,0x18,0x18,0x18,0x18,0x7E,0x00},
/* 0x32 2 */     {0x7C,0xC6,0x06,0x1C,0x30,0x60,0xFE,0x00},
/* 0x33 3 */     {0x7C,0xC6,0x06,0x3C,0x06,0xC6,0x7C,0x00},
/* 0x34 4 */     {0x1C,0x3C,0x6C,0xCC,0xFE,0x0C,0x1E,0x00},
/* 0x35 5 */     {0xFE,0xC0,0xC0,0xFC,0x06,0xC6,0x7C,0x00},
/* 0x36 6 */     {0x38,0x60,0xC0,0xFC,0xC6,0xC6,0x7C,0x00},
/* 0x37 7 */     {0xFE,0xC6,0x0C,0x18,0x30,0x30,0x30,0x00},
/* 0x38 8 */     {0x7C,0xC6,0xC6,0x7C,0xC6,0xC6,0x7C,0x00},
/* 0x39 9 */     {0x7C,0xC6,0xC6,0x7E,0x06,0x0C,0x78,0x00},
/* 0x3A : */     {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x00},
/* 0x3B ; */     {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x30},
/* 0x3C < */     {0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0x00},
/* 0x3D = */     {0x00,0x00,0x7E,0x00,0x00,0x7E,0x00,0x00},
/* 0x3E > */     {0x60,0x30,0x18,0x0C,0x18,0x30,0x60,0x00},
/* 0x3F ? */     {0x7C,0xC6,0x0C,0x18,0x18,0x00,0x18,0x00},
/* 0x40 @ */     {0x7C,0xC6,0xDE,0xDE,0xDE,0xC0,0x7C,0x00},
/* 0x41 A */     {0x10,0x38,0x6C,0xC6,0xFE,0xC6,0xC6,0x00},
/* 0x42 B */     {0xFC,0x66,0x66,0x7C,0x66,0x66,0xFC,0x00},
/* 0x43 C */     {0x3C,0x66,0xC0,0xC0,0xC0,0x66,0x3C,0x00},
/* 0x44 D */     {0xF8,0x6C,0x66,0x66,0x66,0x6C,0xF8,0x00},
/* 0x45 E */     {0xFE,0x62,0x68,0x78,0x68,0x62,0xFE,0x00},
/* 0x46 F */     {0xFE,0x62,0x68,0x78,0x68,0x60,0xF0,0x00},
/* 0x47 G */     {0x3C,0x66,0xC0,0xC0,0xCE,0x66,0x3E,0x00},
/* 0x48 H */     {0xC6,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0x00},
/* 0x49 I */     {0x3C,0x18,0x18,0x18,0x18,0x18,0x3C,0x00},
/* 0x4A J */     {0x1E,0x0C,0x0C,0x0C,0xCC,0xCC,0x78,0x00},
/* 0x4B K */     {0xE6,0x66,0x6C,0x78,0x6C,0x66,0xE6,0x00},
/* 0x4C L */     {0xF0,0x60,0x60,0x60,0x62,0x66,0xFE,0x00},
/* 0x4D M */     {0xC6,0xEE,0xFE,0xFE,0xD6,0xC6,0xC6,0x00},
/* 0x4E N */     {0xC6,0xE6,0xF6,0xDE,0xCE,0xC6,0xC6,0x00},
/* 0x4F O */     {0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00},
/* 0x50 P */     {0xFC,0x66,0x66,0x7C,0x60,0x60,0xF0,0x00},
/* 0x51 Q */     {0x7C,0xC6,0xC6,0xC6,0xC6,0xCE,0x7C,0x0E},
/* 0x52 R */     {0xFC,0x66,0x66,0x7C,0x6C,0x66,0xE6,0x00},
/* 0x53 S */     {0x7C,0xC6,0xC6,0x70,0x1C,0xC6,0x7C,0x00},
/* 0x54 T */     {0x7E,0x7E,0x5A,0x18,0x18,0x18,0x3C,0x00},
/* 0x55 U */     {0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00},
/* 0x56 V */     {0xC6,0xC6,0xC6,0xC6,0xC6,0x6C,0x38,0x00},
/* 0x57 W */     {0xC6,0xC6,0xC6,0xD6,0xFE,0xEE,0xC6,0x00},
/* 0x58 X */     {0xC6,0xC6,0x6C,0x38,0x38,0x6C,0xC6,0x00},
/* 0x59 Y */     {0x66,0x66,0x66,0x3C,0x18,0x18,0x3C,0x00},
/* 0x5A Z */     {0xFE,0xC6,0x8C,0x18,0x32,0x66,0xFE,0x00},
/* 0x5B [ */     {0x3C,0x30,0x30,0x30,0x30,0x30,0x3C,0x00},
/* 0x5C \ */     {0x80,0xC0,0x60,0x30,0x18,0x0C,0x06,0x00},
/* 0x5D ] */     {0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0x00},
/* 0x5E ^ */     {0x10,0x38,0x6C,0xC6,0x00,0x00,0x00,0x00},
/* 0x5F _ */     {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF},
/* 0x60 ` */     {0x30,0x18,0x0C,0x00,0x00,0x00,0x00,0x00},
/* 0x61 a */     {0x00,0x00,0x78,0x0C,0x7C,0xCC,0x76,0x00},
/* 0x62 b */     {0xE0,0x60,0x60,0x7C,0x66,0x66,0xDC,0x00},
/* 0x63 c */     {0x00,0x00,0x7C,0xC6,0xC0,0xC6,0x7C,0x00},
/* 0x64 d */     {0x1C,0x0C,0x0C,0x7C,0xCC,0xCC,0x76,0x00},
/* 0x65 e */     {0x00,0x00,0x7C,0xC6,0xFE,0xC0,0x7C,0x00},
/* 0x66 f */     {0x1C,0x36,0x30,0x78,0x30,0x30,0x78,0x00},
/* 0x67 g */     {0x00,0x00,0x76,0xCC,0xCC,0x7C,0x0C,0xF8},
/* 0x68 h */     {0xE0,0x60,0x6C,0x76,0x66,0x66,0xE6,0x00},
/* 0x69 i */     {0x18,0x00,0x38,0x18,0x18,0x18,0x3C,0x00},
/* 0x6A j */     {0x06,0x00,0x06,0x06,0x06,0x66,0x66,0x3C},
/* 0x6B k */     {0xE0,0x60,0x66,0x6C,0x78,0x6C,0xE6,0x00},
/* 0x6C l */     {0x38,0x18,0x18,0x18,0x18,0x18,0x3C,0x00},
/* 0x6D m */     {0x00,0x00,0xEC,0xFE,0xD6,0xD6,0xC6,0x00},
/* 0x6E n */     {0x00,0x00,0xDC,0x66,0x66,0x66,0x66,0x00},
/* 0x6F o */     {0x00,0x00,0x7C,0xC6,0xC6,0xC6,0x7C,0x00},
/* 0x70 p */     {0x00,0x00,0xDC,0x66,0x66,0x7C,0x60,0xF0},
/* 0x71 q */     {0x00,0x00,0x76,0xCC,0xCC,0x7C,0x0C,0x1E},
/* 0x72 r */     {0x00,0x00,0xDC,0x76,0x66,0x60,0xF0,0x00},
/* 0x73 s */     {0x00,0x00,0x7C,0xC0,0x7C,0x06,0x7C,0x00},
/* 0x74 t */     {0x10,0x30,0x7C,0x30,0x30,0x36,0x1C,0x00},
/* 0x75 u */     {0x00,0x00,0xCC,0xCC,0xCC,0xCC,0x76,0x00},
/* 0x76 v */     {0x00,0x00,0xC6,0xC6,0xC6,0x6C,0x38,0x00},
/* 0x77 w */     {0x00,0x00,0xC6,0xC6,0xD6,0xFE,0x6C,0x00},
/* 0x78 x */     {0x00,0x00,0xC6,0x6C,0x38,0x6C,0xC6,0x00},
/* 0x79 y */     {0x00,0x00,0xCC,0xCC,0xCC,0x7C,0x0C,0xF8},
/* 0x7A z */     {0x00,0x00,0xFE,0x98,0x30,0x62,0xFE,0x00},
/* 0x7B { */     {0x0E,0x18,0x18,0x70,0x18,0x18,0x0E,0x00},
/* 0x7C | */     {0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00},
/* 0x7D } */     {0x70,0x18,0x18,0x0E,0x18,0x18,0x70,0x00},
/* 0x7E ~ */     {0x76,0xDC,0x00,0x00,0x00,0x00,0x00,0x00},
};

/* ------------------------------------------------------------------ */
/* Driver state                                                         */
/* ------------------------------------------------------------------ */
static uint8_t  *g_fb     = 0;   /* mapped framebuffer base           */
static int       g_width  = 0;
static int       g_height = 0;
static int       g_pitch  = 0;   /* bytes per row                     */
static int       g_bpp    = 0;   /* bits per pixel (16/24/32)         */
static int       g_active = 0;   /* 1 after successful gfx_init       */

/* Text cursor for gfx_putchar (glyph-aligned, not pixel) */
static int       g_tx = 0;       /* column in 8-px units              */
static int       g_ty = 0;       /* row    in 8-px units              */

/* ------------------------------------------------------------------ */
/* Pixel write (bpp-aware)                                             */
/* ------------------------------------------------------------------ */
static inline void put_pixel(int x, int y, gfx_color_t c)
{
    if ((unsigned)x >= (unsigned)g_width ||
        (unsigned)y >= (unsigned)g_height) return;

    uint8_t *p = g_fb + (uint32_t)y * (uint32_t)g_pitch
                      + (uint32_t)x * ((uint32_t)g_bpp >> 3);

    if (g_bpp == 32) {
        *(uint32_t *)p = c;
    } else if (g_bpp == 24) {
        p[0] = GFX_B(c);
        p[1] = GFX_G(c);
        p[2] = GFX_R(c);
    } else if (g_bpp == 16) {
        /* RGB-565 */
        uint16_t rgb = (uint16_t)(((GFX_R(c) >> 3) << 11) |
                                   ((GFX_G(c) >> 2) <<  5) |
                                    (GFX_B(c) >> 3));
        *(uint16_t *)p = rgb;
    }
}

gfx_color_t gfx_read_pixel(int x, int y)
{
    if (!g_fb) return 0;
    if ((unsigned)x >= (unsigned)g_width ||
        (unsigned)y >= (unsigned)g_height) return 0;
    uint8_t *p = g_fb + (uint32_t)y * (uint32_t)g_pitch
                      + (uint32_t)x * ((uint32_t)g_bpp >> 3);
    if (g_bpp == 32) return *(uint32_t *)p & 0x00FFFFFFu;
    if (g_bpp == 24) return GFX_RGB(p[2], p[1], p[0]);
    if (g_bpp == 16) {
        uint16_t v = *(uint16_t *)p;
        return GFX_RGB(((v >> 11) & 0x1F) << 3,
                       ((v >>  5) & 0x3F) << 2,
                        (v        & 0x1F) << 3);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Bochs VBE I/O helpers (ports 0x01CE / 0x01CF)                      */
/* These work in 32-bit protected mode — no BIOS call needed.         */
/* ------------------------------------------------------------------ */
#define VBE_PORT_INDEX  0x01CE
#define VBE_PORT_DATA   0x01CF

static inline void gfx_outw(uint16_t port, uint16_t val)
{
    __asm__ volatile ("outw %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint16_t gfx_inw(uint16_t port)
{
    uint16_t r;
    __asm__ volatile ("inw %1, %0" : "=a"(r) : "Nd"(port));
    return r;
}
static inline void gfx_outl(uint16_t port, uint32_t val)
{
    __asm__ volatile ("outl %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint32_t gfx_inl(uint16_t port)
{
    uint32_t r;
    __asm__ volatile ("inl %1, %0" : "=a"(r) : "Nd"(port));
    return r;
}

static void bochs_write(uint16_t idx, uint16_t val)
{
    gfx_outw(VBE_PORT_INDEX, idx);
    gfx_outw(VBE_PORT_DATA,  val);
}
static uint16_t bochs_read(uint16_t idx)
{
    gfx_outw(VBE_PORT_INDEX, idx);
    return gfx_inw(VBE_PORT_DATA);
}

/* ------------------------------------------------------------------ */
/* PCI scan — find VGA device BAR0 (LFB base address)                 */
/* ------------------------------------------------------------------ */
static uint32_t pci_cfg_read(uint8_t dev, uint8_t reg)
{
    uint32_t addr = (1u << 31) | ((uint32_t)dev << 11) | (reg & 0xFCu);
    gfx_outl(0xCF8, addr);
    return gfx_inl(0xCFC);
}

static uint32_t find_vga_lfb(void)
{
    int dev;
    for (dev = 0; dev < 32; dev++) {
        uint32_t class_rev = pci_cfg_read((uint8_t)dev, 0x08);
        if ((class_rev >> 24) == 0x03) {          /* display controller */
            uint32_t bar0 = pci_cfg_read((uint8_t)dev, 0x10);
            if (!(bar0 & 0x1u) && (bar0 & ~0xFu)) /* MMIO, non-zero */
                return bar0 & ~0xFu;
        }
    }
    return 0xE0000000u;  /* QEMU Bochs VBE fallback */
}

/* ------------------------------------------------------------------ */
/* gfx_init — call after idt_init/pic_init, before any output needed  */
/* ------------------------------------------------------------------ */
void gfx_init(void)
{
    uint16_t bochs_id;
    uint32_t fb_addr;

    /* ---- Bochs VBE (protected-mode I/O, always available on QEMU) ---- */
    bochs_id = bochs_read(0);   /* index 0 = VBE_DISPI_INDEX_ID */
    if (bochs_id >= 0xB0C0u && bochs_id <= 0xB0C5u) {
        bochs_write(4, 0x00);   /* VBE_DISPI_INDEX_ENABLE = disabled  */
        bochs_write(1, 800);    /* XRES                               */
        bochs_write(2, 600);    /* YRES                               */
        bochs_write(3, 32);     /* BPP                                */
        bochs_write(6, 800);    /* virtual width = physical width     */
        bochs_write(4, 0x41);   /* enable | LFB                      */

        fb_addr = find_vga_lfb();
        paging_map_frame(fb_addr);   /* map the 4 MB chunk           */

        g_fb     = (uint8_t *)(uintptr_t)fb_addr;
        g_width  = 800;
        g_height = 600;
        g_pitch  = 800 * 4;          /* 32 bpp: 4 bytes/pixel        */
        g_bpp    = 32;
        g_active = 1;

        gfx_clear(GFX_COLOR_BLACK);  /* blank screen before drawing  */
        return;
    }

    /* ---- Fall back: use params stored by stage2 do_vbe (if any) ------ */
    {
        boot_params_t *bp = (boot_params_t *)BOOT_PARAMS_ADDR;
        if (!bp->fb_ok || !bp->fb_addr || !bp->fb_width || !bp->fb_height)
            return;

        fb_addr = bp->fb_addr;
        paging_map_frame(fb_addr);

        g_fb     = (uint8_t *)(uintptr_t)fb_addr;
        g_width  = bp->fb_width;
        g_height = bp->fb_height;
        g_pitch  = bp->fb_pitch;
        g_bpp    = bp->fb_bpp;
        g_active = 1;

        gfx_clear(GFX_COLOR_BLACK);
    }
}

int gfx_is_active(void) { return g_active; }
int gfx_width(void)     { return g_width;  }
int gfx_height(void)    { return g_height; }

/* ------------------------------------------------------------------ */
/* Primitives                                                           */
/* ------------------------------------------------------------------ */
void gfx_pixel(int x, int y, gfx_color_t c)
{
    if (g_active) put_pixel(x, y, c);
}

void gfx_clear(gfx_color_t c)
{
    for (int y = 0; y < g_height; y++)
        for (int x = 0; x < g_width; x++)
            put_pixel(x, y, c);
}

void gfx_hline(int x, int y, int len, gfx_color_t c)
{
    for (int i = 0; i < len; i++) put_pixel(x + i, y, c);
}

void gfx_vline(int x, int y, int len, gfx_color_t c)
{
    for (int i = 0; i < len; i++) put_pixel(x, y + i, c);
}

void gfx_fill_rect(int x, int y, int w, int h, gfx_color_t c)
{
    for (int dy = 0; dy < h; dy++)
        for (int dx = 0; dx < w; dx++)
            put_pixel(x + dx, y + dy, c);
}

void gfx_draw_rect(int x, int y, int w, int h, gfx_color_t c)
{
    gfx_hline(x,         y,         w, c);
    gfx_hline(x,         y + h - 1, w, c);
    gfx_vline(x,         y,         h, c);
    gfx_vline(x + w - 1, y,         h, c);
}

/*
 * Rounded rectangle: fill + optional border colour.
 * Pass GFX_TRANSPARENT for fill or border to skip that layer.
 * Corners are quarter-circles of radius r.
 */
void gfx_rounded_rect(int x, int y, int w, int h, int r,
                       gfx_color_t fill, gfx_color_t border)
{
    if (r < 0) r = 0;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;

    /* Fill inner cross (non-corner rows) */
    if (fill != GFX_TRANSPARENT) {
        gfx_fill_rect(x + r, y,     w - 2 * r, h,     fill); /* vertical band   */
        gfx_fill_rect(x,     y + r, r,          h - 2 * r, fill); /* left strip  */
        gfx_fill_rect(x + w - r, y + r, r,      h - 2 * r, fill); /* right strip */
    }

    /* Corner pixels: iterate over r×r corner squares */
    for (int dy = 0; dy < r; dy++) {
        for (int dx = 0; dx < r; dx++) {
            int dist2 = (r - 1 - dx) * (r - 1 - dx) +
                        (r - 1 - dy) * (r - 1 - dy);
            int inside = (dist2 < r * r);
            /* Determine the pixel positions for all four corners */
            int px_tl = x + dx,         py_tl = y + dy;
            int px_tr = x + w - 1 - dx, py_tr = y + dy;
            int px_bl = x + dx,         py_bl = y + h - 1 - dy;
            int px_br = x + w - 1 - dx, py_br = y + h - 1 - dy;
            if (fill != GFX_TRANSPARENT && inside) {
                put_pixel(px_tl, py_tl, fill);
                put_pixel(px_tr, py_tr, fill);
                put_pixel(px_bl, py_bl, fill);
                put_pixel(px_br, py_br, fill);
            }
            /* Border: the outermost ring of the circle */
            if (border != GFX_TRANSPARENT) {
                int border_ring = inside &&
                    ((r - 1 - dx) * (r - 1 - dx) +
                     (r - 1 - dy) * (r - 1 - dy)) >= (r - 1) * (r - 1);
                if (border_ring) {
                    put_pixel(px_tl, py_tl, border);
                    put_pixel(px_tr, py_tr, border);
                    put_pixel(px_bl, py_bl, border);
                    put_pixel(px_br, py_br, border);
                }
            }
        }
    }

    /* Straight-edge border segments */
    if (border != GFX_TRANSPARENT) {
        gfx_hline(x + r, y,         w - 2 * r, border); /* top    */
        gfx_hline(x + r, y + h - 1, w - 2 * r, border); /* bottom */
        gfx_vline(x,         y + r, h - 2 * r, border); /* left   */
        gfx_vline(x + w - 1, y + r, h - 2 * r, border); /* right  */
    }
}

/* ------------------------------------------------------------------ */
/* Vertical gradient fill                                               */
/* ------------------------------------------------------------------ */
void gfx_gradient_rect(int x, int y, int w, int h,
                        gfx_color_t top, gfx_color_t bottom)
{
    for (int dy = 0; dy < h; dy++) {
        /* Linearly interpolate each channel */
        uint32_t r = GFX_R(top)    + (uint32_t)(GFX_R(bottom)    - (int)GFX_R(top))    * dy / h;
        uint32_t g = GFX_G(top)    + (uint32_t)(GFX_G(bottom)    - (int)GFX_G(top))    * dy / h;
        uint32_t b_ch = GFX_B(top) + (uint32_t)(GFX_B(bottom)    - (int)GFX_B(top))    * dy / h;
        gfx_color_t c = GFX_RGB(r, g, b_ch);
        gfx_hline(x, y + dy, w, c);
    }
}

/* ------------------------------------------------------------------ */
/* Text rendering                                                       */
/* ------------------------------------------------------------------ */
void gfx_char(int x, int y, char ch, gfx_color_t fg, gfx_color_t bg)
{
    unsigned idx = (unsigned char)ch;
    if (idx < 0x20 || idx > 0x7E) idx = 0x20;
    idx -= 0x20;

    const uint8_t *glyph = g_font8[idx];
    for (int row = 0; row < 8; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            if (bits & (0x80 >> col)) {
                put_pixel(x + col, y + row, fg);
            } else if (bg != GFX_TRANSPARENT) {
                put_pixel(x + col, y + row, bg);
            }
        }
    }
}

void gfx_text(int x, int y, const char *s, gfx_color_t fg, gfx_color_t bg)
{
    while (*s) {
        gfx_char(x, y, *s, fg, bg);
        x += 8;
        s++;
    }
}

int gfx_text_width(const char *s)
{
    int n = 0;
    while (*s++) n++;
    return n * 8;
}

/* ------------------------------------------------------------------ */
/* gfx_putchar — used as kprint_redirect target; handles \n, \t       */
/* ------------------------------------------------------------------ */
#define GLYPH_W 8
#define GLYPH_H 8

void gfx_set_cursor(int x, int y) { g_tx = x; g_ty = y; }
void gfx_get_cursor(int *x, int *y) { *x = g_tx; *y = g_ty; }

void gfx_putchar(char c)
{
    if (!g_active) return;

    int cols = g_width  / GLYPH_W;
    int rows = g_height / GLYPH_H;

    if (c == '\n') {
        g_tx = 0; g_ty++;
    } else if (c == '\r') {
        g_tx = 0;
    } else if (c == '\t') {
        g_tx = (g_tx + 8) & ~7;
    } else {
        gfx_char(g_tx * GLYPH_W, g_ty * GLYPH_H, c,
                 GFX_COLOR_WHITE, GFX_TRANSPARENT);
        g_tx++;
        if (g_tx >= cols) { g_tx = 0; g_ty++; }
    }

    /* Scroll up one line when at the bottom */
    if (g_ty >= rows) {
        /* Blit rows 1..n-1 up by one glyph row */
        uint8_t *dst = g_fb;
        uint8_t *src = g_fb + (uint32_t)GLYPH_H * (uint32_t)g_pitch;
        uint32_t move_bytes = (uint32_t)(g_height - GLYPH_H) * (uint32_t)g_pitch;
        for (uint32_t i = 0; i < move_bytes; i++) dst[i] = src[i];
        /* Clear last row */
        uint8_t *last = g_fb + (uint32_t)(g_height - GLYPH_H) * (uint32_t)g_pitch;
        for (uint32_t i = 0; i < (uint32_t)GLYPH_H * (uint32_t)g_pitch; i++)
            last[i] = 0;
        g_ty = rows - 1;
    }
}
