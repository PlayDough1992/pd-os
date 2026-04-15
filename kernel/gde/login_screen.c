/* ============================================================================
 * PD-OS GDE  —  Graphical Login Screen (Greeter)
 * ============================================================================
 * Displays an Ubuntu-style login screen:
 *   • Dark gradient background
 *   • One card per user account (circular avatar + username)
 *   • Click a card to select the user then type the password
 *   • Enter submits; wrong password flashes red; correct sets g_session_user
 *
 * Called from gde_process_main() before the desktop is launched.
 * Blocks (spinning event loop) until authentication succeeds, then returns.
 * ============================================================================ */

#include "gde.h"
#include "gfx.h"
#include "mouse.h"
#include "keyboard.h"
#include "users.h"
#include "pit.h"
#include "boot_info.h"

/* ---- Layout constants ---------------------------------------------------- */

#define CARD_W      130
#define CARD_H      150
#define CARD_GAP     24
#define CARD_TOP    270    /* y-coord of card top edge                        */
#define AVAT_R       36    /* avatar circle radius (px)                       */
#define AVAT_OFF_Y   50    /* avatar centre y-offset from card top            */

/* Password input box (visible only when a user card is selected) */
#define PASS_BOX_W  300
#define PASS_BOX_H   28
#define PASS_TOP    (CARD_TOP + CARD_H + 32)

/* Max password buffer length */
#define PASS_MAX     63

/* Error/success message display duration (in PIT ticks ≈ 100 Hz) */
#define MSG_TICKS   120   /* ~1.2 s */
#define WIN_TICKS    80   /* success "welcome" flash ~0.8 s */

/* ---- Avatar color palette (one per uid slot) ----------------------------- */

static const uint32_t avat_cols[] = {
    GFX_RGB(180, 55, 55),    /* uid 0 = root  – red     */
    GFX_RGB( 55,115,195),    /* uid 1         – blue    */
    GFX_RGB( 55,165, 75),    /* uid 2         – green   */
    GFX_RGB(155, 65,195),    /* uid 3         – purple  */
    GFX_RGB(195,135, 40),    /* uid 4         – orange  */
    GFX_RGB( 40,175,175),    /* uid 5         – teal    */
};
#define AVAT_COL_N  ((int)(sizeof(avat_cols)/sizeof(avat_cols[0])))

/* ---- Module state -------------------------------------------------------- */

static int   s_nusers;
static int   s_sel;          /* selected user index (-1 = none)              */
static char  s_pass[PASS_MAX + 1];
static int   s_pass_len;
static int   s_error;        /* > 0: error flash countdown (ticks)           */
static int   s_welcome;      /* > 0: welcome flash countdown (ticks)         */

/* ---- Cursor ------------------------------------------------------------ */

#define LS_CURSOR_W 14
#define LS_CURSOR_H 18

/* Pre-rendered cursor sprite and mask (built once at startup).
 * Back buffer is always cursor-free; gfx_cursor_blit stamps overlay only
 * onto the real FB via a union-rect single blit — no cursor-invisible frame.*/
static uint32_t s_cursor_sprite[LS_CURSOR_W * LS_CURSOR_H];
static uint8_t  s_cursor_mask  [LS_CURSOR_W * LS_CURSOR_H];
static uint32_t s_cursor_save  [LS_CURSOR_W * LS_CURSOR_H];
static int      s_cursor_x = -999;
static int      s_cursor_y = -999;

static void ls_cursor_sprite_init(void)
{
    int r, c;
    for (r = 0; r < LS_CURSOR_H; r++)
        for (c = 0; c < LS_CURSOR_W; c++)
            s_cursor_mask[r * LS_CURSOR_W + c] = 0;

    /* Arrow head: rows 0..11 — left edge BLACK, interior WHITE, diagonal BLACK */
    for (r = 0; r <= 11; r++) {
        s_cursor_sprite[r * LS_CURSOR_W + 0] = GFX_BLACK;
        s_cursor_mask  [r * LS_CURSOR_W + 0] = 1;
        for (c = 1; c < r; c++) {
            s_cursor_sprite[r * LS_CURSOR_W + c] = GFX_WHITE;
            s_cursor_mask  [r * LS_CURSOR_W + c] = 1;
        }
        if (r > 0) {
            s_cursor_sprite[r * LS_CURSOR_W + r] = GFX_BLACK;
            s_cursor_mask  [r * LS_CURSOR_W + r] = 1;
        }
    }
    /* Stalk: rows 12..17, columns 0..2 */
    for (r = 12; r < LS_CURSOR_H; r++) {
        s_cursor_sprite[r * LS_CURSOR_W + 0] = GFX_BLACK;
        s_cursor_sprite[r * LS_CURSOR_W + 1] = GFX_WHITE;
        s_cursor_sprite[r * LS_CURSOR_W + 2] = GFX_BLACK;
        s_cursor_mask  [r * LS_CURSOR_W + 0] = 1;
        s_cursor_mask  [r * LS_CURSOR_W + 1] = 1;
        s_cursor_mask  [r * LS_CURSOR_W + 2] = 1;
    }
}

