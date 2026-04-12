/* ===============================================================================================
 * PD-Shell  —  Built-in command shell (Tier 1)
 * =============================================================================================== */

#include "shell.h"
#include "kernel.h"
#include "vga.h"
#include "io.h"
#include "keyboard.h"
#include "pit.h"
#include "users.h"
#include "e820.h"
#include "pmm.h"
#include "kheap.h"
#include "ata.h"
#include "vfs.h"
#include "pdfs.h"

/* ---- Session state -------------------------------------------------------- */

static const user_t *g_session_user = NULL;
static int           g_logout       = 0;
static int           g_elevated     = 0;   /* set during an elev sub-command */
static char          g_cwd[128]     = "/"; /* current working directory      */

/* ---- String helpers (no libc) ------------------------------------------- */

static int sh_strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

static void sh_memset(char *p, char v, int n)
{
    while (n--) *p++ = v;
}

/* ---- readline ------------------------------------------------------------- */
/*
 * Reads one line of input into buf (max len-1 chars + NUL).
 * Handles: printable chars, backspace (bounded to anchor), left/right arrows.
 * Returns number of chars in buf (excluding NUL).
 */
static int readline(char *buf, int len)
{
    int  count = 0;          /* chars in buffer */
    int  cursor = 0;         /* insertion point within buffer */

    /* Anchor: cursor position on screen where input begins */
    uint8_t anchor_col = vga_get_col();
    uint8_t anchor_row = vga_get_row();

    sh_memset(buf, 0, len);

    for (;;) {
        char c = keyboard_getchar();

        /* --- Scroll viewport (don't snap back, just scroll) --- */
        if (c == KEY_PGUP) { vga_scroll_up(1);   continue; }
        if (c == KEY_PGDN) { vga_scroll_down(1); continue; }

        /* Any other key: snap back to live view before processing */
        vga_scroll_reset();

        /* --- Enter --- */
        if (c == '\n' || c == '\r') {
            buf[count] = '\0';
            vga_putchar('\n');
            return count;
        }

        /* --- Backspace --- */
        if (c == '\b') {
            if (cursor > 0) {
                /* Shift chars left in buffer */
                int i;
                for (i = cursor - 1; i < count - 1; i++)
                    buf[i] = buf[i + 1];
                buf[--count] = '\0';
                cursor--;

                /* Redraw from anchor */
                vga_set_cursor(anchor_col, anchor_row);
                int i2;
                for (i2 = 0; i2 < count; i2++)
                    vga_putchar(buf[i2]);
                vga_putchar(' ');   /* erase last ghost char */
                /* Reposition cursor */
                uint8_t cx = (uint8_t)((anchor_col + cursor) % VGA_WIDTH);
                uint8_t cy = (uint8_t)(anchor_row + (anchor_col + cursor) / VGA_WIDTH);
                vga_set_cursor(cx, cy);
            }
            continue;
        }

        /* --- Left arrow --- */
        if (c == KEY_LEFT) {
            if (cursor > 0) {
                cursor--;
                uint8_t cx = (uint8_t)((anchor_col + cursor) % VGA_WIDTH);
                uint8_t cy = (uint8_t)(anchor_row + (anchor_col + cursor) / VGA_WIDTH);
                vga_set_cursor(cx, cy);
            }
            continue;
        }

        /* --- Right arrow --- */
        if (c == KEY_RIGHT) {
            if (cursor < count) {
                cursor++;
                uint8_t cx = (uint8_t)((anchor_col + cursor) % VGA_WIDTH);
                uint8_t cy = (uint8_t)(anchor_row + (anchor_col + cursor) / VGA_WIDTH);
                vga_set_cursor(cx, cy);
            }
            continue;
        }

        /* --- Up/Down: ignore in Tier 1 (no history yet) --- */
        if (c == KEY_UP || c == KEY_DOWN)
            continue;

        /* --- Printable character --- */
        if (count >= len - 1) continue;   /* buffer full */

        /* Insert at cursor position */
        int i;
        for (i = count; i > cursor; i--)
            buf[i] = buf[i - 1];
        buf[cursor++] = c;
        count++;

        /* Redraw from cursor's old position */
        vga_set_cursor(anchor_col, anchor_row);
        for (i = 0; i < count; i++)
            vga_putchar(buf[i]);
        /* Reposition cursor after inserted char */
        uint8_t cx = (uint8_t)((anchor_col + cursor) % VGA_WIDTH);
        uint8_t cy = (uint8_t)(anchor_row + (anchor_col + cursor) / VGA_WIDTH);
        vga_set_cursor(cx, cy);
    }
}

/* ---- Password readline (masked) ------------------------------------------ */
/*
 * Like readline but echoes '*' for each character.
 * No cursor movement -- password input is strictly append/backspace.
 */
static int readline_masked(char *buf, int len)
{
    int     count      = 0;
    uint8_t anchor_col = vga_get_col();
    uint8_t anchor_row = vga_get_row();

    sh_memset(buf, 0, len);

    for (;;) {
        char c = keyboard_getchar();

        if (c == '\n' || c == '\r') {
            buf[count] = '\0';
            vga_putchar('\n');
            return count;
        }

        if (c == '\b') {
            if (count > 0) {
                buf[--count] = '\0';
                vga_set_cursor(anchor_col, anchor_row);
                int i;
                for (i = 0; i < count; i++) vga_putchar('*');
                vga_putchar(' ');
                vga_set_cursor(
                    (uint8_t)((anchor_col + count) % VGA_WIDTH),
                    (uint8_t)(anchor_row + (anchor_col + count) / VGA_WIDTH));
            }
            continue;
        }

        if (c == KEY_LEFT || c == KEY_RIGHT || c == KEY_UP || c == KEY_DOWN)
            continue;
        if (c < 0x20) continue;
        if (count >= len - 1) continue;

        buf[count++] = c;
        vga_putchar('*');
    }
}

