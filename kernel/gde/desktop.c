/* ============================================================================
 * PD-OS GDE  —  Desktop: rendering and event dispatch
 * ============================================================================ */

#include "gde.h"
#include "gfx.h"
#include "mouse.h"
#include "keyboard.h"
#include "pit.h"
#include "vfs.h"

static void desktop_build_bg(void); /* defined near desktop_init */

/* ---- Cursor (sprite + atomic union-rect blit) --------------------------- */

#define CURSOR_W 18
#define CURSOR_H 26

/* Pre-rendered sprite and mask — built once in desktop_init(). */
static uint32_t g_cursor_sprite[CURSOR_W * CURSOR_H];
static uint8_t  g_cursor_mask  [CURSOR_W * CURSOR_H];
static uint32_t g_cursor_save  [CURSOR_W * CURSOR_H];
static int      g_cursor_sx = -999, g_cursor_sy = -999;

static void cursor_sprite_init(void)
{
    gfx_cursor_build_arrow(g_cursor_sprite, g_cursor_mask,
                           CURSOR_W, CURSOR_H);
}

/* Move cursor to (nx,ny): atomic union-rect blit, zero flicker. */
static void cursor_move(int nx, int ny)
{
    gfx_cursor_blit(g_cursor_sx, g_cursor_sy, nx, ny,
                    CURSOR_W, CURSOR_H,
                    g_cursor_sprite, g_cursor_mask, g_cursor_save);
    g_cursor_sx = nx;
    g_cursor_sy = ny;
}

/* Public: move cursor without a full desktop redraw (cursor-only update). */
void desktop_cursor_move(int x, int y)
{
    cursor_move(x, y);
}

/* ---- Icons --------------------------------------------------------------- */

static gde_icon_t g_icons[GDE_MAX_ICONS];
static int        g_nicons = 0;

/* Double-click constants */
#define DBLCLICK_TICKS    60   /* 600 ms at 100 Hz — matches OS default */
#define ICON_HIT_MARGIN    8   /* extra px around icon for both clicks */
static int      g_last_icon_idx  = -1;
static uint32_t g_last_icon_tick = 0;

static void icon_add(const char *label, int x, int y, void (*fn)(void))
{
    if (g_nicons >= GDE_MAX_ICONS) return;
    gde_icon_t *ic = &g_icons[g_nicons++];
    int i;
    for (i = 0; i < 31 && label[i]; i++) ic->label[i] = label[i];
    ic->label[i]     = '\0';
    ic->x            = x;
    ic->y            = y;
    ic->on_dblclick  = fn;
}

static void icon_render(const gde_icon_t *ic)
{
    /* 48x48 rounded icon box */
    gfx_fill_rect(ic->x, ic->y, 48, 48, GFX_RGB(60,100,160));
    gfx_draw_rect(ic->x, ic->y, 48, 48, GFX_RGB(100,150,220));
    /* Icon glyph (first char, centered) */
    int gx = ic->x + (48 - GFX_CHAR_W) / 2;
    int gy = ic->y + (48 - GFX_CHAR_H) / 2;
    gfx_draw_char(gx, gy, ic->label[0], GFX_WHITE, GFX_RGB(60,100,160));
    /* Label below */
    int lw  = gfx_string_w(ic->label);
    int lx  = ic->x + (48 - lw) / 2;
    gfx_draw_string(lx, ic->y + 52, ic->label, GFX_WHITE, COL_DESKTOP, 1);
}

/* ---- Context menu -------------------------------------------------------- */

#define CTX_MAX    10
#define CTX_ITEM_H 20
#define CTX_SEP_H   8
#define CTX_W      172

typedef struct { char label[32]; void (*fn)(void); int is_sep; } ctx_item_t;
static ctx_item_t g_ctx_items[CTX_MAX];
static int        g_ctx_n    = 0;
static int        g_ctx_open = 0;
static int        g_ctx_x = 0, g_ctx_y = 0;
static int        g_ctx_hover = -1;

static int ctx_menu_h(void)
{
    int h = 4, i;
    for (i = 0; i < g_ctx_n; i++)
        h += g_ctx_items[i].is_sep ? CTX_SEP_H : CTX_ITEM_H;
    return h;
}

static void ctx_open(int x, int y)
{
    g_ctx_x     = x;
    g_ctx_y     = y;
    g_ctx_open  = 1;
    g_ctx_hover = -1;
    if (g_ctx_x + CTX_W > gfx_width())
        g_ctx_x = gfx_width() - CTX_W - 2;
    if (g_ctx_y + ctx_menu_h() > gfx_height() - GDE_TASKBAR_H)
        g_ctx_y = gfx_height() - GDE_TASKBAR_H - ctx_menu_h();
}

