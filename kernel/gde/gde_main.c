/* ============================================================================
 * PD-OS GDE  —  Main entry point
 * ============================================================================ */

#include "gde.h"
#include "gfx.h"
#include "mouse.h"
#include "keyboard.h"
#include "boot_info.h"
#include "paging.h"
#include "pit.h"

/* Current session user — set by login_screen_run() before the desktop
 * launches.  Other subsystems (explorer, shell) read this for the UID. */
const user_t *g_session_user = (void *)0;

/* ---- Shared init --------------------------------------------------------- */

static void gde_hw_init(void)
{
    volatile boot_info_t *bi = g_boot_info;
    paging_map_framebuffer(bi->vbe_fb);
    gfx_init((uint32_t)bi->vbe_fb,
              bi->vbe_width, bi->vbe_height, bi->vbe_pitch);
    mouse_init();
}

/* ---- Desktop event loop (shared by both entry points) ------------------- */

static void gde_run_desktop(void)
{
    taskbar_init();
    desktop_init();
    desktop_render();  /* initial full render */

    uint32_t last_sec = 0;

    for (;;) {
        int dirty = 0;

        /* ---- Keyboard ---- */
        char k;
        while ((k = keyboard_poll()) != 0) {
            desktop_handle_key(k);
            /* Keyboard changes window content — mark full screen to ensure
             * the changed window region is sent to the framebuffer. */
            gfx_dirty_mark(0, 0, GDE_SCREEN_W, GDE_SCREEN_H);
            dirty = 1;
        }

        /* ---- Mouse ---- */
        if (mouse_changed()) {
            mouse_clear_changed();
            if (desktop_handle_mouse()) {
                /* desktop_handle_mouse() already marked the precise dirty rect
                 * (small for drag/resize/menu, full-screen for clicks etc.). */
                dirty = 1;
            } else {
                /* Cursor position only — update real FB without a scene redraw */
                desktop_cursor_move(mouse_get_x(), mouse_get_y());
            }
        }

        /* ---- Clock tick — refresh taskbar every second ---- */
        {
            uint32_t now_sec = (uint32_t)pit_get_ticks() / 100u;
            if (now_sec != last_sec) {
                last_sec = now_sec;
                /* Mark only the taskbar strip dirty — cheap 1024×30 partial flip */
                gfx_dirty_mark(0, GDE_SCREEN_H - GDE_TASKBAR_H,
                               GDE_SCREEN_W, GDE_TASKBAR_H);
                dirty = 1;
            }
        }

        /* ---- Present ---- */
        if (dirty) {
            desktop_present();  /* partial or full based on dirty rect size */
        } else {
            __asm__ volatile ("hlt");
        }
    }
}

/* ---- Process entry point (called via proc_create) ----------------------- */
/*
 * This is the GDE "session" kernel process.  It runs as a scheduled task
 * (not as kernel_main directly), enabling the kernel to remain responsive.
 * Flow: hardware init → GUI login/greeter → desktop event loop (forever).
 */
void gde_process_main(void)
{
    gde_hw_init();
    login_screen_run();   /* blocks until authenticated; sets g_session_user */
    gde_run_desktop();    /* never returns */
}

/* ---- Legacy direct entry (kept for reference; not called in GDE builds) - */
void gde_main(void)
{
    gde_hw_init();
    gde_run_desktop();
}