/* Move cursor to (nx,ny) using the atomic union-rect blit — zero flicker. */
static void ls_cursor_move(int nx, int ny)
{
    gfx_cursor_blit(s_cursor_x, s_cursor_y, nx, ny,
                    LS_CURSOR_W, LS_CURSOR_H,
                    s_cursor_sprite, s_cursor_mask, s_cursor_save);
    s_cursor_x = nx;
    s_cursor_y = ny;
}

/* ---- Internal helpers ---------------------------------------------------- */

static int ls_strlen(const char *s)
{
    int n = 0;
    while (s[n]) n++;
    return n;
}

/* Filled circle — uses gfx_putpixel, so writes to back buffer */
static void draw_circle(int cx, int cy, int r, uint32_t col)
{
    int x, y;
    for (y = cy - r; y <= cy + r; y++) {
        for (x = cx - r; x <= cx + r; x++) {
            int dx = x - cx, dy = y - cy;
            if (dx * dx + dy * dy <= r * r)
                gfx_putpixel(x, y, col);
        }
    }
}

/* Draw a single character scaled 2× by expanding each source pixel to a 2×2
 * block.  Uses the 8×16 font accessed via gfx_draw_char by drawing to a
 * surrogate position then overwriting, but that's complex.  Instead we
 * reconstruct from the font pointer embedded in GDE_FONT_ADDR. */
static void draw_char_2x(int x, int y, char c, uint32_t fg)
{
    /* Access VGA BIOS font at GDE_FONT_ADDR (physical 0x3000, mapped R/O) */
    const uint8_t *font = (const uint8_t *)GDE_FONT_ADDR;
    const uint8_t *glyph = font + (uint8_t)c * 16;
    int row, col;
    for (row = 0; row < 16; row++) {
        uint8_t bits = glyph[row];
        for (col = 0; col < 8; col++) {
            if (bits & (0x80 >> col)) {
                gfx_putpixel(x + col*2,     y + row*2,     fg);
                gfx_putpixel(x + col*2 + 1, y + row*2,     fg);
                gfx_putpixel(x + col*2,     y + row*2 + 1, fg);
                gfx_putpixel(x + col*2 + 1, y + row*2 + 1, fg);
            }
        }
    }
}

static void draw_string_2x(int x, int y, const char *s, uint32_t fg)
{
    while (*s) {
        draw_char_2x(x, y, *s, fg);
        x += 16;   /* 8px × 2 = 16 per char */
        s++;
    }
}

/* Return x-coord of the left edge of card `i` for `n` total cards */
static int card_x(int i, int n)
{
    int total_w = n * CARD_W + (n - 1) * CARD_GAP;
    int start   = (GDE_SCREEN_W - total_w) / 2;
    return start + i * (CARD_W + CARD_GAP);
}

/* ---- Rendering ---------------------------------------------------------- */

/* Render scene to back buffer and snapshot into BG cache.
 * Only when content changes — NOT on every mouse move. */
