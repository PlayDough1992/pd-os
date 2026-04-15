/*
 * pdwm.c — PD Window Manager
 *
 * Windows ME-inspired desktop environment for PD-OS.
 *
 * Layout
 * ──────
 *   ┌────────────────────── screen ─────────────────────────────┐
 *   │  teal desktop (GFX_COLOR_DESKTOP)                          │
 *   │   ┌──── window ────────────────────────────────┐           │
 *   │   │ [gradient title bar]   [_][□][×]           │           │
 *   │   │ client area (white)                        │           │
 *   │   └────────────────────────────────────────────┘           │
 *   ├── taskbar ─────────────────────────── clock ──────────────┤
 *   │ [Start]  [window buttons...]            HH:MM:SS DD-MM-YY  │
 *   └────────────────────────────────────────────────────────────┘
 *
 * The event loop:
 *   - polls keyboard via keyboard_getchar() (non-blocking)
 *   - mouse events arrive via IRQ12 callback
 *   - redraws dirty windows each iteration
 *   - updates taskbar clock once per second (via PIT tick counter)
 */

#include "pdwm.h"
#include "gfx.h"
#include "keyboard.h"
#include "mouse.h"
#include "rtc.h"
#include "shell.h"
#include "pit.h"
#include "io.h"
#include "kernel.h"
#include "users.h"

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */
static pdwm_window_t g_windows[PDWM_MAX_WINDOWS];
static int           g_win_count  = 0;
static int           g_focused    = -1;
static volatile int     g_mouse_x    = 0;
static volatile int     g_mouse_y    = 0;
static volatile uint8_t g_mouse_btns = 0;
static volatile int     g_dirty      = 1;  /* full redraw needed          */
static volatile int     g_mouse_moved = 0; /* cursor position changed      */

/* ------------------------------------------------------------------ */
/* Cursor sprite background save buffer                               */
/*                                                                    */
/* The arrow sprite draws:                                            */
/*   - black fill  at  (x+0..7,  y+0..11)                            */
/*   - white outline at (x-1, y+r) and (x+c, y-1)  ← OUTSIDE box!  */
/*   - shadow        at (x+1..8, y+1..12)                            */
/* Save a padded region so cursor_erase() catches every pixel.        */
/* ------------------------------------------------------------------ */
#define CURS_W     16
#define CURS_H     16
#define CURS_PAD    2   /* pixels of padding on each side              */
#define CURS_SAVE_W (CURS_W + 2 * CURS_PAD)
#define CURS_SAVE_H (CURS_H + 2 * CURS_PAD)
static gfx_color_t g_cursor_bg[CURS_SAVE_W * CURS_SAVE_H];
static int         g_curs_saved_x = -1;  /* top-left of saved region  */
static int         g_curs_saved_y = -1;

/* Mouse cursor arrow shape — 8-wide × 12-tall bitmask (MSB left) */
static const uint16_t g_cursor_shape[12] = {
    0x8000, 0xC000, 0xE000, 0xF000,
    0xF800, 0xFC00, 0xFE00, 0xFF00,
    0xF800, 0xD800, 0x8C00, 0x0600,
};

/* ------------------------------------------------------------------ */
/* Active terminal window wid                                          */
/* ------------------------------------------------------------------ */
static int g_term_wid = -1;

/* ------------------------------------------------------------------ */
/* Forward declarations                                                */
/* ------------------------------------------------------------------ */
static void draw_desktop(void);
static void draw_cursor(int x, int y);
static void on_mouse(int x, int y, uint8_t btns);
static void handle_click(int x, int y, uint8_t btns);

/* ------------------------------------------------------------------ */
/* Window helpers                                                      */
/* ------------------------------------------------------------------ */

static void compute_client(pdwm_window_t *w)
{
    w->client_x = w->x + PDWM_BORDER_W;
    w->client_y = w->y + PDWM_TITLEBAR_H + PDWM_BORDER_W;
    w->client_w = w->w - 2 * PDWM_BORDER_W;
    w->client_h = w->h - PDWM_TITLEBAR_H - 2 * PDWM_BORDER_W;
    if (w->client_w < 1) w->client_w = 1;
    if (w->client_h < 1) w->client_h = 1;
    w->tx = 0;
    w->ty = 0;
}