static void ctx_render(void)
{
    if (!g_ctx_open) return;
    int menu_h = ctx_menu_h();
    gfx_fill_rect(g_ctx_x + 2, g_ctx_y + 2, CTX_W, menu_h, GFX_RGB(0,0,0));
    gfx_fill_rect(g_ctx_x, g_ctx_y, CTX_W, menu_h, COL_MENU_BG);
    gfx_draw_rect(g_ctx_x, g_ctx_y, CTX_W, menu_h, COL_MENU_BORDER);
    int i, iy = g_ctx_y + 3;
    for (i = 0; i < g_ctx_n; i++) {
        if (g_ctx_items[i].is_sep) {
            gfx_hline(g_ctx_x + 6, iy + CTX_SEP_H / 2, CTX_W - 12, COL_MENU_BORDER);
            iy += CTX_SEP_H;
        } else {
            if (i == g_ctx_hover)
                gfx_fill_rect(g_ctx_x + 1, iy, CTX_W - 2, CTX_ITEM_H, COL_MENU_HOVER);
            gfx_draw_string(g_ctx_x + 8,
                             iy + (CTX_ITEM_H - GFX_CHAR_H) / 2,
                             g_ctx_items[i].label, COL_MENU_TEXT,
                             (i == g_ctx_hover) ? COL_MENU_HOVER : COL_MENU_BG, 0);
            iy += CTX_ITEM_H;
        }
    }
}

/* ---- Window hit testing & title bar rendering ---------------------------- */

static void win_draw_titlebar(gde_window_t *win)
{
    uint32_t tc1 = win->focused ? COL_TITLEBAR_A1 : COL_TITLEBAR_I1;
    uint32_t tc2 = win->focused ? COL_TITLEBAR_A2 : COL_TITLEBAR_I2;
    uint32_t bc  = win->focused ? COL_WIN_BORDER_A : COL_WIN_BORDER_I;

    /* Outer border */
    gfx_draw_rect(win->x, win->y, win->w, win->h, bc);

    /* Highlight line at very top — 3-D raised feel */
    uint32_t hl = win->focused ? GFX_RGB(115,115,124) : GFX_RGB(132,132,138);
    gfx_hline(win->x + 1, win->y + 1, win->w - 2, hl);

    /* Title bar gradient */
    gfx_fill_rect_grad(win->x + 1, win->y + 2,
                        win->w - 2, GDE_TITLEBAR_H - 3, tc1, tc2);

    /* Inner shadow at bottom of title bar */
    gfx_hline(win->x + 1, win->y + GDE_TITLEBAR_H - 1,
               win->w - 2, GFX_RGB(18, 18, 22));

    /* Window icon — rounded corners blend with the titlebar gradient behind */
    {
        uint32_t ic_c[36];
        gfx_save_corners(win->x + 5, win->y + 5, 14, 14, 3, ic_c);
        gfx_fill_rect(win->x + 5, win->y + 5, 14, 14, GFX_RGB(40, 100, 200));
        gfx_draw_rect(win->x + 5, win->y + 5, 14, 14, GFX_RGB(80, 150, 240));
        gfx_restore_corners(win->x + 5, win->y + 5, 14, 14, 3, ic_c);
    }

    /* Title text — centered, clamped past icon */
    int tw = gfx_string_w(win->title);
    int tx = win->x + (win->w - tw) / 2;
    if (tx < win->x + 26) tx = win->x + 26;
    int ty = win->y + 2 + (GDE_TITLEBAR_H - 2 - GFX_CHAR_H) / 2;
    gfx_draw_string(tx, ty, win->title, GFX_WHITE, tc2, 1);

    /* Buttons right-aligned: [_] [□] [X] */
    int bx = win->x + win->w - GDE_BTN_W - 3;
    int by = win->y + (GDE_TITLEBAR_H - GDE_BTN_H) / 2;

    /* Close [X] — red */
    gfx_fill_rect(bx, by, GDE_BTN_W, GDE_BTN_H, COL_BTN_CLOSE);
    gfx_hline(bx, by, GDE_BTN_W, GFX_RGB(225, 85, 85));
    gfx_draw_string(bx + (GDE_BTN_W - GFX_CHAR_W) / 2,
                     by + (GDE_BTN_H - GFX_CHAR_H) / 2,
                     "X", GFX_WHITE, COL_BTN_CLOSE, 0);

    /* Max [+] — blue */
    bx -= GDE_BTN_W + 2;
    gfx_fill_rect(bx, by, GDE_BTN_W, GDE_BTN_H, COL_BTN_MAX);
    gfx_hline(bx, by, GDE_BTN_W, GFX_RGB(65, 155, 215));
    gfx_draw_string(bx + (GDE_BTN_W - GFX_CHAR_W) / 2,
                     by + (GDE_BTN_H - GFX_CHAR_H) / 2,
                     "+", GFX_WHITE, COL_BTN_MAX, 0);

    /* Min [-] — blue */
    bx -= GDE_BTN_W + 2;
    gfx_fill_rect(bx, by, GDE_BTN_W, GDE_BTN_H, COL_BTN_MIN);
    gfx_hline(bx, by, GDE_BTN_W, GFX_RGB(65, 155, 215));
    gfx_draw_string(bx + (GDE_BTN_W - GFX_CHAR_W) / 2,
                     by + (GDE_BTN_H - GFX_CHAR_H) / 2,
                     "-", GFX_WHITE, COL_BTN_MIN, 0);

    /* Menu bar strip */
    gfx_fill_rect(win->x + 1, win->y + GDE_TITLEBAR_H,
                   win->w - 2, GDE_MENUBAR_H, COL_WIN_MENUBAR);
    gfx_hline(win->x + 1, win->y + GDE_TITLEBAR_H + GDE_MENUBAR_H,
               win->w - 2, GFX_RGB(38, 38, 44));

    /* Status bar strip */
    int sy = win->y + win->h - GDE_STATUSBAR_H - 1;
    gfx_hline(win->x + 1, sy, win->w - 2, GFX_RGB(42, 42, 48));
    gfx_fill_rect(win->x + 1, sy + 1, win->w - 2, GDE_STATUSBAR_H - 1,
                   COL_WIN_STATUSBAR);

    /* Resize grip */
    if (win->state == GDE_WIN_NORMAL) {
        gfx_draw_string(win->x + win->w - GDE_RESIZE_SZ - 2,
                         sy + (GDE_STATUSBAR_H - GFX_CHAR_H) / 2,
                         "//", GFX_RGB(105, 105, 112), COL_WIN_STATUSBAR, 0);
    }
}

