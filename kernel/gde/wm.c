/* ============================================================================
 * PD-OS GDE  —  Window Manager
 * ============================================================================ */

#include "gde.h"
#include "gfx.h"

/* ---- Window pool --------------------------------------------------------- */

static gde_window_t g_wins[GDE_MAX_WINDOWS];
static int          g_nwins    = 0;
static int          g_next_id  = 1;

/* z-order: g_zorder[0]=bottom, g_zorder[g_nwins-1]=top (focused) */
static int g_zorder[GDE_MAX_WINDOWS];
static int g_focused_idx = -1;   /* index into g_zorder */

/* ---- Helpers ------------------------------------------------------------- */

/* Append pool_idx to end of zorder array (makes it topmost).
 * Used internally. */
__attribute__((unused))
static void wm_push_zorder(int pool_idx)
{
    /* Put pool_idx at end of zorder (topmost) */
    int i;
    for (i = 0; i < g_nwins - 1; i++) g_zorder[i] = g_zorder[i + 1];
    g_zorder[g_nwins - 1] = pool_idx;
    g_focused_idx = g_nwins - 1;
}

/* Remove from z-order array by value */
static void wm_remove_zorder(int pool_idx)
{
    int i, j = 0;
    for (i = 0; i < g_nwins; i++)
        if (g_zorder[i] != pool_idx) g_zorder[j++] = g_zorder[i];
    if (g_focused_idx >= j) g_focused_idx = j - 1;
}

/* ---- Public API ---------------------------------------------------------- */

gde_window_t *wm_create(const char *title, int x, int y, int w, int h,
                          void (*draw_cb)(gde_window_t *),
                          void (*key_cb)(gde_window_t *, char))
{
    int i;
    if (g_nwins >= GDE_MAX_WINDOWS) return (void*)0;

    /* Find free slot */
    for (i = 0; i < GDE_MAX_WINDOWS; i++) {
        if (!g_wins[i].visible) break;
    }
    if (i == GDE_MAX_WINDOWS) return (void*)0;

    gde_window_t *win = &g_wins[i];

    win->id       = g_next_id++;
    win->x        = x;  win->y = y;
    win->w        = w;  win->h = h;
    win->prev_x   = x;  win->prev_y = y;
    win->prev_w   = w;  win->prev_h = h;
    win->state    = GDE_WIN_NORMAL;
    win->visible  = 1;
    win->focused  = 0;
    win->draw_content  = draw_cb;
    win->on_keychar    = key_cb;
    win->on_mousedown  = (void*)0;
    win->priv     = (void*)0;

    /* Copy title */
    int ti;
    for (ti = 0; ti < 63 && title[ti]; ti++) win->title[ti] = title[ti];
    win->title[ti] = '\0';

    /* Defocus all others */
    int zi;
    for (zi = 0; zi < g_nwins; zi++) g_wins[g_zorder[zi]].focused = 0;

    g_zorder[g_nwins++] = i;
    g_focused_idx = g_nwins - 1;
    win->focused = 1;

    return win;
}

void wm_close(gde_window_t *win)
{
    if (!win) return;
    int pool_idx = (int)(win - g_wins);
    wm_remove_zorder(pool_idx);
    g_nwins--;
    win->visible = 0;
    win->focused = 0;

    /* Re-focus next topmost window */
    if (g_nwins > 0) {
        g_focused_idx = g_nwins - 1;
        g_wins[g_zorder[g_focused_idx]].focused = 1;
    } else {
        g_focused_idx = -1;
    }
}

void wm_focus(gde_window_t *win)
{
    if (!win || !win->visible) return;
    int pool_idx = (int)(win - g_wins);

    /* Defocus all */
    int i;
    for (i = 0; i < g_nwins; i++) g_wins[g_zorder[i]].focused = 0;

    /* Remove from current z position and push to top */
    wm_remove_zorder(pool_idx);
    g_nwins--;   /* temporarily decrement so push lands at correct index */
    g_nwins++;
    g_zorder[g_nwins - 1] = pool_idx;
    g_focused_idx = g_nwins - 1;
    win->focused = 1;
}

void wm_raise(gde_window_t *win) { wm_focus(win); }

int wm_count(void) { return g_nwins; }

gde_window_t *wm_get(int i)
{
    if (i < 0 || i >= g_nwins) return (void*)0;
    return &g_wins[g_zorder[i]];
}

gde_window_t *wm_focused(void)
{
    if (g_focused_idx < 0) return (void*)0;
    return &g_wins[g_zorder[g_focused_idx]];
}

void wm_toggle_maximized(gde_window_t *win)
{
    if (!win) return;
    if (win->state == GDE_WIN_MAXIMIZED) {
        win->x = win->prev_x; win->y = win->prev_y;
        win->w = win->prev_w; win->h = win->prev_h;
        win->state = GDE_WIN_NORMAL;
    } else {
        win->prev_x = win->x; win->prev_y = win->y;
        win->prev_w = win->w; win->prev_h = win->h;
        win->x = 0;
        win->y = 0;
        win->w = GDE_SCREEN_W;
        win->h = GDE_SCREEN_H - GDE_TASKBAR_H;
        win->state = GDE_WIN_MAXIMIZED;
    }
}

void wm_minimize(gde_window_t *win)
{
    if (!win) return;
    win->state = GDE_WIN_MINIMIZED;
    /* Defocus this window, focus next topmost visible */
    win->focused = 0;
    int pool_idx = (int)(win - g_wins);
    int i;
    for (i = g_nwins - 1; i >= 0; i--) {
        if (g_zorder[i] != pool_idx && g_wins[g_zorder[i]].state != GDE_WIN_MINIMIZED) {
            g_focused_idx = i;
            g_wins[g_zorder[i]].focused = 1;
            return;
        }
    }
    g_focused_idx = -1;
}