int pdwm_create_window(const char *title, int x, int y, int w, int h,
                        uint8_t flags)
{
    if (g_win_count >= PDWM_MAX_WINDOWS) return -1;
    int wid = g_win_count++;
    pdwm_window_t *win = &g_windows[wid];
    win->x = x; win->y = y; win->w = w; win->h = h;
    win->flags = flags | PDWM_WIN_VISIBLE;
    /* Copy title */
    int i = 0;
    while (title[i] && i < 63) { win->title[i] = title[i]; i++; }
    win->title[i] = '\0';
    compute_client(win);
    return wid;
}

void pdwm_close_window(int wid)
{
    if (wid < 0 || wid >= g_win_count) return;
    g_windows[wid].flags &= ~PDWM_WIN_VISIBLE;
    if (g_focused == wid) g_focused = -1;
    g_dirty = 1;
}

void pdwm_focus_window(int wid)
{
    for (int i = 0; i < g_win_count; i++)
        g_windows[i].flags &= ~PDWM_WIN_FOCUSED;
    if (wid >= 0 && wid < g_win_count)
        g_windows[wid].flags |= PDWM_WIN_FOCUSED;
    g_focused = wid;
    g_dirty = 1;
}

/* ------------------------------------------------------------------ */
/* Draw a single window                                                */
/* ------------------------------------------------------------------ */
void pdwm_draw_window(int wid)
{
    if (wid < 0 || wid >= g_win_count) return;
    pdwm_window_t *w = &g_windows[wid];
    if (!(w->flags & PDWM_WIN_VISIBLE)) return;

    int focused = (w->flags & PDWM_WIN_FOCUSED) ? 1 : 0;

    /* Outer frame / border */
    gfx_rounded_rect(w->x, w->y, w->w, w->h,
                     PDWM_CORNER_R,
                     GFX_COLOR_WIN_BORDER, GFX_COLOR_SHADOW);

    /* Title bar gradient */
    gfx_color_t tb_top = focused ? GFX_COLOR_TITLEBAR_ACT : GFX_COLOR_TITLEBAR_INA;
    gfx_color_t tb_bot = focused ? GFX_COLOR_TITLEBAR_HI  : GFX_COLOR_TITLEBAR_INA;
    gfx_gradient_rect(w->x + PDWM_BORDER_W,
                      w->y + PDWM_BORDER_W,
                      w->w - 2 * PDWM_BORDER_W,
                      PDWM_TITLEBAR_H - PDWM_BORDER_W,
                      tb_top, tb_bot);

    /* Title text */
    gfx_text(w->x + PDWM_BORDER_W + 4,
             w->y + PDWM_BORDER_W + (PDWM_TITLEBAR_H - PDWM_BORDER_W - 8) / 2,
             w->title,
             GFX_COLOR_TEXT_LIGHT, GFX_TRANSPARENT);

    /* Close button [×] — right side of title bar */
    int btn_y = w->y + PDWM_BORDER_W + 2;
    int btn_x = w->x + w->w - PDWM_BORDER_W - 16;
    gfx_rounded_rect(btn_x, btn_y, 14, 14, 2,
                     GFX_RGB(220, 50, 32), GFX_COLOR_SHADOW);
    gfx_char(btn_x + 3, btn_y + 3, 'x', GFX_COLOR_WHITE, GFX_TRANSPARENT);

    /* Minimise button [_] */
    int btn_x2 = btn_x - 16;
    gfx_rounded_rect(btn_x2, btn_y, 14, 14, 2,
                     GFX_COLOR_WIN_BORDER, GFX_COLOR_SHADOW);
    gfx_char(btn_x2 + 3, btn_y + 3, '_', GFX_COLOR_TEXT_DARK, GFX_TRANSPARENT);

    /* Client area */
    gfx_fill_rect(w->client_x, w->client_y,
                  w->client_w, w->client_h,
                  GFX_COLOR_WIN_CLIENT);
}

void pdwm_draw_all(void)
{
    draw_desktop();
    for (int i = 0; i < g_win_count; i++)
        pdwm_draw_window(i);
    pdwm_draw_taskbar();
}

