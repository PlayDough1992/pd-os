# PD-OS Desktop Environment Developer Guide

## Overview

A PD-OS Desktop Environment (DE) is a **plain flat binary** compiled to a fixed
memory address.  At boot the kernel checks PDFS for an installed DE, loads it
into memory, and jumps to its entry point.  No special packaging format is needed.

The kernel exposes every service (graphics, mouse, keyboard, filesystem, users,
memory) through a single API table pointer written at a known memory address
before the DE start.

---

## Memory Map

| Address       | Contents                                                      |
|---------------|---------------------------------------------------------------|
| `0x0000_3000` | VGA BIOS 8×16 font (256 glyphs × 16 bytes) — read-only       |
| `0x0000_4000` | Boot info block (VBE framebuffer address, width, height, etc) |
| `0x0000_5000` | **`pd_api_t *` — kernel API table pointer** (YOUR gateway)   |
| `0x0040_0000` | GFX back buffer (1024×768×4 = 4 MB)                          |
| `0x0080_0000` | GFX background cache (4 MB)                                   |
| `0x0100_0000` | **DE load address — your binary is placed here**              |
| `0x0200_0000` | Kernel heap (1 MB, managed by `kmalloc` / `kfree`)            |

Your DE binary is compiled with `-Ttext 0x01000000` so all its internal
addresses are absolute and correct after the kernel copies it to `0x1000000`.

---

## The API Table

Before jumping to your DE, the kernel fills a `pd_api_t` struct (defined in
`sdk/include/de_api.h`) and writes its address at physical `0x5000`.  Access
it with the `PDAPI` macro:

```c
#include "de_api.h"
#define PDAPI  (*(pd_api_t **)DE_API_PTR_ADDR)
```

Every kernel service is a function pointer in that struct.  Example:

```c
PDAPI->gfx_fill_rect(0, 0, 1024, 768, GFX_RGB(20, 30, 50));
PDAPI->gfx_flip();
```

---

## Minimal DE

The absolute minimum DE that shows something on screen:

```c
#include "de_api.h"
#define PDAPI (*(pd_api_t **)DE_API_PTR_ADDR)

void de_main(void)
{
    PDAPI->gfx_fill_rect(0, 0, SCREEN_W, SCREEN_H, GFX_RGB(20, 50, 100));
    PDAPI->gfx_draw_string(200, 360, "Hello from my DE!", GFX_WHITE, 0, 1);
    PDAPI->gfx_flip();
    for (;;) __asm__ volatile ("hlt");
}
```

Rules:
- The entry function **must** be named `de_main`.
- It must **never return**.
- You must call `gfx_flip()` to push frames to the real display.
- Hardware is already initialised by the kernel — do not call `gfx_init()` or `mouse_init()`.

---

## Using the Built-in Login Screen

The kernel ships a graphical login screen you can delegate to:

```c
void de_main(void)
{
    PDAPI->login_screen_run();            // blocks until authenticated
    const user_t *me = *(PDAPI->session_user_ptr);
    // me->username is now valid
    ...
}
```

If you want your own login UI, call `PDAPI->users_verify(username, password)`
and set `*(PDAPI->session_user_ptr) = PDAPI->users_get(username)` yourself.

---

## Full API Reference

### Screen

| Member       | Type  | Value  | Description                  |
|--------------|-------|--------|------------------------------|
| `screen_w`   | `int` | `1024` | Framebuffer width in pixels  |
| `screen_h`   | `int` | `768`  | Framebuffer height in pixels |

### Graphics

All drawing targets the **back buffer**.  Call `gfx_flip()` once per frame.  
Coordinates: `(0,0)` = top-left, `x` increases right, `y` increases down.  
Color format: `0x00RRGGBB` — use the `GFX_RGB(r,g,b)` macro.