/* ---- Drag state ---------------------------------------------------------- */

static int g_drag_win     = -1;  /* pool index of window being dragged */
static int g_drag_ox      = 0;   /* mouse offset from win->x on drag start */
static int g_drag_oy      = 0;
static int g_resize_win   = -1;  /* pool index of window being resized */
static int g_resize_ox    = 0;
static int g_resize_oy    = 0;
static int g_prev_buttons = 0;
static int g_pending_res_w = 0, g_pending_res_h = 0; /* deferred VBE mode switch */

/* ---- Desktop mouse event dispatch --------------------------------------- */

/* Returns pool index of window under (mx, my), or -1.
 * Searches top-to-bottom (last z-order entry first). */
static gde_window_t *hit_window(int mx, int my)
{
    int i;
    for (i = wm_count() - 1; i >= 0; i--) {
        gde_window_t *w = wm_get(i);
        if (!w || w->state == GDE_WIN_MINIMIZED) continue;
        if (mx >= w->x && mx < w->x + w->w &&
            my >= w->y && my < w->y + w->h)
            return w;
    }
    return (void*)0;
}

/* Check if (mx, my) is on a title bar button; returns 'c','m','-' or 0 */
static char title_btn_at(gde_window_t *win, int mx, int my)
{
    int by = win->y + (GDE_TITLEBAR_H - GDE_BTN_H) / 2;
    /* Close */
    int bx = win->x + win->w - GDE_BTN_W - 3;
    if (mx >= bx && mx < bx + GDE_BTN_W && my >= by && my < by + GDE_BTN_H) return 'c';
    /* Max */
    bx -= GDE_BTN_W + 2;
    if (mx >= bx && mx < bx + GDE_BTN_W && my >= by && my < by + GDE_BTN_H) return 'm';
    /* Min */
    bx -= GDE_BTN_W + 2;
    if (mx >= bx && mx < bx + GDE_BTN_W && my >= by && my < by + GDE_BTN_H) return '-';
    return 0;
}

/* Returns 1 if screen content changed (needs desktop_render), 0 if only the
 * cursor position changed (caller can use desktop_cursor_move instead). */