/* ------------------------------------------------------------------ */
/* Taskbar                                                             */
/* ------------------------------------------------------------------ */
void pdwm_draw_taskbar(void)
{
    int sw = gfx_width();
    int sh = gfx_height();
    int ty = sh - PDWM_TASKBAR_H;

    /* Background */
    gfx_fill_rect(0, ty, sw, PDWM_TASKBAR_H, GFX_COLOR_TASKBAR);
    /* Top edge shadow */
    gfx_hline(0, ty, sw, GFX_COLOR_SHADOW);

    /* Start button */
    gfx_rounded_rect(2, ty + 2, PDWM_START_BTN_W, PDWM_TASKBAR_H - 4, 4,
                     GFX_COLOR_START_BTN, GFX_COLOR_SHADOW);
    gfx_text(8, ty + (PDWM_TASKBAR_H - 8) / 2, "Start",
             GFX_COLOR_WHITE, GFX_TRANSPARENT);

    /* Clock */
    rtc_time_t rt;
    rtc_read(&rt);
    char tbuf[9];
    rtc_format_time(&rt, tbuf);
    int clock_x = sw - gfx_text_width(tbuf) - 8;
    gfx_text(clock_x, ty + (PDWM_TASKBAR_H - 8) / 2,
             tbuf, GFX_COLOR_TEXT_DARK, GFX_COLOR_TASKBAR);

    /* Windows buttons in taskbar */
    int bx = PDWM_START_BTN_W + 8;
    for (int i = 0; i < g_win_count && bx < clock_x - 4; i++) {
        pdwm_window_t *w = &g_windows[i];
        if (!(w->flags & PDWM_WIN_VISIBLE)) continue;
        int btn_focused = (w->flags & PDWM_WIN_FOCUSED) ? 1 : 0;
        gfx_color_t bc = btn_focused ? GFX_RGB(180,180,220) : GFX_COLOR_WIN_BORDER;
        gfx_rounded_rect(bx, ty + 3, 80, PDWM_TASKBAR_H - 6, 3,
                         bc, GFX_COLOR_SHADOW);
        gfx_text(bx + 4, ty + (PDWM_TASKBAR_H - 8) / 2,
                 w->title, GFX_COLOR_TEXT_DARK, GFX_TRANSPARENT);
        bx += 84;
    }
}

/* ------------------------------------------------------------------ */
/* Desktop background                                                  */
/* ------------------------------------------------------------------ */
static void draw_desktop(void)
{
    int sh = gfx_height();
    gfx_fill_rect(0, 0, gfx_width(), sh - PDWM_TASKBAR_H, GFX_COLOR_DESKTOP);
}

/* ------------------------------------------------------------------ */
/* Cursor sprite — erase (restore saved background)                   */
/* ------------------------------------------------------------------ */
static void cursor_erase(void)
{
    if (g_curs_saved_x < 0) return;
    for (int r = 0; r < CURS_SAVE_H; r++)
        for (int c = 0; c < CURS_SAVE_W; c++)
            gfx_pixel(g_curs_saved_x + c, g_curs_saved_y + r,
                      g_cursor_bg[r * CURS_SAVE_W + c]);
    g_curs_saved_x = -1;
    g_curs_saved_y = -1;
}

/* ------------------------------------------------------------------ */
/* Cursor sprite — save background then draw arrow at (x, y)          */
/* ------------------------------------------------------------------ */
static void draw_cursor(int x, int y)
{
    /* Save a padded region so the outline/shadow pixels are covered */
    int sx = x - CURS_PAD;
    int sy = y - CURS_PAD;
    if (sx < 0) sx = 0;
    if (sy < 0) sy = 0;
    for (int r = 0; r < CURS_SAVE_H; r++)
        for (int c = 0; c < CURS_SAVE_W; c++)
            g_cursor_bg[r * CURS_SAVE_W + c] = gfx_read_pixel(sx + c, sy + r);
    g_curs_saved_x = sx;
    g_curs_saved_y = sy;

    /* Draw black shadow (offset 1,1) */
    for (int row = 0; row < 12; row++) {
        uint16_t bits = g_cursor_shape[row];
        for (int col = 0; col < 8; col++)
            if (bits & (0x8000u >> col))
                gfx_pixel(x + col + 1, y + row + 1, GFX_COLOR_SHADOW);
    }
    /* Draw white outline pixels */
    for (int row = 0; row < 12; row++) {
        uint16_t bits = g_cursor_shape[row];
        for (int col = 0; col < 8; col++) {
            if (bits & (0x8000u >> col)) {
                if (!(row > 0 && (g_cursor_shape[row-1] & (0x8000u >> col))))
                    gfx_pixel(x + col, y + row - 1, GFX_COLOR_WHITE);
                if (!(col > 0 && (bits & (0x8000u >> (col-1)))))
                    gfx_pixel(x + col - 1, y + row, GFX_COLOR_WHITE);
            }
        }
    }
    /* Draw black arrow fill */
    for (int row = 0; row < 12; row++) {
        uint16_t bits = g_cursor_shape[row];
        for (int col = 0; col < 8; col++)
            if (bits & (0x8000u >> col))
                gfx_pixel(x + col, y + row, GFX_COLOR_BLACK);
    }
}

