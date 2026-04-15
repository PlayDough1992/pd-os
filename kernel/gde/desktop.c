/* ============================================================================
 * PD-OS GDE  —  Desktop: rendering and event dispatch
 * ============================================================================ */

#include "gde.h"
#include "gfx.h"
#include "mouse.h"
#include "keyboard.h"
#include "pit.h"

/* ---- Cursor (sprite + atomic union-rect blit) --------------------------- */

#define CURSOR_W 14
#define CURSOR_H 18

/* Pre-rendered sprite and mask — built once in desktop_init(). */
static uint32_t g_cursor_sprite[CURSOR_W * CURSOR_H];
static uint8_t  g_cursor_mask  [CURSOR_W * CURSOR_H];
static uint32_t g_cursor_save  [CURSOR_W * CURSOR_H];
static int      g_cursor_sx = -999, g_cursor_sy = -999;

static void cursor_sprite_init(void)
{
    int r, c;
    for (r = 0; r < CURSOR_H; r++)
        for (c = 0; c < CURSOR_W; c++)
            g_cursor_mask[r * CURSOR_W + c] = 0;

    for (r = 0; r <= 11; r++) {
        g_cursor_sprite[r * CURSOR_W + 0] = GFX_BLACK;
        g_cursor_mask  [r * CURSOR_W + 0] = 1;
        for (c = 1; c < r; c++) {
            g_cursor_sprite[r * CURSOR_W + c] = GFX_WHITE;
            g_cursor_mask  [r * CURSOR_W + c] = 1;
        }
        if (r > 0) {
            g_cursor_sprite[r * CURSOR_W + r] = GFX_BLACK;
            g_cursor_mask  [r * CURSOR_W + r] = 1;
        }
    }
    for (r = 12; r < CURSOR_H; r++) {
        g_cursor_sprite[r * CURSOR_W + 0] = GFX_BLACK;
        g_cursor_sprite[r * CURSOR_W + 1] = GFX_WHITE;
        g_cursor_sprite[r * CURSOR_W + 2] = GFX_BLACK;
        g_cursor_mask  [r * CURSOR_W + 0] = 1;
        g_cursor_mask  [r * CURSOR_W + 1] = 1;
        g_cursor_mask  [r * CURSOR_W + 2] = 1;
    }
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

#define CTX_MAX 6

typedef struct { char label[32]; void (*fn)(void); } ctx_item_t;
static ctx_item_t g_ctx_items[CTX_MAX];
static int        g_ctx_n    = 0;
static int        g_ctx_open = 0;
static int        g_ctx_x = 0, g_ctx_y = 0;
static int        g_ctx_hover = -1;

#define CTX_ITEM_H 20
#define CTX_W      160

static void ctx_open(int x, int y)
{
    g_ctx_x    = x;
    g_ctx_y    = y;
    g_ctx_open = 1;
    g_ctx_hover = -1;
    /* Clamp to screen */
    if (g_ctx_x + CTX_W > GDE_SCREEN_W)
        g_ctx_x = GDE_SCREEN_W - CTX_W - 2;
    if (g_ctx_y + g_ctx_n * CTX_ITEM_H + 6 > GDE_SCREEN_H - GDE_TASKBAR_H)
        g_ctx_y = GDE_SCREEN_H - GDE_TASKBAR_H - g_ctx_n * CTX_ITEM_H - 6;
}

static void ctx_render(void)
{
    if (!g_ctx_open) return;
    int menu_h = g_ctx_n * CTX_ITEM_H + 6;
    gfx_fill_rect(g_ctx_x + 2, g_ctx_y + 2, CTX_W, menu_h, GFX_RGB(0,0,0));
    gfx_fill_rect(g_ctx_x, g_ctx_y, CTX_W, menu_h, COL_MENU_BG);
    gfx_draw_rect(g_ctx_x, g_ctx_y, CTX_W, menu_h, COL_MENU_BORDER);
    int i;
    for (i = 0; i < g_ctx_n; i++) {
        int iy = g_ctx_y + 3 + i * CTX_ITEM_H;
        if (i == g_ctx_hover)
            gfx_fill_rect(g_ctx_x + 1, iy, CTX_W - 2, CTX_ITEM_H, COL_MENU_HOVER);
        gfx_draw_string(g_ctx_x + 8,
                         iy + (CTX_ITEM_H - GFX_CHAR_H) / 2,
                         g_ctx_items[i].label, COL_MENU_TEXT,
                         (i == g_ctx_hover) ? COL_MENU_HOVER : COL_MENU_BG, 0);
    }
}

/* ---- Window hit testing & title bar rendering ---------------------------- */

static void win_draw_titlebar(gde_window_t *win)
{
    uint32_t tc1 = win->focused ? COL_TITLEBAR_A1 : COL_TITLEBAR_I1;
    uint32_t tc2 = win->focused ? COL_TITLEBAR_A2 : COL_TITLEBAR_I2;
    uint32_t bc  = win->focused ? COL_WIN_BORDER_A : COL_WIN_BORDER_I;

    /* Border */
    gfx_draw_rect(win->x, win->y, win->w, win->h, bc);

    /* Title bar gradient */
    gfx_fill_rect_grad(win->x + 1, win->y + 1,
                        win->w - 2, GDE_TITLEBAR_H - 2, tc1, tc2);

    /* Title text */
    int ty = win->y + (GDE_TITLEBAR_H - GFX_CHAR_H) / 2;
    gfx_draw_string(win->x + 8, ty, win->title, GFX_WHITE, tc2, 1);

    /* Buttons (close / max / min) from right */
    int bx = win->x + win->w - GDE_BTN_W - 3;
    int by = win->y + (GDE_TITLEBAR_H - GDE_BTN_H) / 2;

    /* Close [X] */
    gfx_fill_rect(bx, by, GDE_BTN_W, GDE_BTN_H, COL_BTN_CLOSE);
    gfx_draw_rect(bx, by, GDE_BTN_W, GDE_BTN_H, GFX_RGB(220,60,60));
    gfx_draw_string(bx + (GDE_BTN_W - GFX_CHAR_W) / 2,
                     by + (GDE_BTN_H - GFX_CHAR_H) / 2,
                     "X", GFX_WHITE, COL_BTN_CLOSE, 0);

    /* Max [□] */
    bx -= GDE_BTN_W + 2;
    gfx_fill_rect(bx, by, GDE_BTN_W, GDE_BTN_H, COL_BTN_MAX);
    gfx_draw_rect(bx, by, GDE_BTN_W, GDE_BTN_H, GFX_RGB(70,200,70));
    gfx_draw_string(bx + (GDE_BTN_W - GFX_CHAR_W) / 2,
                     by + (GDE_BTN_H - GFX_CHAR_H) / 2,
                     "+", GFX_WHITE, COL_BTN_MAX, 0);

    /* Min [-] */
    bx -= GDE_BTN_W + 2;
    gfx_fill_rect(bx, by, GDE_BTN_W, GDE_BTN_H, COL_BTN_MIN);
    gfx_draw_rect(bx, by, GDE_BTN_W, GDE_BTN_H, GFX_RGB(220,170,20));
    gfx_draw_string(bx + (GDE_BTN_W - GFX_CHAR_W) / 2,
                     by + (GDE_BTN_H - GFX_CHAR_H) / 2,
                     "-", GFX_WHITE, COL_BTN_MIN, 0);

    /* Resize handle (bottom-right corner) */
    if (win->state == GDE_WIN_NORMAL) {
        int rx = win->x + win->w - GDE_RESIZE_SZ;
        int ry = win->y + win->h - GDE_RESIZE_SZ;
        gfx_fill_rect(rx, ry, GDE_RESIZE_SZ, GDE_RESIZE_SZ,
                       GFX_RGB(80,80,90));
        gfx_draw_string(rx + 2, ry + 2, "//", GFX_RGB(130,130,140),
                         GFX_RGB(80,80,90), 0);
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
        if (mx >= g_ctx_x && mx < g_ctx_x + CTX_W && my >= g_ctx_y) {
            g_ctx_hover = (my - g_ctx_y - 3) / CTX_ITEM_H;
            if (g_ctx_hover < 0 || g_ctx_hover >= g_ctx_n) g_ctx_hover = -1;
        } else {
            g_ctx_hover = -1;
        }
        if (g_ctx_hover != old_hover) {
            /* Only the context menu rect changed */
            gfx_dirty_mark(g_ctx_x - 2, g_ctx_y - 2,
                           CTX_W + 6, g_ctx_n * CTX_ITEM_H + 10);
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
            if (w->y + nh > GDE_SCREEN_H - GDE_TASKBAR_H)
                nh = GDE_SCREEN_H - GDE_TASKBAR_H - w->y;
            w->w = nw;
            w->h = nh;
            /* Mark new extent after resize */
            gfx_dirty_mark(w->x - 1, w->y - 1, w->w + 6, w->h + 6);
        }
        if (released & 1) {
            g_resize_win = -1;
            /* Resize ended: full redraw to re-render content at new size. */
            gfx_dirty_mark(0, 0, GDE_SCREEN_W, GDE_SCREEN_H);
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
            if (w->x + w->w > GDE_SCREEN_W)  w->x = GDE_SCREEN_W - w->w;
            if (w->y + GDE_TITLEBAR_H > GDE_SCREEN_H - GDE_TASKBAR_H)
                w->y = GDE_SCREEN_H - GDE_TASKBAR_H - GDE_TITLEBAR_H;
            /* Mark new position dirty */
            gfx_dirty_mark(w->x - 1, w->y - 1, w->w + 6, w->h + 6);
        }
        if (released & 1) {
            g_drag_win = -1;
            gfx_dragsave(0, 0, 0, 0);  /* invalidate drag cache */
            /* Drag ended: do a full redraw to render content from scratch. */
            gfx_dirty_mark(0, 0, GDE_SCREEN_W, GDE_SCREEN_H);
        }
        g_prev_buttons = buttons;
        return 1;
    }

    /* Left button press */
    if (pressed & 1) {
        /* Close context menu on any click outside */
        if (g_ctx_open) {
            if (mx >= g_ctx_x && mx < g_ctx_x + CTX_W && my >= g_ctx_y &&
                my < g_ctx_y + g_ctx_n * CTX_ITEM_H + 6) {
                if (g_ctx_hover >= 0 && g_ctx_items[g_ctx_hover].fn)
                    g_ctx_items[g_ctx_hover].fn();
            }
            g_ctx_open  = 0;
            g_ctx_hover = -1;
            g_prev_buttons = buttons;
            return 1;
        }

        /* Taskbar / start menu */
        if (my >= GDE_SCREEN_H - GDE_TASKBAR_H) {
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
    if ((pressed & 2) && my < GDE_SCREEN_H - GDE_TASKBAR_H) {
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

        /* Drop shadow */
        gfx_fill_rect(w->x + 4, w->y + 4, w->w, w->h, GFX_RGB(0,0,0));

        /* Window body */
        gfx_fill_rect(WIN_CX(w), WIN_CY(w), WIN_CW(w), WIN_CH(w), COL_WIN_BODY);

        /* Title bar + buttons */
        win_draw_titlebar(w);

        /* Content:
         * During drag of THIS window, blit the pre-snapped drag cache instead
         * of calling draw_content().  One rep-movsl per row vs 1628 draw_char
         * calls — the per-frame work drops from ~200K scattered pixel writes
         * to a single tight memcpy, eliminating the drag stutter.
         * During resize, or for all other windows, always call draw_content. */
        if (i == g_drag_win && gfx_dragcache_valid()) {
            gfx_dragstamp(WIN_CX(w), WIN_CY(w));
        } else if (w->draw_content) {
            w->draw_content(w);
        }
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
    gfx_dirty_mark(0, 0, GDE_SCREEN_W, GDE_SCREEN_H);
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
#define FULL_THRESH (GDE_SCREEN_W * GDE_SCREEN_H * 6 / 10)

void desktop_present(void)
{
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
        if (dy + dh > GDE_SCREEN_H - GDE_TASKBAR_H)
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

static void action_about(void)
{
    gde_window_t *w = wm_create("About PD-OS", 312, 284, 400, 200,
                                  (void*)0, (void*)0);
    if (!w) return;
    /* Simple static content — drawn inline each frame via draw_content */
    (void)w;
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

void desktop_init(void)
{
    /* Build cursor sprite once */
    cursor_sprite_init();

    /* Desktop icons */
    icon_add("T", 16, 16, action_open_terminal);   /* Terminal */
    icon_add("E", 16, 80, action_open_explorer);   /* PD-Explorer */

    /* Context menu items */
    g_ctx_n = 0;
    {
        ctx_item_t *it = &g_ctx_items[g_ctx_n++];
        const char *s = "New Terminal"; int i;
        for (i = 0; s[i] && i < 31; i++) { it->label[i] = s[i]; }
        it->label[i] = '\0';
        it->fn = action_open_terminal;
    }
    {
        ctx_item_t *it = &g_ctx_items[g_ctx_n++];
        const char *s = "About PD-OS"; int i;
        for (i = 0; s[i] && i < 31; i++) { it->label[i] = s[i]; }
        it->label[i] = '\0';
        it->fn = action_about;
    }

    /* Start menu */
    taskbar_add_menu_item("Terminal",   0, action_open_terminal);
    taskbar_add_menu_item("Explorer",   0, action_open_explorer);
    taskbar_add_menu_item("",           1, (void*)0); /* separator */
    taskbar_add_menu_item("About",      0, action_about);
    taskbar_add_menu_item("",           1, (void*)0);
    taskbar_add_menu_item("Reboot",     0, action_exit_gde);

    /* Pre-render desktop background once and cache it.
     * Each frame restores the snapshot via gfx_restore_bg() instead of
     * recomputing 738 gradient rows and all the grid lines. */
    gfx_fill_rect_grad(0, 0, GDE_SCREEN_W, GDE_SCREEN_H - GDE_TASKBAR_H,
                        COL_DESKTOP, GFX_RGB(0,60,80));
    {
        int gx, gy;
        for (gy = 0; gy < GDE_SCREEN_H - GDE_TASKBAR_H; gy += 48)
            gfx_hline(0, gy, GDE_SCREEN_W, GFX_RGB(0,80,100));
        for (gx = 0; gx < GDE_SCREEN_W; gx += 48)
            gfx_vline(gx, 0, GDE_SCREEN_H - GDE_TASKBAR_H, GFX_RGB(0,80,100));
    }
    gfx_cache_bg();
}