/* ---- Tokenizer ------------------------------------------------------------ */

static int tokenize(char *line, char *argv[], int max_args)
{
    int argc = 0;
    char *p = line;

    while (*p) {
        /* Skip spaces */
        while (*p == ' ') p++;
        if (!*p) break;
        if (argc >= max_args) break;

        argv[argc++] = p;

        /* Find end of token */
        while (*p && *p != ' ') p++;
        if (*p) *p++ = '\0';
    }
    return argc;
}

/* ---- Built-in commands ---------------------------------------------------- */

/* ---- Help row helper: prints one aligned, word-wrapped help entry --------- */
#define HELP_CMD_COL  22   /* chars reserved for command+args (after 4-space indent) */
#define HELP_DESC_COL 28   /* column where descriptions start (4 + 22 + 2 gap)      */
#define HELP_DESC_W   52   /* available width for description text (80 - 28)         */

static void help_row(const char *cmd, const char *desc)
{
    int i;
    /* Yellow: 4-space indent + command padded to HELP_CMD_COL */
    vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
    for (i = 0; i < 4; i++) vga_putchar(' ');
    int clen = 0;
    while (cmd[clen]) { vga_putchar(cmd[clen]); clen++; }
    for (i = clen; i < HELP_CMD_COL; i++) vga_putchar(' ');
    vga_putchar(' '); vga_putchar(' ');   /* 2-space gap */

    /* White: description with word wrapping */
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    const char *p = desc;
    int col = 0;
    while (*p) {
        /* Skip inter-word spaces; note whether there was a separator */
        int had_space = 0;
        while (*p == ' ') { p++; had_space = 1; }
        if (!*p) break;

        /* Measure next word */
        int wlen = 0;
        while (p[wlen] && p[wlen] != ' ') wlen++;

        /* Wrap if word won't fit on current line */
        int need = (col == 0) ? wlen : col + 1 + wlen;
        if (need > HELP_DESC_W && col > 0) {
            vga_putchar('\n');
            for (i = 0; i < HELP_DESC_COL; i++) vga_putchar(' ');
            col = 0;
        } else if (had_space && col > 0) {
            vga_putchar(' '); col++;
        }

        for (i = 0; i < wlen; i++) { vga_putchar(p[i]); col++; }
        p += wlen;
    }
    vga_putchar('\n');
}

static void cmd_help(int argc, char *argv[])
{
    (void)argc; (void)argv;
    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    kprintf("\n  PD-Shell built-in commands:\n\n");
    help_row("help",                 "Show this help message");
    help_row("clear",                "Clear the screen");
    help_row("print [text]",         "Print text to the screen");
    help_row("version",              "Show kernel and shell version");
    help_row("uptime",               "Show system uptime in seconds");
    help_row("color [fg] [bg]",      "Set text color (0-15 each)");
    help_row("whoami",               "Show current user");
    help_row("rammap",               "Show physical memory map (E820)");
    help_row("raminfo",              "Show memory usage (free/used/total)");
    help_row("heapinfo",             "Show kernel heap stats");
    help_row("diskinfo",             "Show ATA drive info and layout");
    help_row("list",                 "List files on PDFS");
    help_row("read <file>",          "Print file contents");
    help_row("write <f> <text>",     "Create or overwrite a file");
    help_row("delete <file>",        "Delete a file");
    help_row("format",               "Format PDFS v2 (requires admin, erases all files)");
    help_row("makedir <dir>",        "Create a subdirectory");
    help_row("setperm <f> <oct>",    "Set file permissions (octal mode)");
    help_row("setowner <f> <u>:<g>", "Set file owner (e.g. setowner f.txt pd:pd, requires admin)");
    help_row("goto [path]",          "Change directory (~, .., /abs, relative)");
    help_row("copy <src> <dst>",     "Copy a file");
    help_row("move <src> <dst>",     "Move or rename a file");
    help_row("admin <cmd>",          "Run a command with admin privileges");
    help_row("alias [name]",         "List all aliases, or look up one alias");
    help_row("logout",               "Log out and return to login screen");
    help_row("reboot",               "Reboot the system");
    help_row("shutdown",             "Shut the system down completely");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    kprintf("\n");
}

static void cmd_clear(int argc, char *argv[])
{
    (void)argc; (void)argv;
    vga_clear();
}

static void cmd_print(int argc, char *argv[])
{
    int i;
    kprintf("  ");
    for (i = 1; i < argc; i++) {
        kprintf("%s", argv[i]);
        if (i < argc - 1) vga_putchar(' ');
    }
    vga_putchar('\n');
}

