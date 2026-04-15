/* ============================================================================
 * PD-OS Example DE  —  "SimplE DE"
 * ============================================================================
 * A minimal but complete Desktop Environment demonstrating every PDAPI call.
 * Shows:
 *  - Using the built-in login screen
 *  - Drawing a gradient desktop background
 *  - A centered welcome panel
 *  - A taskbar at the bottom showing the username and a clock
 *  - Real-time mouse cursor (crosshair)
 *  - Keyboard: press Q to clear screen, any printable key echoed at bottom
 * ============================================================================ */

#include "de_api.h"

/* ---- Helpers ------------------------------------------------------------- */

static int se_strlen(const char *s)
{
    int n = 0;
    while (s[n]) n++;
    return n;
}

static void se_itoa(uint32_t n, char *buf)
{
    /* Write decimal number into buf (must be ≥ 12 bytes).  No stdlib. */
    char tmp[12];
    int  i = 0, j;
    if (n == 0) { buf[0] = '0'; buf[1] = '\0'; return; }
    while (n > 0) { tmp[i++] = '0' + (n % 10); n /= 10; }
    for (j = 0; j < i; j++) buf[j] = tmp[i - 1 - j];
    buf[i] = '\0';
}

static void se_strcat(char *dst, const char *src, int dst_max)
{
    int d = se_strlen(dst);
    int s = 0;
    while (src[s] && d + 1 < dst_max) dst[d++] = src[s++];
    dst[d] = '\0';
}

/* ---- Colors -------------------------------------------------------------- */

#define COL_BG_TOP   GFX_RGB( 12,  22,  45)
#define COL_BG_BOT   GFX_RGB(  5,  10,  22)
#define COL_TASKBAR  GFX_RGB( 20,  20,  28)
#define COL_PANEL    GFX_RGB( 28,  38,  62)
#define COL_BORDER   GFX_RGB( 60, 100, 180)
#define COL_TEXT     GFX_RGB(220, 225, 245)
#define COL_SUBTEXT  GFX_RGB(130, 140, 170)
#define COL_CURSOR   GFX_RGB(255, 220,  50)

#define TASKBAR_H  28
#define TASKBAR_Y  (SCREEN_H - TASKBAR_H)

/* ---- State --------------------------------------------------------------- */

static char  s_last_key  = 0;   /* most-recently pressed printable key */


/* ---- Draw cursor --------------------------------------------------------- */

static void draw_cursor(int mx, int my, uint32_t col)
{
    PDAPI->gfx_hline(mx - 8, my,      16, col);
    PDAPI->gfx_vline(mx,     my - 8,  16, col);
}

/* ---- Draw a full frame --------------------------------------------------- */