int desktop_handle_mouse(void)
{
    int mx      = mouse_get_x();
    int my      = mouse_get_y();
    int buttons = (int)mouse_get_buttons();
    /* Use the ISR press latch for `pressed` so rapid clicks (down+up between
     * loop iterations) are never dropped.  `released` still uses prev-vs-current
     * because we only care about the final up-state for ending drags. */
    int pressed  = (int)mouse_get_btn_latch();
    mouse_clear_btn_latch();
    int released = g_prev_buttons & ~buttons;

    /* Update context menu hover */
    if (g_ctx_open) {
        int old_hover = g_ctx_hover;
        if (mx >= g_ctx_x && mx < g_ctx_x + CTX_W &&
            my >= g_ctx_y && my < g_ctx_y + ctx_menu_h()) {
            int iy = g_ctx_y + 3, j;
            g_ctx_hover = -1;
            for (j = 0; j < g_ctx_n; j++) {
                if (g_ctx_items[j].is_sep) { iy += CTX_SEP_H; continue; }
                if (my >= iy && my < iy + CTX_ITEM_H) { g_ctx_hover = j; break; }
                iy += CTX_ITEM_H;
            }
        } else {
            g_ctx_hover = -1;
        }
        if (g_ctx_hover != old_hover) {
            gfx_dirty_mark(g_ctx_x - 2, g_ctx_y - 2, CTX_W + 6, ctx_menu_h() + 6);
            g_prev_buttons = buttons;
            return 1;
        }
    }

    /* Resize drag */
    if (g_resize_win >= 0) {
        gde_window_t *w = wm_get(g_resize_win);
        if (w) {
            /* Mark current extent before resize */
            gfx_dirty_mark(w->x - 1, w->y - 1, w->w + 6, w->h + 6);
            int nw = mx - w->x + g_resize_ox;
            int nh = my - w->y + g_resize_oy;
            if (nw < 150) nw = 150;
            if (nh < 80)  nh = 80;
            if (w->y + nh > gfx_height() - GDE_TASKBAR_H)
                nh = gfx_height() - GDE_TASKBAR_H - w->y;
            w->w = nw;
            w->h = nh;
            /* Mark new extent after resize */
            gfx_dirty_mark(w->x - 1, w->y - 1, w->w + 6, w->h + 6);
        }
        if (released & 1) {
            g_resize_win = -1;
            /* Resize ended: full redraw to re-render content at new size. */
            gfx_dirty_mark(0, 0, gfx_width(), gfx_height());
        }
        g_prev_buttons = buttons;
        return 1;
    }

    /* Title bar drag */
    if (g_drag_win >= 0) {
        gde_window_t *w = wm_get(g_drag_win);
        if (w) {
            /* Mark old position (with shadow padding) before moving */
            gfx_dirty_mark(w->x - 1, w->y - 1, w->w + 6, w->h + 6);
            w->x = mx - g_drag_ox;
            w->y = my - g_drag_oy;
            /* Clamp: keep title bar visible */
            if (w->x < 0) w->x = 0;
            if (w->y < 0) w->y = 0;
            if (w->x + w->w > gfx_width())  w->x = gfx_width() - w->w;
            if (w->y + GDE_TITLEBAR_H > gfx_height() - GDE_TASKBAR_H)
                w->y = gfx_height() - GDE_TASKBAR_H - GDE_TITLEBAR_H;
            /* Mark new position dirty */
            gfx_dirty_mark(w->x - 1, w->y - 1, w->w + 6, w->h + 6);
        }
        if (released & 1) {
            g_drag_win = -1;
            gfx_dragsave(0, 0, 0, 0);  /* invalidate drag cache */
            /* Drag ended: do a full redraw to render content from scratch. */
            gfx_dirty_mark(0, 0, gfx_width(), gfx_height());
        }
        g_prev_buttons = buttons;
        return 1;
    }

    /* Left button press */
    if (pressed & 1) {
        /* Close context menu on any click outside */
        if (g_ctx_open) {
            if (mx >= g_ctx_x && mx < g_ctx_x + CTX_W && my >= g_ctx_y &&
                my < g_ctx_y + ctx_menu_h()) {
                if (g_ctx_hover >= 0 && g_ctx_items[g_ctx_hover].fn)
                    g_ctx_items[g_ctx_hover].fn();
            }
            g_ctx_open  = 0;
            g_ctx_hover = -1;
            g_prev_buttons = buttons;
            return 1;
        }

        /* Taskbar / start menu */
        if (my >= gfx_height() - GDE_TASKBAR_H) {
            taskbar_handle_click(mx, my);
            g_prev_buttons = buttons;
            return 1;
        }

        /* Start menu click */
        if (taskbar_menu_open()) {
            taskbar_handle_click(mx, my);
            g_prev_buttons = buttons;
            return 1;
        }

        gde_window_t *hit = hit_window(mx, my);
        if (!hit) {
            /* Click on desktop — check icons (double-click to activate) */
            int i;
            for (i = 0; i < g_nicons; i++) {
                gde_icon_t *ic = &g_icons[i];
                /* Primary hitbox: icon area + margin */
                if (mx >= ic->x - ICON_HIT_MARGIN && mx < ic->x + 48 + ICON_HIT_MARGIN &&
                    my >= ic->y - ICON_HIT_MARGIN && my < ic->y + 48 + ICON_HIT_MARGIN) {
                    uint32_t now = pit_get_ticks();
                    if (g_last_icon_idx == i &&
                        (now - g_last_icon_tick) <= DBLCLICK_TICKS) {
                        /* Second click — activate */
                        if (ic->on_dblclick) ic->on_dblclick();
                        g_last_icon_idx  = -1;
                        g_last_icon_tick = 0;
                    } else {
                        /* First click — record, wait for second */
                        g_last_icon_idx  = i;
                        g_last_icon_tick = now;
                    }
                    g_prev_buttons = buttons;
                    return 1;
                }
            }
            /* Clicked blank desktop — reset double-click state */
            g_last_icon_idx = -1;
        }
        if (hit) {
            /* Focus */
            if (!hit->focused) wm_focus(hit);

            /* Title bar region */
            if (my >= hit->y && my < hit->y + GDE_TITLEBAR_H) {
                char btn = title_btn_at(hit, mx, my);
                if      (btn == 'c') { wm_close(hit); g_prev_buttons = buttons; return 1; }
                else if (btn == 'm') { wm_toggle_maximized(hit); g_prev_buttons = buttons; return 1; }
                else if (btn == '-') { wm_minimize(hit); g_prev_buttons = buttons; return 1; }
                /* Start drag — snapshot window content into drag cache */
                if (hit->state == GDE_WIN_NORMAL) {
                    g_drag_win = wm_count() - 1; /* topmost = just-focused */
                    g_drag_ox  = mx - hit->x;
                    g_drag_oy  = my - hit->y;
                    /* Back buffer should currently have a complete clean frame.
                     * Save the content area into the drag cache so each drag
                     * tick can blit it in ~1 us instead of re-rendering it. */
                    gfx_dragsave(WIN_CX(hit), WIN_CY(hit),
                                 WIN_CW(hit), WIN_CH(hit));
                }
                g_prev_buttons = buttons;
                return 1;
            }

            /* Resize handle (bottom-right corner) */
            if (hit->state == GDE_WIN_NORMAL) {
                int rx = hit->x + hit->w - GDE_RESIZE_SZ;
                int ry = hit->y + hit->h - GDE_RESIZE_SZ;
                if (mx >= rx && my >= ry) {
                    g_resize_win = wm_count() - 1;
                    g_resize_ox  = hit->w - (mx - hit->x);
                    g_resize_oy  = hit->h - (my - hit->y);
                    g_prev_buttons = buttons;
                    return 1;
                }
            }

            /* Content area — route to window's mouse handler */
            if (hit->on_mousedown) hit->on_mousedown(hit, mx, my);
            g_prev_buttons = buttons;
            return 1;
        }
    }

    /* Right button press on desktop → context menu */
    if ((pressed & 2) && my < gfx_height() - GDE_TASKBAR_H) {
        gde_window_t *hit = hit_window(mx, my);
        if (!hit) { ctx_open(mx, my); g_prev_buttons = buttons; return 1; }
    }

    /* Update start menu hover — only dirty if menu actually open */
    if (taskbar_menu_open()) {
        taskbar_menu_hover(mx, my);
        /* Mark only the start menu region — avoids full flip for hover changes */
        int smx, smy, smw, smh;
        taskbar_get_menu_rect(&smx, &smy, &smw, &smh);
        if (smw > 0) gfx_dirty_mark(smx, smy, smw, smh);
        g_prev_buttons = buttons;
        return 1;
    }

    g_prev_buttons = buttons;
    /* No content changed — caller can use desktop_cursor_move() only */
    return 0;
}