/* ------------------------------------------------------------------ */
/* Click handling                                                       */
/* ------------------------------------------------------------------ */
static void handle_click(int mx, int my, uint8_t btns)
{
    if (!(btns & 0x01)) return;  /* only left button */

    int sh = gfx_height();
    int taskbar_y = sh - PDWM_TASKBAR_H;

    /* Click in taskbar */
    if (my >= taskbar_y) {
        /* Start button */
        if (mx >= 2 && mx <= 2 + PDWM_START_BTN_W) {
            /* TODO: open start menu */
            return;
        }
        /* Window buttons */
        int bx = PDWM_START_BTN_W + 8;
        for (int i = 0; i < g_win_count; i++) {
            pdwm_window_t *w = &g_windows[i];
            if (!(w->flags & PDWM_WIN_VISIBLE)) continue;
            if (mx >= bx && mx <= bx + 80) {
                pdwm_focus_window(i);
                return;
            }
            bx += 84;
        }
        return;
    }

    /* Click on a window — check from top (last) to bottom (first) */
    for (int i = g_win_count - 1; i >= 0; i--) {
        pdwm_window_t *w = &g_windows[i];
        if (!(w->flags & PDWM_WIN_VISIBLE)) continue;
        if (mx < w->x || mx >= w->x + w->w ||
            my < w->y || my >= w->y + w->h) continue;

        pdwm_focus_window(i);

        /* Close button? */
        int btn_y = w->y + PDWM_BORDER_W + 2;
        int btn_x = w->x + w->w - PDWM_BORDER_W - 16;
        if (mx >= btn_x && mx <= btn_x + 14 &&
            my >= btn_y && my <= btn_y + 14) {
            pdwm_close_window(i);
        }
        return;
    }
}

/* ------------------------------------------------------------------ */
/* Mouse event callback (called from IRQ12 handler)                   */
/* ------------------------------------------------------------------ */
static void on_mouse(int x, int y, uint8_t btns)
{
    static uint8_t prev_btns = 0;
    g_mouse_x    = x;
    g_mouse_y    = y;
    g_mouse_btns = btns;
    g_mouse_moved = 1;   /* cursor needs repositioning */

    /* Detect button-down edge — focus/click changes trigger full redraw */
    if ((btns & ~prev_btns) & 0x07)
        handle_click(x, y, btns & ~prev_btns);
    prev_btns = btns;
    /* NOTE: g_dirty is set by handle_click → pdwm_focus_window when needed.
       Pure mouse movement does NOT set g_dirty — the sprite system handles it. */
}

/* ------------------------------------------------------------------ */
/* Terminal window putchar                                             */
/* ------------------------------------------------------------------ */
void pdwm_terminal_putchar(int wid, char c)
{
    if (wid < 0 || wid >= g_win_count) return;
    pdwm_window_t *w = &g_windows[wid];
    if (!(w->flags & PDWM_WIN_TERMINAL)) return;

    int cols = w->client_w / 8;
    int rows = w->client_h / 8;

    if (c == '\n') {
        w->tx = 0; w->ty++;
    } else if (c == '\r') {
        w->tx = 0;
    } else if (c == '\t') {
        w->tx = (w->tx + 8) & ~7;
    } else {
        gfx_char(w->client_x + w->tx * 8,
                 w->client_y + w->ty * 8,
                 c, GFX_COLOR_TEXT_DARK, GFX_TRANSPARENT);
        w->tx++;
        if (w->tx >= cols) { w->tx = 0; w->ty++; }
    }

    /* Scroll terminal contents up on overflow */
    if (w->ty >= rows && rows > 0) {
        /* Shift pixels up by one glyph row */
        int row_px = 8;
        for (int py = w->client_y; py < w->client_y + w->client_h - row_px; py++) {
            for (int px = w->client_x; px < w->client_x + w->client_w; px++) {
                /* We only have write access; redraw approach: clear and rerender.
                   For now, blank the bottom row and reset ty. */
            }
        }
        /* Clear client area and reset — simple scroll strategy */
        gfx_fill_rect(w->client_x, w->client_y,
                      w->client_w, w->client_h, GFX_COLOR_WIN_CLIENT);
        w->tx = 0;
        w->ty = 0;
    }
}

