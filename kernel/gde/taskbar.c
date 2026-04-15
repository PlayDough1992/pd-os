/* ============================================================================
 * PD-OS GDE  —  Taskbar + Start Menu
 * ============================================================================ */

#include "gde.h"
#include "gfx.h"
#include "pit.h"

/* ---- State --------------------------------------------------------------- */

static int g_startmenu_open = 0;

#define MENU_X      2
#define MENU_ITEM_H 22
#define START_BTN_W 68

/* Items in the start menu (populated externally via callbacks) */
static gde_menu_item_t g_menu_items[GDE_MAX_MENU_ITEMS];
static int             g_menu_n = 0;

/* Hover state */
static int g_hover_item = -1;

/* ---- External action callbacks ------------------------------------------ */
/* Defined in desktop.c — registered during desktop_init() */
void taskbar_add_menu_item(const char *label, int sep, void (*fn)(void))
{
    if (g_menu_n >= GDE_MAX_MENU_ITEMS) return;
    int i;
    for (i = 0; i < 31 && label[i]; i++) g_menu_items[g_menu_n].label[i] = label[i];
    g_menu_items[g_menu_n].label[i]  = '\0';
    g_menu_items[g_menu_n].separator = sep;
    g_menu_items[g_menu_n].on_click  = fn;
    g_menu_n++;
}

void taskbar_clear_menu(void) { g_menu_n = 0; }

/* ---- Rendering ----------------------------------------------------------- */

void taskbar_init(void) {}

static void draw_clock(void)
{
    /* Show tick count as HH:MM — PIT ticks at 100 Hz */
    uint32_t ticks   = (uint32_t)pit_get_ticks();
    uint32_t seconds = ticks / 100u;
    uint32_t minutes = (seconds / 60u) % 60u;
    uint32_t hours   = (seconds / 3600u) % 24u;

    char buf[9];  /* "HH:MM   " */
    buf[0] = (char)('0' + hours / 10);
    buf[1] = (char)('0' + hours % 10);
    buf[2] = ':';
    buf[3] = (char)('0' + minutes / 10);
    buf[4] = (char)('0' + minutes % 10);
    buf[5] = '\0';

    int tw = gfx_string_w(buf);
    int tx = GDE_SCREEN_W - tw - 8;
    int ty = GDE_SCREEN_H - GDE_TASKBAR_H + (GDE_TASKBAR_H - GFX_CHAR_H) / 2;
    gfx_draw_string(tx, ty, buf, GFX_RGB(220,220,220), COL_TASKBAR, 0);
}

void taskbar_render(void)
{
    int y0 = GDE_SCREEN_H - GDE_TASKBAR_H;

    /* Background gradient */
    gfx_fill_rect_grad(0, y0, GDE_SCREEN_W, GDE_TASKBAR_H,
                        GFX_RGB(42,42,48), GFX_RGB(28,28,34));

    /* Top separator line */
    gfx_hline(0, y0, GDE_SCREEN_W, GFX_RGB(65,65,75));

    /* Start button */
    uint32_t sbg = g_startmenu_open ? GFX_RGB(55,90,170) : GFX_RGB(45,75,145);
    gfx_fill_rect(2, y0 + 2, START_BTN_W, GDE_TASKBAR_H - 4, sbg);
    gfx_draw_rect(2, y0 + 2, START_BTN_W, GDE_TASKBAR_H - 4,
                   g_startmenu_open ? GFX_RGB(100,140,220) : GFX_RGB(80,110,190));
    int sy = y0 + (GDE_TASKBAR_H - GFX_CHAR_H) / 2;
    gfx_draw_string(2 + (START_BTN_W - gfx_string_w("  Start  ")) / 2,
                    sy, "  Start  ", GFX_WHITE, sbg, 0);

    /* Window buttons — one per non-minimized window */
    int bx = START_BTN_W + 6;
    int bi;
    for (bi = 0; bi < wm_count(); bi++) {
        gde_window_t *w = wm_get(bi);
        if (!w) continue;
        int bw = 120;
        if (bx + bw > GDE_SCREEN_W - 100) break;
        uint32_t bbg = (w->focused && w->state != GDE_WIN_MINIMIZED)
                        ? GFX_RGB(55,90,170) : GFX_RGB(50,50,58);
        gfx_fill_rect(bx, y0 + 3, bw, GDE_TASKBAR_H - 6, bbg);
        gfx_draw_rect(bx, y0 + 3, bw, GDE_TASKBAR_H - 6,
                       w->focused ? GFX_RGB(90,130,200) : GFX_RGB(65,65,75));
        int ty2 = y0 + (GDE_TASKBAR_H - GFX_CHAR_H) / 2;
        gfx_draw_string_n(bx + 4, ty2, w->title, (bw - 8) / GFX_CHAR_W,
                           GFX_RGB(230,230,230), bbg, 0);
        bx += bw + 3;
    }

    draw_clock();

    /* ---- Start menu (rendered above taskbar when open) ------------------- */
    if (!g_startmenu_open) return;

    int menu_h  = g_menu_n * MENU_ITEM_H + 8;
    int menu_y  = y0 - menu_h;
    int menu_w  = 180;

    /* Shadow */
    gfx_fill_rect(MENU_X + 3, menu_y + 3, menu_w, menu_h, GFX_RGB(0,0,0));
    /* Body */
    gfx_fill_rect(MENU_X, menu_y, menu_w, menu_h, COL_MENU_BG);
    gfx_draw_rect(MENU_X, menu_y, menu_w, menu_h, COL_MENU_BORDER);

    int mx, my2, mi;
    for (mi = 0; mi < g_menu_n; mi++) {
        mx  = MENU_X + 2;
        my2 = menu_y + 4 + mi * MENU_ITEM_H;

        if (g_menu_items[mi].separator) {
            gfx_hline(MENU_X + 4, my2 + MENU_ITEM_H / 2, menu_w - 8,
                       GFX_RGB(70,70,80));
            continue;
        }

        if (mi == g_hover_item) {
            gfx_fill_rect(mx, my2, menu_w - 4, MENU_ITEM_H, COL_MENU_HOVER);
        }

        int ty3 = my2 + (MENU_ITEM_H - GFX_CHAR_H) / 2;
        gfx_draw_string(mx + 10, ty3, g_menu_items[mi].label,
                         COL_MENU_TEXT,
                         (mi == g_hover_item) ? COL_MENU_HOVER : COL_MENU_BG,
                         0);
    }
}