| Function | Signature | Description |
|----------|-----------|-------------|
| `gfx_putpixel` | `(int x, int y, uint32_t color)` | Write single pixel |
| `gfx_getpixel` | `(int x, int y) → uint32_t` | Read pixel from back buffer |
| `gfx_fill_rect` | `(int x, int y, int w, int h, uint32_t color)` | Filled rectangle |
| `gfx_draw_rect` | `(int x, int y, int w, int h, uint32_t color)` | Hollow rectangle (1px border) |
| `gfx_hline` | `(int x, int y, int len, uint32_t color)` | Horizontal line |
| `gfx_vline` | `(int x, int y, int len, uint32_t color)` | Vertical line |
| `gfx_fill_rect_grad` | `(int x, int y, int w, int h, uint32_t top, uint32_t bot)` | Vertical gradient rectangle |
| `gfx_draw_char` | `(int x, int y, char c, uint32_t fg, uint32_t bg)` | Single 8×16 character |
| `gfx_draw_string` | `(int x, int y, const char *s, uint32_t fg, uint32_t bg, int transparent_bg)` | NUL-terminated string; `transparent_bg=1` skips bg pixels |
| `gfx_draw_string_n` | `(int x, int y, const char *s, int len, uint32_t fg, uint32_t bg, int transparent_bg)` | First `len` chars of string |
| `gfx_string_w` | `(const char *s) → int` | Pixel width of string (for centering) |
| `gfx_string_w_n` | `(const char *s, int len) → int` | Pixel width of first `len` chars |
| `gfx_save_region` | `(int x, int y, int w, int h, uint32_t *dst)` | Copy `w×h` pixels to caller buffer |
| `gfx_restore_region` | `(int x, int y, int w, int h, const uint32_t *src)` | Paste saved region back |
| `gfx_flip` | `(void)` | **Push back buffer to real display** |
| `gfx_cache_bg` | `(void)` | Snapshot back buffer as background cache |
| `gfx_restore_bg` | `(void)` | Restore background cache into back buffer (fast frame start) |
| `gfx_blend_pixel` | `(int x, int y, uint32_t color, uint8_t alpha)` | Alpha-blend: 0=transparent, 255=opaque |

**Background cache pattern** (recommended for animated UIs):

```c
// Once, after drawing the static background:
PDAPI->gfx_cache_bg();

// Each frame:
PDAPI->gfx_restore_bg();       // restore background instantly
// ... draw dynamic elements on top ...
PDAPI->gfx_flip();
```

### Keyboard

| Function | Returns | Description |
|----------|---------|-------------|
| `keyboard_poll()` | `char` | Next ASCII character from buffer, or `0` if empty. Call in a loop. |

Special keys returned as values below `0x20`:
- `'\b'` (0x08) = Backspace
- `'\n'` (0x0A) = Enter
- `'\t'` (0x09) = Tab
- `0x1B`        = Escape

### Mouse

| Function | Returns | Description |
|----------|---------|-------------|
| `mouse_get_x()` | `int` | Current cursor X (0 = left edge) |
| `mouse_get_y()` | `int` | Current cursor Y (0 = top edge) |
| `mouse_get_buttons()` | `uint8_t` | Bitmask: bit 0 = left, bit 1 = right, bit 2 = middle |
| `mouse_changed()` | `int` | Returns 1 if mouse moved or button changed since last clear |
| `mouse_clear_changed()` | `void` | Reset the changed flag; call after processing |

**Mouse cursor rendering** — the kernel does NOT draw a hardware cursor.
Your DE is responsible for drawing the cursor as part of each frame.

### Users

| Function | Returns | Description |
|----------|---------|-------------|
| `users_verify(username, password)` | `int` | 1 = correct password |
| `users_count()` | `int` | Number of user accounts |
| `users_get_by_index(i)` | `const user_t *` | User by 0-based index; NULL if out of range |
| `users_get(username)` | `const user_t *` | User by name; NULL if not found |

`user_t` fields:
- `username[32]` — NUL-terminated name
- `uid` — user ID (0 = root)
- `flags` — `USER_FLAG_ROOT = 0x01`

### Filesystem (VFS / PDFS)

PDFS is always mounted at `/`.  All paths are absolute.

| Function | Returns | Description |
|----------|---------|-------------|
| `vfs_open(path, &node)` | `int` | 0 on success; fills `vfs_node_t` |
| `vfs_read(&node, offset, len, buf)` | `int` | Bytes read; negative on error |
| `vfs_write(&node, offset, len, buf)` | `int` | Bytes written; negative on error |
| `vfs_create(path)` | `int` | 0 on success |
| `vfs_unlink(path)` | `int` | Delete file; 0 on success |
| `vfs_readdir(path, idx, &node)` | `int` | 0 if entry `idx` exists; negative when exhausted |

