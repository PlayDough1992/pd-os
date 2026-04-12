/* ============================================================================
 * PD-Shell  —  Built-in command shell (Tier 1)
 * ============================================================================ */

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

static void cmd_help(int argc, char *argv[])
{
    (void)argc; (void)argv;
    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    kprintf("\n  PD-Shell built-in commands:\n\n");
    vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
    kprintf("    help             ");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    kprintf("Show this help message\n");
    vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
    kprintf("    clear            ");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    kprintf("Clear the screen\n");
    vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
    kprintf("    echo [text]      ");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    kprintf("Print text to the screen\n");
    vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
    kprintf("    version          ");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    kprintf("Show kernel and shell version\n");
    vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
    kprintf("    uptime           ");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    kprintf("Show system uptime in seconds\n");
    vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
    kprintf("    color [fg] [bg]  ");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    kprintf("Set text color (0-15 each)\n");
    vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
    kprintf("    whoami           ");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    kprintf("Show current user\n");
    vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
    kprintf("    memmap           ");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    kprintf("Show physical memory map (E820)\n");
    vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
    kprintf("    meminfo          ");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    kprintf("Show memory usage (free/used/total)\n");
    vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
    kprintf("    heap             ");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    kprintf("Show kernel heap stats\n");
    vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
    kprintf("    diskinfo         ");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    kprintf("Show ATA drive info and layout\n");
    vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
    kprintf("    ls               ");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    kprintf("List files on PDFS\n");
    vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
    kprintf("    cat <file>       ");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    kprintf("Print file contents\n");
    vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
    kprintf("    write <f> <text> ");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    kprintf("Create/overwrite a file\n");
    vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
    kprintf("    rm <file>        ");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    kprintf("Delete a file\n");
    vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
    kprintf("    mkpdfs           ");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    kprintf("Format PDFS (erases all files)\n");
    vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
    kprintf("    elev <cmd>       ");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    kprintf("Run a command with admin privileges\n");
    vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
    kprintf("    logout           ");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    kprintf("Log out and return to login screen\n");
    vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
    kprintf("    reboot           ");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    kprintf("Reboot the system\n");
    vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
    kprintf("    shutdown         ");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    kprintf("Shut the system down completely\n");
    kprintf("\n");
}

static void cmd_clear(int argc, char *argv[])
{
    (void)argc; (void)argv;
    vga_clear();
}

static void cmd_echo(int argc, char *argv[])
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

static void cmd_memmap(int argc, char *argv[])
{
    (void)argc; (void)argv;
    e820_print();
}

static void cmd_meminfo(int argc, char *argv[])
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

static void cmd_heap(int argc, char *argv[])
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
    kprintf("    LBA  200-202   PDFS metadata\n");
    kprintf("    LBA  203-2047  PDFS data\n");
    kprintf("    LBA  2048-4095 FAT32 volume (/mnt/fat)\n");
    kprintf("    LBA  4096-69631 ext2 volume  (/mnt/ext2)\n");
    kprintf("    LBA  69632+    NTFS volume  (/mnt/ntfs, read-only)\n\n");
}

/* ---- Filesystem helpers --------------------------------------------------- */

/* Build an absolute path from a name.  If name already starts with '/',
 * use it as-is (absolute path).  Otherwise prepend '/' (PDFS root). */
static void make_path(char *out, const char *name)
{
    int i;
    if (name[0] == '/') {
        for (i = 0; i < 63 && name[i]; i++) out[i] = name[i];
        out[i] = '\0';
    } else {
        out[0] = '/';
        for (i = 0; i < 62 && name[i]; i++) out[i + 1] = name[i];
        out[i + 1] = '\0';
    }
}

/* Print s padded to `width` spaces (no libc needed). */
static void sh_pad(const char *s, int width)
{
    int len = 0;
    while (s[len]) { vga_putchar(s[len]); len++; }
    while (len < width) { vga_putchar(' '); len++; }
}

/* ---- FS commands ---------------------------------------------------------- */

static void cmd_ls(int argc, char *argv[])
{
    vfs_node_t  node;
    uint32_t    count = 0;
    const char *path  = (argc >= 2) ? argv[1] : "/";

    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    if (argc < 2) {
        kprintf("\n  PDFS  /  (%u KB free)\n", pdfs_free_sectors() / 2u);
    } else {
        kprintf("\n  %s\n", path);
    }
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    kprintf("  ");
    sh_pad("Name", 16);
    kprintf("  Size\n");
    kprintf("  ----------------  --------\n");

    for (;;) {
        if (vfs_readdir(path, count, &node) != 0) break;
        kprintf("  ");
        vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
        sh_pad(node.name, 16);
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        kprintf("  %u bytes\n", node.size);
        count++;
    }

    if (count == 0) {
        vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
        kprintf("  (empty)\n");
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    } else {
        kprintf("  %u file%s\n", count, count == 1 ? "" : "s");
    }
    kprintf("\n");
}