/* ---- Hit testing --------------------------------------------------------- */

int taskbar_menu_open(void) { return g_startmenu_open; }

int taskbar_handle_click(int x, int y)
{
    int y0 = GDE_SCREEN_H - GDE_TASKBAR_H;

    /* Click on start button */
    if (y >= y0 && y < GDE_SCREEN_H && x >= 2 && x < 2 + START_BTN_W) {
        g_startmenu_open ^= 1;
        g_hover_item = -1;
        return 1;
    }

    /* Click on taskbar window button */
    if (y >= y0 && y < GDE_SCREEN_H) {
        int bx = START_BTN_W + 6;
        int bi;
        for (bi = 0; bi < wm_count(); bi++) {
            gde_window_t *w = wm_get(bi);
            if (!w) continue;
            int bw = 120;
            if (x >= bx && x < bx + bw) {
                if (w->state == GDE_WIN_MINIMIZED) {
                    w->state = GDE_WIN_NORMAL;
                    wm_focus(w);
                } else if (w->focused) {
                    wm_minimize(w);
                } else {
                    wm_focus(w);
                }
                return 1;
            }
            bx += bw + 3;
        }
        return 1; /* clicked taskbar but nothing specific */
    }

    /* Click inside open start menu */
    if (g_startmenu_open) {
        int menu_h = g_menu_n * MENU_ITEM_H + 8;
        int menu_y = y0 - menu_h;
        int menu_w = 180;
        if (x >= MENU_X && x < MENU_X + menu_w && y >= menu_y && y < y0) {
            int mi = (y - menu_y - 4) / MENU_ITEM_H;
            if (mi >= 0 && mi < g_menu_n && !g_menu_items[mi].separator) {
                g_startmenu_open = 0;
                if (g_menu_items[mi].on_click)
                    g_menu_items[mi].on_click();
            }
            return 1;
        }
        /* Click outside menu closes it */
        g_startmenu_open = 0;
    }

    return 0;
}

/* Update hover state for start menu (called on mouse move) */
void taskbar_menu_hover(int x, int y)
{
    if (!g_startmenu_open) return;
    int y0     = GDE_SCREEN_H - GDE_TASKBAR_H;
    int menu_h = g_menu_n * MENU_ITEM_H + 8;
    int menu_y = y0 - menu_h;
    int menu_w = 180;

    if (x >= MENU_X && x < MENU_X + menu_w && y >= menu_y && y < y0) {
        int mi = (y - menu_y - 4) / MENU_ITEM_H;
        g_hover_item = (mi >= 0 && mi < g_menu_n) ? mi : -1;
    } else {
        g_hover_item = -1;
    }
}

/* Return the bounding rect of the open start menu (including shadow).
 * w/h == 0 if menu is closed.  Used by desktop for dirty rect tracking. */
void taskbar_get_menu_rect(int *ox, int *oy, int *ow, int *oh)
{
    if (!g_startmenu_open) { *ow = 0; *oh = 0; return; }
    int menu_w = 180 + 4;  /* +4 for shadow */
    int menu_h = g_menu_n * MENU_ITEM_H + 8 + 4;
    *ox = MENU_X;
    *oy = GDE_SCREEN_H - GDE_TASKBAR_H - menu_h;
    *ow = menu_w;
    *oh = menu_h + GDE_TASKBAR_H;  /* include taskbar row (start button state changes) */
}