/* ------------------------------------------------------------------ */
/* Terminal putchar — routes kprintf into the active terminal window  */
/* ------------------------------------------------------------------ */
static void pdwm_term_putchar(char c)
{
    if (g_term_wid >= 0)
        pdwm_terminal_putchar(g_term_wid, c);
}

/* ------------------------------------------------------------------ */
/* Graphical login dialog                                              */
/* ------------------------------------------------------------------ */
#define LOGIN_W   380
#define LOGIN_H   230
#define FIELD_W   220
#define FIELD_H    20

/* Draw the full login dialog; ulen/plen are byte lengths of each field */
static void draw_login(const char *uname, int ulen,
                        int plen,
                        int field_focus,   /* 0=user 1=pass */
                        const char *errmsg)
{
    int sw = gfx_width(), sh = gfx_height();
    int lx = (sw - LOGIN_W) / 2;
    int ly = (sh - LOGIN_H) / 2;

    /* Desktop background */
    gfx_fill_rect(0, 0, sw, sh, GFX_COLOR_DESKTOP);

    /* Dialog shadow */
    gfx_fill_rect(lx + 4, ly + 4, LOGIN_W, LOGIN_H, GFX_COLOR_SHADOW);

    /* Dialog frame */
    gfx_rounded_rect(lx, ly, LOGIN_W, LOGIN_H, PDWM_CORNER_R,
                     GFX_COLOR_WIN_CLIENT, GFX_COLOR_WIN_BORDER);

    /* Title bar */
    gfx_gradient_rect(lx + PDWM_BORDER_W, ly + PDWM_BORDER_W,
                      LOGIN_W - 2 * PDWM_BORDER_W, PDWM_TITLEBAR_H,
                      GFX_COLOR_TITLEBAR_ACT, GFX_COLOR_TITLEBAR_HI);
    gfx_text(lx + PDWM_BORDER_W + 4, ly + PDWM_BORDER_W + 6,
             "PD-OS Login", GFX_COLOR_TEXT_LIGHT, GFX_TRANSPARENT);

    /* OS label */
    gfx_text(lx + (LOGIN_W - gfx_text_width("PD-OS")) / 2,
             ly + PDWM_TITLEBAR_H + 16,
             "PD-OS", GFX_COLOR_TITLEBAR_ACT, GFX_TRANSPARENT);

    int fy = ly + PDWM_TITLEBAR_H + 44;
    int fx = lx + 60;

    /* Username row */
    gfx_text(fx, fy + 2, "Username:", GFX_COLOR_TEXT_DARK, GFX_TRANSPARENT);
    {
        gfx_color_t bc = (field_focus == 0) ? GFX_COLOR_TITLEBAR_ACT : GFX_COLOR_SHADOW;
        gfx_rounded_rect(fx + 72, fy - 2, FIELD_W, FIELD_H, 2,
                         GFX_COLOR_WHITE, bc);
        char disp[40]; int i;
        for (i = 0; i < ulen && i < 37; i++) disp[i] = uname[i];
        if (field_focus == 0) { disp[i] = '_'; i++; }
        disp[i] = '\0';
        gfx_text(fx + 76, fy + 2, disp, GFX_COLOR_TEXT_DARK, GFX_TRANSPARENT);
    }

    /* Password row */
    fy += 36;
    gfx_text(fx, fy + 2, "Password:", GFX_COLOR_TEXT_DARK, GFX_TRANSPARENT);
    {
        gfx_color_t bc = (field_focus == 1) ? GFX_COLOR_TITLEBAR_ACT : GFX_COLOR_SHADOW;
        gfx_rounded_rect(fx + 72, fy - 2, FIELD_W, FIELD_H, 2,
                         GFX_COLOR_WHITE, bc);
        char stars[40]; int i;
        for (i = 0; i < plen && i < 37; i++) stars[i] = '*';
        if (field_focus == 1) { stars[i] = '_'; i++; }
        stars[i] = '\0';
        if (i > 0)
            gfx_text(fx + 76, fy + 2, stars, GFX_COLOR_TEXT_DARK, GFX_TRANSPARENT);
    }

    /* Login button */
    fy += 48;
    int btn_w = 80, btn_x = lx + (LOGIN_W - btn_w) / 2;
    gfx_rounded_rect(btn_x, fy, btn_w, 24, 4,
                     GFX_COLOR_START_BTN, GFX_COLOR_SHADOW);
    gfx_text(btn_x + (btn_w - gfx_text_width("Login")) / 2, fy + 8,
             "Login", GFX_COLOR_WHITE, GFX_TRANSPARENT);

    /* Hint */
    fy += 36;
    gfx_text(lx + (LOGIN_W - gfx_text_width("Default: root/root  pd/pd")) / 2,
             fy, "Default: root/root  pd/pd",
             GFX_COLOR_SHADOW, GFX_TRANSPARENT);

    /* Error message */
    if (errmsg) {
        gfx_text(lx + 16, fy + 16, errmsg,
                 GFX_RGB(200, 0, 0), GFX_TRANSPARENT);
    }
}

