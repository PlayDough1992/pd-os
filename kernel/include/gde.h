#pragma once

/* ============================================================================
 * PD-OS GDE  —  Graphical Desktop Environment: types and shared API
 * ============================================================================ */

#include "kernel.h"
#include "gfx.h"

/* ---- Screen constants ---------------------------------------------------- */

#define GDE_SCREEN_W    1024
#define GDE_SCREEN_H     768
#define GDE_TASKBAR_H     32   /* pixels, pinned to bottom */
#define GDE_TITLEBAR_H    26   /* title bar height         */
#define GDE_BORDER_W       2   /* window border thickness  */
#define GDE_BTN_W         22   /* title-bar button width   */
#define GDE_BTN_H         16   /* title-bar button height  */
#define GDE_RESIZE_SZ     16   /* resize handle (corner)   */
#define GDE_MENUBAR_H     22   /* menu-bar strip           */
#define GDE_STATUSBAR_H   18   /* status-bar strip         */

/* ---- Limits -------------------------------------------------------------- */

#define GDE_MAX_WINDOWS    8
#define GDE_MAX_ICONS      8
#define GDE_MAX_MENU_ITEMS 12

/* ---- Color palette ------------------------------------------------------- */

#define COL_DESKTOP       GFX_RGB(  0,  96, 118)
#define COL_TASKBAR       GFX_RGB( 20,  20,  24)
#define COL_TASKBAR_BTN   GFX_RGB( 45,  45,  52)
#define COL_TASKBAR_SEP   GFX_RGB( 55,  55,  62)
#define COL_START_BG      GFX_RGB( 28,  28,  36)
#define COL_START_HOVER   GFX_RGB( 50,  50,  65)
#define COL_START_SEP     GFX_RGB( 52,  52,  62)
#define COL_WIN_BORDER_A  GFX_RGB( 26,  26,  30)  /* focused border    */
#define COL_WIN_BORDER_I  GFX_RGB( 68,  68,  74)  /* unfocused border  */
#define COL_TITLEBAR_A1   GFX_RGB( 86,  86,  92)  /* gradient top      */
#define COL_TITLEBAR_A2   GFX_RGB( 48,  48,  55)  /* gradient bottom   */
#define COL_TITLEBAR_I1   GFX_RGB(108, 108, 114)
#define COL_TITLEBAR_I2   GFX_RGB( 78,  78,  84)
#define COL_WIN_BODY      GFX_RGB(102, 102, 108)
#define COL_WIN_MENUBAR   GFX_RGB( 80,  80,  86)
#define COL_WIN_STATUSBAR GFX_RGB( 58,  58,  64)
#define COL_WIN_SHADOW    GFX_RGB(  0,   0,   0)
#define COL_BTN_CLOSE     GFX_RGB(185,  38,  38)
#define COL_BTN_MAX       GFX_RGB( 24, 112, 172)
#define COL_BTN_MIN       GFX_RGB( 24, 112, 172)
#define COL_BTN_HOVER     GFX_RGB(255, 255, 255)
#define COL_ICON_BG       GFX_RGB(255, 255, 255)
#define COL_ICON_TEXT     GFX_RGB(255, 255, 255)
#define COL_MENU_BG       GFX_RGB( 36,  36,  43)
#define COL_MENU_BORDER   GFX_RGB( 75,  75,  85)
#define COL_MENU_HOVER    GFX_RGB( 50,  88, 165)
#define COL_MENU_TEXT     GFX_RGB(228, 228, 228)
#define COL_TERM_BG       GFX_RGB( 14,  14,  18)
#define COL_TERM_FG       GFX_RGB(195, 228, 195)
#define COL_TERM_CURSOR   GFX_RGB(175, 228, 175)

/* ---- Window state -------------------------------------------------------- */

#define GDE_WIN_NORMAL    0
#define GDE_WIN_MINIMIZED 1
#define GDE_WIN_MAXIMIZED 2

/* Terminal private data (embedded in gde_window_t.priv as pointer) */
#define GDE_TERM_COLS     74
#define GDE_TERM_ROWS     22
#define GDE_TERM_INPUT    256