**Read a file example:**
```c
vfs_node_t n;
if (PDAPI->vfs_open("/home/pd/note.txt", &n) == 0) {
    char *buf = PDAPI->kmalloc(n.size + 1);
    PDAPI->vfs_read(&n, 0, n.size, buf);
    buf[n.size] = '\0';
    // use buf
    PDAPI->kfree(buf);
}
```

**List a directory:**
```c
vfs_node_t entry;
uint32_t idx = 0;
while (PDAPI->vfs_readdir("/home/pd", idx++, &entry) == 0) {
    // entry.name, entry.is_dir, entry.size
}
```

### Memory

| Function | Returns | Description |
|----------|---------|-------------|
| `kmalloc(size)` | `void *` | Allocate `size` bytes; NULL on OOM |
| `kfree(ptr)` | `void` | Free a `kmalloc`'d allocation |

The kernel heap is **1 MB** (addresses `0x200000`–`0x2FFFFF`).  Use it for
window structs, file buffers, etc.  Do not allocate multi-hundred-KB arrays —
use static globals in your DE binary instead.

### Timing

| Function | Returns | Description |
|----------|---------|-------------|
| `pit_get_ticks()` | `uint32_t` | Ticks since boot at ~100 Hz (≈ 10 ms each) |

Use for animations and double-click detection:
```c
uint32_t t0 = PDAPI->pit_get_ticks();
// ... do work ...
uint32_t elapsed = PDAPI->pit_get_ticks() - t0;  // in ticks (~10ms each)
```

### Session

| Member | Type | Description |
|--------|------|-------------|
| `session_user_ptr` | `const user_t **` | Points to the kernel session user pointer |
| `login_screen_run` | `void (*)(void)` | Run built-in login screen; sets `*session_user_ptr` |

---

## Color Constants

Defined in `de_api.h`:

| Constant | Value | Color |
|----------|-------|-------|
| `GFX_BLACK` | `GFX_RGB(0,0,0)` | Black |
| `GFX_WHITE` | `GFX_RGB(255,255,255)` | White |
| `GFX_RED` | `GFX_RGB(200,30,30)` | Red |
| `GFX_GREEN` | `GFX_RGB(30,180,30)` | Green |
| `GFX_BLUE` | `GFX_RGB(30,80,200)` | Blue |
| `GFX_CYAN` | `GFX_RGB(0,180,200)` | Cyan |
| `GFX_YELLOW` | `GFX_RGB(240,200,0)` | Yellow |
| `GFX_DARK_GREY` | `GFX_RGB(60,60,65)` | Dark grey |
| `GFX_MID_GREY` | `GFX_RGB(130,130,135)` | Mid grey |
| `GFX_LIGHT_GREY` | `GFX_RGB(210,210,215)` | Light grey |

Custom colors: `GFX_RGB(r, g, b)` where each component is 0–255.

---

## Building Your DE

### Prerequisites

- Cross-compiler: `i686-linux-gnu-gcc`  (install with `sudo apt install gcc-i686-linux-gnu`)
- PD-OS SDK headers: `sdk/include/` in the PD-OS source tree

### Compile

```bash
./build_de.sh my_de.c               # output: my_de.bin
./build_de.sh my_de.c build/my_de.bin  # explicit output path
```

Or manually:
```bash
i686-linux-gnu-gcc \
    -m32 -ffreestanding -nostdlib -nostdinc \
    -fno-builtin -fno-stack-protector \
    -fno-pic -fno-pie \
    -Wall -Wextra \
    -I sdk/include \
    -Wl,-Ttext,0x01000000 \
    -Wl,-e,de_main \
    my_de.c -o my_de.bin
```

Key flags explained:
- `-m32` — 32-bit x86 output
- `-ffreestanding -nostdlib -nostdinc` — no standard library whatsoever
- `-Wl,-Ttext,0x01000000` — fix the binary's load address to 16 MB
- `-Wl,-e,de_main` — make `de_main` the ELF entry point (the flat binary tool strips it)

### Size limit

DE binaries must be **≤ 512 KB**.  The build script checks and rejects oversized binaries.

### Multiple source files

```bash
i686-linux-gnu-gcc [flags] -c file1.c -o build/f1.o
i686-linux-gnu-gcc [flags] -c file2.c -o build/f2.o
i686-linux-gnu-ld -m elf_i386 -nostdlib \
    -Ttext 0x01000000 -e de_main \
    build/f1.o build/f2.o \
    -o build/my_de.bin
```

