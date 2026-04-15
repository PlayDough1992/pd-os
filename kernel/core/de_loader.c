/* ============================================================================
 * PD-OS  —  Desktop Environment Loader
 * ============================================================================
 * Populates the pd_api_t table and attempts to load an external DE binary
 * from PDFS (/sys/de/<name>.bin).  Falls back to the built-in GDE if no
 * external DE is configured or the binary cannot be read.
 * ============================================================================ */

#include "de_api.h"
#include "vfs.h"
#include "gfx.h"
#include "mouse.h"
#include "keyboard.h"
#include "users.h"
#include "kheap.h"
#include "pit.h"
#include "gde.h"     /* for gde_process_main, login_screen_run, g_session_user */

/* ---- Static API table (kernel-owned, pointer written to DE_API_PTR_ADDR) - */

static pd_api_t s_api;

/* ---- Small helpers ------------------------------------------------------- */

static void de_memcpy(void *dst, const void *src, uint32_t n)
{
    uint8_t       *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
}

/* ---- Step 1: populate API table ----------------------------------------- */

void de_populate_api(void)
{
    /* Screen geometry */
    s_api.screen_w = GDE_SCREEN_W;
    s_api.screen_h = GDE_SCREEN_H;

    /* Graphics */
    s_api.gfx_putpixel       = gfx_putpixel;
    s_api.gfx_getpixel       = gfx_getpixel;
    s_api.gfx_fill_rect      = gfx_fill_rect;
    s_api.gfx_draw_rect      = gfx_draw_rect;
    s_api.gfx_hline          = gfx_hline;
    s_api.gfx_vline          = gfx_vline;
    s_api.gfx_fill_rect_grad = gfx_fill_rect_grad;
    s_api.gfx_draw_char      = gfx_draw_char;
    s_api.gfx_draw_string    = gfx_draw_string;
    s_api.gfx_draw_string_n  = gfx_draw_string_n;
    s_api.gfx_string_w       = gfx_string_w;
    s_api.gfx_string_w_n     = gfx_string_w_n;
    s_api.gfx_save_region    = gfx_save_region;
    s_api.gfx_restore_region = gfx_restore_region;
    s_api.gfx_flip           = gfx_flip;
    s_api.gfx_cache_bg       = gfx_cache_bg;
    s_api.gfx_restore_bg     = gfx_restore_bg;
    s_api.gfx_blend_pixel    = gfx_blend_pixel;

    /* Keyboard */
    s_api.keyboard_poll = keyboard_poll;

    /* Mouse */
    s_api.mouse_get_x          = mouse_get_x;
    s_api.mouse_get_y          = mouse_get_y;
    s_api.mouse_get_buttons    = mouse_get_buttons;
    s_api.mouse_changed        = mouse_changed;
    s_api.mouse_clear_changed  = mouse_clear_changed;

    /* Users */
    s_api.users_verify       = users_verify;
    s_api.users_count        = users_count;
    s_api.users_get_by_index = users_get_by_index;
    s_api.users_get          = users_get;

    /* VFS */
    s_api.vfs_open    = vfs_open;
    s_api.vfs_read    = vfs_read;
    s_api.vfs_write   = vfs_write;
    s_api.vfs_create  = vfs_create;
    s_api.vfs_unlink  = vfs_unlink;
    s_api.vfs_readdir = vfs_readdir;

    /* Memory */
    s_api.kmalloc = kmalloc;
    s_api.kfree   = kfree;

    /* Timing */
    s_api.pit_get_ticks = pit_get_ticks;

    /* Session */
    s_api.session_user_ptr = &g_session_user;
    s_api.login_screen_run = login_screen_run;

    /* Write pointer to API struct at fixed address DE_API_PTR_ADDR */
    *((pd_api_t **)DE_API_PTR_ADDR) = &s_api;
}

/* ---- Step 2: try to load and run external DE from PDFS ------------------ */
/*
 * Returns 1 if an external DE was loaded and jumped to (never returns in
 * that case — the 1 is only logical).
 * Returns 0 if no external DE is configured or the binary could not be read,
 * so the caller falls back to the built-in GDE.
 */
int de_load_and_run(void)
{
    vfs_node_t  active_node;
    char        de_name[64];
    int         name_len;
    char        de_path[80];
    int         pi;
    vfs_node_t  bin_node;
    uint8_t    *load_buf;
    uint32_t    bytes_read;
    void      (*entry)(void);

    /* ---- Read /sys/de/active ---- */
    if (vfs_open("/sys/de/active", &active_node) != 0)
        return 0;   /* file absent — use built-in GDE */

    if (active_node.size == 0 || active_node.size >= sizeof(de_name))
        return 0;

    if (vfs_read(&active_node, 0, active_node.size, de_name) < 0)
        return 0;

    /* Strip trailing newline / whitespace */
    name_len = (int)active_node.size;
    while (name_len > 0 &&
           (de_name[name_len - 1] == '\n' || de_name[name_len - 1] == '\r' ||
            de_name[name_len - 1] == ' ')) {
        name_len--;
    }
    de_name[name_len] = '\0';

    /* If the active name IS "gde", fall back to built-in */
    if (name_len == 3 &&
        de_name[0] == 'g' && de_name[1] == 'd' && de_name[2] == 'e')
        return 0;

    /* ---- Build path /sys/de/<name>.bin ---- */
    const char *prefix = "/sys/de/";
    const char *suffix = ".bin";
    pi = 0;
    int i;
    for (i = 0; prefix[i]; i++) de_path[pi++] = prefix[i];
    for (i = 0; i < name_len; i++) de_path[pi++] = de_name[i];
    for (i = 0; suffix[i]; i++)  de_path[pi++] = suffix[i];
    de_path[pi] = '\0';

    /* ---- Open the binary ---- */
    if (vfs_open(de_path, &bin_node) != 0)
        return 0;   /* binary not found — use built-in GDE */

    if (bin_node.size == 0 || bin_node.size > DE_MAX_SIZE)
        return 0;

    /* ---- Load binary into a temporary heap buffer then copy to load addr - */
    load_buf = (uint8_t *)kmalloc(bin_node.size);
    if (!load_buf)
        return 0;

    bytes_read = (uint32_t)vfs_read(&bin_node, 0, bin_node.size, load_buf);
    if (bytes_read != bin_node.size) {
        kfree(load_buf);
        return 0;
    }

    /* Copy to fixed load address */
    de_memcpy((void *)DE_LOAD_ADDR, load_buf, bin_node.size);
    kfree(load_buf);

    /* ---- Jump to DE entry point ---- */
    entry = (void (*)(void))DE_LOAD_ADDR;
    entry();   /* never returns */

    return 1;  /* unreachable */
}
