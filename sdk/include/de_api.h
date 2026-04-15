#pragma once

/* ============================================================================
 * PD-OS DE SDK  —  de_api.h
 * ============================================================================
 * This is THE header for building a PD-OS Desktop Environment.
 * Include it in every source file of your DE.
 *
 * Usage:
 *   #include "de_api.h"
 *   #define GFX_RGB(r,g,b) ...   (provided below)
 *
 *   void de_main(void) {
 *       PDAPI->login_screen_run();          // optional built-in login
 *       PDAPI->gfx_fill_rect(0,0,1024,768,0x001A2B3C);
 *       PDAPI->gfx_flip();
 *       for(;;) __asm__("hlt");
 *   }
 * ============================================================================ */

#include "pdos_types.h"
#include "pdos_users.h"
#include "pdos_vfs.h"

/* ---- Fixed kernel/DE interface addresses --------------------------------- */

/* Physical 0x5000: kernel writes a pd_api_t* here before jumping to DE.
 * Always read this address to get the API pointer — never hardcode it. */
#define DE_API_PTR_ADDR  0x5000u

/* Physical 0x01000000: where the kernel loads the DE binary.
 * Your linker script must use this as the load address (-Ttext 0x01000000). */
#define DE_LOAD_ADDR     0x01000000u

/* Convenience macro — use this everywhere in your DE: */
#define PDAPI  (*(pd_api_t **)DE_API_PTR_ADDR)

/* ---- Color helpers -------------------------------------------------------- */

/* Pack 8-bit R, G, B into the 32-bit pixel format (0x00RRGGBB). */
#define GFX_RGB(r,g,b) \
    ((uint32_t)(((uint32_t)(uint8_t)(r) << 16) | \
                ((uint32_t)(uint8_t)(g) <<  8) | \
                 (uint32_t)(uint8_t)(b)))

/* Ready-made color constants */
#define GFX_BLACK      GFX_RGB(  0,   0,   0)
#define GFX_WHITE      GFX_RGB(255, 255, 255)
#define GFX_RED        GFX_RGB(200,  30,  30)
#define GFX_GREEN      GFX_RGB( 30, 180,  30)
#define GFX_BLUE       GFX_RGB( 30,  80, 200)
#define GFX_CYAN       GFX_RGB(  0, 180, 200)
#define GFX_YELLOW     GFX_RGB(240, 200,   0)
#define GFX_DARK_GREY  GFX_RGB( 60,  60,  65)
#define GFX_MID_GREY   GFX_RGB(130, 130, 135)
#define GFX_LIGHT_GREY GFX_RGB(210, 210, 215)

/* Font character dimensions (8×16 VGA BIOS bitmap font) */
#define GFX_CHAR_W  8
#define GFX_CHAR_H  16

/* Screen constants */
#define SCREEN_W  1024
#define SCREEN_H   768

/* ---- The kernel API table ------------------------------------------------ */

