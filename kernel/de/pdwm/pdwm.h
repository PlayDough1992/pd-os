#ifndef PDWM_H
#define PDWM_H

#include "kernel.h"
#include "gfx.h"

/* ---------------------------------------------------------------
 * pdwm — PD Window Manager
 *
 * Windows ME-inspired desktop: teal background, silver taskbar,
 * gradient title bars, rounded window corners (r=4).
 * ---------------------------------------------------------------*/

/* Maximum open windows */
#define PDWM_MAX_WINDOWS 16

/* Title bar height (px) + title bar gradient colours */
#define PDWM_TITLEBAR_H   20
#define PDWM_BORDER_W      2
#define PDWM_CORNER_R      4
#define PDWM_TASKBAR_H    28
#define PDWM_START_BTN_W  54

/* Window flags */
#define PDWM_WIN_VISIBLE   0x01
#define PDWM_WIN_FOCUSED   0x02
#define PDWM_WIN_CLOSEABLE 0x04
#define PDWM_WIN_TERMINAL  0x08   /* routes gfx_putchar into client area */

typedef struct {
    int x, y, w, h;    /* outer frame position and size                */
    uint8_t flags;
    char title[64];

    /* Terminal state (used when PDWM_WIN_TERMINAL set) */
    int tx, ty;         /* cursor column/row in 8-px glyph units        */
    int client_x;       /* pixel x of client area origin                */
    int client_y;       /* pixel y of client area origin                */
    int client_w;       /* pixel width  of client area                  */
    int client_h;       /* pixel height of client area                  */
} pdwm_window_t;

/* Lifecycle */
void pdwm_main(const char *username);  /* does not return               */

/* Window management (usable from within pdwm) */
int  pdwm_create_window(const char *title, int x, int y, int w, int h,
                         uint8_t flags);
void pdwm_close_window(int wid);
void pdwm_focus_window(int wid);
void pdwm_draw_window(int wid);
void pdwm_draw_all(void);

/* Write a character into a terminal window's client area */
void pdwm_terminal_putchar(int wid, char c);

/* Redraw taskbar clock (called from main loop) */
void pdwm_draw_taskbar(void);

#endif /* PDWM_H */
