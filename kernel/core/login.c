/* ============================================================================
 * PD-Kernel  —  Login screen
 * ============================================================================ */

#include "login.h"
#include "kernel.h"
#include "vga.h"
#include "io.h"
#include "keyboard.h"
#include "users.h"

#define MAX_ATTEMPTS  3
#define INPUT_BUF     64

/* ---- Static helpers ------------------------------------------------------- */

static void lg_memset(char *p, char v, int n)
{
    while (n--) *p++ = v;
}

/* ---- Input readers -------------------------------------------------------- */
/*
 * Read a line of visible input (for username).
 * Handles printable chars and backspace. Enter submits.
 */
static void read_input(char *buf, int len)
{
    int count = 0;
    lg_memset(buf, 0, len);

    for (;;) {
        char c = keyboard_getchar();

        if (c == '\n' || c == '\r') {
            vga_putchar('\n');
            return;
        }
        if (c == '\b') {
            if (count > 0) {
                buf[--count] = '\0';
                vga_backspace();
            }
            continue;
        }
        /* Ignore control characters and non-ASCII */
        if (c < ' ' || c > 126) continue;
        if (count >= len - 1)   continue;

        buf[count++] = c;
        vga_putchar(c);
    }
}

/*
 * Read a password — echoes '*' for each character.
 * The buffer is always NUL-terminated.
 */
static void read_password(char *buf, int len)
{
    int count = 0;
    lg_memset(buf, 0, len);

    for (;;) {
        char c = keyboard_getchar();

        if (c == '\n' || c == '\r') {
            vga_putchar('\n');
            return;
        }
        if (c == '\b') {
            if (count > 0) {
                buf[--count] = '\0';
                vga_backspace();
            }
            continue;
        }
        if (c < ' ' || c > 126) continue;
        if (count >= len - 1)   continue;

        buf[count++] = c;
        vga_putchar('*');
    }
}

/* ---- Login banner --------------------------------------------------------- */

static void login_banner(void)
{
    vga_clear();
    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    kprintf("===============================================================================\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    kprintf("                              PD-OS  Login\n");
    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    kprintf("===============================================================================\n");
    vga_set_color(VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK);
    kprintf("  Default accounts:  root / root   pd / pd\n\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
}

/* ---- login_prompt --------------------------------------------------------- */

const user_t *login_prompt(void)
{
    char username[INPUT_BUF];
    char password[INPUT_BUF];
    int  attempt;

    for (attempt = 0; attempt < MAX_ATTEMPTS; attempt++) {
        login_banner();

        if (attempt > 0) {
            int remaining = MAX_ATTEMPTS - attempt;
            vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
            kprintf("  Login incorrect. (%d attempt%s remaining)\n\n",
                    remaining, remaining == 1 ? "" : "s");
            vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        }

        vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
        kprintf("  Username: ");
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        read_input(username, INPUT_BUF);

        vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
        kprintf("  Password: ");
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        read_password(password, INPUT_BUF);

        if (users_verify(username, password)) {
            const user_t *u = users_get(username);
            lg_memset(password, 0, INPUT_BUF);
            return u;
        }

        /* Clear sensitive data on failure */
        lg_memset(password, 0, INPUT_BUF);
        lg_memset(username, 0, INPUT_BUF);
    }

    /* Exhausted all attempts — halt */
    vga_clear();
    vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
    kprintf("\n\n  Too many failed login attempts. System halted.\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    __asm__ volatile ("cli\n\t"
                      "1: hlt\n\t"
                      "jmp 1b\n");
    return NULL;  /* unreachable */
}