---

## Installing Your DE

### Method 1 — host-side injection (before boot, requires disk image)

```bash
# Build your DE
./build_de.sh sdk/example_de/example_de.c build/example_de.bin

# Inject into the disk image (runs pdfs_inject.py internally)
./build_de.sh --install example_de build/example_de.bin

# Boot
./build_gde.sh run
```

This writes `/sys/de/example_de.bin` and `/sys/de/active` into the PDFS
partition of `build/pd-os-gde.img` without booting the kernel.

### Method 2 — from inside PD-OS (at runtime via the shell)

Once PD-OS is running with the built-in GDE, open a terminal and use the
`write` shell command (or a future file manager) to place the binary on disk,
then write the active name:

```
write /sys/de/my_de.bin   # paste binary somehow — TBD in a future phase
echo my_de > /sys/de/active
reboot
```

### Switching back to the built-in GDE

```bash
python3 tools/pdfs_inject.py build/pd-os-gde.img 1024 /sys/de/active <(printf 'gde')
# or simply delete /sys/de/active — the kernel falls back to GDE automatically
```

---

## Recommended Event Loop Pattern

```c
void de_main(void)
{
    // 1. Optionally run the built-in login screen
    PDAPI->login_screen_run();

    // 2. Draw the static background once and cache it
    draw_background();
    PDAPI->gfx_cache_bg();

    int mx = SCREEN_W / 2, my = SCREEN_H / 2;

    for (;;) {
        int dirty = 0;

        // 3. Drain keyboard
        char k;
        while ((k = PDAPI->keyboard_poll()) != 0) {
            handle_key(k);
            dirty = 1;
        }

        // 4. Drain mouse
        if (PDAPI->mouse_changed()) {
            PDAPI->mouse_clear_changed();
            mx = PDAPI->mouse_get_x();
            my = PDAPI->mouse_get_y();
            dirty = 1;
        }

        // 5. Render only when something changed
        if (dirty) {
            PDAPI->gfx_restore_bg();   // fast background restore
            draw_dynamic_elements(mx, my);
            PDAPI->gfx_flip();
        } else {
            __asm__ volatile ("hlt");  // sleep until next interrupt (~10ms)
        }
    }
}
```

The `hlt` instruction halts the CPU until the next interrupt (PIT at 100 Hz,
keyboard, or mouse IRQ).  This keeps CPU usage near 0% when nothing is happening.

---

## Project Layout

```
pd-os/
├── sdk/
│   ├── include/
│   │   ├── de_api.h        ← Main DE header (include this)
│   │   ├── pdos_types.h    ← Integer types
│   │   ├── pdos_users.h    ← user_t definition
│   │   └── pdos_vfs.h      ← vfs_node_t definition
│   └── example_de/
│       └── example_de.c    ← Full working example DE
├── tools/
│   └── pdfs_inject.py      ← Host-side PDFS file injector
├── build_de.sh             ← Compile + install tool for DEs
└── build_gde.sh            ← Main OS build + run script
```

---

## Common Mistakes

| Mistake | Fix |
|---------|-----|
| `de_main` returns | It must loop forever with `for(;;)` |
| Drawing but nothing appears | You forgot `PDAPI->gfx_flip()` |
| Crashed the kernel / triple fault | A NULL pointer dereference via PDAPI — check all pointers |
| Binary too large | Add `-Os` (optimize for size) or split giant static arrays |
| Colors wrong | Use `GFX_RGB(r,g,b)` — format is `0x00RRGGBB`, not `0xBBGGRR` |
| Keyboard input ignored | Call `keyboard_poll()` in a loop until it returns `0` |
| Mouse cursor flickers | Use `gfx_cache_bg` / `gfx_restore_bg` pattern above |
| `/sys/de/active` not read | PDFS must be formatted first — boot once with built-in GDE |

---

## Example DE

A complete working example is in `sdk/example_de/example_de.c`.  It demonstrates:
- Using the built-in login screen
- Drawing a gradient background
- A centered info panel
- Live mouse cursor drawn in every frame
- Keyboard echo
- Tick-based clock in the taskbar

Build and test it:

```bash
./build_gde.sh all                                      # build OS image
./build_de.sh sdk/example_de/example_de.c build/example_de.bin   # compile DE
./build_de.sh --install example_de build/example_de.bin           # install
./build_gde.sh run                                      # boot
```