/* ---- Keyboard dispatch --------------------------------------------------- */

void desktop_handle_key(char k)
{
    /* Escape closes context/start menu */
    if (k == 27) {
        g_ctx_open = 0;
        return;
    }
    /* Route to focused window */
    gde_window_t *w = wm_focused();
    if (w && w->on_keychar && w->state != GDE_WIN_MINIMIZED)
        w->on_keychar(w, k);
}

/* ---- Scene rendering ----------------------------------------------------- */

static void render_windows(void)
{
    int i;
    for (i = 0; i < wm_count(); i++) {
        gde_window_t *w = wm_get(i);
        if (!w || w->state == GDE_WIN_MINIMIZED) continue;

        /* Save corner pixels from live back-buffer before drawing this window.
         * Restoring them afterwards shows whatever was behind (other windows or
         * desktop), not just the static bg-cache colour. */
        uint32_t sh_c[128], wn_c[128];
        gfx_save_corners(w->x + 4, w->y + 4, w->w, w->h, 6, sh_c);
        gfx_save_corners(w->x,     w->y,     w->w, w->h, 6, wn_c);

        /* Drop shadow */
        gfx_fill_rect(w->x + 4, w->y + 4, w->w, w->h, GFX_RGB(0,0,0));

        /* Window body — extend to x+1 to close the 1px gap left by BORDER_W=2 */
        gfx_fill_rect(w->x + 1, WIN_CY(w), w->w - 2, WIN_CH(w), COL_WIN_BODY);

        /* Title bar + buttons */
        win_draw_titlebar(w);

        /* Content */
        if (i == g_drag_win && gfx_dragcache_valid()) {
            gfx_dragstamp(WIN_CX(w), WIN_CY(w));
        } else if (w->draw_content) {
            w->draw_content(w);
        }
        /* Restore: corners are transparent through to what was composited below */
        gfx_restore_corners(w->x + 4, w->y + 4, w->w, w->h, 6, sh_c);
        gfx_restore_corners(w->x,     w->y,     w->w, w->h, 6, wn_c);
    }
}