static void cmd_cat(int argc, char *argv[])
{
    vfs_node_t node;
    char path[64];
    char *buf;
    int   r, i;

    if (argc < 2) { kprintf("  Usage: cat <file>\n"); return; }

    make_path(path, argv[1]);
    if (vfs_open(path, &node) != 0) {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        kprintf("  cat: '%s': file not found\n", argv[1]);
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        return;
    }
    if (node.size == 0) { kprintf("  (empty file)\n"); return; }

    /* Read up to 4 KB */
    {
        uint32_t to_read = node.size > 4096u ? 4096u : node.size;
        buf = (char *)kmalloc(to_read + 1u);
        if (!buf) { kprintf("  cat: out of memory\n"); return; }

        r = vfs_read(&node, 0, to_read, buf);
        if (r < 0) {
            kprintf("  cat: read error\n");
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
    char path[64];
    char content[256];
    uint32_t clen = 0;
    int i;

    if (argc < 2) { kprintf("  Usage: write <file> [text]\n"); return; }

    make_path(path, argv[1]);

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

static void cmd_rm(int argc, char *argv[])
{
    char path[64];
    if (argc < 2) { kprintf("  Usage: rm <file>\n"); return; }
    make_path(path, argv[1]);
    if (vfs_unlink(path) == 0)
        kprintf("  Removed '%s'\n", argv[1]);
    else {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        kprintf("  rm: '%s': file not found\n", argv[1]);
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    }
}

static void cmd_mkpdfs(int argc, char *argv[])
{
    (void)argc; (void)argv;
    kprintf("  Formatting PDFS at LBA 69... ");
    if (pdfs_format(69) != 0) {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        kprintf("FAILED\n");
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        return;
    }
    vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
    kprintf("done\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    kprintf("  PDFS reformatted. All files erased.\n");
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

static void cmd_elev(int argc, char *argv[]);   /* defined after commands[] */

/* ---- Command table -------------------------------------------------------- */

typedef struct {
    const char *name;
    void (*fn)(int argc, char *argv[]);
} command_t;

static const command_t commands[] = {
    { "help",    cmd_help    },
    { "clear",   cmd_clear   },
    { "echo",    cmd_echo    },
    { "version", cmd_version },
    { "uptime",  cmd_uptime  },
    { "color",   cmd_color   },
    { "whoami",   cmd_whoami   },
    { "memmap",   cmd_memmap   },
    { "meminfo",  cmd_meminfo  },
    { "heap",     cmd_heap     },
    { "diskinfo", cmd_diskinfo },
    { "ls",       cmd_ls       },
    { "cat",      cmd_cat      },
    { "write",    cmd_write    },
    { "rm",       cmd_rm       },
    { "mkpdfs",   cmd_mkpdfs   },
    { "elev",     cmd_elev     },
    { "logout",   cmd_logout   },
    { "reboot",   cmd_reboot   },
    { "shutdown", cmd_shutdown },
    { NULL,       NULL         }
};

/* ---- elev: privileged command dispatch ------------------------------------ */

static void cmd_elev(int argc, char *argv[])
{
    char pwd[64];
    int  i, found;

    if (argc < 2) {
        kprintf("  Usage: elev <command> [args...]\n");
        return;
    }

    /* Prevent recursion */
    if (sh_strcmp(argv[1], "elev") == 0) {
        vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
        kprintf("  elev: cannot elevate elev\n");
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        return;
    }

    /* Root users skip re-authentication */
    if (!(g_session_user && (g_session_user->flags & USER_FLAG_ROOT))) {
        vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
        kprintf("  [elev] root password: ");
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        readline_masked(pwd, sizeof(pwd));

        if (!users_verify("root", pwd)) {
            sh_memset(pwd, 0, (int)sizeof(pwd));
            vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
            kprintf("  elev: authentication failure\n");
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
        kprintf("  elev: unknown command: %s\n", argv[1]);
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    }
}

/* ---- Shell banner --------------------------------------------------------- */

static void shell_banner(void)
{
    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    kprintf("============================================================\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    kprintf("          PD-Shell  v0.1  -  Type 'help' for commands\n");
    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    kprintf("============================================================\n");
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

    shell_banner();

    for (;;) {
        /* Prompt: username@pd-shell> */
        vga_set_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK);
        kprintf("%s", user->username);
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        kprintf("@pd-shell> ");

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
            vga_set_color(VGA_COLOR_LIGHT_RED, VGA_COLOR_BLACK);
            kprintf("  Unknown command: ");
            vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
            kprintf("%s", argv[0]);
            kprintf("  (type 'help' for commands)\n");
        }
    }
}