typedef struct {
    char   text[GDE_TERM_ROWS][GDE_TERM_COLS + 1]; /* display lines           */
    int    nlines;                                  /* lines used (0..ROWS-1)  */
    int    out_col;                                 /* current output column   */
    char   input[GDE_TERM_INPUT];                   /* current input line      */
    int    input_len;                               /* chars in input          */
} gde_term_t;

/* Forward-declare window struct (needed for callback signatures) */
typedef struct gde_window gde_window_t;

struct gde_window {
    int  id;
    int  x, y, w, h;
    int  prev_x, prev_y, prev_w, prev_h; /* maximize/restore */
    char title[64];
    int  state;     /* GDE_WIN_NORMAL / MINIMIZED / MAXIMIZED */
    int  visible;
    int  focused;

    /* Content callbacks */
    void (*draw_content)(gde_window_t *);
    void (*on_keychar)(gde_window_t *, char);
    void (*on_mousedown)(gde_window_t *, int mx, int my);
    void *priv;
};

/* ---- Desktop icon -------------------------------------------------------- */

typedef struct {
    int  x, y;
    char label[32];
    void (*on_dblclick)(void);
} gde_icon_t;

/* ---- Start-menu item ----------------------------------------------------- */

typedef struct {
    char label[32];
    int  separator;              /* 1 = draw separator line, ignore label */
    void (*on_click)(void);
} gde_menu_item_t;

/* ---- Subsystem APIs ------------------------------------------------------ */

/* wm.c */
gde_window_t *wm_create(const char *title, int x, int y, int w, int h,
                         void (*draw_cb)(gde_window_t *),
                         void (*key_cb)(gde_window_t *, char));
void          wm_close(gde_window_t *win);
void          wm_focus(gde_window_t *win);
void          wm_raise(gde_window_t *win);
int           wm_count(void);
gde_window_t *wm_get(int i);             /* iterate z-order (0=back) */
gde_window_t *wm_focused(void);
void          wm_toggle_maximized(gde_window_t *win);
void          wm_minimize(gde_window_t *win);

/* Geometry helpers — use _w to avoid collision with the ->w width field */
#define WIN_CX(_w)  ((_w)->x + GDE_BORDER_W)
#define WIN_CY(_w)  ((_w)->y + GDE_TITLEBAR_H + GDE_MENUBAR_H)
#define WIN_CW(_w)  ((_w)->w - GDE_BORDER_W * 2)
#define WIN_CH(_w)  ((_w)->h - GDE_TITLEBAR_H - GDE_MENUBAR_H - GDE_STATUSBAR_H - GDE_BORDER_W)

/* taskbar.c */
void taskbar_init(void);
void taskbar_add_menu_item(const char *label, int sep, void (*fn)(void));
void taskbar_clear_menu(void);
void taskbar_render(void);
int  taskbar_handle_click(int x, int y);  /* 1 = handled */
int  taskbar_menu_open(void);
void taskbar_menu_hover(int x, int y);
void taskbar_get_menu_rect(int *x, int *y, int *w, int *h);

/* terminal.c */
void terminal_open(void);
void terminal_draw(gde_window_t *win);
void terminal_key(gde_window_t *win, char k);

/* ---- PD-Explorer --------------------------------------------------------- */
void explorer_open(void);

/* ---- PD-Text editor ------------------------------------------------------ */
void text_editor_open(const char *path);

/* ---- Login screen (greeter) ---------------------------------------------- */
/* Blocks until the user authenticates; sets g_session_user on success.       */
void login_screen_run(void);

void terminal_hook_char(char c);          /* vga_putchar hook target */

/* desktop.c */
void desktop_init(void);
void desktop_render(void);        /* full redraw (startup / keyboard events) */
void desktop_present(void);       /* incremental render using dirty rect      */
void desktop_handle_key(char k);
int  desktop_handle_mouse(void);   /* returns 1=content dirty, 0=cursor only */
void desktop_cursor_move(int x, int y);
void desktop_mark_dirty(void);

/* gde_main.c */
void gde_main(void);          /* legacy direct entry, not used in GDE builds */
void gde_process_main(void);  /* kernel process entry: init→greeter→desktop  */

/* Current session user (set by login_screen_run before desktop launches).
 * NULL if not yet authenticated (should not happen during normal operation). */
#include "users.h"
extern const user_t *g_session_user;