static void ls_draw_scene(void)
{
    int i, n = s_nusers;

    /* Background gradient */
    gfx_fill_rect_grad(0, 0, GDE_SCREEN_W, GDE_SCREEN_H,
                       GFX_RGB(22, 28, 45),
                       GFX_RGB( 8, 12, 25));

    /* ---- OS title ---- */
    {
        const char *title = "PD-OS";
        int tw = ls_strlen(title) * 16;   /* 2× chars = 16px each */
        draw_string_2x((GDE_SCREEN_W - tw) / 2, 110, title,
                       GFX_RGB(230, 235, 255));
    }

    /* Subtitle */
    {
        const char *sub = (s_sel < 0) ? "Select a user to sign in"
                                      : "Enter your password";
        int sw = gfx_string_w(sub);
        gfx_draw_string((GDE_SCREEN_W - sw) / 2, 160, sub,
                        GFX_RGB(170, 175, 200), 0, 1);
    }

    /* ---- User cards ---- */
    for (i = 0; i < n; i++) {
        const user_t *u = users_get_by_index(i);
        if (!u) continue;

        int cx = card_x(i, n);
        int cy = CARD_TOP;
        int sel = (i == s_sel);

        /* Card background */
        uint32_t card_bg = sel ? GFX_RGB(40, 60, 100) : GFX_RGB(35, 40, 60);
        gfx_fill_rect(cx, cy, CARD_W, CARD_H, card_bg);

        /* Card border */
        uint32_t bord = sel ? GFX_RGB(80, 130, 230) : GFX_RGB(55, 62, 90);
        gfx_draw_rect(cx, cy, CARD_W, CARD_H, bord);

        /* Avatar circle */
        int uid_idx = (int)u->uid % AVAT_COL_N;
        uint32_t acol = avat_cols[uid_idx];
        int avx = cx + CARD_W / 2;
        int avy = cy + AVAT_OFF_Y;
        draw_circle(avx, avy, AVAT_R, acol);

        /* Avatar border ring */
        {
            int r = AVAT_R + 2;
            int ax, ay;
            for (ay = avy - r; ay <= avy + r; ay++) {
                for (ax = avx - r; ax <= avx + r; ax++) {
                    int dx = ax - avx, dy = ay - avy;
                    int d2 = dx*dx + dy*dy;
                    if (d2 <= r*r && d2 >= (AVAT_R+1)*(AVAT_R+1))
                        gfx_putpixel(ax, ay, GFX_RGB(255,255,255));
                }
            }
        }

        /* First letter of username in avatar */
        {
            char letter[2] = { u->username[0], 0 };
            /* Make uppercase */
            if (letter[0] >= 'a' && letter[0] <= 'z')
                letter[0] -= 32;
            int lx = avx - GFX_CHAR_W / 2;
            int ly = avy - GFX_CHAR_H / 2;
            gfx_draw_string(lx, ly, letter,
                            GFX_RGB(255, 255, 255), 0, 1);
        }

        /* Username label centered below avatar */
        {
            int uw = gfx_string_w(u->username);
            int ux = cx + (CARD_W - uw) / 2;
            int uy = cy + AVAT_OFF_Y + AVAT_R + 10;
            gfx_draw_string(ux, uy, u->username,
                            GFX_RGB(220, 225, 245), 0, 1);
        }
    }

    /* ---- Password section (only when a user is selected) ---- */
    if (s_sel >= 0) {
        /* "Password" label */
        {
            const char *lbl = "Password:";
            int lw = gfx_string_w(lbl);
            int lx = (GDE_SCREEN_W - PASS_BOX_W) / 2;
            gfx_draw_string(lx, PASS_TOP - 20, lbl,
                            GFX_RGB(180, 185, 210), 0, 1);
            (void)lw;
        }

        /* Input box background */
        int bx = (GDE_SCREEN_W - PASS_BOX_W) / 2;
        int by = PASS_TOP;
        gfx_fill_rect(bx, by, PASS_BOX_W, PASS_BOX_H, GFX_RGB(15, 20, 35));
        gfx_draw_rect(bx, by, PASS_BOX_W, PASS_BOX_H,
                      s_error > 0 ? GFX_RGB(200, 50, 50)
                                  : GFX_RGB(80, 100, 160));

        /* Asterisks */
        {
            int j;
            int tx = bx + 8;
            int ty = by + (PASS_BOX_H - GFX_CHAR_H) / 2;
            for (j = 0; j < s_pass_len; j++) {
                gfx_draw_char(tx + j * GFX_CHAR_W, ty, '*',
                              GFX_RGB(220, 225, 245), 0);
            }
            /* Blinking cursor placeholder — always shown */
            gfx_fill_rect(tx + s_pass_len * GFX_CHAR_W, ty + 2,
                          2, GFX_CHAR_H - 4,
                          GFX_RGB(150, 175, 235));
        }

        /* Error message */
        if (s_error > 0) {
            const char *err = "Incorrect password. Try again.";
            int ew = gfx_string_w(err);
            gfx_draw_string((GDE_SCREEN_W - ew) / 2,
                            PASS_TOP + PASS_BOX_H + 10,
                            err, GFX_RGB(220, 70, 70), 0, 1);
        }

        /* Hint */
        if (s_error == 0 && s_welcome == 0) {
            const char *hint = "Press Enter to sign in";
            int hw = gfx_string_w(hint);
            gfx_draw_string((GDE_SCREEN_W - hw) / 2,
                            PASS_TOP + PASS_BOX_H + 10,
                            hint, GFX_RGB(100, 110, 140), 0, 1);
        }
    }

    /* ---- Welcome flash ---- */
    if (s_welcome > 0) {
        const user_t *u = users_get_by_index(s_sel);
        if (u) {
            /* Semi-transparent overlay (approximated with a dark rect) */
            gfx_fill_rect(0, 0, GDE_SCREEN_W, GDE_SCREEN_H,
                          GFX_RGB(10, 15, 25));

            /* "Welcome, <name>" */
            char msg[64];
            int mi = 0;
            const char *pre = "Welcome, ";
            while (pre[mi]) { msg[mi] = pre[mi]; mi++; }
            int j = 0;
            while (u->username[j] && mi < 60) { msg[mi++] = u->username[j++]; }
            msg[mi++] = '!';
            msg[mi]   = '\0';

            int tw = ls_strlen(msg) * 16;
            draw_string_2x((GDE_SCREEN_W - tw) / 2,
                           (GDE_SCREEN_H - 32) / 2,
                           msg, GFX_RGB(200, 220, 255));
        }
    }

    /* Back buffer is now cursor-free. Flush it to the real FB. */
    gfx_flip();
    /* Reposition cursor: gfx_cursor_blit correctly union-blits old+new
     * rects (≤50×180 px total). No -999 hack — stored position is always valid. */
    ls_cursor_move(mouse_get_x(), mouse_get_y());
}