static char g_login_buf[USERNAME_LEN];
static char g_pass_buf[USERNAME_LEN];

/* Blocking graphical login; returns verified user_t pointer */
static const user_t *pdwm_graphical_login(void)
{
    int ulen = 0, plen = 0, field = 0;
    const char *errmsg = 0;
    g_login_buf[0] = '\0';
    g_pass_buf[0]  = '\0';

    /* Pre-compute hit-boxes for click detection (matches draw_login layout) */
    int sw = gfx_width(), sh = gfx_height();
    int lx = (sw - LOGIN_W) / 2;
    int ly = (sh - LOGIN_H) / 2;
    int hfx       = lx + 60 + 72;          /* field left edge (pixel x)   */
    int hfy_user  = ly + PDWM_TITLEBAR_H + 44 - 2;   /* username field top  */
    int hfy_pass  = hfy_user + 36;         /* password field top          */
    int hfy_btn   = hfy_pass + 48;         /* login button top            */
    int hbtn_x    = lx + (LOGIN_W - 80) / 2;

    draw_login(g_login_buf, ulen, plen, field, errmsg);
    draw_cursor(g_mouse_x, g_mouse_y);

    uint8_t prev_btns = 0;

    while (1) {
        __asm__ volatile ("sti; hlt");

        /* ---- Detect left-button click edge ---- */
        uint8_t cur_btns  = (uint8_t)g_mouse_btns;
        uint8_t new_click = cur_btns & ~prev_btns & 0x01;
        prev_btns = cur_btns;

        /* ---- Cursor move — sprite only ---- */
        if (g_mouse_moved) {
            g_mouse_moved = 0;
            cursor_erase();
            draw_cursor(g_mouse_x, g_mouse_y);
        }

        /* ---- Mouse click on login form fields / button ---- */
        if (new_click) {
            int mx = g_mouse_x, my = g_mouse_y;
            int do_login = 0;
            int new_field = field;

            if (mx >= hfx && mx < hfx + FIELD_W &&
                my >= hfy_user && my < hfy_user + FIELD_H)
                new_field = 0;   /* clicked username box */
            else if (mx >= hfx && mx < hfx + FIELD_W &&
                     my >= hfy_pass && my < hfy_pass + FIELD_H)
                new_field = 1;   /* clicked password box */
            else if (mx >= hbtn_x && mx < hbtn_x + 80 &&
                     my >= hfy_btn && my < hfy_btn + 24)
                do_login = 1;    /* clicked Login button */

            if (new_field != field || do_login) {
                field = new_field;
                if (do_login) {
                    if (users_verify(g_login_buf, g_pass_buf)) {
                        const user_t *u = users_get(g_login_buf);
                        if (u) return u;
                    }
                    errmsg = "Invalid username or password.";
                    ulen = 0; plen = 0;
                    g_login_buf[0] = '\0';
                    g_pass_buf[0]  = '\0';
                    field = 0;
                }
                cursor_erase();
                draw_login(g_login_buf, ulen, plen, field, errmsg);
                draw_cursor(g_mouse_x, g_mouse_y);
                g_dirty = 0;
            }
        }

        /* ---- Keyboard ---- */
        char c = keyboard_getchar();
        if (!c) continue;

        cursor_erase();

        if (c == '\t') {
            field = 1 - field;
        } else if (c == '\b') {
            if (field == 0 && ulen > 0) { ulen--; g_login_buf[ulen] = '\0'; }
            if (field == 1 && plen > 0) { plen--; g_pass_buf[plen]  = '\0'; }
        } else if (c == '\n' || c == '\r') {
            if (field == 0) {
                field = 1;
            } else {
                if (users_verify(g_login_buf, g_pass_buf)) {
                    const user_t *u = users_get(g_login_buf);
                    if (u) return u;
                }
                errmsg = "Invalid username or password.";
                ulen = 0; plen = 0;
                g_login_buf[0] = '\0';
                g_pass_buf[0]  = '\0';
                field = 0;
            }
        } else if (c >= 0x20 && c < 0x7F) {
            if (field == 0 && ulen < (int)USERNAME_LEN - 1) {
                g_login_buf[ulen++] = c;
                g_login_buf[ulen]   = '\0';
            } else if (field == 1 && plen < (int)USERNAME_LEN - 1) {
                g_pass_buf[plen++] = c;
                g_pass_buf[plen]   = '\0';
            }
        }

        draw_login(g_login_buf, ulen, plen, field, errmsg);
        draw_cursor(g_mouse_x, g_mouse_y);
        g_dirty = 0;
    }
}