static void render_icons(void)
{
    int i;
    for (i = 0; i < g_nicons; i++) icon_render(&g_icons[i]);
}

void desktop_render(void)
{
    /* Full render: mark entire screen dirty so desktop_present does full flip. */
    gfx_dirty_mark(0, 0, gfx_width(), gfx_height());
    desktop_present();
}

/* Partial-or-full render based on the accumulated dirty rect.
 *
 * If the dirty area is small (typical for drag/resize/menu hover):
 *   - Restore bg only in that rect   (~40-400 KB instead of 3 MB)
 *   - Draw all layers into back buffer (RAM writes — fast)
 *   - Flip only the dirty rect to VRAM (~40-400 KB instead of 3 MB)
 *
 * If the dirty area covers > 60% of the screen (or nothing is marked):
 *   - Fall back to a full render + full flip for correctness.
 *
 * Call gfx_dirty_reset() is done internally at the end. */
#define FULL_THRESH (gfx_width() * gfx_height() * 6 / 10)

void desktop_present(void)
{
    /* Apply any pending resolution change from display settings. */
    if (g_pending_res_w > 0) {
        gfx_set_resolution(g_pending_res_w, g_pending_res_h);
        desktop_build_bg();
        while (wm_count() > 0) wm_close(wm_get(wm_count() - 1));
        g_pending_res_w = 0;
        g_pending_res_h = 0;
        gfx_dirty_mark(0, 0, gfx_width(), gfx_height());
    }

    int dx, dy, dw, dh;
    int partial = gfx_dirty_get(&dx, &dy, &dw, &dh);

    if (!partial || dw * dh >= FULL_THRESH) {
        /* Full render path */
        gfx_restore_bg();
        render_icons();
        render_windows();
        taskbar_render();
        ctx_render();
        gfx_flip();
    } else {
        /* Partial render path:
         * Back buffer compositing is done fully (cheap — all RAM writes).
         * Only the dirty region gets sent to VRAM via gfx_flip_rect(). */
        gfx_restore_bg_rect(dx, dy, dw, dh);
        render_icons();
        render_windows();
        /* Taskbar: only re-render if dirty rect touches it */
        if (dy + dh > gfx_height() - GDE_TASKBAR_H)
            taskbar_render();
        ctx_render();
        gfx_flip_rect(dx, dy, dw, dh);
    }

    gfx_dirty_reset();

    /* Stamp cursor on real FB using stored position — no -999 hack.
     * gfx_cursor_blit unions old+new rects (each 14×18 = 252 px).
     * Whether or not the old position was inside the dirty region:
     *   - was inside: partial flip already wrote back-buffer there (cursor-free);
     *     the union-blit's erase step overwrites with identical data. Safe.
     *   - was outside: cursor still showing in real FB; erase step removes it. */
    cursor_move(mouse_get_x(), mouse_get_y());
}

void desktop_mark_dirty(void) {}  /* Hook for future use */

/* ---- Start menu actions (forward-declared, registered in desktop_init) -- */

static void action_open_terminal(void) { terminal_open(); }
static void action_open_explorer(void) { explorer_open(); }

/* Write decimal integer v into buf at offset off; returns new offset. */
static int buf_dec(char *buf, int off, int v)
{
    char tmp[12]; int n = 0, i;
    do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v > 0);
    for (i = n - 1; i >= 0; i--) buf[off++] = tmp[i];
    return off;
}