static void draw_frame(const user_t *user, int mx, int my)
{
    char msg[80];

    /* 1. Desktop background gradient */
    PDAPI->gfx_fill_rect_grad(0, 0, SCREEN_W, SCREEN_H - TASKBAR_H,
                               COL_BG_TOP, COL_BG_BOT);

    /* 2. Centered welcome panel */
    {
        int pw = 440, ph = 120;
        int px = (SCREEN_W - pw) / 2;
        int py = (SCREEN_H - TASKBAR_H - ph) / 2;

        PDAPI->gfx_fill_rect(px, py, pw, ph, COL_PANEL);
        PDAPI->gfx_draw_rect(px, py, pw, ph, COL_BORDER);

        /* Title */
        const char *title = "SimplE Desktop Environment";
        int tw = PDAPI->gfx_string_w(title);
        PDAPI->gfx_draw_string(px + (pw - tw) / 2, py + 14,
                               title, COL_TEXT, 0, 1);

        /* Subtitle */
        const char *sub = "An example DE for PD-OS.  Press keys to test input.";
        int sw = PDAPI->gfx_string_w(sub);
        PDAPI->gfx_draw_string(px + (pw - sw) / 2, py + 38,
                               sub, COL_SUBTEXT, 0, 1);

        /* Separator line */
        PDAPI->gfx_hline(px + 16, py + 58, pw - 32, COL_BORDER);

        /* Last key pressed */
        if (s_last_key) {
            char kstr[32];
            kstr[0] = 'K'; kstr[1] = 'e'; kstr[2] = 'y'; kstr[3] = ':';
            kstr[4] = ' '; kstr[5] = s_last_key; kstr[6] = '\0';
            int kw = PDAPI->gfx_string_w(kstr);
            PDAPI->gfx_draw_string(px + (pw - kw) / 2, py + 68,
                                   kstr, GFX_YELLOW, 0, 1);
        }

        /* Mouse position */
        {
            msg[0] = 'M'; msg[1] = 'o'; msg[2] = 'u'; msg[3] = 's';
            msg[4] = 'e'; msg[5] = ':'; msg[6] = ' '; msg[7] = '\0';
            char num[12];
            se_itoa((uint32_t)mx, num);
            se_strcat(msg, num, sizeof(msg));
            se_strcat(msg, " , ", sizeof(msg));
            se_itoa((uint32_t)my, num);
            se_strcat(msg, num, sizeof(msg));
            int mw = PDAPI->gfx_string_w(msg);
            PDAPI->gfx_draw_string(px + (pw - mw) / 2, py + 90,
                                   msg, COL_SUBTEXT, 0, 1);
        }
    }

    /* 3. Taskbar */
    PDAPI->gfx_fill_rect(0, TASKBAR_Y, SCREEN_W, TASKBAR_H, COL_TASKBAR);
    PDAPI->gfx_hline(0, TASKBAR_Y, SCREEN_W, COL_BORDER);

    /* Username on left */
    if (user) {
        msg[0] = ' '; msg[1] = '\0';
        se_strcat(msg, user->username, sizeof(msg));
        PDAPI->gfx_draw_string(8, TASKBAR_Y + 6, msg, COL_TEXT, 0, 1);
    }

    /* Tick count on right (simple "clock") */
    {
        char tstr[24];
        tstr[0] = 't'; tstr[1] = 'i'; tstr[2] = 'c'; tstr[3] = 'k';
        tstr[4] = 's'; tstr[5] = ':'; tstr[6] = ' '; tstr[7] = '\0';
        char num[12];
        se_itoa(PDAPI->pit_get_ticks(), num);
        se_strcat(tstr, num, sizeof(tstr));
        int tw = PDAPI->gfx_string_w(tstr);
        PDAPI->gfx_draw_string(SCREEN_W - tw - 8, TASKBAR_Y + 6,
                               tstr, COL_SUBTEXT, 0, 1);
    }

    /* 4. Mouse cursor (drawn last so it's always on top) */
    draw_cursor(mx, my, COL_CURSOR);

    /* 5. Flip to display */
    PDAPI->gfx_flip();
}

/* ---- Entry point --------------------------------------------------------- */

void de_main(void)
{
    /* Use the built-in PD-OS login screen.
     * It blocks until the user authenticates and sets *session_user_ptr. */
    PDAPI->login_screen_run();

    const user_t *user = *(PDAPI->session_user_ptr);

    /* Draw the first frame */
    int mx = SCREEN_W / 2;
    int my = SCREEN_H / 2;
    draw_frame(user, mx, my);

    /* Main event loop */
    for (;;) {
        int dirty = 0;

        /* Keyboard */
        char k;
        while ((k = PDAPI->keyboard_poll()) != 0) {
            if (k >= 0x20 && k < 0x7F)
                s_last_key = k;
            dirty = 1;
        }

        /* Mouse */
        if (PDAPI->mouse_changed()) {
            PDAPI->mouse_clear_changed();
            mx = PDAPI->mouse_get_x();
            my = PDAPI->mouse_get_y();
            dirty = 1;
        }

        if (dirty) {
            draw_frame(user, mx, my);
        } else {
            __asm__ volatile ("hlt");
        }
    }
}
