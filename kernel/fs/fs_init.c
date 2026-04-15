/* ============================================================================
 * PD-Kernel  —  Filesystem population + Desktop Environment launcher
 *               (Phase 11)
 *
 * fs_populate():
 *   Creates the standard directory tree and seeds system files on first boot.
 *   Every operation is guarded by a vfs_open existence check, so it is safe
 *   to call on every boot (idempotent).
 *
 * de_select_and_launch():
 *   Scans /de/ for installed DEs and runs selection logic.
 *   Phase 11 stub: prints launch banner; binary execution is Phase 12.
 * ============================================================================ */

#include "fs_init.h"
#include "vfs.h"
#include "pdfs.h"
#include "vga.h"
#include "io.h"
#include "kernel.h"
#include "keyboard.h"
#include "pit.h"
#include "gfx.h"
#include "../de/pdwm/pdwm.h"

/* ---- Internal helpers ----------------------------------------------------- */

static int fi_strlen(const char *s)
{
    int n = 0; while (s[n]) n++; return n;
}

static void fi_strncpy(char *dst, const char *src, int n)
{
    int i;
    for (i = 0; i < n - 1 && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
}

static int fi_strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

/*
 * Ensure directory at `path` exists with the given uid/gid/mode.
 * No-ops if it already exists.
 */
static void ensure_dir(const char *path, uint8_t uid, uint8_t gid, uint16_t mode)
{
    vfs_node_t n;
    if (vfs_open(path, &n) == 0 && n.is_dir) return;  /* already exists */
    /* Run as root with elevation — this is kernel boot, not user code */
    pdfs_mkdir(path, uid, gid, mode);
}

/*
 * Write a text file at `path` with the content `text` (NUL-terminated).
 * No-ops if the file already exists (don't overwrite on subsequent boots).
 */
static void seed_file(const char *path, const char *text)
{
    vfs_node_t n;
    if (vfs_open(path, &n) == 0) return;  /* already exists */
    if (vfs_create(path) != 0) return;
    if (vfs_open(path, &n) != 0) return;
    vfs_write(&n, 0, (uint32_t)fi_strlen(text), text);
}

/* ---- Directory tree ------------------------------------------------------- */

/*
 * Initial /etc/passwd content — two built-in accounts.
 * Mirror of the hardcoded user_table in users.c.
 * Password field 'x' means hash lives in kernel (not in this file).
 */
#define INITIAL_PASSWD \
    "root:x:0:0:root:/root:/bin/shell\n" \
    "pd:x:1:1:pd:/home/pd:/bin/shell\n"

#define VER_STRING "PD-OS 0.1.0\n"

void fs_populate(void)
{
    /* Run all FS operations as root with elevation */
    pdfs_set_context((const user_t *)0, 1);  /* NULL caller but elevated=1 forces root */

    /* ---- Standard Unix/Debian hierarchy ---------------------------------- */
    ensure_dir("/bin",         0, 0, 0755);
    ensure_dir("/sbin",        0, 0, 0755);
    ensure_dir("/lib",         0, 0, 0755);
    ensure_dir("/lib32",       0, 0, 0755);
    ensure_dir("/etc",         0, 0, 0755);
    ensure_dir("/etc/pd-os",   0, 0, 0755);
    ensure_dir("/home",        0, 0, 0755);
    ensure_dir("/home/root",   0, 0, 0700);
    ensure_dir("/home/pd",     1, 1, 0755);
    ensure_dir("/root",        0, 0, 0700);
    ensure_dir("/dev",         0, 0, 0755);
    ensure_dir("/proc",        0, 0, 0755);
    ensure_dir("/sys",         0, 0, 0755);
    ensure_dir("/tmp",         0, 0, 01777);  /* sticky */
    ensure_dir("/var",         0, 0, 0755);
    ensure_dir("/var/log",     0, 0, 0755);
    ensure_dir("/var/tmp",     0, 0, 01777);  /* sticky */
    ensure_dir("/usr",         0, 0, 0755);
    ensure_dir("/usr/bin",     0, 0, 0755);
    ensure_dir("/usr/sbin",    0, 0, 0755);
    ensure_dir("/usr/lib",     0, 0, 0755);
    ensure_dir("/usr/share",   0, 0, 0755);
    ensure_dir("/mnt",         0, 0, 0755);
    ensure_dir("/mnt/fat",     0, 0, 0755);
    ensure_dir("/mnt/ext2",    0, 0, 0755);
    ensure_dir("/mnt/ntfs",    0, 0, 0755);

    /* ---- PD-OS specific -------------------------------------------------- */
    ensure_dir("/pdsys",         0, 0, 0755);
    ensure_dir("/pdsys/kernel",  0, 0, 0755);
    ensure_dir("/pdsys/drivers", 0, 0, 0755);
    ensure_dir("/pdapps",        0, 0, 0755);
    ensure_dir("/pdapps/system", 0, 0, 0755);
    ensure_dir("/pdapps/user",   0, 1, 0775);

    /* ---- /de — Desktop Environment directory ----------------------------- */
    ensure_dir("/de",            0, 0, 0755);

    /* ---- Seed system files ----------------------------------------------- */
    seed_file("/etc/pd-os/version", VER_STRING);
    seed_file("/pdsys/version",     VER_STRING);
    seed_file("/etc/passwd",        INITIAL_PASSWD);

    /* Clear elevation after population */
    pdfs_set_context((const user_t *)0, 0);
}

/* ---- DE launcher ---------------------------------------------------------- */

#define DE_NAME_LEN   28
#define DE_MAX        16

/* Timeout in PIT ticks for the selection prompt (10 seconds at 100 Hz) */
#define DE_PROMPT_TIMEOUT_TICKS  1000u

/*
 * Scan /de/ for subdirs that contain a "launch" entry.
 * Fills names[][DE_NAME_LEN] and returns the count found (0..DE_MAX).
 */
static int de_scan(char names[][DE_NAME_LEN], int max)
{
    int count = 0;
    vfs_node_t dir_entry, launch_node;
    char launch_path[64];
    int  i = 0;

    while (count < max) {
        if (vfs_readdir("/de", (uint32_t)i, &dir_entry) != 0) break;
        i++;
        if (!dir_entry.is_dir) continue;

        /* Build /de/<name>/launch path */
        launch_path[0] = '\0';
        int p = 0;
        const char *prefix = "/de/";
        int k;
        for (k = 0; prefix[k] && p < 60; k++) launch_path[p++] = prefix[k];
        for (k = 0; dir_entry.name[k] && p < 60; k++) launch_path[p++] = dir_entry.name[k];
        const char *launch_suffix = "/launch";
        for (k = 0; launch_suffix[k] && p < 63; k++) launch_path[p++] = launch_suffix[k];
        launch_path[p] = '\0';

        if (vfs_open(launch_path, &launch_node) != 0) continue;

        fi_strncpy(names[count], dir_entry.name, DE_NAME_LEN);
        count++;
    }
    return count;
}

/*
 * Read /de/default into `out` (max DE_NAME_LEN-1 chars, NUL-terminated).
 * Strips trailing newline if present.
 * Returns 0 on success, -1 if file doesn't exist or is empty.
 */
static int de_read_default(char *out)
{
    vfs_node_t n;
    char buf[DE_NAME_LEN];
    int  i;

    if (vfs_open("/de/default", &n) != 0) return -1;
    if (n.size == 0 || n.size >= (uint32_t)DE_NAME_LEN) return -1;

    if (vfs_read(&n, 0, n.size, buf) <= 0) return -1;
    buf[n.size] = '\0';

    /* Strip trailing newline */
    for (i = 0; buf[i]; i++) {
        if (buf[i] == '\n' || buf[i] == '\r') { buf[i] = '\0'; break; }
    }
    if (buf[0] == '\0') return -1;

    fi_strncpy(out, buf, DE_NAME_LEN);
    return 0;
}

/*
 * de_launch: starts the named DE.
 * If VBE graphics are active and the DE is 'pdwm', run the full WM.
 * Otherwise print a launch banner (text-mode fallback).
 */
static void de_launch(const char *name)
{
    if (gfx_is_active()) {
        /* pdwm is the only built-in graphical DE */
        (void)name;
        pdwm_main("user");
        /* pdwm_main does not return under normal operation */
    }
    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    kprintf("\n  Launching desktop environment: %s\n", name);
    vga_set_color(VGA_COLOR_YELLOW, VGA_COLOR_BLACK);
    kprintf("  [Text-mode fallback — VBE not available]\n");
    kprintf("  Returning to PD-Shell...\n\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
}

void de_select_and_launch(const user_t *user)
{
    char de_names[DE_MAX][DE_NAME_LEN];
    char def_name[DE_NAME_LEN];
    int  de_count;
    int  i;

    (void)user;  /* reserved for per-user DE preferences (Phase 12) */

    de_count = de_scan(de_names, DE_MAX);

    /* --- Case 0: no DEs installed → straight to shell --- */
    if (de_count == 0) return;

    /* --- Case 1: exactly one DE → auto-launch --- */
    if (de_count == 1) {
        de_launch(de_names[0]);
        return;
    }

    /* --- Case 2: multiple DEs, check for a default --- */
    if (de_read_default(def_name) == 0) {
        /* Validate the name exists in the scan list */
        for (i = 0; i < de_count; i++) {
            if (fi_strcmp(de_names[i], def_name) == 0) {
                de_launch(def_name);
                return;
            }
        }
        /* default named a non-existent DE — fall through to prompt */
    }

    /* --- Case 3: multiple DEs, no valid default → prompt user --- */
    vga_set_color(VGA_COLOR_LIGHT_CYAN, VGA_COLOR_BLACK);
    kprintf("\n  Desktop environments available:\n");
    vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
    for (i = 0; i < de_count; i++) {
        kprintf("    [%d] %s\n", i + 1, de_names[i]);
    }
    kprintf("    [S] Skip  (continue to PD-Shell)\n");
    kprintf("\n  Select [1");
    if (de_count > 1) kprintf("-%d", de_count);
    kprintf("/S] (auto-skip in 10s): ");

    /* Read one key with timeout */
    uint32_t deadline = pit_get_ticks() + DE_PROMPT_TIMEOUT_TICKS;
    char choice = 0;

    while (pit_get_ticks() < deadline) {
        /* keyboard_getchar() is non-blocking when the queue is empty
         * (returns 0); spin until a key arrives or timeout fires.      */
        char c = keyboard_getchar();
        if (c != 0) { choice = c; break; }
    }

    /* Echo the choice and newline */
    if (choice) {
        kprintf("%c\n", choice);
    } else {
        kprintf("(timeout)\n");
    }

    /* Interpret choice */
    if (choice >= '1' && choice <= '0' + de_count) {
        int sel = choice - '1';
        de_launch(de_names[sel]);
    } else {
        /* 'S', Enter, timeout, or anything else → drop to shell */
        vga_set_color(VGA_COLOR_WHITE, VGA_COLOR_BLACK);
        kprintf("  Continuing to PD-Shell.\n\n");
    }
}