static void about_draw(gde_window_t *win)
{
    int cx=WIN_CX(win), cy=WIN_CY(win), cw=WIN_CW(win), ch=WIN_CH(win);
    gfx_fill_rect(cx, cy, cw, ch, GFX_RGB(22,24,34));
    /* Logo tile */
    gfx_fill_rect(cx+12, cy+12, 52, 52, GFX_RGB(38,78,180));
    gfx_draw_rect(cx+12, cy+12, 52, 52, GFX_RGB(78,128,240));
    gfx_draw_string(cx+20, cy+20, "PD",  GFX_WHITE, GFX_RGB(38,78,180), 0);
    gfx_draw_string(cx+20, cy+38, "OS",  GFX_RGB(180,200,255), GFX_RGB(38,78,180), 0);
    /* Info */
    gfx_draw_string(cx+76, cy+14, "PD-OS",         GFX_WHITE,            GFX_RGB(22,24,34), 1);
    gfx_draw_string(cx+76, cy+32, "Version 1.0",   GFX_RGB(175,180,210), GFX_RGB(22,24,34), 1);
    gfx_draw_string(cx+76, cy+48, "Build 2026.08", GFX_RGB(135,140,168), GFX_RGB(22,24,34), 1);
    gfx_hline(cx+8, cy+76, cw-16, GFX_RGB(48,52,78));
    gfx_draw_string(cx+12, cy+86,  "Architecture : i686 (32-bit x86 protected mode)", GFX_RGB(180,185,215), GFX_RGB(22,24,34), 1);
    {
        char ds[48]; int i = 0;
        const char *pre = "Display      : ", *suf = ", 32 bpp VBE";
        while (*pre) ds[i++] = *pre++;
        i = buf_dec(ds, i, gfx_width());
        ds[i++] = ' '; ds[i++] = 'x'; ds[i++] = ' ';
        i = buf_dec(ds, i, gfx_height());
        while (*suf) ds[i++] = *suf++;
        ds[i] = '\0';
        gfx_draw_string(cx+12, cy+102, ds, GFX_RGB(180,185,215), GFX_RGB(22,24,34), 1);
    }
    gfx_draw_string(cx+12, cy+118, "Filesystem   : PDFS v3 (PD chained-dir FS)",      GFX_RGB(180,185,215), GFX_RGB(22,24,34), 1);
    gfx_draw_string(cx+12, cy+134, "Memory       : 128 MB",                            GFX_RGB(180,185,215), GFX_RGB(22,24,34), 1);
    gfx_hline(cx+8, cy+154, cw-16, GFX_RGB(48,52,78));
    gfx_draw_string(cx+12, cy+162, "(c) 2026 PD-OS Project. All rights reserved.", GFX_RGB(105,110,138), GFX_RGB(22,24,34), 1);
}

/* ---- Display Settings --------------------------------------------------- */

static int g_disp_sel = 2;

static const int  g_disp_res[6][2]  = {{640,480},{800,600},{1024,768},{1280,720},{1366,768},{1920,1080}};
static const char *g_disp_labels[6] = {"640 x 480","800 x 600","1024 x 768","1280 x 720","1366 x 768","1920 x 1080"};
#define NDISP_RES 6

static void display_settings_draw(gde_window_t *win)
{
    int cx=WIN_CX(win), cy=WIN_CY(win), cw=WIN_CW(win), ch=WIN_CH(win);
    int i;
    gfx_fill_rect(cx, cy, cw, ch, GFX_RGB(24,26,36));
    gfx_draw_string(cx+12, cy+10, "Resolution", GFX_RGB(158,163,192), GFX_RGB(24,26,36), 1);
    gfx_hline(cx+8, cy+28, cw-16, GFX_RGB(52,56,84));

    for (i = 0; i < NDISP_RES; i++) {
        int ry = cy + 36 + i * 27;
        int sel = (i == g_disp_sel);
        uint32_t bg = sel ? GFX_RGB(36,62,138) : GFX_RGB(30,32,48);
        uint32_t bd = sel ? GFX_RGB(68,108,220) : GFX_RGB(52,56,84);
        uint32_t fg = sel ? GFX_WHITE : GFX_RGB(158,163,192);
        gfx_fill_rect(cx+12, ry, cw-24, 23, bg);
        gfx_draw_rect(cx+12, ry, cw-24, 23, bd);
        if (sel)
            gfx_fill_rect(cx+20, ry+8, 6, 6, GFX_RGB(100,160,255));
        gfx_draw_string(cx+32, ry+3, g_disp_labels[i], fg, bg, 0);
        /* mark current active resolution */
        if (g_disp_res[i][0] == gfx_width() && g_disp_res[i][1] == gfx_height())
            gfx_draw_string(cx+cw-60, ry+3, "(active)", GFX_RGB(88,128,200), bg, 0);
    }

    gfx_hline(cx+8, cy+36+NDISP_RES*27+4, cw-16, GFX_RGB(52,56,84));
    gfx_draw_string(cx+12, cy+36+NDISP_RES*27+14,
                    "Color depth: 32 bpp   Refresh: 60 Hz",
                    GFX_RGB(88,93,118), GFX_RGB(24,26,36), 1);

    gfx_hline(cx+8, cy+ch-52, cw-16, GFX_RGB(52,56,84));
    gfx_draw_string(cx+12, cy+ch-40,
                    "Resolution takes effect immediately.",
                    GFX_RGB(115,120,148), GFX_RGB(24,26,36), 1);

    int is_active = (g_disp_res[g_disp_sel][0] == gfx_width() &&
                     g_disp_res[g_disp_sel][1] == gfx_height());
    uint32_t bbg = is_active ? GFX_RGB(30,32,48)  : GFX_RGB(36,62,138);
    uint32_t bbd = is_active ? GFX_RGB(52,56,84)  : GFX_RGB(68,108,220);
    uint32_t bfg = is_active ? GFX_RGB(68,73,98)  : GFX_WHITE;
    gfx_fill_rect(cx+cw-88, cy+ch-34, 72, 24, bbg);
    gfx_draw_rect(cx+cw-88, cy+ch-34, 72, 24, bbd);
    gfx_draw_string(cx+cw-68, cy+ch-26, "Apply", bfg, bbg, 0);
}

