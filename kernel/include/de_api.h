#pragma once

/* ============================================================================
 * PD-OS  —  Desktop Environment API
 * ============================================================================
 * A Desktop Environment (DE) is a flat binary compiled to load at
 * DE_LOAD_ADDR (physical 0x01000000) and installed on PDFS as
 * /sys/de/<name>.bin.  The active DE is named in /sys/de/active.
 *
 * Before jumping to the DE binary, the kernel:
 *   1. Fills a pd_api_t struct (in kernel BSS)
 *   2. Writes a pointer to that struct at DE_API_PTR_ADDR (physical 0x5000)
 *
 * The DE entry point reads the pointer and calls kernel services through it:
 *
 *   #include "de_api.h"
 *   #define PDAPI (*(pd_api_t **)DE_API_PTR_ADDR)
 *
 *   void de_main(void) {
 *       PDAPI->gfx_fill_rect(0, 0, 1024, 768, 0x001A2B3C);
 *       PDAPI->gfx_flip();
 *       for (;;) __asm__("hlt");
 *   }
 *
 * Compile your DE:
 *   i686-linux-gnu-gcc -m32 -ffreestanding -nostdlib -fno-pic \
 *       -Ttext 0x01000000 -I<pdos-include-dir> \
 *       my_de.c -o my_de.bin
 *
 * Install your DE:
 *   Copy my_de.bin to /sys/de/my_de.bin on the PDFS partition.
 *   Echo "my_de" (no .bin) to /sys/de/active.
 *   Reboot.
 *
 * The DE is responsible for:
 *   - Calling gfx_flip() to push frames to the real display
 *   - Calling login_screen_run() (from PDAPI) or implementing its own login
 *   - Running its own event loop forever (it must not return)
 * ============================================================================ */

#include "kernel.h"
#include "users.h"
#include "vfs.h"

/* Fixed addresses (shared between kernel and every DE binary) */
#define DE_API_PTR_ADDR  0x5000u    /* kernel writes pd_api_t* here          */
#define DE_LOAD_ADDR     0x1000000u /* DE binary is loaded here (16 MB mark) */
#define DE_MAX_SIZE      (512u * 1024u)  /* 512 KB max DE binary size        */

/* Convenience macro for use inside DE binaries */
#define PDAPI (*(pd_api_t **)DE_API_PTR_ADDR)

/* ---- Exported kernel API ------------------------------------------------- */

typedef struct pd_api {
    /* --- Screen dimensions ------------------------------------------------ */
    int  screen_w;     /* always 1024 */
    int  screen_h;     /* always 768  */

    /* --- Graphics --------------------------------------------------------- */
    void     (*gfx_putpixel)      (int x, int y, uint32_t color);
    uint32_t (*gfx_getpixel)      (int x, int y);
    void     (*gfx_fill_rect)     (int x, int y, int w, int h, uint32_t color);
    void     (*gfx_draw_rect)     (int x, int y, int w, int h, uint32_t color);
    void     (*gfx_hline)         (int x, int y, int len, uint32_t color);
    void     (*gfx_vline)         (int x, int y, int len, uint32_t color);
    void     (*gfx_fill_rect_grad)(int x, int y, int w, int h,
                                   uint32_t top, uint32_t bot);
    void     (*gfx_draw_char)     (int x, int y, char c,
                                   uint32_t fg, uint32_t bg);
    void     (*gfx_draw_string)   (int x, int y, const char *s,
                                   uint32_t fg, uint32_t bg, int transparent);
    void     (*gfx_draw_string_n) (int x, int y, const char *s, int len,
                                   uint32_t fg, uint32_t bg, int transparent);
    int      (*gfx_string_w)      (const char *s);
    int      (*gfx_string_w_n)    (const char *s, int len);
    void     (*gfx_save_region)   (int x, int y, int w, int h, uint32_t *dst);
    void     (*gfx_restore_region)(int x, int y, int w, int h,
                                   const uint32_t *src);
    void     (*gfx_flip)          (void);
    void     (*gfx_cache_bg)      (void);
    void     (*gfx_restore_bg)    (void);
    void     (*gfx_blend_pixel)   (int x, int y, uint32_t color, uint8_t alpha);

    /* --- Keyboard --------------------------------------------------------- */
    char (*keyboard_poll)(void);

    /* --- Mouse ------------------------------------------------------------ */
    int     (*mouse_get_x)       (void);
    int     (*mouse_get_y)       (void);
    uint8_t (*mouse_get_buttons) (void);
    int     (*mouse_changed)     (void);
    void    (*mouse_clear_changed)(void);

    /* --- Users ------------------------------------------------------------ */
    int           (*users_verify)      (const char *username,
                                        const char *password);
    int           (*users_count)       (void);
    const user_t *(*users_get_by_index)(int i);
    const user_t *(*users_get)         (const char *username);

    /* --- VFS -------------------------------------------------------------- */
    int (*vfs_open)   (const char *path, vfs_node_t *out);
    int (*vfs_read)   (vfs_node_t *node, uint32_t offset,
                       uint32_t len, void *buf);
    int (*vfs_write)  (vfs_node_t *node, uint32_t offset,
                       uint32_t len, const void *buf);
    int (*vfs_create) (const char *path);
    int (*vfs_unlink) (const char *path);
    int (*vfs_readdir)(const char *path, uint32_t idx, vfs_node_t *out);

    /* --- Memory ----------------------------------------------------------- */
    void *(*kmalloc)(uint32_t size);
    void  (*kfree)  (void *ptr);

    /* --- Timing ----------------------------------------------------------- */
    uint32_t (*pit_get_ticks)(void);

    /* --- Session ---------------------------------------------------------- */
    /* Pointer to the session-user pointer set by login_screen_run().
     * The DE can read *session_user_ptr to get the logged-in user. */
    const user_t **session_user_ptr;

    /* --- Login screen (built-in greeter, optional use) ------------------- */
    /* Run the PD-OS graphical login screen; sets *session_user_ptr.
     * Blocks until the user authenticates. */
    void (*login_screen_run)(void);

} pd_api_t;