/* ------------------------------------------------------------------ */
/* pdwm_main — entry point, does not return                           */
/* ------------------------------------------------------------------ */
void pdwm_main(const char *username)
{
    /* Set up mouse callback BEFORE login so cursor is visible */
    mouse_set_callback(on_mouse);
    g_mouse_x = gfx_width()  / 2;
    g_mouse_y = gfx_height() / 2;

    /* ---- Graphical login (when username not pre-supplied) ---- */
    const char *logged_user = username;
    if (!logged_user || logged_user[0] == '\0') {
        const user_t *u = pdwm_graphical_login();
        logged_user = u->username;
    }

    /* ---- Desktop ---- */
    g_dirty = 1;
    pdwm_draw_all();

    /* Create default terminal window */
    int term_x = 60, term_y = 40;
    int term_w = gfx_width()  - 120;
    int term_h = gfx_height() - PDWM_TASKBAR_H - 80;
    if (term_w < 200) term_w = 200;
    if (term_h < 100) term_h = 100;

    g_term_wid = pdwm_create_window("Terminal", term_x, term_y,
                                     term_w, term_h,
                                     PDWM_WIN_TERMINAL | PDWM_WIN_CLOSEABLE);
    pdwm_focus_window(g_term_wid);
    pdwm_draw_window(g_term_wid);

    /* Redirect ALL kprintf output into the terminal window */
    kprint_redirect(pdwm_term_putchar);

    kprintf("PD-OS  --  Welcome, %s\n", logged_user);
    kprintf("Type 'help' for a list of commands.\n\n");

    /* Draw cursor at initial position */
    draw_cursor(g_mouse_x, g_mouse_y);

    /*
     * Main event loop.
     *
     * g_dirty      = windows/desktop changed → full redraw needed
     * g_mouse_moved = cursor moved           → sprite update only (fast)
     *
     * Full redraw: ~480,000 pixel writes.
     * Sprite cursor move: ~256 pixel reads + ~200 pixel writes.  (1800x faster)
     */
    uint32_t last_clock_tick = pit_get_ticks();

    while (1) {
        /* Sleep until any interrupt wakes us (mouse, keyboard, timer) */
        __asm__ volatile ("sti; hlt");

        uint32_t now = pit_get_ticks();

        if (g_dirty) {
            /* Windows changed — composite everything, then draw cursor on top */
            g_dirty = 0;
            cursor_erase();
            pdwm_draw_all();
            draw_cursor(g_mouse_x, g_mouse_y);
            g_mouse_moved = 0;
        } else if (g_mouse_moved) {
            /* Pure cursor move — sprite-only update, no full redraw */
            g_mouse_moved = 0;
            cursor_erase();
            draw_cursor(g_mouse_x, g_mouse_y);
        }

        /* Update clock every ~1 s without disturbing cursor */
        if (now - last_clock_tick >= 100) {
            last_clock_tick = now;
            cursor_erase();
            pdwm_draw_taskbar();
            draw_cursor(g_mouse_x, g_mouse_y);
        }

        /* Keyboard input → focused terminal window */
        char c = keyboard_getchar();
        if (c && g_focused >= 0 &&
            (g_windows[g_focused].flags & PDWM_WIN_TERMINAL)) {
            cursor_erase();
            pdwm_terminal_putchar(g_focused, c);
            draw_cursor(g_mouse_x, g_mouse_y);
        }
    }
}