void login_screen_run(void)
{
    s_nusers   = users_count();
    s_sel      = -1;
    s_pass_len = 0;
    s_error    = 0;
    s_welcome  = 0;

    /* Build the cursor sprite/mask once */
    ls_cursor_sprite_init();

    /* Render initial scene — cursor placement happens inside ls_draw_scene() */
    ls_draw_scene();

    for (;;) {
        int scene_dirty  = 0;   /* cards/password/title changed */
        int cursor_dirty = 0;   /* only mouse moved */

        /* ---- Keyboard ---- */
        char k;
        while ((k = keyboard_poll()) != 0) {
            if (s_welcome > 0) {
                /* Ignore input during welcome flash */
                continue;
            }

            if (s_sel < 0) {
                /* No user selected — ignore most keys */
                continue;
            }

            if (k == '\b') {
                if (s_pass_len > 0) {
                    s_pass_len--;
                    s_pass[s_pass_len] = '\0';
                }
                s_error = 0;
                scene_dirty = 1;
            } else if (k == '\n' || k == '\r') {
                const user_t *u = users_get_by_index(s_sel);
                if (u && users_verify(u->username, s_pass)) {
                    g_session_user = u;
                    s_welcome = WIN_TICKS;
                    s_error   = 0;
                } else {
                    s_error    = MSG_TICKS;
                    s_pass_len = 0;
                    s_pass[0]  = '\0';
                }
                scene_dirty = 1;
            } else if (k >= 0x20 && k < 0x7F) {
                if (s_pass_len < PASS_MAX) {
                    s_pass[s_pass_len++] = k;
                    s_pass[s_pass_len]   = '\0';
                }
                s_error = 0;
                scene_dirty = 1;
            }
        }

        /* ---- Mouse ---- */
        if (mouse_changed()) {
            mouse_clear_changed();
            cursor_dirty = 1;
            uint8_t btns = mouse_get_buttons();
            if (btns & 1) {
                int mx = mouse_get_x();
                int my = mouse_get_y();
                int i;
                for (i = 0; i < s_nusers; i++) {
                    int cx = card_x(i, s_nusers);
                    int cy = CARD_TOP;
                    if (mx >= cx && mx < cx + CARD_W &&
                        my >= cy && my < cy + CARD_H) {
                        if (s_sel != i) {
                            s_sel      = i;
                            s_pass_len = 0;
                            s_pass[0]  = '\0';
                            s_error    = 0;
                            scene_dirty = 1;
                        }
                        break;
                    }
                }
            }
        }

        /* ---- Countdown timers ---- */
        if (s_error > 0) {
            s_error--;
            /* Only redraw when the error expires (hint text replaces it). */
            if (s_error == 0) scene_dirty = 1;
        }
        if (s_welcome > 0) {
            s_welcome--;
            if (s_welcome == 0) {
                /* Authentication complete — return to launch desktop */
                return;
            }
            /* Welcome screen is always the same visual — skip per-tick redraws.
             * The scene was already drawn when the password was accepted. */
        }

        /* ---- Render ---- */
        if (scene_dirty) {
            /* Full scene redraw: flip whole frame, reposition cursor */
            ls_draw_scene();
        } else if (cursor_dirty) {
            /* Mouse only: move cursor with two tiny rect blits */
            ls_cursor_move(mouse_get_x(), mouse_get_y());
        } else {
            __asm__ volatile ("hlt");
        }
    }
}