typedef struct pd_api {
    /* Screen dimensions (always 1024×768 in this release) */
    int  screen_w;
    int  screen_h;

    /* ------------------------------------------------------------------ */
    /* GRAPHICS                                                             */
    /* All drawing goes to an off-screen back buffer.                      */
    /* Call gfx_flip() once per frame to push it to the real display.     */
    /* ------------------------------------------------------------------ */

    /* Write a single pixel at (x, y) */
    void     (*gfx_putpixel)      (int x, int y, uint32_t color);

    /* Read a pixel from the back buffer */
    uint32_t (*gfx_getpixel)      (int x, int y);

    /* Fill a solid rectangle */
    void     (*gfx_fill_rect)     (int x, int y, int w, int h, uint32_t color);

    /* Draw hollow rectangle (1-pixel border) */
    void     (*gfx_draw_rect)     (int x, int y, int w, int h, uint32_t color);

    /* Draw a horizontal / vertical line */
    void     (*gfx_hline)         (int x, int y, int len, uint32_t color);
    void     (*gfx_vline)         (int x, int y, int len, uint32_t color);

    /* Fill a rectangle with a vertical gradient (top_color → bot_color) */
    void     (*gfx_fill_rect_grad)(int x, int y, int w, int h,
                                   uint32_t top_color, uint32_t bot_color);

    /* Draw a single character (8×16 font).
     * bg is ignored when transparent_bg=1. */
    void     (*gfx_draw_char)     (int x, int y, char c,
                                   uint32_t fg, uint32_t bg);

    /* Draw a NUL-terminated string.
     * transparent_bg=1 skips painting background pixels. */
    void     (*gfx_draw_string)   (int x, int y, const char *s,
                                   uint32_t fg, uint32_t bg, int transparent_bg);

    /* Draw up to `len` characters of a string */
    void     (*gfx_draw_string_n) (int x, int y, const char *s, int len,
                                   uint32_t fg, uint32_t bg, int transparent_bg);

    /* Pixel width of a string (useful for centering text) */
    int      (*gfx_string_w)      (const char *s);
    int      (*gfx_string_w_n)    (const char *s, int len);

    /* Save/restore a rectangular region of the back buffer.
     * `dst`/`src` must point to w*h uint32_t values you own. */
    void     (*gfx_save_region)   (int x, int y, int w, int h, uint32_t *dst);
    void     (*gfx_restore_region)(int x, int y, int w, int h,
                                   const uint32_t *src);

    /* Push back buffer → real display.  Call once per frame. */
    void     (*gfx_flip)          (void);

    /* Snapshot the current back buffer as the "background cache"
     * (used for fast per-frame restoration without redrawing gradients). */
    void     (*gfx_cache_bg)      (void);

    /* Restore the background snapshot into the back buffer.
     * Call at the start of each frame instead of redrawing the background. */
    void     (*gfx_restore_bg)    (void);

    /* Alpha-blend a pixel at (x,y): 0=transparent, 255=opaque */
    void     (*gfx_blend_pixel)   (int x, int y, uint32_t color, uint8_t alpha);

    /* ------------------------------------------------------------------ */
    /* KEYBOARD                                                             */
    /* ------------------------------------------------------------------ */

    /* Return the next ASCII character from the keyboard buffer, or 0 if
     * empty.  Call in a loop to drain all pending keystrokes. */
    char (*keyboard_poll)(void);

    /* ------------------------------------------------------------------ */
    /* MOUSE                                                                */
    /* ------------------------------------------------------------------ */

    /* Current cursor position in pixels */
    int     (*mouse_get_x)        (void);
    int     (*mouse_get_y)        (void);

    /* Button state bitmask: bit0=left, bit1=right, bit2=middle */
    uint8_t (*mouse_get_buttons)  (void);

    /* Returns 1 if mouse position or buttons changed since last clear */
    int     (*mouse_changed)      (void);

    /* Reset the "changed" flag; call after processing mouse state */
    void    (*mouse_clear_changed)(void);

    /* ------------------------------------------------------------------ */
    /* USERS                                                                */
    /* ------------------------------------------------------------------ */

    /* Returns 1 if the password is correct for the given username */
    int           (*users_verify)      (const char *username,
                                        const char *password);

    /* Number of user accounts on this system */
    int           (*users_count)       (void);

    /* Get user by index (0-based); returns NULL if out of range */
    const user_t *(*users_get_by_index)(int i);

    /* Get user by username; returns NULL if not found */
    const user_t *(*users_get)         (const char *username);

    /* ------------------------------------------------------------------ */
    /* FILESYSTEM  (PDFS — /  is always mounted)                           */
    /* ------------------------------------------------------------------ */

    /* Open a file by absolute path; fills `out`.  Returns 0 or negative. */
    int (*vfs_open)   (const char *path, vfs_node_t *out);

    /* Read `len` bytes at byte-offset `offset` from an open node. */
    int (*vfs_read)   (vfs_node_t *node, uint32_t offset,
                       uint32_t len, void *buf);

    /* Write `len` bytes at byte-offset `offset` to an open node. */
    int (*vfs_write)  (vfs_node_t *node, uint32_t offset,
                       uint32_t len, const void *buf);

    /* Create a new file; returns 0 on success. */
    int (*vfs_create) (const char *path);

    /* Delete a file; returns 0 on success. */
    int (*vfs_unlink) (const char *path);

    /* Read directory entry `idx`; returns 0 if entry exists, negative when
     * exhausted.  idx starts from 0 and increments until negative returned. */
    int (*vfs_readdir)(const char *path, uint32_t idx, vfs_node_t *out);

    /* ------------------------------------------------------------------ */
    /* MEMORY                                                               */
    /* ------------------------------------------------------------------ */

    /* Allocate `size` bytes from the kernel heap.  Returns NULL on OOM. */
    void *(*kmalloc)(uint32_t size);

    /* Free a previously kmalloc'd block. */
    void  (*kfree)  (void *ptr);

    /* ------------------------------------------------------------------ */
    /* TIMING                                                               */
    /* ------------------------------------------------------------------ */

    /* Returns system tick count (PIT at 100 Hz → one tick ≈ 10 ms).
     * Useful for animations, double-click detection, timeouts. */
    uint32_t (*pit_get_ticks)(void);

    /* ------------------------------------------------------------------ */
    /* SESSION                                                              */
    /* ------------------------------------------------------------------ */

    /* Pointer to the kernel's session-user pointer.
     * After login_screen_run() returns, *session_user_ptr points to the
     * logged-in user_t.  You can also set it yourself if you implement
     * your own login UI and call users_verify() directly. */
    const user_t **session_user_ptr;

    /* Run the built-in PD-OS graphical login screen.
     * Blocks until the user authenticates; sets *session_user_ptr on success.
     * Optional — implement your own login UI if you prefer. */
    void (*login_screen_run)(void);

} pd_api_t;
