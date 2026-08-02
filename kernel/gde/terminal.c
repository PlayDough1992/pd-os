/* ============================================================================
 * PD-OS GDE  —  Terminal window
 * ============================================================================ */

#include "gde.h"
#include "gfx.h"
#include "vga.h"
#include "shell.h"
#include "users.h"
#include "kheap.h"

/* The active terminal that owns the vga_putchar hook */
static gde_window_t *g_active_term = (void*)0;

/* ---- Hook: receives all vga_putchar output during command execution ------ */

void terminal_hook_char(char c)
{
    if (!g_active_term || !g_active_term->priv) return;
    gde_term_t *term = (gde_term_t *)g_active_term->priv;

    if (c == '\n' || c == '\r') {
        /* Move to next line */
        if (term->nlines < GDE_TERM_ROWS - 1) {
            term->nlines++;
        } else {
            /* Scroll up: shift all lines up by 1 */
            int row;
            for (row = 0; row < GDE_TERM_ROWS - 1; row++) {
                int col;
                for (col = 0; col <= GDE_TERM_COLS; col++)
                    term->text[row][col] = term->text[row + 1][col];
            }
            /* Clear last line */
            int col;
            for (col = 0; col <= GDE_TERM_COLS; col++)
                term->text[GDE_TERM_ROWS - 1][col] = '\0';
        }
        term->out_col = 0;
        return;
    }
    if (c == '\b') {
        if (term->out_col > 0) {
            term->out_col--;
            term->text[term->nlines][term->out_col] = '\0';
        }
        return;
    }
    if (c < 0x20) return; /* ignore other control chars */

    if (term->out_col < GDE_TERM_COLS) {
        term->text[term->nlines][term->out_col] = c;
        term->out_col++;
        term->text[term->nlines][term->out_col] = '\0';
    }
}

/* ---- Drawing ------------------------------------------------------------ */

void terminal_draw(gde_window_t *win)
{
    if (!win || !win->priv) return;
    gde_term_t *term = (gde_term_t *)win->priv;

    int cx = WIN_CX(win);
    int cy = WIN_CY(win);
    int cw = WIN_CW(win);
    int ch = WIN_CH(win);

    /* Background */
    gfx_fill_rect(cx, cy, cw, ch, COL_TERM_BG);

    /* Output lines */
    int row;
    for (row = 0; row <= term->nlines; row++) {
        int ty = cy + 4 + row * (GFX_CHAR_H + 2);
        if (ty + GFX_CHAR_H > cy + ch) break;
        gfx_draw_string(cx + 4, ty, term->text[row],
                         COL_TERM_FG, COL_TERM_BG, 0);
    }

    /* Input line */
    int input_y = cy + ch - GFX_CHAR_H - 6;
    if (input_y < cy + 4) return;

    gfx_fill_rect(cx, input_y - 2, cw, GFX_CHAR_H + 4, GFX_RGB(20,25,22));
    gfx_hline(cx, input_y - 2, cw, GFX_RGB(40,60,40));

    /* Prompt + input text */
    char prompt[] = "> ";
    int px = cx + 4;
    gfx_draw_string(px, input_y, prompt, GFX_RGB(80,180,80), GFX_RGB(20,25,22), 0);
    px += gfx_string_w(prompt);
    gfx_draw_string(px, input_y, term->input, COL_TERM_FG, GFX_RGB(20,25,22), 0);

    /* Cursor blink (always visible for simplicity) */
    int cursor_x = px + term->input_len * GFX_CHAR_W;
    gfx_fill_rect(cursor_x, input_y, 2, GFX_CHAR_H, COL_TERM_CURSOR);
}

/* ---- Keyboard handling --------------------------------------------------- */

void terminal_key(gde_window_t *win, char k)
{
    if (!win || !win->priv) return;
    gde_term_t *term = (gde_term_t *)win->priv;

    if (k == '\r' || k == '\n') {
        /* Echo the command to output */
        terminal_hook_char('>');
        terminal_hook_char(' ');
        int i;
        for (i = 0; i < term->input_len; i++) terminal_hook_char(term->input[i]);
        terminal_hook_char('\n');

        /* Execute command */
        if (term->input_len > 0) {
            term->input[term->input_len] = '\0';

            /* Install hook so all output goes to this terminal */
            g_active_term = win;
            vga_set_hook(terminal_hook_char);

            /* Find a suitable user (root) to run commands */
            const user_t *u = users_get("root");
            if (!u) u = users_get("admin");
            if (u) shell_exec_line(u, term->input);

            vga_set_hook((void*)0);
            g_active_term = (void*)0;
        }

        /* Clear input */
        term->input_len = 0;
        term->input[0]  = '\0';
        return;
    }

    if (k == '\b') {
        if (term->input_len > 0) {
            term->input_len--;
            term->input[term->input_len] = '\0';
        }
        return;
    }

    if (k >= 0x20 && k < 127 && term->input_len < GDE_TERM_INPUT - 1) {
        term->input[term->input_len++] = k;
        term->input[term->input_len]   = '\0';
    }
}

/* ---- Spawn a terminal window -------------------------------------------- */

void terminal_open(void)
{
    /* Allocate a terminal state block on the kernel heap */
    gde_term_t *term = (gde_term_t *)kmalloc(sizeof(gde_term_t));
    if (!term) return;

    /* Zero-initialise */
    int r, c;
    for (r = 0; r < GDE_TERM_ROWS; r++)
        for (c = 0; c <= GDE_TERM_COLS; c++)
            term->text[r][c] = '\0';
    term->nlines    = 0;
    term->out_col   = 0;
    term->input_len = 0;
    term->input[0]  = '\0';

    /* Welcome message */
    const char *welcome = "PD-OS Terminal  (type 'help' for commands)";
    int i;
    for (i = 0; welcome[i] && i < GDE_TERM_COLS; i++)
        term->text[0][i] = welcome[i];
    term->text[0][i] = '\0';
    term->nlines = 1;

    gde_window_t *win = wm_create("Terminal",
                                    80, 60, 620, 420,
                                    terminal_draw,
                                    terminal_key);
    if (!win) {
        kfree(term);
        return;
    }
    win->priv = term;
}