static void cmd_version(int argc, char *argv[])
{
    (void)argc; (void)argv;
    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    kprintf("\n  PD-Kernel  v0.1\n");
    kprintf("  PD-Shell   v0.1  (Tier 1 - built-ins only)\n\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
}

static void cmd_uptime(int argc, char *argv[])
{
    (void)argc; (void)argv;
    uint32_t secs = pit_get_ticks() / 100;   /* PIT runs at 100 Hz */
    kprintf("  Uptime: %u second%s\n", secs, secs == 1 ? "" : "s");
}

static void cmd_color(int argc, char *argv[])
{
    if (argc < 3) {
        kprintf("  Usage: color <fg 0-15> <bg 0-15>\n");
        return;
    }
    /* Simple atoi — no libc */
    int fg = 0, bg = 0;
    char *p;
    for (p = argv[1]; *p >= '0' && *p <= '9'; p++) fg = fg * 10 + (*p - '0');
    for (p = argv[2]; *p >= '0' && *p <= '9'; p++) bg = bg * 10 + (*p - '0');
    if (fg > 15) fg = 15;
    if (bg > 15) bg = 15;
    vga_set_color((vga_color_t)fg, (vga_color_t)bg);
    kprintf("  Color set.\n");
}

static void cmd_reboot(int argc, char *argv[])
{
    (void)argc; (void)argv;
    kprintf("  Rebooting...\n");
    /* Pulse CPU reset line via keyboard controller */
    __asm__ volatile (
        "cli\n"
        "1: inb $0x64, %al\n"
        "testb $0x02, %al\n"
        "jnz 1b\n"
        "movb $0xFE, %al\n"
        "outb %al, $0x64\n"
        "hlt\n"
    );
}

static void cmd_whoami(int argc, char *argv[])
{
    (void)argc; (void)argv;
    if (!g_session_user) return;
    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    kprintf("  %s", g_session_user->username);
    vga_set_color(VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK);
    kprintf("  (uid=%u%s)\n",
            (uint32_t)g_session_user->uid,
            (g_session_user->flags & USER_FLAG_ROOT) ? ", root" : "");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
}

static void cmd_rammap(int argc, char *argv[])
{
    (void)argc; (void)argv;
    e820_print();
}

static void cmd_raminfo(int argc, char *argv[])
{
    (void)argc; (void)argv;
    uint32_t free_mb  = (pmm_free_frames()  * PMM_PAGE_SIZE) / (1024u * 1024u);
    uint32_t used_mb  = (pmm_used_frames()  * PMM_PAGE_SIZE) / (1024u * 1024u);
    uint32_t total_mb = (pmm_total_frames() * PMM_PAGE_SIZE) / (1024u * 1024u);
    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    kprintf("\n  Memory usage:\n");
    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    kprintf("    Free:   %u MB  (%u frames)\n", free_mb,  pmm_free_frames());
    vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
    kprintf("    Used:   %u MB  (%u frames)\n", used_mb,  pmm_used_frames());
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    kprintf("    Total:  %u MB  (%u frames)\n\n", total_mb, pmm_total_frames());
}

static void cmd_heapinfo(int argc, char *argv[])
{
    (void)argc; (void)argv;
    uint32_t free_b  = kheap_free_bytes();
    uint32_t used_b  = kheap_used_bytes();
    uint32_t blocks  = kheap_block_count();
    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    kprintf("\n  Kernel heap (0x200000 - 0x2FFFFF, 1 MB pool):\n");
    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    kprintf("    Free:   %u bytes  (%u KB)\n", free_b, free_b / 1024u);
    vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
    kprintf("    Used:   %u bytes  (%u KB)\n", used_b, used_b / 1024u);
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    kprintf("    Blocks: %u\n\n", blocks);
}

static void cmd_diskinfo(int argc, char *argv[])
{
    (void)argc; (void)argv;
    const ata_drive_t *drv = ata_get_drive();
    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    kprintf("\n  ATA Primary Master:\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    if (!drv->present) {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        kprintf("    No drive detected\n\n");
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        return;
    }
    kprintf("    Model:    %s\n", drv->model);
    kprintf("    Sectors:  %u\n", drv->total_sectors);
    kprintf("    Capacity: %u KB  (%u MB)\n",
            drv->total_sectors / 2u,
            drv->total_sectors / 2048u);
    kprintf("    LBA28:    %s\n", drv->lba_supported ? "yes" : "no");
    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    kprintf("\n  Disk layout:\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    kprintf("    LBA   0        Stage 1 bootloader\n");
    kprintf("    LBA   1-5      Stage 2 bootloader\n");
    kprintf("    LBA   6-133    Kernel image (64 KB window)\n");
    kprintf("    LBA  200-205   PDFS v2 metadata\n");
    kprintf("    LBA  206-2047  PDFS v2 data\n");
    kprintf("    LBA  2048-4095 FAT32 volume (/mnt/fat)\n");
    kprintf("    LBA  4096-69631 ext2 volume  (/mnt/ext2)\n");
    kprintf("    LBA  69632+    NTFS volume  (/mnt/ntfs, read-only)\n\n");
}

/* ---- Filesystem helpers --------------------------------------------------- */

/*
 * Build a canonical absolute path from `input` against the current `g_cwd`.
 * Supports:
 *   /absolute/path     — used directly
 *   ~                  — expands to /home/<username>
 *   ~/subdir           — expands to /home/<username>/subdir
 *   ..                 — walk up one level
 *   .                  — current directory (no-op)
 *   relative/name      — appended to g_cwd
 * Output written into `out` (must be at least 128 bytes).
 */
static void normalize_path(const char *input, char *out)
{
    char buf[128];
    int  len = 0;

    if (input[0] == '/') {
        /* Absolute path */
        buf[len++] = '/';
        input++;
    } else if (input[0] == '~') {
        /* Expand ~ to /home/<username> */
        const char *u = (g_session_user && g_session_user->username[0])
                        ? g_session_user->username : "user";
        buf[len++] = '/';
        const char *h = "home";
        while (*h && len < 120) buf[len++] = *h++;
        buf[len++] = '/';
        while (*u && len < 120) buf[len++] = *u++;
        buf[len] = '\0';
        input++;
        if (*input == '/') input++;
    } else {
        /* Relative — start from g_cwd */
        int ci = 0;
        while (g_cwd[ci] && len < 120) buf[len++] = g_cwd[ci++];
        buf[len] = '\0';
    }

    /* Process remaining components */
    while (*input) {
        while (*input == '/') input++;
        if (!*input) break;

        /* Extract next component */
        char comp[32]; int ci = 0;
        while (*input && *input != '/' && ci < 30) comp[ci++] = *input++;
        comp[ci] = '\0';

        if (ci == 2 && comp[0] == '.' && comp[1] == '.') {
            /* Go up — find last '/' */
            int last = 0, j;
            for (j = 0; j < len; j++) if (buf[j] == '/') last = j;
            if (last == 0) { buf[1] = '\0'; len = 1; }
            else           { buf[last] = '\0'; len = last; }
        } else if (ci == 1 && comp[0] == '.') {
            /* Current dir — skip */
        } else {
            /* Append /component */
            if (len == 0 || buf[len - 1] != '/') buf[len++] = '/';
            int j = 0;
            while (comp[j] && len < 126) buf[len++] = comp[j++];
            buf[len] = '\0';
        }
    }

    if (len == 0) { buf[0] = '/'; buf[1] = '\0'; len = 1; }
    int k; for (k = 0; k <= len && k < 127; k++) out[k] = buf[k];
    out[k] = '\0';
}

/* Thin wrapper kept for callers that don't need the full normalize_path name. */
static void make_path(char *out, const char *name) { normalize_path(name, out); }

/* Print s padded to `width` spaces (no libc needed). */
static void sh_pad(const char *s, int width)
{
    int len = 0;
    while (s[len]) { vga_putchar(s[len]); len++; }
    while (len < width) { vga_putchar(' '); len++; }
}

/* ---- FS commands ---------------------------------------------------------- */

/* Format Unix mode bits as rwxrwxrwx string into buf[10] (incl NUL). */
static void fmt_mode(uint16_t mode, char *buf)
{
    buf[0] = (mode & 0x100u) ? 'r' : '-';
    buf[1] = (mode & 0x080u) ? 'w' : '-';
    buf[2] = (mode & 0x040u) ? 'x' : '-';
    buf[3] = (mode & 0x020u) ? 'r' : '-';
    buf[4] = (mode & 0x010u) ? 'w' : '-';
    buf[5] = (mode & 0x008u) ? 'x' : '-';
    buf[6] = (mode & 0x004u) ? 'r' : '-';
    buf[7] = (mode & 0x002u) ? 'w' : '-';
    buf[8] = (mode & 0x001u) ? 'x' : '-';
    buf[9] = '\0';
}

static void cmd_list(int argc, char *argv[])
{
    uint32_t    count = 0;
    char        dir_path[128];

    /* Arg or fall back to CWD */
    if (argc >= 2)
        normalize_path(argv[1], dir_path);
    else {
        int ci = 0;
        while (g_cwd[ci] && ci < 127) { dir_path[ci] = g_cwd[ci]; ci++; }
        dir_path[ci] = '\0';
    }

    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    if (dir_path[0] == '/' && dir_path[1] == '\0')
        kprintf("\n  PDFS  /  (%u KB free)\n", pdfs_free_sectors() / 2u);
    else
        kprintf("\n  PDFS  %s  (%u KB free)\n", dir_path, pdfs_free_sectors() / 2u);
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    kprintf("  ");
    sh_pad("Name", 20);
    kprintf("  ");
    sh_pad("Mode", 10);
    kprintf("  Uid  Size\n");
    kprintf("  --------------------  ----------  ---  --------\n");

    /* Strip leading '/' for pdfs_stat_dir */
    const char *dp = dir_path;
    while (*dp == '/') dp++;

    for (;;) {
        pdfs_dirent_t de;
        if (pdfs_stat_dir(dp, count, &de) != 0) break;
        char mbuf[10];
        fmt_mode(de.mode, mbuf);
        kprintf("  ");
        if (de.flags & PDFS_FLAG_DIR) vga_set_color(VGA_COLOR_LIGHT_CYAN,  VGA_COLOR_BLACK);
        else                          vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
        sh_pad(de.name, 20);
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        kprintf("  ");
        sh_pad(mbuf, 10);
        kprintf("  %u", (uint32_t)de.uid);
        if (de.flags & PDFS_FLAG_DIR) kprintf("    <DIR>\n");
        else                          kprintf("    %u bytes\n", de.size);
        count++;
    }

    if (count == 0) {
        vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
        kprintf("  (empty)\n");
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    } else {
        kprintf("  %u entr%s\n", count, count == 1 ? "y" : "ies");
    }
    kprintf("\n");
}

static void cmd_read(int argc, char *argv[])
{
    vfs_node_t node;
    char path[128];
    char *buf;
    int   r, i;

    if (argc < 2) { kprintf("  Usage: read <file>\n"); return; }

    make_path(path, argv[1]);
    if (vfs_open(path, &node) != 0) {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        kprintf("  read: '%s': file not found\n", argv[1]);
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        return;
    }
    if (node.size == 0) { kprintf("  (empty file)\n"); return; }

    /* Read up to 4 KB */
    {
        uint32_t to_read = node.size > 4096u ? 4096u : node.size;
        buf = (char *)kmalloc(to_read + 1u);
        if (!buf) { kprintf("  read: out of memory\n"); return; }

        r = vfs_read(&node, 0, to_read, buf);
        if (r < 0) {
            kprintf("  read: read error\n");
            kfree(buf);
            return;
        }
        buf[r] = '\0';
        kprintf("\n");
        for (i = 0; i < r; i++) vga_putchar(buf[i]);
        kprintf("\n");
        if (node.size > 4096u)
            kprintf("  ... (truncated at 4096 bytes)\n");
        kfree(buf);
    }
}

static void cmd_write(int argc, char *argv[])
{
    vfs_node_t node;
    char path[128];
    char content[256];
    uint32_t clen = 0;
    int i;

    if (argc < 2) { kprintf("  Usage: write <file> [text]\n"); return; }

    make_path(path, argv[1]);
    pdfs_set_context(g_session_user, g_elevated);

    /* Assemble content from remaining args */
    for (i = 2; i < argc; i++) {
        char *w = argv[i];
        while (*w && clen < 253u) content[clen++] = *w++;
        if (i < argc - 1 && clen < 253u) content[clen++] = ' ';
    }
    content[clen++] = '\n';
    content[clen]   = '\0';

    /* Open or create */
    if (vfs_open(path, &node) != 0) {
        if (vfs_create(path) != 0) {
            vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
            kprintf("  write: cannot create '%s'\n", argv[1]);
            vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
            return;
        }
        vfs_open(path, &node);
    }

    if (vfs_write(&node, 0, clen, content) < 0) {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        kprintf("  write: failed\n");
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    } else {
        kprintf("  Wrote %u bytes to '%s'\n", clen, argv[1]);
    }
}

static void cmd_delete(int argc, char *argv[])
{
    char path[128];
    if (argc < 2) { kprintf("  Usage: delete <file>\n"); return; }
    make_path(path, argv[1]);
    pdfs_set_context(g_session_user, g_elevated);
    int r = vfs_unlink(path);
    if (r == 0)
        kprintf("  Removed '%s'\n", argv[1]);
    else {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        if (r == -3)
            kprintf("  delete: '%s': permission denied\n", argv[1]);
        else
            kprintf("  delete: '%s': file not found\n", argv[1]);
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    }
}

static void cmd_format(int argc, char *argv[])
{
    (void)argc; (void)argv;
    /* Require elevated privileges */
    if (!g_elevated && !(g_session_user && (g_session_user->flags & USER_FLAG_ROOT))) {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        kprintf("  format: requires elevated privileges (use admin format)\n");
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        return;
    }
    kprintf("  Formatting PDFS v2 at LBA 200... ");
    if (pdfs_format(200) != 0) {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        kprintf("FAILED\n");
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        return;
    }
    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    kprintf("done\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    kprintf("  PDFS v2 formatted. Journal ready. All files erased.\n");
}

static void cmd_makedir(int argc, char *argv[])
{
    char path[128];
    if (argc < 2) { kprintf("  Usage: makedir <dir>\n"); return; }
    make_path(path, argv[1]);
    pdfs_set_context(g_session_user, g_elevated);
    uint8_t uid = g_session_user ? g_session_user->uid : 0u;
    if (pdfs_mkdir(path, uid, 0, 0) == 0)
        kprintf("  makedir: created '%s'\n", argv[1]);
    else {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        kprintf("  makedir: failed (exists, full, or read-only)\n");
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    }
}

static void cmd_setperm(int argc, char *argv[])
{
    char path[128];
    int  mode = 0;
    char *p;
    if (argc < 3) { kprintf("  Usage: setperm <file> <octal-mode>\n"); return; }
    /* Parse octal */
    for (p = argv[2]; *p >= '0' && *p <= '7'; p++) mode = mode * 8 + (*p - '0');
    make_path(path, argv[1]);
    pdfs_set_context(g_session_user, g_elevated);
    if (pdfs_chmod(path, (uint16_t)mode) == 0)
        kprintf("  setperm: mode set to 0%u\n", (uint32_t)mode);
    else {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        kprintf("  setperm: failed (not found or permission denied)\n");
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    }
}

/* Parse a uid/gid token: either a username looked up in the user table,
 * or a plain decimal number.  Returns the resolved id, or 0 if not found. */
static uint8_t resolve_id(const char *token)
{
    /* Try user table first */
    const user_t *u = users_get(token);
    if (u) return u->uid;
    /* Fall back to decimal */
    uint8_t v = 0;
    const char *p;
    for (p = token; *p >= '0' && *p <= '9'; p++) v = (uint8_t)(v * 10 + (*p - '0'));
    return v;
}

static void cmd_setowner(int argc, char *argv[])
{
    char path[128];
    char utoken[32], gtoken[32];
    uint8_t uid, gid;

    /* Accept:  seto <file> <user>:<group>
     *      or  seto <file> <user> <group>   */
    if (argc < 3) {
        kprintf("  Usage: setowner <file> <user>:<group>\n");
        kprintf("  e.g.:  setowner file.txt pd:pd\n");
        return;
    }

    if (argc == 3) {
        /* Single arg — split on ':' */
        char *colon = argv[2];
        uint32_t i = 0;
        while (colon[i] && colon[i] != ':') i++;
        if (colon[i] != ':') {
            kprintf("  Usage: setowner <file> <user>:<group>\n");
            return;
        }
        uint32_t k;
        for (k = 0; k < i && k < 31u; k++) utoken[k] = colon[k];
        utoken[k] = '\0';
        /* gtoken from after ':' */
        k = 0;
        const char *gp = colon + i + 1;
        while (*gp && k < 31u) gtoken[k++] = *gp++;
        gtoken[k] = '\0';
    } else {
        /* Two separate args */
        uint32_t k = 0;
        while (argv[2][k] && k < 31u) { utoken[k] = argv[2][k]; k++; }
        utoken[k] = '\0';
        k = 0;
        while (argv[3][k] && k < 31u) { gtoken[k] = argv[3][k]; k++; }
        gtoken[k] = '\0';
    }

    uid = resolve_id(utoken);
    gid = resolve_id(gtoken);

    make_path(path, argv[1]);
    pdfs_set_context(g_session_user, g_elevated);
    if (pdfs_chown(path, uid, gid) == 0)
        kprintf("  setowner: owner set to %s:%s\n", utoken, gtoken);
    else {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        kprintf("  setowner: failed (not found or permission denied)\n");
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    }
}

static void cmd_goto(int argc, char *argv[])
{
    char target[128];

    /* sdir with no arg goes to ~ (home) */
    if (argc < 2) {
        normalize_path("~", target);
    } else {
        normalize_path(argv[1], target);
    }

    /* Root is always valid */
    if (target[0] == '/' && target[1] == '\0') {
        g_cwd[0] = '/'; g_cwd[1] = '\0';
        return;
    }

    /* Verify the target exists and is a directory */
    vfs_node_t node;
    if (vfs_open(target, &node) != 0 || !node.is_dir) {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        kprintf("  goto: '%s': no such directory\n",
                argc >= 2 ? argv[1] : "~");
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        return;
    }

    /* Commit */
    int i = 0;
    while (target[i] && i < 127) { g_cwd[i] = target[i]; i++; }
    g_cwd[i] = '\0';
}

static void cmd_copy(int argc, char *argv[])
{
    char src[128], dst[128];
    vfs_node_t src_node, dst_node;
    char *buf;
    int r;

    if (argc < 3) { kprintf("  Usage: copy <src> <dst>\n"); return; }

    normalize_path(argv[1], src);
    normalize_path(argv[2], dst);

    if (vfs_open(src, &src_node) != 0) {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        kprintf("  copy: '%s': not found\n", argv[1]);
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        return;
    }
    if (src_node.is_dir) {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        kprintf("  copy: '%s' is a directory\n", argv[1]);
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        return;
    }

    pdfs_set_context(g_session_user, g_elevated);

    /* Allocate read buffer */
    uint32_t sz = src_node.size;
    if (sz == 0) sz = 1;
    buf = (char *)kmalloc(sz);
    if (!buf) { kprintf("  copy: out of memory\n"); return; }

    r = (sz > 1 || src_node.size > 0) ? vfs_read(&src_node, 0, src_node.size, buf) : 0;
    if (r < 0) {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        kprintf("  copy: read error\n");
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        kfree(buf);
        return;
    }

    /* Create dst if it doesn't exist */
    if (vfs_open(dst, &dst_node) != 0) {
        if (vfs_create(dst) != 0) {
            vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
            kprintf("  copy: cannot create '%s'\n", argv[2]);
            vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
            kfree(buf);
            return;
        }
        vfs_open(dst, &dst_node);
    }

    if (src_node.size > 0) {
        if (vfs_write(&dst_node, 0, (uint32_t)r, buf) < 0) {
            vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
            kprintf("  copy: write failed\n");
            vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
            kfree(buf);
            return;
        }
    }
    kprintf("  Copied %u bytes: '%s' -> '%s'\n", (uint32_t)r, argv[1], argv[2]);
    kfree(buf);
}

static void cmd_move(int argc, char *argv[])
{
    char src[128], dst[128];
    vfs_node_t src_node, dst_node;
    char *buf;
    int r;

    if (argc < 3) { kprintf("  Usage: move <src> <dst>\n"); return; }

    normalize_path(argv[1], src);
    normalize_path(argv[2], dst);

    if (vfs_open(src, &src_node) != 0) {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        kprintf("  move: '%s': not found\n", argv[1]);
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        return;
    }
    if (src_node.is_dir) {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        kprintf("  move: '%s' is a directory\n", argv[1]);
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        return;
    }

    pdfs_set_context(g_session_user, g_elevated);

    uint32_t sz = src_node.size;
    if (sz == 0) sz = 1;
    buf = (char *)kmalloc(sz);
    if (!buf) { kprintf("  move: out of memory\n"); return; }

    r = (src_node.size > 0) ? vfs_read(&src_node, 0, src_node.size, buf) : 0;
    if (r < 0) {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        kprintf("  move: read error\n");
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        kfree(buf);
        return;
    }

    /* Create dst */
    if (vfs_open(dst, &dst_node) != 0) {
        if (vfs_create(dst) != 0) {
            vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
            kprintf("  move: cannot create '%s'\n", argv[2]);
            vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
            kfree(buf);
            return;
        }
        vfs_open(dst, &dst_node);
    }

    if (src_node.size > 0) {
        if (vfs_write(&dst_node, 0, (uint32_t)r, buf) < 0) {
            vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
            kprintf("  move: write failed\n");
            vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
            kfree(buf);
            return;
        }
    }
    kfree(buf);

    /* Remove source */
    if (vfs_unlink(src) != 0) {
        vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
        kprintf("  move: copied but could not remove source '%s'\n", argv[1]);
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        return;
    }
    kprintf("  Moved '%s' -> '%s'\n", argv[1], argv[2]);
}

static void cmd_logout(int argc, char *argv[])
{
    (void)argc; (void)argv;
    kprintf("  Logging out...\n");
    g_logout = 1;
}

static void cmd_shutdown(int argc, char *argv[])
{
    (void)argc; (void)argv;
    kprintf("  Shutting down...\n");
    /* ACPI shutdown (QEMU default ACPI PM1a control port) */
    __asm__ volatile (
        "cli\n"
        "outw %w0, %w1\n"   /* QEMU ACPI: port 0x604, value 0x2000 */
        : : "a"((uint16_t)0x2000), "Nd"((uint16_t)0x604)
    );
    /* Fallback: older Bochs/QEMU ISA ACPI port */
    __asm__ volatile (
        "outw %w0, %w1\n"
        : : "a"((uint16_t)0x2000), "Nd"((uint16_t)0xB004)
    );
    /* Last resort: halt */
    __asm__ volatile ("1: hlt\njmp 1b\n");
}

static void cmd_admin(int argc, char *argv[]);   /* defined after commands[] */

/* ---- Alias table ---------------------------------------------------------- */

typedef struct {
    const char *alias;
    const char *native;
} alias_t;

/* Flat table — used by the shell dispatcher */
static const alias_t aliases[] = {
    { "ls",     "list"     },
    { "dir",    "list"     },
    { "cat",    "read"     },
    { "type",   "read"     },
    { "rm",     "delete"   },
    { "del",    "delete"   },
    { "erase",  "delete"   },
    { "mkdir",  "makedir"  },
    { "md",     "makedir"  },
    { "cd",     "goto"     },
    { "mv",     "move"     },
    { "ren",    "move"     },
    { "rename", "move"     },
    { "echo",   "print"    },
    { "chmod",  "setperm"  },
    { "chown",  "setowner" },
    { "sudo",   "admin"    },
    { "runas",  "admin"    },
    { "cls",    "clear"    },
    { "exit",   "logout"   },
    { "ver",    "version"  },
    { NULL,     NULL       }
};

/* Grouped table — used only for display */
typedef struct {
    const char *aliases_str;  /* comma-separated aliases */
    const char *native;
    const char *desc;
} alias_group_t;

static const alias_group_t alias_groups[] = {
    { "ls, dir",         "list",     "List files in the current directory"       },
    { "cat, type",       "read",     "Print the contents of a file"              },
    { "rm, del, erase",  "delete",   "Delete a file"                             },
    { "mkdir, md",       "makedir",  "Create a subdirectory"                     },
    { "cd",              "goto",     "Change the current directory"              },
    { "mv, ren, rename", "move",     "Move or rename a file"                     },
    { "echo",            "print",    "Print text to the screen"                  },
    { "chmod",           "setperm",  "Set file permissions (octal mode)"         },
    { "chown",           "setowner", "Set file owner"                            },
    { "sudo, runas",     "admin",    "Run a command with admin privileges"       },
    { "cls",             "clear",    "Clear the screen"                          },
    { "exit",            "logout",   "Log out and return to the login screen"    },
    { "ver",             "version",  "Show kernel and shell version"             },
    { NULL,              NULL,       NULL                                         }
};

/* ---- Alias row: same column geometry as help_row -------------------- */
#define ALIAS_LEFT_W  28   /* space for "aliases  ->  native" (after 4-space indent) */
#define ALIAS_DESC_COL 34  /* 4 + 28 + 2-gap */
#define ALIAS_DESC_W  46   /* 80 - 34 */

static void alias_row(const alias_group_t *g)
{
    int i;
    /* 4-space indent, then yellow aliases */
    for (i = 0; i < 4; i++) vga_putchar(' ');
    vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
    int left = 0;
    const char *a = g->aliases_str;
    while (*a) { vga_putchar(*a); a++; left++; }
    /* "  ->  " separator */
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    kprintf("  ->  "); left += 6;
    /* native in light green */
    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    const char *n = g->native;
    while (*n) { vga_putchar(*n); n++; left++; }
    /* Pad to ALIAS_LEFT_W then 2-space gap */
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    for (i = left; i < ALIAS_LEFT_W; i++) vga_putchar(' ');
    vga_putchar(' '); vga_putchar(' ');

    /* Description with word-wrap (same logic as help_row) */
    const char *p = g->desc;
    int col = 0;
    while (*p) {
        int had_space = 0;
        while (*p == ' ') { p++; had_space = 1; }
        if (!*p) break;
        int wlen = 0;
        while (p[wlen] && p[wlen] != ' ') wlen++;
        int need = (col == 0) ? wlen : col + 1 + wlen;
        if (need > ALIAS_DESC_W && col > 0) {
            vga_putchar('\n');
            for (i = 0; i < ALIAS_DESC_COL; i++) vga_putchar(' ');
            col = 0;
        } else if (had_space && col > 0) {
            vga_putchar(' '); col++;
        }
        for (i = 0; i < wlen; i++) { vga_putchar(p[i]); col++; }
        p += wlen;
    }
    vga_putchar('\n');
}

static void cmd_alias(int argc, char *argv[])
{
    int i;
    if (argc >= 2) {
        /* Look up a single alias */
        for (i = 0; aliases[i].alias != NULL; i++) {
            if (sh_strcmp(argv[1], aliases[i].alias) == 0) {
                vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
                kprintf("  %s", aliases[i].alias);
                vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
                kprintf("  ->  ");
                vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
                kprintf("%s\n", aliases[i].native);
                vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
                return;
            }
        }
        vga_set_color(VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK);
        kprintf("  '%s' is not an alias\n", argv[1]);
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        return;
    }
    /* Print formatted table */
    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    kprintf("\n  Aliases (Linux/Windows synonyms for PD-OS commands):\n\n");
    for (i = 0; alias_groups[i].aliases_str != NULL; i++)
        alias_row(&alias_groups[i]);
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    kprintf("\n");
}

/* ---- Command table -------------------------------------------------------- */

typedef struct {
    const char *name;
    void (*fn)(int argc, char *argv[]);
} command_t;

static const command_t commands[] = {
    { "help",     cmd_help     },
    { "clear",    cmd_clear    },
    { "print",    cmd_print    },
    { "version",  cmd_version  },
    { "uptime",   cmd_uptime   },
    { "color",    cmd_color    },
    { "whoami",   cmd_whoami   },
    { "rammap",   cmd_rammap   },
    { "raminfo",  cmd_raminfo  },
    { "heapinfo", cmd_heapinfo },
    { "diskinfo", cmd_diskinfo },
    { "list",     cmd_list     },
    { "read",     cmd_read     },
    { "write",    cmd_write    },
    { "delete",   cmd_delete   },
    { "format",   cmd_format   },
    { "makedir",  cmd_makedir  },
    { "setperm",  cmd_setperm  },
    { "setowner", cmd_setowner },
    { "goto",     cmd_goto     },
    { "copy",     cmd_copy     },
    { "move",     cmd_move     },
    { "admin",    cmd_admin    },
    { "alias",    cmd_alias    },
    { "logout",   cmd_logout   },
    { "reboot",   cmd_reboot   },
    { "shutdown", cmd_shutdown },
    { NULL,       NULL         }
};

/* ---- elev: privileged command dispatch ------------------------------------ */

static void cmd_admin(int argc, char *argv[])
{
    char pwd[64];
    int  i, found;

    if (argc < 2) {
        kprintf("  Usage: admin <command> [args...]\n");
        return;
    }

    /* Prevent recursion */
    if (sh_strcmp(argv[1], "admin") == 0) {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        kprintf("  admin: cannot elevate admin\n");
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        return;
    }

    /* Root users skip re-authentication */
    if (!(g_session_user && (g_session_user->flags & USER_FLAG_ROOT))) {
        vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
        kprintf("  [admin] root password: ");
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        readline_masked(pwd, sizeof(pwd));

        if (!users_verify("root", pwd)) {
            sh_memset(pwd, 0, (int)sizeof(pwd));
            vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
            kprintf("  admin: authentication failure\n");
            vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
            return;
        }
        sh_memset(pwd, 0, (int)sizeof(pwd));
    }

    g_elevated = 1;
    found = 0;
    for (i = 0; commands[i].name != NULL; i++) {
        if (sh_strcmp(argv[1], commands[i].name) == 0) {
            commands[i].fn(argc - 1, argv + 1);
            found = 1;
            break;
        }
    }
    g_elevated = 0;

    if (!found) {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        kprintf("  admin: unknown command: %s\n", argv[1]);
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    }
}

/* ---- Shell banner --------------------------------------------------------- */

static void shell_banner(void)
{
    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    kprintf("===============================================================================\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    kprintf("               PD-Shell  v0.1  -  Type 'help' for commands\n");
    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    kprintf("===============================================================================\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    kprintf("\n");
}

/* ---- Main shell loop ------------------------------------------------------ */

void shell_run(const user_t *user)
{
    char line[SHELL_BUF_SIZE];
    char *argv[SHELL_MAX_ARGS];

    g_session_user = user;
    g_logout       = 0;
    g_cwd[0] = '/'; g_cwd[1] = '\0';

    vga_clear();
    shell_banner();

    for (;;) {
        /* Prompt: username@pd-shell:/cwd> */
        vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
        kprintf("%s", user->username);
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        kprintf("@pd-shell:");
        vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
        kprintf("%s", g_cwd);
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        kprintf("> ");

        int n = readline(line, SHELL_BUF_SIZE);

        if (n == 0) continue;   /* blank line */

        int argc = tokenize(line, argv, SHELL_MAX_ARGS);
        if (argc == 0) continue;

        /* Look up command */
        int i;
        int found = 0;
        for (i = 0; commands[i].name != NULL; i++) {
            if (sh_strcmp(argv[0], commands[i].name) == 0) {
                commands[i].fn(argc, argv);
                found = 1;
                break;
            }
        }

        if (g_logout) return;

        if (!found) {
            /* Check alias table */
            int ai;
            for (ai = 0; aliases[ai].alias != NULL; ai++) {
                if (sh_strcmp(argv[0], aliases[ai].alias) == 0) {
                    /* Print hint then run the native command */
                    vga_set_color(VGA_COLOR_DARK_GREY, VGA_COLOR_BLACK);
                    kprintf("  (alias for '%s')\n", aliases[ai].native);
                    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
                    int ci;
                    for (ci = 0; commands[ci].name != NULL; ci++) {
                        if (sh_strcmp(aliases[ai].native, commands[ci].name) == 0) {
                            commands[ci].fn(argc, argv);
                            found = 1;
                            break;
                        }
                    }
                    break;
                }
            }
        }

        if (g_logout) return;

        if (!found) {
            vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
            kprintf("  Unknown command: ");
            vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
            kprintf("%s", argv[0]);
            kprintf("  (type 'help' for commands)\n");
        }
    }
}