static void display_settings_mousedown(gde_window_t *win, int mx, int my)
{
    int cx=WIN_CX(win), cy=WIN_CY(win), cw=WIN_CW(win), ch=WIN_CH(win);
    int i;
    /* Resolution list rows */
    for (i = 0; i < NDISP_RES; i++) {
        int ry = cy + 36 + i * 27;
        if (mx >= cx+12 && mx < cx+12+(cw-24) && my >= ry && my < ry+23) {
            g_disp_sel = i;
            return;
        }
    }
    /* Apply button */
    if (mx >= cx+cw-88 && mx < cx+cw-16 && my >= cy+ch-34 && my < cy+ch-10) {
        int nw = g_disp_res[g_disp_sel][0];
        int nh = g_disp_res[g_disp_sel][1];
        if (nw != gfx_width() || nh != gfx_height()) {
            g_pending_res_w = nw;
            g_pending_res_h = nh;
        }
    }
}

static void action_about(void)
{
    gde_window_t *w = wm_create("About PD-OS", 312, 224, 420, 210,
                                  about_draw, (void*)0);
    (void)w;
}

static void action_display_settings(void)
{
    gde_window_t *w = wm_create("Display Settings", 242, 124, 480, 420,
                                  display_settings_draw, (void*)0);
    if (w) w->on_mousedown = display_settings_mousedown;
}

static void action_new_textfile(void)
{
    const char *path = "/home/root/untitled.txt";
    vfs_node_t node;
    if (vfs_open(path, &node) != 0) vfs_create(path);
    text_editor_open(path);
}

static void action_exit_gde(void)
{
    /* Reboot: triple-fault or 8042 reset */
    /* Use 8042 keyboard controller reset line (port 0x64, command 0xFE) */
    __asm__ volatile (
        "cli\n"
        "movb  $0xFE, %%al\n"
        "outb  %%al, $0x64\n"
        "hlt\n"
        ::: "eax"
    );
}

/* ---- Init ---------------------------------------------------------------- */

static void desktop_build_bg(void)
{
    int w = gfx_width(), h = gfx_height() - GDE_TASKBAR_H;
    gfx_fill_rect_grad(0, 0, w, h, COL_DESKTOP, GFX_RGB(0,60,80));
    int gx, gy;
    for (gy = 0; gy < h; gy += 48) gfx_hline(0, gy, w, GFX_RGB(0,80,100));
    for (gx = 0; gx < w; gx += 48) gfx_vline(gx, 0, h, GFX_RGB(0,80,100));
    gfx_cache_bg();
}

void desktop_init(void)
{
    /* Build cursor sprite once */
    cursor_sprite_init();

    /* Desktop icons */
    icon_add("T", 16, 16, action_open_terminal);   /* Terminal */
    icon_add("E", 16, 80, action_open_explorer);   /* PD-Explorer */

    /* Desktop right-click context menu */
    g_ctx_n = 0;
    #define CTXITEM(lbl, f, sep) do { \
        ctx_item_t *it = &g_ctx_items[g_ctx_n++]; \
        const char *_s = (lbl); int _i; \
        for (_i=0; _s[_i] && _i<31; _i++) it->label[_i]=_s[_i]; it->label[_i]='\0'; \
        it->fn=(f); it->is_sep=(sep); } while(0)
    CTXITEM("New Text File",    action_new_textfile,      0);
    CTXITEM("New Terminal",     action_open_terminal,     0);
    CTXITEM("",                 (void*)0,                 1);
    CTXITEM("Display Settings", action_display_settings,  0);
    CTXITEM("",                 (void*)0,                 1);
    CTXITEM("About PD-OS",      action_about,             0);
    #undef CTXITEM

    /* Start menu */
    taskbar_add_menu_item("Terminal",   0, action_open_terminal);
    taskbar_add_menu_item("Explorer",   0, action_open_explorer);
    taskbar_add_menu_item("",           1, (void*)0); /* separator */
    taskbar_add_menu_item("About",      0, action_about);
    taskbar_add_menu_item("",           1, (void*)0);
    taskbar_add_menu_item("Reboot",     0, action_exit_gde);

    desktop_build_bg();
}
